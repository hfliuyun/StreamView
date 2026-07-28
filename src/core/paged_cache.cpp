#include <streamview/core/paged_cache.h>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>

#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace streamview::core {
namespace {

constexpr quint32 kApplicationId = 0x53564348U;
constexpr int kBusyTimeoutMilliseconds = 5000;
constexpr auto kLockStaleTime = std::chrono::milliseconds::zero();
constexpr auto kLockWaitTime = std::chrono::milliseconds(250);

QString createNamespacesSql() {
    return QStringLiteral("CREATE TABLE cache_namespaces ("
                          "namespace_name TEXT PRIMARY KEY NOT NULL "
                          "CHECK(length(namespace_name) BETWEEN 1 AND 512)"
                          ") WITHOUT ROWID");
}

QString createPagesSql() {
    return QStringLiteral("CREATE TABLE cache_pages ("
                          "namespace_name TEXT NOT NULL, "
                          "page_kind INTEGER NOT NULL CHECK(page_kind IN (0, 1)), "
                          "stream_id INTEGER NOT NULL CHECK(stream_id >= 0), "
                          "page_index INTEGER NOT NULL CHECK(page_index >= 0), "
                          "payload BLOB NOT NULL CHECK(length(payload) BETWEEN 1 AND 65536), "
                          "PRIMARY KEY(namespace_name, page_kind, stream_id, page_index), "
                          "FOREIGN KEY(namespace_name) REFERENCES cache_namespaces(namespace_name) "
                          "ON DELETE CASCADE"
                          ") WITHOUT ROWID");
}

QString createPendingBatchesSql() {
    return QStringLiteral("CREATE TABLE pending_batches ("
                          "token TEXT PRIMARY KEY NOT NULL, "
                          "namespace_name TEXT NOT NULL, "
                          "started_at INTEGER NOT NULL, "
                          "FOREIGN KEY(namespace_name) REFERENCES cache_namespaces(namespace_name) "
                          "ON DELETE CASCADE"
                          ") WITHOUT ROWID");
}

QString canonicalSql(QString sql) {
    sql = sql.toLower();
    sql.remove(QLatin1Char(' '));
    sql.remove(QLatin1Char('\n'));
    sql.remove(QLatin1Char('\r'));
    sql.remove(QLatin1Char('\t'));
    return sql;
}

QString sqlErrorMessage(const QString& operation, const QSqlError& error) {
    const QString detail = error.text().trimmed();
    return detail.isEmpty() ? operation : QStringLiteral("%1: %2").arg(operation, detail);
}

void closeAndRemoveConnection(const QString& connectionName) {
    {
        QSqlDatabase database = QSqlDatabase::database(connectionName, false);
        if (database.isValid()) {
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
}

class ConnectionRegistration final {
  public:
    explicit ConnectionRegistration(QString connectionName)
        : connectionName_(std::move(connectionName)) {}

    ~ConnectionRegistration() {
        if (!released_) {
            closeAndRemoveConnection(connectionName_);
        }
    }

    ConnectionRegistration(const ConnectionRegistration&) = delete;
    ConnectionRegistration& operator=(const ConnectionRegistration&) = delete;

    void release() noexcept { released_ = true; }

  private:
    QString connectionName_;
    bool released_ = false;
};

bool executeSql(const QSqlDatabase& database, const QString& statement,
                QString* errorMessage = nullptr) {
    QSqlQuery query(database);
    if (query.exec(statement)) {
        return true;
    }
    if (errorMessage != nullptr) {
        *errorMessage = sqlErrorMessage(statement, query.lastError());
    }
    return false;
}

std::optional<qlonglong> queryInteger(const QSqlDatabase& database, const QString& statement,
                                      QString* errorMessage) {
    QSqlQuery query(database);
    if (!query.exec(statement)) {
        if (errorMessage != nullptr) {
            *errorMessage = sqlErrorMessage(statement, query.lastError());
        }
        return std::nullopt;
    }
    if (!query.next()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("%1 returned no value").arg(statement);
        }
        return std::nullopt;
    }

    bool converted = false;
    const qlonglong value = query.value(0).toLongLong(&converted);
    if (!converted) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("%1 returned a non-integer value").arg(statement);
        }
        return std::nullopt;
    }
    return value;
}

