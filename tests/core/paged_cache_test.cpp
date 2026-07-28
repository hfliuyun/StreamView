#include <streamview/core/paged_cache.h>

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using streamview::core::PagedCache;
using streamview::core::PagedCacheCommitStatus;
using streamview::core::PagedCacheOpenStatus;
using streamview::core::PagedCachePageKey;
using streamview::core::PagedCachePageKind;
using streamview::core::PagedCachePageWrite;
using streamview::core::PagedCacheReadStatus;

namespace {

class DirectSqliteConnection final {
  public:
    explicit DirectSqliteConnection(const QString& path)
        : name_(QStringLiteral("streamview-paged-cache-test-%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))),
          database_(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_)) {
        database_.setDatabaseName(path);
        database_.open();
    }

    ~DirectSqliteConnection() {
        database_.close();
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }

    DirectSqliteConnection(const DirectSqliteConnection&) = delete;
    DirectSqliteConnection& operator=(const DirectSqliteConnection&) = delete;

    [[nodiscard]] bool isOpen() const noexcept { return database_.isOpen(); }
    [[nodiscard]] QString errorMessage() const { return database_.lastError().text(); }
    [[nodiscard]] QSqlDatabase& database() noexcept { return database_; }

  private:
    QString name_;
    QSqlDatabase database_;
};

QString databasePath(const QTemporaryDir& directory) {
    return directory.filePath(QStringLiteral("analysis-cache.sqlite"));
}

std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    std::transform(values.begin(), values.end(), std::back_inserter(result),
                   [](unsigned int value) { return static_cast<std::byte>(value); });
    return result;
}

PagedCachePageWrite page(PagedCachePageKind kind, quint64 streamId, quint64 pageIndex,
                         std::vector<std::byte> payload) {
    return {{kind, streamId, pageIndex}, std::move(payload)};
}

bool execute(QSqlDatabase& database, const QString& statement, QString* errorMessage = nullptr) {
    QSqlQuery query(database);
    if (query.exec(statement)) {
        return true;
    }
    if (errorMessage != nullptr) {
        *errorMessage = query.lastError().text();
    }
    return false;
}

QVariant scalar(QSqlDatabase& database, const QString& statement, QString* errorMessage = nullptr) {
    QSqlQuery query(database);
    if (!query.exec(statement) || !query.next()) {
        if (errorMessage != nullptr) {
            *errorMessage = query.lastError().text();
        }
        return {};
    }
    return query.value(0);
}

} // namespace

class PagedCacheTest final : public QObject {
    Q_OBJECT

  private slots:
    void reportsQsqliteRuntimeAvailability() {
        QVERIFY2(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")),
                 qPrintable(QSqlDatabase::drivers().join(QStringLiteral(", "))));
    }

    void opensANewVersionedWalStoreAndRemovesItsConnection() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = databasePath(directory);
        const auto connectionsBefore = QSqlDatabase::connectionNames();

        auto opened = PagedCache::open(path, QStringLiteral("session-one"));

        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        QCOMPARE(opened.recoveredIncompleteBatchCount, quint64{0});
        const auto connectionsDuring = QSqlDatabase::connectionNames();
        QCOMPARE(connectionsDuring.size(), connectionsBefore.size() + 1);
        QString cacheConnectionName;
        for (const QString& connectionName : connectionsDuring) {
            if (!connectionsBefore.contains(connectionName)) {
                cacheConnectionName = connectionName;
            }
        }
        QVERIFY(!cacheConnectionName.isEmpty());
        {
            QSqlDatabase configured = QSqlDatabase::database(cacheConnectionName, false);
            QCOMPARE(scalar(configured, QStringLiteral("PRAGMA foreign_keys")).toInt(), 1);
            QCOMPARE(scalar(configured, QStringLiteral("PRAGMA synchronous")).toInt(), 1);
            QCOMPARE(scalar(configured, QStringLiteral("PRAGMA busy_timeout")).toInt(), 5000);
        }
        opened.cache.reset();
        QCOMPARE(QSqlDatabase::connectionNames(), connectionsBefore);

