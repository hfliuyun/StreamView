#include <streamview/rules/rule_package_store.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>

#include <utility>

namespace streamview::rules {

namespace {

constexpr int installLockWaitMilliseconds = 5'000;
constexpr int installLockStaleMilliseconds = 30'000;

[[nodiscard]] bool ensureRealDirectory(const QString& path, QString* errorMessage) {
    if (!QDir().mkpath(path)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to create rule store directory: %1").arg(path);
        }
        return false;
    }
    const QFileInfo information(path);
    if (!information.isDir() || information.isSymLink()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Rule store path is not a real directory: %1").arg(path);
        }
        return false;
    }
    return true;
}

[[nodiscard]] RulePackageInstallResult validateExisting(const RulePackage& package,
                                                        const QString& path) {
    RulePackageImportResult imported = RulePackageStore::importDirectory(path);
    if (!imported.succeeded() || imported.package->identity() != package.identity()) {
        const QString detail =
            imported.errorMessage.isEmpty()
                ? QStringLiteral("content identity does not match its digest path")
                : imported.errorMessage;
        return {RulePackageInstallStatus::CorruptExistingContent,
                {},
                QStringLiteral("Existing content-addressed package is corrupt: %1").arg(detail)};
    }
    constexpr QFileDevice::Permissions writePermissions =
        QFileDevice::WriteOwner | QFileDevice::WriteGroup | QFileDevice::WriteOther;
    const QDir root(path);
    for (const RulePackageFile& packageFile : package.files()) {
        const QFileInfo information(root.filePath(packageFile.path));
        if ((information.permissions() & writePermissions) != 0) {
            return {RulePackageInstallStatus::CorruptExistingContent,
                    {},
                    QStringLiteral(
                        "Existing content-addressed package is corrupt: file is writable: %1")
                        .arg(packageFile.path)};
        }
    }
    return {RulePackageInstallStatus::AlreadyInstalled, path, {}};
}

[[nodiscard]] bool writeStagingTree(const RulePackage& package, const QString& stagingRoot,
                                    QString* errorMessage) {
    QDir root(stagingRoot);
    for (const RulePackageFile& packageFile : package.files()) {
        const QFileInfo relativeInformation(packageFile.path);
        const QString relativeParent = relativeInformation.path();
        if (relativeParent != QStringLiteral(".") && !root.mkpath(relativeParent)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Unable to create staged package directory: %1")
                                    .arg(relativeParent);
            }
            return false;
        }
        const QString outputPath = root.filePath(packageFile.path);
        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (errorMessage != nullptr) {
                *errorMessage = file.errorString();
            }
            return false;
        }
        if (file.write(packageFile.contents) != packageFile.contents.size()) {
            if (errorMessage != nullptr) {
                *errorMessage = file.errorString();
            }
            return false;
        }
        file.close();
        if (file.error() != QFileDevice::NoError ||
            !QFile::setPermissions(outputPath, QFileDevice::ReadOwner | QFileDevice::ReadGroup |
                                                   QFileDevice::ReadOther)) {
            if (errorMessage != nullptr) {
                *errorMessage = file.error() == QFileDevice::NoError
                                    ? QStringLiteral("Unable to make staged package file read-only")
                                    : file.errorString();
            }
            return false;
        }
    }
    return true;
}

} // namespace

RulePackageInstallResult RulePackageStore::install(const RulePackage& package,
                                                   const QString& storeRoot) {
    if (storeRoot.isEmpty()) {
        return {
            RulePackageInstallStatus::InvalidStore, {}, QStringLiteral("Rule store path is empty")};
    }
    const QString digest = QString::fromLatin1(package.identity().contentHash().toHex());
    const QString algorithmRoot = QDir(storeRoot).filePath(QStringLiteral("sha256"));
    const QString shardRoot = QDir(algorithmRoot).filePath(digest.first(2));
    QString directoryError;
    if (!ensureRealDirectory(storeRoot, &directoryError) ||
        !ensureRealDirectory(algorithmRoot, &directoryError) ||
        !ensureRealDirectory(shardRoot, &directoryError)) {
        return {RulePackageInstallStatus::InvalidStore, {}, std::move(directoryError)};
    }

    const QString destination = QDir(shardRoot).filePath(digest);
    QLockFile installLock(QDir(shardRoot).filePath(QStringLiteral(".install-%1.lock").arg(digest)));
    installLock.setStaleLockTime(installLockStaleMilliseconds);
    if (!installLock.tryLock(installLockWaitMilliseconds)) {
        return {RulePackageInstallStatus::IoError,
                {},
                QStringLiteral("Unable to lock the content-addressed package destination")};
    }
    if (QFileInfo::exists(destination)) {
        return validateExisting(package, destination);
    }

    QTemporaryDir staging(QDir(shardRoot).filePath(QStringLiteral(".install-XXXXXX")));
    if (!staging.isValid()) {
        return {RulePackageInstallStatus::IoError,
                {},
                QStringLiteral("Unable to create private package staging directory")};
    }
    QString writeError;
    if (!writeStagingTree(package, staging.path(), &writeError)) {
        return {RulePackageInstallStatus::IoError, {}, std::move(writeError)};
    }
    RulePackageImportResult staged = importDirectory(staging.path());
    if (!staged.succeeded() || staged.package->identity() != package.identity()) {
        const QString detail =
            staged.errorMessage.isEmpty()
                ? QStringLiteral("content identity does not match expected package")
                : staged.errorMessage;
        return {RulePackageInstallStatus::IoError,
                {},
                QStringLiteral("Staged package failed identity verification: %1").arg(detail)};
    }

    if (QFileInfo::exists(destination)) {
        return validateExisting(package, destination);
    }
    if (QDir().rename(staging.path(), destination)) {
        staging.setAutoRemove(false);
        return {RulePackageInstallStatus::Installed, destination, {}};
    }
    if (QFileInfo::exists(destination)) {
        return validateExisting(package, destination);
    }
    return {RulePackageInstallStatus::IoError,
            {},
            QStringLiteral("Unable to atomically install content-addressed package")};
}

} // namespace streamview::rules