std::optional<QString> queryText(const QSqlDatabase& database, const QString& statement,
                                 QString* errorMessage) {
    QSqlQuery query(database);
    if (!query.exec(statement)) {
        if (errorMessage != nullptr) {
            *errorMessage = sqlErrorMessage(statement, query.lastError());
        }
        return std::nullopt;
    }
    if (!query.next()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("%1 returned no value").arg(statement);
        }
        return std::nullopt;
    }
    return query.value(0).toString();
}

bool configureConnection(const QSqlDatabase& database, QString* errorMessage) {
    if (!executeSql(database, QStringLiteral("PRAGMA busy_timeout = 5000"), errorMessage)) {
        return false;
    }
    const auto busyTimeout =
        queryInteger(database, QStringLiteral("PRAGMA busy_timeout"), errorMessage);
    if (!busyTimeout || *busyTimeout != kBusyTimeoutMilliseconds) {
        if (busyTimeout && errorMessage != nullptr) {
            *errorMessage = QStringLiteral("SQLite busy timeout was not applied");
        }
        return false;
    }

    const auto journalMode =
        queryText(database, QStringLiteral("PRAGMA journal_mode = WAL"), errorMessage);
    if (!journalMode || journalMode->compare(QStringLiteral("wal"), Qt::CaseInsensitive) != 0) {
        if (journalMode && errorMessage != nullptr) {
            *errorMessage = QStringLiteral("SQLite did not enable WAL journal mode");
        }
        return false;
    }

    if (!executeSql(database, QStringLiteral("PRAGMA foreign_keys = ON"), errorMessage)) {
        return false;
    }
    const auto foreignKeys =
        queryInteger(database, QStringLiteral("PRAGMA foreign_keys"), errorMessage);
    if (!foreignKeys || *foreignKeys != 1) {
        if (foreignKeys && errorMessage != nullptr) {
            *errorMessage = QStringLiteral("SQLite foreign-key enforcement was not enabled");
        }
        return false;
    }

    if (!executeSql(database, QStringLiteral("PRAGMA synchronous = NORMAL"), errorMessage)) {
        return false;
    }
    const auto synchronous =
        queryInteger(database, QStringLiteral("PRAGMA synchronous"), errorMessage);
    if (!synchronous || *synchronous != 1) {
        if (synchronous && errorMessage != nullptr) {
            *errorMessage = QStringLiteral("SQLite synchronous mode was not set to NORMAL");
        }
        return false;
    }
    return true;
}

bool rollback(const QSqlDatabase& database) {
    return executeSql(database, QStringLiteral("ROLLBACK"));
}

bool initializeSchema(const QSqlDatabase& database, QString* errorMessage) {
    if (!executeSql(database, QStringLiteral("BEGIN IMMEDIATE"), errorMessage)) {
        return false;
    }

    const QStringList statements{
        createNamespacesSql(),
        createPagesSql(),
        createPendingBatchesSql(),
        QStringLiteral("PRAGMA application_id = 0x53564348"),
        QStringLiteral("PRAGMA user_version = 1"),
    };

    for (const QString& statement : statements) {
        if (!executeSql(database, statement, errorMessage)) {
            rollback(database);
            return false;
        }
    }
    if (!executeSql(database, QStringLiteral("COMMIT"), errorMessage)) {
        rollback(database);
        return false;
    }
    return true;
}

bool validateSchema(const QSqlDatabase& database, QString* errorMessage) {
    const std::vector<std::pair<QString, QString>> definitions{
        {QStringLiteral("cache_namespaces"), createNamespacesSql()},
        {QStringLiteral("cache_pages"), createPagesSql()},
        {QStringLiteral("pending_batches"), createPendingBatchesSql()},
    };
    for (const auto& [tableName, expectedSql] : definitions) {
        QSqlQuery query(database);
        if (!query.prepare(QStringLiteral(
                "SELECT sql FROM sqlite_schema WHERE type = 'table' AND name = ?"))) {
            if (errorMessage != nullptr) {
                *errorMessage =
                    sqlErrorMessage(QStringLiteral("Prepare schema validation"), query.lastError());
            }
            return false;
        }
        query.addBindValue(tableName);
        if (!query.exec()) {
            if (errorMessage != nullptr) {
                *errorMessage =
                    sqlErrorMessage(QStringLiteral("Read cache schema"), query.lastError());
            }
            return false;
        }
        if (!query.next() || canonicalSql(query.value(0).toString()) != canonicalSql(expectedSql)) {
            if (errorMessage != nullptr) {
                *errorMessage =
                    QStringLiteral("Cache table %1 does not match schema version 1").arg(tableName);
            }
            return false;
        }
    }
    return true;
}

