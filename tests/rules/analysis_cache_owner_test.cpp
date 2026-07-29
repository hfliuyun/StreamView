#include <streamview/core/paged_cache.h>
#include <streamview/core/source_fingerprint.h>
#include <streamview/rules/analysis_cache_owner.h>
#include <streamview/rules/rule_package.h>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <chrono>
#include <cstddef>
#include <future>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using streamview::core::AnalysisNodeId;
using streamview::core::AnalysisNodeKind;
using streamview::core::MaterializationState;
using streamview::core::PagedCache;
using streamview::core::PagedCachePageKey;
using streamview::core::PagedCachePageKind;
using streamview::core::PagedCachePageWrite;
using streamview::core::PagedCacheReadStatus;
using streamview::core::SourceFingerprint;
using streamview::core::SourceFingerprintMode;
using streamview::rules::AnalysisCacheBodyDecodeStatus;
using streamview::rules::AnalysisCacheNamespace;
using streamview::rules::AnalysisCacheOwner;
using streamview::rules::AnalysisCacheOwnerFlushStatus;
using streamview::rules::AnalysisCacheOwnerOptions;
using streamview::rules::AnalysisCacheOwnerReadStatus;
using streamview::rules::AnalysisCacheOwnerStartStatus;
using streamview::rules::AnalysisCacheOwnerSubmitStatus;
using streamview::rules::AnalysisCacheOwnerWriteStatus;
using streamview::rules::H264ProgressiveIndexCachePage;
using streamview::rules::H264ProgressiveIndexCacheReadSubmission;
using streamview::rules::MaterializedResultCacheNode;
using streamview::rules::MaterializedResultCachePage;
using streamview::rules::RuleEntryPointIdentity;
using streamview::rules::RulePackageIdentity;

namespace {

class DirectSqliteConnection final {
public:
    explicit DirectSqliteConnection(const QString& path)
        : name_(QStringLiteral("streamview-cache-owner-test-%1")
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

    [[nodiscard]] bool execute(const QString& statement, QString* errorMessage = nullptr) {
        QSqlQuery query(database_);
        if (query.exec(statement)) {
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = query.lastError().text();
        }
        return false;
    }

private:
    QString name_;
    QSqlDatabase database_;
};

[[nodiscard]] AnalysisCacheNamespace cacheNamespace() {
    auto source = SourceFingerprint::create(
        SourceFingerprint::algorithmVersion(), SourceFingerprintMode::FullContentSha256, 5,
        std::nullopt, QByteArray(32, '\x11'));
    Q_ASSERT(source.has_value());
    auto package = RulePackageIdentity::create(QStringLiteral("org.example.packet"),
                                               QStringLiteral("1.2.3"),
                                               QByteArray(32, '\x22'));
    Q_ASSERT(package.has_value());
    auto rule = RuleEntryPointIdentity::create(*package, QStringLiteral("packet"));
    Q_ASSERT(rule.has_value());
    auto result = AnalysisCacheNamespace::create(*source, *rule);
    Q_ASSERT(result.has_value());
    return *result;
}

[[nodiscard]] H264ProgressiveIndexCachePage progressivePage(quint64 pageIndex = 0) {
    H264ProgressiveIndexCachePage page;
    page.key = {PagedCachePageKind::ProgressiveIndex, 7, pageIndex};
    page.firstRecordIndex = pageIndex;
    page.indexedThroughByteOffset = 4 + pageIndex;
    page.endOfSource = true;
    return page;
}

[[nodiscard]] MaterializedResultCachePage materializedPage() {
    MaterializedResultCachePage page;
    page.key = {PagedCachePageKind::MaterializedResult, 11, 0};
    MaterializedResultCacheNode root;
    root.id = AnalysisNodeId(1);
    root.spec.kind = AnalysisNodeKind::Root;
    root.spec.name = QStringLiteral("root");
    root.spec.state = MaterializationState::Materialized;
    page.nodes.push_back(std::move(root));
    return page;
}

template <typename Result>
[[nodiscard]] bool finishes(std::future<Result>& future) {
    return future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
}

} // namespace

class AnalysisCacheOwnerTest final : public QObject {
    Q_OBJECT

private slots:
    void roundTripsTypedPagesFromCallerThreads() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("analysis-cache.sqlite"));
        auto started = AnalysisCacheOwner::start(path, cacheNamespace());
        QVERIFY2(started.succeeded(), qPrintable(started.errorMessage));
        QCOMPARE(started.cacheStatus,
                 std::optional(streamview::core::PagedCacheOpenStatus::Opened));

