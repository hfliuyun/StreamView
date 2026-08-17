#pragma once

#include <streamview/rules/rule_package.h>

#include <QHash>
#include <QString>
#include <QStringView>

#include <memory>

namespace streamview::rules {

enum class RuleCatalogRegistrationStatus {
    Registered,
    AlreadyRegistered,
    VersionConflict,
};

struct RuleCatalogRegistrationResult final {
    RuleCatalogRegistrationStatus status = RuleCatalogRegistrationStatus::VersionConflict;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == RuleCatalogRegistrationStatus::Registered ||
               status == RuleCatalogRegistrationStatus::AlreadyRegistered;
    }
};

enum class RuleCatalogLookupStatus {
    Found,
    MissingContent,
    VersionConflict,
    UnknownEntryPoint,
    IncompatibleLanguage,
    IncompatibleEngine,
};

struct RuleCatalogLookupResult final {
    RuleCatalogLookupStatus status = RuleCatalogLookupStatus::MissingContent;
    std::shared_ptr<const RulePackage> package;
    std::optional<RulePackageEntryPoint> entryPoint;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == RuleCatalogLookupStatus::Found && package && entryPoint.has_value();
    }
};

class RulePackageCatalog final {
public:
    [[nodiscard]] RuleCatalogRegistrationResult registerPackage(RulePackage&& package);
    [[nodiscard]] RuleCatalogLookupResult resolve(const RulePackageIdentity& identity,
                                                  QStringView entryPointId,
                                                  QStringView runningLanguage,
                                                  QStringView runningEngine) const;
    [[nodiscard]] RuleCatalogLookupResult resolveByFormat(QStringView format,
                                                          QStringView runningLanguage,
                                                          QStringView runningEngine) const;
    [[nodiscard]] qsizetype packageCount() const noexcept { return packageCount_; }

private:
    QHash<QString, QHash<QString, std::shared_ptr<const RulePackage>>> packages_;
    qsizetype packageCount_ = 0;
};

} // namespace streamview::rules