bool quickCheck(const QSqlDatabase& database, QString* errorMessage) {
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA quick_check"))) {
        if (errorMessage != nullptr) {
            *errorMessage =
                sqlErrorMessage(QStringLiteral("SQLite quick_check"), query.lastError());
        }
        return false;
    }

    bool sawResult = false;
    while (query.next()) {
        sawResult = true;
        const QString result = query.value(0).toString();
        if (result.compare(QStringLiteral("ok"), Qt::CaseInsensitive) != 0) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("SQLite quick_check failed: %1").arg(result);
            }
            return false;
        }
    }
    if (!sawResult && errorMessage != nullptr) {
        *errorMessage = QStringLiteral("SQLite quick_check returned no result");
    }
    return sawResult;
}

std::optional<quint64> recoverAndRegisterNamespace(const QSqlDatabase& database,
                                                   const QString& namespaceName,
                                                   QString* errorMessage) {
    if (!executeSql(database, QStringLiteral("BEGIN IMMEDIATE"), errorMessage)) {
        return std::nullopt;
    }

    const auto pendingCount = queryInteger(
        database, QStringLiteral("SELECT COUNT(*) FROM pending_batches"), errorMessage);
    if (!pendingCount || *pendingCount < 0 ||
        !executeSql(database, QStringLiteral("DELETE FROM pending_batches"), errorMessage)) {
        rollback(database);
        return std::nullopt;
    }

    QSqlQuery namespaceInsert(database);
    if (!namespaceInsert.prepare(
            QStringLiteral("INSERT OR IGNORE INTO cache_namespaces(namespace_name) VALUES (?)"))) {
        if (errorMessage != nullptr) {
            *errorMessage = sqlErrorMessage(QStringLiteral("Prepare cache namespace"),
                                            namespaceInsert.lastError());
        }
        rollback(database);
        return std::nullopt;
    }
    namespaceInsert.addBindValue(namespaceName);
    if (!namespaceInsert.exec()) {
        if (errorMessage != nullptr) {
            *errorMessage = sqlErrorMessage(QStringLiteral("Register cache namespace"),
                                            namespaceInsert.lastError());
        }
        rollback(database);
        return std::nullopt;
    }

    if (!executeSql(database, QStringLiteral("COMMIT"), errorMessage)) {
        rollback(database);
        return std::nullopt;
    }
    return static_cast<quint64>(*pendingCount);
}

std::optional<int> pageKindValue(PagedCachePageKind kind) {
    switch (kind) {
    case PagedCachePageKind::ProgressiveIndex:
        return 0;
    case PagedCachePageKind::MaterializedResult:
        return 1;
    }
    return std::nullopt;
}

bool validateKey(const PagedCachePageKey& key, QString* errorMessage) {
    if (!pageKindValue(key.kind)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cache page kind is invalid");
        }
        return false;
    }
    constexpr quint64 maximumSqliteInteger =
        static_cast<quint64>(std::numeric_limits<qlonglong>::max());
    if (key.streamId > maximumSqliteInteger || key.pageIndex > maximumSqliteInteger) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cache page coordinate exceeds SQLite's integer range");
        }
        return false;
    }
    return true;
}

PagedCacheOpenResult openFailure(PagedCacheOpenStatus status, QString errorMessage) {
    PagedCacheOpenResult result;
    result.status = status;
    result.errorMessage = std::move(errorMessage);
    return result;
}

void deletePendingMarker(const QSqlDatabase& database, const QString& token) {
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral("DELETE FROM pending_batches WHERE token = ?"))) {
        return;
    }
    query.addBindValue(token);
    query.exec();
}

} // namespace

class PagedCache::Impl final {
  public:
    QString connectionName;
    QString namespaceName;
    QThread* ownerThread = nullptr;
    std::unique_ptr<QLockFile> databaseLock;
};