        auto progressiveWrite =
            started.owner->writeProgressiveIndex({progressivePage()});
        QVERIFY2(progressiveWrite.accepted(), qPrintable(progressiveWrite.errorMessage));
        QVERIFY(finishes(progressiveWrite.completion));
        QVERIFY2(progressiveWrite.completion.get().succeeded(), "progressive write failed");

        auto materializedWrite =
            started.owner->writeMaterializedResult({materializedPage()});
        QVERIFY2(materializedWrite.accepted(), qPrintable(materializedWrite.errorMessage));
        QVERIFY(finishes(materializedWrite.completion));
        QVERIFY2(materializedWrite.completion.get().succeeded(), "materialized write failed");

        std::optional<H264ProgressiveIndexCacheReadSubmission> submittedFromThread;
        std::thread caller([&] {
            submittedFromThread.emplace(started.owner->readProgressiveIndex(
                {PagedCachePageKind::ProgressiveIndex, 7, 0}));
        });
        caller.join();
        QVERIFY(submittedFromThread.has_value());
        QVERIFY2(submittedFromThread->accepted(),
                 qPrintable(submittedFromThread->errorMessage));
        QVERIFY(finishes(submittedFromThread->completion));
        const auto progressiveRead = submittedFromThread->completion.get();
        QVERIFY2(progressiveRead.found(), qPrintable(progressiveRead.errorMessage));
        QCOMPARE(progressiveRead.page->indexedThroughByteOffset, quint64{4});
        QVERIFY(progressiveRead.page->endOfSource);

        auto materializedRead = started.owner->readMaterializedResult(
            {PagedCachePageKind::MaterializedResult, 11, 0});
        QVERIFY(materializedRead.accepted());
        QVERIFY(finishes(materializedRead.completion));
        const auto materialized = materializedRead.completion.get();
        QVERIFY2(materialized.found(), qPrintable(materialized.errorMessage));
        QCOMPARE(materialized.page->nodes.size(), std::size_t{1});

