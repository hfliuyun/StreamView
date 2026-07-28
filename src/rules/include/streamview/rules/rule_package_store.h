#pragma once

#include <streamview/rules/rule_package.h>

#include <QString>

#include <optional>

namespace streamview::rules {

enum class RulePackageImportStatus {
    Imported,
    InvalidInput,
    IoError,
    InvalidPackage,
    InvalidArchive,
};

struct RulePackageImportResult final {
    RulePackageImportStatus status = RulePackageImportStatus::InvalidInput;
    std::optional<RulePackage> package;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == RulePackageImportStatus::Imported && package.has_value();
    }
};

enum class RulePackageWriteStatus {
    Written,
    InvalidDestination,
    ArchiveTooLarge,
    IoError,
};

struct RulePackageWriteResult final {
    RulePackageWriteStatus status = RulePackageWriteStatus::InvalidDestination;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == RulePackageWriteStatus::Written;
    }
};

enum class RulePackageInstallStatus {
    Installed,
    AlreadyInstalled,
    InvalidStore,
    IoError,
    CorruptExistingContent,
};

struct RulePackageInstallResult final {
    RulePackageInstallStatus status = RulePackageInstallStatus::InvalidStore;
    QString installedPath;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == RulePackageInstallStatus::Installed ||
               status == RulePackageInstallStatus::AlreadyInstalled;
    }
};

class RulePackageStore final {
  public:
    [[nodiscard]] static RulePackageImportResult importDirectory(const QString& rootPath);
    [[nodiscard]] static RulePackageImportResult importArchive(const QString& archivePath);
    [[nodiscard]] static RulePackageWriteResult writeArchive(const RulePackage& package,
                                                             const QString& archivePath);
    [[nodiscard]] static RulePackageInstallResult install(const RulePackage& package,
                                                          const QString& storeRoot);
};

} // namespace streamview::rules