PagedCache::PagedCache(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

PagedCache::~PagedCache() {
    if (!impl_) {
        return;
    }
    if (QThread::currentThread() != impl_->ownerThread) {
        qFatal("PagedCache must be destroyed on its owning thread");
    }
    closeAndRemoveConnection(impl_->connectionName);
}

PagedCacheOpenResult PagedCache::open(const QString& databasePath, const QString& namespaceName) {
    if (databasePath.isEmpty()) {
        return openFailure(PagedCacheOpenStatus::InvalidArgument,
                           QStringLiteral("Cache database path must not be empty"));
    }
    if (namespaceName.isEmpty() || namespaceName.size() > maximumNamespaceLength()) {
        return openFailure(PagedCacheOpenStatus::InvalidArgument,
                           QStringLiteral("Cache namespace length must be between 1 and 512"));
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return openFailure(PagedCacheOpenStatus::MissingSqliteDriver,
                           QStringLiteral("QSQLITE driver is unavailable; available drivers: %1")
                               .arg(QSqlDatabase::drivers().join(QStringLiteral(", "))));
    }

    const QFileInfo pathInfo(databasePath);
    if (pathInfo.exists() && pathInfo.isDir()) {
        return openFailure(PagedCacheOpenStatus::InvalidArgument,
                           QStringLiteral("Cache database path names a directory"));
    }
    const bool existingDatabaseWithData = pathInfo.exists() && pathInfo.size() > 0;
    if (!QDir().mkpath(pathInfo.absolutePath())) {
        return openFailure(PagedCacheOpenStatus::OpenFailed,
                           QStringLiteral("Unable to create the cache database directory"));
    }

    auto databaseLock =
        std::make_unique<QLockFile>(pathInfo.absoluteFilePath() + QStringLiteral(".lock"));
    databaseLock->setStaleLockTime(kLockStaleTime);
    if (!databaseLock->tryLock(kLockWaitTime)) {
        qint64 processId = 0;
        QString hostName;
        QString applicationName;
        const bool hasOwner = databaseLock->getLockInfo(&processId, &hostName, &applicationName);
        const QString owner = hasOwner ? QStringLiteral(" by %1 on %2 (pid %3)")
                                             .arg(applicationName, hostName)
                                             .arg(processId)
                                       : QString();
        return openFailure(PagedCacheOpenStatus::OpenFailed,
                           QStringLiteral("Cache database is already in use%1").arg(owner));
    }

    const QString connectionName = QStringLiteral("streamview-paged-cache-%1")
                                       .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    ConnectionRegistration registration(connectionName);
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    database.setDatabaseName(pathInfo.absoluteFilePath());
    if (!database.open()) {
        return openFailure(
            PagedCacheOpenStatus::OpenFailed,
            sqlErrorMessage(QStringLiteral("Open cache database"), database.lastError()));
    }

    QString errorMessage;
    const auto applicationId =
        queryInteger(database, QStringLiteral("PRAGMA application_id"), &errorMessage);
    const auto userVersion =
        queryInteger(database, QStringLiteral("PRAGMA user_version"), &errorMessage);
    const auto userTableCount =
        queryInteger(database,
                     QStringLiteral("SELECT COUNT(*) FROM sqlite_schema "
                                    "WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"),
                     &errorMessage);
    if (!applicationId || !userVersion || !userTableCount) {
        return openFailure(existingDatabaseWithData ? PagedCacheOpenStatus::CorruptStore
                                                    : PagedCacheOpenStatus::OpenFailed,
                           errorMessage);
    }

    const bool newDatabase = *applicationId == 0 && *userVersion == 0 && *userTableCount == 0;
    if (!newDatabase && (*applicationId != static_cast<qlonglong>(kApplicationId) ||
                         *userVersion != static_cast<qlonglong>(schemaVersion()))) {
        return openFailure(
            PagedCacheOpenStatus::IncompatibleSchema,
            QStringLiteral("Cache application ID or schema version is incompatible"));
    }

    if (!configureConnection(database, &errorMessage)) {
        return openFailure(PagedCacheOpenStatus::OpenFailed, errorMessage);
    }
    if (newDatabase && !initializeSchema(database, &errorMessage)) {
        return openFailure(PagedCacheOpenStatus::StorageError, errorMessage);
    }
    if (!validateSchema(database, &errorMessage)) {
        return openFailure(PagedCacheOpenStatus::CorruptStore, errorMessage);
    }
    if (!quickCheck(database, &errorMessage)) {
        return openFailure(PagedCacheOpenStatus::CorruptStore, errorMessage);
    }

    const auto recovered = recoverAndRegisterNamespace(database, namespaceName, &errorMessage);
    if (!recovered) {
        return openFailure(PagedCacheOpenStatus::StorageError, errorMessage);
    }

    auto impl = std::make_unique<Impl>();
    impl->connectionName = connectionName;
    impl->namespaceName = namespaceName;
    impl->ownerThread = QThread::currentThread();
    impl->databaseLock = std::move(databaseLock);

    PagedCacheOpenResult result;
    result.status = PagedCacheOpenStatus::Opened;
    result.cache = std::unique_ptr<PagedCache>(new PagedCache(std::move(impl)));
    result.recoveredIncompleteBatchCount = *recovered;
    registration.release();
    return result;
}

PagedCacheReadResult PagedCache::readPage(const PagedCachePageKey& key) const {
    PagedCacheReadResult result;
    if (QThread::currentThread() != impl_->ownerThread) {
        result.status = PagedCacheReadStatus::ThreadViolation;
        result.errorMessage = QStringLiteral("PagedCache read attempted from a non-owning thread");
        return result;
    }
    if (!validateKey(key, &result.errorMessage)) {
        result.status = PagedCacheReadStatus::InvalidRequest;
        return result;
    }

    const auto kind = pageKindValue(key.kind);
    QSqlDatabase database = QSqlDatabase::database(impl_->connectionName, false);
    if (!database.isValid() || !database.isOpen()) {
        result.status = PagedCacheReadStatus::StorageError;
        result.errorMessage = QStringLiteral("Cache database connection is unavailable");
        return result;
    }

    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "SELECT payload FROM cache_pages "
            "WHERE namespace_name = ? AND page_kind = ? AND stream_id = ? AND page_index = ?"))) {
        result.status = PagedCacheReadStatus::StorageError;
        result.errorMessage =
            sqlErrorMessage(QStringLiteral("Prepare cache page read"), query.lastError());
        return result;
    }
    query.addBindValue(impl_->namespaceName);
    query.addBindValue(*kind);
    query.addBindValue(static_cast<qlonglong>(key.streamId));
    query.addBindValue(static_cast<qlonglong>(key.pageIndex));
    if (!query.exec()) {
        result.status = PagedCacheReadStatus::StorageError;
        result.errorMessage = sqlErrorMessage(QStringLiteral("Read cache page"), query.lastError());
        return result;
    }
    if (!query.next()) {
        result.status = PagedCacheReadStatus::Missing;
        return result;
    }

    const QByteArray payload = query.value(0).toByteArray();
    if (payload.isEmpty() || static_cast<std::size_t>(payload.size()) > pageSizeBytes()) {
        result.status = PagedCacheReadStatus::StorageError;
        result.errorMessage = QStringLiteral("Cached page payload violates the schema limits");
        return result;
    }
    result.bytes.resize(static_cast<std::size_t>(payload.size()));
    std::memcpy(result.bytes.data(), payload.constData(), result.bytes.size());
    result.status = PagedCacheReadStatus::Found;
    return result;
}