        auto missing = started.owner->readProgressiveIndex(
            {PagedCachePageKind::ProgressiveIndex, 7, 99});
        QVERIFY(missing.accepted());
        QVERIFY(finishes(missing.completion));
        const auto missingResult = missing.completion.get();
        QCOMPARE(missingResult.status, AnalysisCacheOwnerReadStatus::Missing);
        QCOMPARE(missingResult.cacheStatus,
                 std::optional(streamview::core::PagedCacheReadStatus::Missing));
    }

    void preflightsInvalidWritesAndReadKeysBeforeQueueing() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto started = AnalysisCacheOwner::start(
            directory.filePath(QStringLiteral("analysis-cache.sqlite")), cacheNamespace());
        QVERIFY2(started.succeeded(), qPrintable(started.errorMessage));

        auto empty = started.owner->writeProgressiveIndex({});
        QCOMPARE(empty.status, AnalysisCacheOwnerSubmitStatus::InvalidArgument);
        QVERIFY(!empty.completion.valid());

        auto wrongKind = progressivePage();
        wrongKind.key.kind = PagedCachePageKind::MaterializedResult;
        QCOMPARE(started.owner->writeProgressiveIndex({wrongKind}).status,
                 AnalysisCacheOwnerSubmitStatus::InvalidArgument);

        auto transient = materializedPage();
        transient.nodes.front().spec.state = MaterializationState::Indexing;
        QCOMPARE(started.owner->writeMaterializedResult({transient}).status,
                 AnalysisCacheOwnerSubmitStatus::InvalidArgument);

        const auto duplicate = progressivePage();
        QCOMPARE(started.owner->writeProgressiveIndex({duplicate, duplicate}).status,
                 AnalysisCacheOwnerSubmitStatus::InvalidArgument);

        QCOMPARE(started.owner->readProgressiveIndex(
                     {PagedCachePageKind::MaterializedResult, 7, 0})
                     .status,
                 AnalysisCacheOwnerSubmitStatus::InvalidArgument);
        QCOMPARE(started.owner->readProgressiveIndex(
                     {PagedCachePageKind::ProgressiveIndex,
                      static_cast<quint64>(std::numeric_limits<qlonglong>::max()) + 1U, 0})
                     .status,
                 AnalysisCacheOwnerSubmitStatus::InvalidArgument);

        auto valid = started.owner->writeProgressiveIndex({progressivePage()});
        QVERIFY(valid.accepted());
        QVERIFY(finishes(valid.completion));
        QVERIFY(valid.completion.get().succeeded());
    }

    void rejectsQueuePressureWithoutBlockingOrRetainingTheRequest() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("analysis-cache.sqlite"));
        AnalysisCacheOwnerOptions options;
        options.maximumOutstandingRequests = 1;
        auto started = AnalysisCacheOwner::start(path, cacheNamespace(), options);
        QVERIFY2(started.succeeded(), qPrintable(started.errorMessage));

        DirectSqliteConnection blocker(path);
        QVERIFY2(blocker.isOpen(), qPrintable(blocker.errorMessage()));
        QString sqlError;
        QVERIFY2(blocker.execute(QStringLiteral("BEGIN IMMEDIATE"), &sqlError),
                 qPrintable(sqlError));

        auto first = started.owner->writeProgressiveIndex({progressivePage()});
        QVERIFY(first.accepted());
        auto rejected = started.owner->readProgressiveIndex(
            {PagedCachePageKind::ProgressiveIndex, 7, 0});
        QCOMPARE(rejected.status, AnalysisCacheOwnerSubmitStatus::QueueFull);
        QVERIFY(!rejected.completion.valid());

        QVERIFY2(blocker.execute(QStringLiteral("ROLLBACK"), &sqlError), qPrintable(sqlError));
        QVERIFY(finishes(first.completion));
        QVERIFY2(first.completion.get().succeeded(), "blocked write did not recover");

        auto acceptedAfterDrain = started.owner->readProgressiveIndex(
            {PagedCachePageKind::ProgressiveIndex, 7, 0});
        QVERIFY(acceptedAfterDrain.accepted());
        QVERIFY(finishes(acceptedAfterDrain.completion));
        QVERIFY(acceptedAfterDrain.completion.get().found());
    }

    void enforcesTheRetainedWriteByteBudgetIndependently() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AnalysisCacheOwnerOptions options;
        options.maximumOutstandingRequests = 2;
        options.maximumRetainedWriteBytes = 1;
        auto started = AnalysisCacheOwner::start(
            directory.filePath(QStringLiteral("analysis-cache.sqlite")), cacheNamespace(),
            options);
        QVERIFY2(started.succeeded(), qPrintable(started.errorMessage));

        auto rejected = started.owner->writeProgressiveIndex({progressivePage()});
        QCOMPARE(rejected.status, AnalysisCacheOwnerSubmitStatus::QueueFull);
        QVERIFY(!rejected.completion.valid());

        auto read = started.owner->readProgressiveIndex(
            {PagedCachePageKind::ProgressiveIndex, 7, 0});
        QVERIFY(read.accepted());
        QVERIFY(finishes(read.completion));
        QCOMPARE(read.completion.get().status, AnalysisCacheOwnerReadStatus::Missing);
    }

    void drainingShutdownCompletesAcceptedWorkAndReleasesTheDatabase() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("analysis-cache.sqlite"));
        const auto cacheId = cacheNamespace();
        auto started = AnalysisCacheOwner::start(path, cacheId);
        QVERIFY2(started.succeeded(), qPrintable(started.errorMessage));

        auto flushed = started.owner->writeProgressiveIndex({progressivePage()});
        QVERIFY(flushed.accepted());
        QCOMPARE(started.owner->flush(), AnalysisCacheOwnerFlushStatus::Drained);
        QCOMPARE(flushed.completion.wait_for(std::chrono::seconds(0)),
                 std::future_status::ready);
        QVERIFY(flushed.completion.get().succeeded());

        auto pending = started.owner->writeProgressiveIndex({progressivePage(1)});
        QVERIFY(pending.accepted());
        started.owner->shutdown();
        QVERIFY(finishes(pending.completion));
        QVERIFY2(pending.completion.get().succeeded(), "shutdown dropped accepted work");
        QCOMPARE(started.owner->flush(), AnalysisCacheOwnerFlushStatus::ShutDown);
        QCOMPARE(started.owner->readProgressiveIndex(
                     {PagedCachePageKind::ProgressiveIndex, 7, 0})
                     .status,
                 AnalysisCacheOwnerSubmitStatus::ShuttingDown);

        auto reopened = PagedCache::open(path, cacheId.name());
        QVERIFY2(reopened.succeeded(), qPrintable(reopened.errorMessage));
        QCOMPARE(reopened.cache
                     ->readPage({PagedCachePageKind::ProgressiveIndex, 7, 1})
                     .status,
                 PagedCacheReadStatus::Found);
    }

    void rejectsAValidBodyCopiedUnderAnotherFullPageKey() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("analysis-cache.sqlite"));
        const auto cacheId = cacheNamespace();
        {
            auto started = AnalysisCacheOwner::start(path, cacheId);
            QVERIFY2(started.succeeded(), qPrintable(started.errorMessage));
            auto write = started.owner->writeProgressiveIndex({progressivePage(0)});
            QVERIFY(write.accepted());
            QVERIFY(finishes(write.completion));
            QVERIFY(write.completion.get().succeeded());
        }

        {
            auto raw = PagedCache::open(path, cacheId.name());
            QVERIFY2(raw.succeeded(), qPrintable(raw.errorMessage));
            const auto original =
                raw.cache->readPage({PagedCachePageKind::ProgressiveIndex, 7, 0});
            QVERIFY(original.found());
            const std::vector<PagedCachePageWrite> copied{{
                {PagedCachePageKind::ProgressiveIndex, 7, 1}, original.bytes}};
            QVERIFY(raw.cache->commitBatch(copied).succeeded());
        }

        auto started = AnalysisCacheOwner::start(path, cacheId);
        QVERIFY2(started.succeeded(), qPrintable(started.errorMessage));
        auto copiedRead = started.owner->readProgressiveIndex(
            {PagedCachePageKind::ProgressiveIndex, 7, 1});
        QVERIFY(copiedRead.accepted());
        QVERIFY(finishes(copiedRead.completion));
        const auto result = copiedRead.completion.get();
        QCOMPARE(result.status, AnalysisCacheOwnerReadStatus::Corrupt);
        QCOMPARE(result.bodyStatus,
                 std::optional(AnalysisCacheBodyDecodeStatus::PageKeyMismatch));
        QVERIFY(!result.page.has_value());

        auto laterWrite = started.owner->writeProgressiveIndex({progressivePage(2)});
        QVERIFY(laterWrite.accepted());
        QVERIFY(finishes(laterWrite.completion));
        QVERIFY(laterWrite.completion.get().succeeded());
    }

    void keepsProcessingAfterAnAtomicStorageFailure() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("analysis-cache.sqlite"));
        auto started = AnalysisCacheOwner::start(path, cacheNamespace());
        QVERIFY2(started.succeeded(), qPrintable(started.errorMessage));

        DirectSqliteConnection injector(path);
        QVERIFY2(injector.isOpen(), qPrintable(injector.errorMessage()));
        QString sqlError;
        QVERIFY2(injector.execute(
                     QStringLiteral(
                         "CREATE TRIGGER fail_owner_page BEFORE INSERT ON cache_pages "
                         "WHEN NEW.stream_id = 9 BEGIN "
                         "SELECT RAISE(ABORT, 'forced owner test failure'); END"),
                     &sqlError),
                 qPrintable(sqlError));

        auto rejectedPage = progressivePage();
        rejectedPage.key.streamId = 9;
        auto rejected = started.owner->writeProgressiveIndex({rejectedPage});
        QVERIFY(rejected.accepted());
        QVERIFY(finishes(rejected.completion));
        const auto rejectedResult = rejected.completion.get();
        QCOMPARE(rejectedResult.status, AnalysisCacheOwnerWriteStatus::StorageError);
        QCOMPARE(rejectedResult.cacheStatus,
                 std::optional(streamview::core::PagedCacheCommitStatus::StorageError));

        QVERIFY2(injector.execute(QStringLiteral("DROP TRIGGER fail_owner_page"), &sqlError),
                 qPrintable(sqlError));
        auto later = started.owner->writeProgressiveIndex({progressivePage(1)});
        QVERIFY(later.accepted());
        QVERIFY(finishes(later.completion));
        QVERIFY2(later.completion.get().succeeded(), "worker stopped after storage failure");
    }

    void reportsInvalidOptionsAndWorkerCacheOpenFailure() {
        const auto cacheId = cacheNamespace();
        AnalysisCacheOwnerOptions invalid;
        invalid.maximumOutstandingRequests = 0;
        const auto invalidStart =
            AnalysisCacheOwner::start(QStringLiteral("unused.sqlite"), cacheId, invalid);
        QCOMPARE(invalidStart.status, AnalysisCacheOwnerStartStatus::InvalidArgument);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto failed = AnalysisCacheOwner::start(directory.path(), cacheId);
        QCOMPARE(failed.status, AnalysisCacheOwnerStartStatus::CacheOpenFailed);
        QCOMPARE(failed.cacheStatus,
                 std::optional(streamview::core::PagedCacheOpenStatus::InvalidArgument));
        QVERIFY(failed.owner == nullptr);
    }
};

QTEST_GUILESS_MAIN(AnalysisCacheOwnerTest)
#include "analysis_cache_owner_test.moc"