        DirectSqliteConnection database(path);
        QVERIFY2(database.isOpen(), qPrintable(database.errorMessage()));
        QCOMPARE(
            scalar(database.database(), QStringLiteral("PRAGMA journal_mode")).toString().toLower(),
            QStringLiteral("wal"));
        QCOMPARE(scalar(database.database(), QStringLiteral("PRAGMA user_version")).toUInt(),
                 PagedCache::schemaVersion());
        QVERIFY(scalar(database.database(), QStringLiteral("PRAGMA application_id")).toUInt() !=
                0U);
    }

    void commitsReadsAndReplacesOpaquePages() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto opened = PagedCache::open(databasePath(directory), QStringLiteral("session-pages"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));

        const std::vector<PagedCachePageWrite> firstBatch{
            page(PagedCachePageKind::ProgressiveIndex, 4, 0, bytes({0x00, 0x11, 0xFF})),
            page(PagedCachePageKind::MaterializedResult, 8, 3, bytes({0x22, 0x00, 0x33})),
        };
        const auto committed = opened.cache->commitBatch(firstBatch);
        QVERIFY2(committed.succeeded(), qPrintable(committed.errorMessage));

        const auto index = opened.cache->readPage(firstBatch.at(0).key);
        QCOMPARE(index.status, PagedCacheReadStatus::Found);
        QCOMPARE(index.bytes, firstBatch.at(0).bytes);
        const auto result = opened.cache->readPage(firstBatch.at(1).key);
        QCOMPARE(result.status, PagedCacheReadStatus::Found);
        QCOMPARE(result.bytes, firstBatch.at(1).bytes);
        QCOMPARE(opened.cache->readPage({PagedCachePageKind::ProgressiveIndex, 4, 99}).status,
                 PagedCacheReadStatus::Missing);

        const std::vector<PagedCachePageWrite> replacement{
            page(PagedCachePageKind::ProgressiveIndex, 4, 0, bytes({0x44, 0x55})),
        };
        QVERIFY(opened.cache->commitBatch(replacement).succeeded());
        QCOMPARE(opened.cache->readPage(replacement.front().key).bytes, replacement.front().bytes);
    }

    void enforcesPageAndBatchBoundsBeforeStorage() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto opened = PagedCache::open(databasePath(directory), QStringLiteral("session-bounds"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));

        QVERIFY(!opened.cache->commitBatch({}).succeeded());
        const std::vector<PagedCachePageWrite> emptyPayload{
            page(PagedCachePageKind::ProgressiveIndex, 1, 0, {}),
        };
        QCOMPARE(opened.cache->commitBatch(emptyPayload).status,
                 PagedCacheCommitStatus::InvalidBatch);

        const std::vector<PagedCachePageWrite> oversized{
            page(PagedCachePageKind::ProgressiveIndex, 1, 0,
                 std::vector<std::byte>(PagedCache::pageSizeBytes() + 1U)),
        };
        QCOMPARE(opened.cache->commitBatch(oversized).status, PagedCacheCommitStatus::InvalidBatch);

        const std::vector<PagedCachePageWrite> duplicate{
            page(PagedCachePageKind::ProgressiveIndex, 1, 2, bytes({0x01})),
            page(PagedCachePageKind::ProgressiveIndex, 1, 2, bytes({0x02})),
        };
        QCOMPARE(opened.cache->commitBatch(duplicate).status, PagedCacheCommitStatus::InvalidBatch);

        std::vector<PagedCachePageWrite> maximumBatch;
        maximumBatch.reserve(PagedCache::maximumBatchPages());
        for (std::size_t index = 0; index < PagedCache::maximumBatchPages(); ++index) {
            maximumBatch.push_back(page(PagedCachePageKind::ProgressiveIndex, 2,
                                        static_cast<quint64>(index), bytes({0x7F})));
        }
        QVERIFY(opened.cache->commitBatch(maximumBatch).succeeded());
        maximumBatch.push_back(page(PagedCachePageKind::ProgressiveIndex, 2, 256, bytes({0x7F})));
        QCOMPARE(opened.cache->commitBatch(maximumBatch).status,
                 PagedCacheCommitStatus::InvalidBatch);

        const auto beyondSqliteInteger =
            static_cast<quint64>(std::numeric_limits<qlonglong>::max()) + 1U;
        const std::vector<PagedCachePageWrite> invalidCoordinate{
            page(PagedCachePageKind::ProgressiveIndex, beyondSqliteInteger, 0, bytes({0x01})),
        };
        QCOMPARE(opened.cache->commitBatch(invalidCoordinate).status,
                 PagedCacheCommitStatus::InvalidBatch);
        QCOMPARE(opened.cache->readPage(invalidCoordinate.front().key).status,
                 PagedCacheReadStatus::InvalidRequest);

        const auto invalidKind = static_cast<PagedCachePageKind>(255);
        const std::vector<PagedCachePageWrite> invalidKindPage{
            page(invalidKind, 0, 0, bytes({0x01})),
        };
        QCOMPARE(opened.cache->commitBatch(invalidKindPage).status,
                 PagedCacheCommitStatus::InvalidBatch);
    }

    void isolatesNamespacesAndAcceptsAFullPage() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = databasePath(directory);
        const PagedCachePageKey key{PagedCachePageKind::MaterializedResult, 3, 7};
        std::vector<std::byte> fullPayload(PagedCache::pageSizeBytes(), std::byte{0x5A});

        auto first = PagedCache::open(path, QStringLiteral("session-first"));
        QVERIFY2(first.succeeded(), qPrintable(first.errorMessage));
        const std::vector<PagedCachePageWrite> firstBatch{{key, fullPayload}};
        QVERIFY(first.cache->commitBatch(firstBatch).succeeded());
        first.cache.reset();

        auto second = PagedCache::open(path, QStringLiteral("session-second"));
        QVERIFY2(second.succeeded(), qPrintable(second.errorMessage));
        QCOMPARE(second.cache->readPage(key).status, PagedCacheReadStatus::Missing);
        const std::vector<PagedCachePageWrite> secondBatch{
            {key, bytes({0x11, 0x22})},
        };
        QVERIFY(second.cache->commitBatch(secondBatch).succeeded());
        second.cache.reset();

        first = PagedCache::open(path, QStringLiteral("session-first"));
        QVERIFY2(first.succeeded(), qPrintable(first.errorMessage));
        const auto restored = first.cache->readPage(key);
        QCOMPARE(restored.status, PagedCacheReadStatus::Found);
        QCOMPARE(restored.bytes, fullPayload);
    }

    void rejectsASecondLiveOwnerOfTheDatabasePath() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = databasePath(directory);
        auto first = PagedCache::open(path, QStringLiteral("session-first"));
        QVERIFY2(first.succeeded(), qPrintable(first.errorMessage));
        QTest::qWait(1100);

        const auto second = PagedCache::open(path, QStringLiteral("session-second"));

        QCOMPARE(second.status, PagedCacheOpenStatus::OpenFailed);
        QVERIFY(!second.errorMessage.isEmpty());
        QCOMPARE(first.cache->readPage({PagedCachePageKind::ProgressiveIndex, 0, 0}).status,
                 PagedCacheReadStatus::Missing);
    }

    void rollsBackEveryPageWhenOneWriteFails() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = databasePath(directory);
        auto opened = PagedCache::open(path, QStringLiteral("session-atomic"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        opened.cache.reset();

        {
            DirectSqliteConnection database(path);
            QVERIFY2(database.isOpen(), qPrintable(database.errorMessage()));
            QString sqlError;
            QVERIFY2(
                execute(database.database(),
                        QStringLiteral("CREATE TRIGGER fail_test_page BEFORE INSERT ON cache_pages "
                                       "WHEN NEW.stream_id = 9 BEGIN "
                                       "SELECT RAISE(ABORT, 'forced test failure'); END"),
                        &sqlError),
                qPrintable(sqlError));
        }

        opened = PagedCache::open(path, QStringLiteral("session-atomic"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        const std::vector<PagedCachePageWrite> batch{
            page(PagedCachePageKind::ProgressiveIndex, 8, 0, bytes({0x01})),
            page(PagedCachePageKind::ProgressiveIndex, 9, 0, bytes({0x02})),
        };

        const auto failed = opened.cache->commitBatch(batch);

        QCOMPARE(failed.status, PagedCacheCommitStatus::StorageError);
        QCOMPARE(opened.cache->readPage(batch.front().key).status, PagedCacheReadStatus::Missing);
    }

    void recoversAnAbandonedBatchMarkerOnOpen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = databasePath(directory);
        auto opened = PagedCache::open(path, QStringLiteral("session-recovery"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        opened.cache.reset();

        {
            DirectSqliteConnection database(path);
            QVERIFY2(database.isOpen(), qPrintable(database.errorMessage()));
            QSqlQuery marker(database.database());
            QVERIFY(marker.prepare(
                QStringLiteral("INSERT INTO pending_batches(token, namespace_name, started_at) "
                               "VALUES (?, ?, ?)")));
            marker.addBindValue(QStringLiteral("abandoned-test-batch"));
            marker.addBindValue(QStringLiteral("session-recovery"));
            marker.addBindValue(1);
            QVERIFY2(marker.exec(), qPrintable(marker.lastError().text()));
        }

        opened = PagedCache::open(path, QStringLiteral("session-recovery"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        QCOMPARE(opened.recoveredIncompleteBatchCount, quint64{1});
        opened.cache.reset();
        const auto cleanReopen = PagedCache::open(path, QStringLiteral("session-recovery"));
        QVERIFY2(cleanReopen.succeeded(), qPrintable(cleanReopen.errorMessage));
        QCOMPARE(cleanReopen.recoveredIncompleteBatchCount, quint64{0});
    }

    void rejectsIncompatibleAndMalformedStores() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString futurePath = directory.filePath(QStringLiteral("future.sqlite"));
        auto opened = PagedCache::open(futurePath, QStringLiteral("session-schema"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        opened.cache.reset();
        {
            DirectSqliteConnection database(futurePath);
            QVERIFY2(database.isOpen(), qPrintable(database.errorMessage()));
            QVERIFY(execute(database.database(), QStringLiteral("PRAGMA user_version = 2")));
        }
        QCOMPARE(PagedCache::open(futurePath, QStringLiteral("session-schema")).status,
                 PagedCacheOpenStatus::IncompatibleSchema);

        const QString missingTablePath = directory.filePath(QStringLiteral("missing-table.sqlite"));
        opened = PagedCache::open(missingTablePath, QStringLiteral("session-schema"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        opened.cache.reset();
        {
            DirectSqliteConnection database(missingTablePath);
            QVERIFY2(database.isOpen(), qPrintable(database.errorMessage()));
            QVERIFY(execute(database.database(), QStringLiteral("DROP TABLE cache_pages")));
        }
        QCOMPARE(PagedCache::open(missingTablePath, QStringLiteral("session-schema")).status,
                 PagedCacheOpenStatus::CorruptStore);

        const QString wrongDefinitionPath =
            directory.filePath(QStringLiteral("wrong-definition.sqlite"));
        opened = PagedCache::open(wrongDefinitionPath, QStringLiteral("session-schema"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        opened.cache.reset();
        {
            DirectSqliteConnection database(wrongDefinitionPath);
            QVERIFY2(database.isOpen(), qPrintable(database.errorMessage()));
            QVERIFY(execute(database.database(),
                            QStringLiteral("ALTER TABLE cache_pages RENAME TO old_cache_pages")));
            QVERIFY(
                execute(database.database(),
                        QStringLiteral("CREATE TABLE cache_pages ("
                                       "namespace_name TEXT, page_kind INTEGER, stream_id INTEGER, "
                                       "page_index INTEGER, payload BLOB)")));
            QVERIFY(execute(database.database(), QStringLiteral("DROP TABLE old_cache_pages")));
        }
        QCOMPARE(PagedCache::open(wrongDefinitionPath, QStringLiteral("session-schema")).status,
                 PagedCacheOpenStatus::CorruptStore);

        const QString corruptPath = directory.filePath(QStringLiteral("corrupt.sqlite"));
        QFile corrupt(corruptPath);
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        QCOMPARE(corrupt.write("not a sqlite database"), qint64{21});
        corrupt.close();
        QCOMPARE(PagedCache::open(corruptPath, QStringLiteral("session-schema")).status,
                 PagedCacheOpenStatus::CorruptStore);
    }

    void rejectsWrongThreadAccessWithoutTouchingStorage() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto opened = PagedCache::open(databasePath(directory), QStringLiteral("session-thread"));
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        PagedCacheReadStatus status = PagedCacheReadStatus::StorageError;

        std::thread otherThread([&opened, &status]() {
            status = opened.cache->readPage({PagedCachePageKind::ProgressiveIndex, 0, 0}).status;
        });
        otherThread.join();

        QCOMPARE(status, PagedCacheReadStatus::ThreadViolation);
        QCOMPARE(opened.cache->readPage({PagedCachePageKind::ProgressiveIndex, 0, 0}).status,
                 PagedCacheReadStatus::Missing);
    }

    void rejectsInvalidOpenArguments() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QCOMPARE(PagedCache::open({}, QStringLiteral("session")).status,
                 PagedCacheOpenStatus::InvalidArgument);
        QCOMPARE(PagedCache::open(databasePath(directory), {}).status,
                 PagedCacheOpenStatus::InvalidArgument);
        QCOMPARE(
            PagedCache::open(databasePath(directory),
                             QString(PagedCache::maximumNamespaceLength() + 1, QLatin1Char('x')))
                .status,
            PagedCacheOpenStatus::InvalidArgument);
    }
};

QTEST_GUILESS_MAIN(PagedCacheTest)

#include "paged_cache_test.moc"