PagedCacheCommitResult PagedCache::commitBatch(std::span<const PagedCachePageWrite> pages) {
    PagedCacheCommitResult result;
    if (QThread::currentThread() != impl_->ownerThread) {
        result.status = PagedCacheCommitStatus::ThreadViolation;
        result.errorMessage =
            QStringLiteral("PagedCache commit attempted from a non-owning thread");
        return result;
    }
    if (pages.empty() || pages.size() > maximumBatchPages()) {
        result.errorMessage = QStringLiteral("Cache batch must contain between 1 and 256 pages");
        return result;
    }

    std::set<PagedCachePageKey> keys;
    for (const PagedCachePageWrite& page : pages) {
        if (!validateKey(page.key, &result.errorMessage)) {
            return result;
        }
        if (page.bytes.empty() || page.bytes.size() > pageSizeBytes()) {
            result.errorMessage =
                QStringLiteral("Cache page payload must contain 1 to 65536 bytes");
            return result;
        }
        if (!keys.insert(page.key).second) {
            result.errorMessage = QStringLiteral("Cache batch contains a duplicate page key");
            return result;
        }
    }

    QSqlDatabase database = QSqlDatabase::database(impl_->connectionName, false);
    if (!database.isValid() || !database.isOpen()) {
        result.status = PagedCacheCommitStatus::StorageError;
        result.errorMessage = QStringLiteral("Cache database connection is unavailable");
        return result;
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery marker(database);
    if (!marker.prepare(QStringLiteral(
            "INSERT INTO pending_batches(token, namespace_name, started_at) VALUES (?, ?, ?)"))) {
        result.status = PagedCacheCommitStatus::StorageError;
        result.errorMessage =
            sqlErrorMessage(QStringLiteral("Prepare pending cache batch"), marker.lastError());
        return result;
    }
    marker.addBindValue(token);
    marker.addBindValue(impl_->namespaceName);
    marker.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!marker.exec()) {
        result.status = PagedCacheCommitStatus::StorageError;
        result.errorMessage =
            sqlErrorMessage(QStringLiteral("Record pending cache batch"), marker.lastError());
        return result;
    }

    QString transactionError;
    if (!executeSql(database, QStringLiteral("BEGIN IMMEDIATE"), &transactionError)) {
        deletePendingMarker(database, token);
        result.status = PagedCacheCommitStatus::StorageError;
        result.errorMessage = transactionError;
        return result;
    }

    QSqlQuery write(database);
    if (!write.prepare(
            QStringLiteral("INSERT INTO cache_pages("
                           "namespace_name, page_kind, stream_id, page_index, payload"
                           ") VALUES (?, ?, ?, ?, ?) "
                           "ON CONFLICT(namespace_name, page_kind, stream_id, page_index) "
                           "DO UPDATE SET payload = excluded.payload"))) {
        rollback(database);
        deletePendingMarker(database, token);
        result.status = PagedCacheCommitStatus::StorageError;
        result.errorMessage =
            sqlErrorMessage(QStringLiteral("Prepare cache page write"), write.lastError());
        return result;
    }

    for (const PagedCachePageWrite& page : pages) {
        const auto kind = pageKindValue(page.key.kind);
        const QByteArray payload(reinterpret_cast<const char*>(page.bytes.data()),
                                 static_cast<qsizetype>(page.bytes.size()));
        write.bindValue(0, impl_->namespaceName);
        write.bindValue(1, *kind);
        write.bindValue(2, static_cast<qlonglong>(page.key.streamId));
        write.bindValue(3, static_cast<qlonglong>(page.key.pageIndex));
        write.bindValue(4, payload);
        if (!write.exec()) {
            result.errorMessage =
                sqlErrorMessage(QStringLiteral("Write cache page"), write.lastError());
            rollback(database);
            deletePendingMarker(database, token);
            result.status = PagedCacheCommitStatus::StorageError;
            return result;
        }
        write.finish();
    }

    QSqlQuery deleteMarker(database);
    if (!deleteMarker.prepare(QStringLiteral("DELETE FROM pending_batches WHERE token = ?"))) {
        result.errorMessage = sqlErrorMessage(QStringLiteral("Prepare pending-batch cleanup"),
                                              deleteMarker.lastError());
        rollback(database);
        deletePendingMarker(database, token);
        result.status = PagedCacheCommitStatus::StorageError;
        return result;
    }
    deleteMarker.addBindValue(token);
    if (!deleteMarker.exec()) {
        result.errorMessage =
            sqlErrorMessage(QStringLiteral("Clean pending cache batch"), deleteMarker.lastError());
        rollback(database);
        deletePendingMarker(database, token);
        result.status = PagedCacheCommitStatus::StorageError;
        return result;
    }

    if (!executeSql(database, QStringLiteral("COMMIT"), &transactionError)) {
        rollback(database);
        deletePendingMarker(database, token);
        result.status = PagedCacheCommitStatus::StorageError;
        result.errorMessage = transactionError;
        return result;
    }

    result.status = PagedCacheCommitStatus::Committed;
    return result;
}

} // namespace streamview::core
