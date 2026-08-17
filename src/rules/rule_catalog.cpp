#include <streamview/rules/rule_catalog.h>

#include <algorithm>
#include <utility>

namespace streamview::rules {

RuleCatalogRegistrationResult RulePackageCatalog::registerPackage(RulePackage&& package) {
    const RulePackageIdentity identity = package.identity();
    const auto packageIt = packages_.find(identity.packageId());
    if (packageIt != packages_.end()) {
        const auto existing = packageIt->constFind(identity.packageVersion());
        if (existing == packageIt->cend()) {
            packageIt->insert(identity.packageVersion(),
                              std::make_shared<const RulePackage>(std::move(package)));
            ++packageCount_;
            return {RuleCatalogRegistrationStatus::Registered, {}};
        }
        if ((*existing)->identity().contentHash() == identity.contentHash()) {
            return {RuleCatalogRegistrationStatus::AlreadyRegistered, {}};
        }
        return {RuleCatalogRegistrationStatus::VersionConflict,
                QStringLiteral("Package %1@%2 is already registered with different content")
                    .arg(identity.packageId(), identity.packageVersion())};
    }

    auto stored = std::make_shared<const RulePackage>(std::move(package));
    QHash<QString, std::shared_ptr<const RulePackage>> versions;
    versions.insert(identity.packageVersion(), std::move(stored));
    packages_.insert(identity.packageId(), std::move(versions));
    ++packageCount_;
    return {RuleCatalogRegistrationStatus::Registered, {}};
}

RuleCatalogLookupResult RulePackageCatalog::resolve(const RulePackageIdentity& identity,
                                                    QStringView entryPointId,
                                                    QStringView runningLanguage,
                                                    QStringView runningEngine) const {
    const auto packageIt = packages_.constFind(identity.packageId());
    if (packageIt == packages_.cend()) {
        return {RuleCatalogLookupStatus::MissingContent,
                {},
                std::nullopt,
                QStringLiteral("Package is not installed: %1").arg(identity.toString())};
    }
    const auto versionIt = packageIt->constFind(identity.packageVersion());
    if (versionIt == packageIt->cend()) {
        return {RuleCatalogLookupStatus::MissingContent,
                {},
                std::nullopt,
                QStringLiteral("Package version is not installed: %1")
                    .arg(identity.toString())};
    }
    const std::shared_ptr<const RulePackage>& package = *versionIt;
    if (package->identity().contentHash() != identity.contentHash()) {
        return {RuleCatalogLookupStatus::VersionConflict,
                package,
                std::nullopt,
                QStringLiteral("Installed package content differs from the requested identity: %1")
                    .arg(identity.toString())};
    }

    const auto entry = std::find_if(package->manifest().entryPoints.begin(),
                                    package->manifest().entryPoints.end(),
                                    [entryPointId](const RulePackageEntryPoint& candidate) {
                                        return candidate.id == entryPointId;
                                    });
    if (entry == package->manifest().entryPoints.end()) {
        return {RuleCatalogLookupStatus::UnknownEntryPoint,
                package,
                std::nullopt,
                QStringLiteral("Package entry point is not installed: %1")
                    .arg(entryPointId)};
    }
    if (!package->manifest().languageContract.accepts(runningLanguage)) {
        return {RuleCatalogLookupStatus::IncompatibleLanguage,
                package,
                *entry,
                QStringLiteral("Package requires DSL %1, running DSL is %2")
                    .arg(package->manifest().languageContract.text(), runningLanguage)};
    }
    if (!package->manifest().engineRange.contains(runningEngine)) {
        return {RuleCatalogLookupStatus::IncompatibleEngine,
                package,
                *entry,
                QStringLiteral("Package requires engine %1, running engine is %2")
                    .arg(package->manifest().engineRange.text(), runningEngine)};
    }
    return {RuleCatalogLookupStatus::Found, package, *entry, {}};
}

RuleCatalogLookupResult RulePackageCatalog::resolveByFormat(QStringView format,
                                                            QStringView runningLanguage,
                                                            QStringView runningEngine) const {
    if (format.isEmpty()) {
        return {RuleCatalogLookupStatus::MissingContent,
                {},
                std::nullopt,
                QStringLiteral("Format descriptor is empty")};
    }

    std::shared_ptr<const RulePackage> matchedPackage;
    std::optional<RulePackageEntryPoint> matchedEntryPoint;

    for (const auto& versions : packages_) {
        for (const auto& package : versions) {
            for (const RulePackageEntryPoint& entry : package->manifest().entryPoints) {
                if (entry.format == format) {
                    if (matchedPackage) {
                        return {
                            RuleCatalogLookupStatus::VersionConflict,
                            {},
                            std::nullopt,
                            QStringLiteral("Multiple installed package entry points match format: %1")
                                .arg(format)};
                    }
                    matchedPackage = package;
                    matchedEntryPoint = entry;
                }
            }
        }
    }

    if (!matchedPackage || !matchedEntryPoint) {
        return {RuleCatalogLookupStatus::MissingContent,
                {},
                std::nullopt,
                QStringLiteral("No installed package matches format: %1").arg(format)};
    }

    if (!matchedPackage->manifest().languageContract.accepts(runningLanguage)) {
        return {RuleCatalogLookupStatus::IncompatibleLanguage,
                matchedPackage,
                *matchedEntryPoint,
                QStringLiteral("Package requires DSL %1, running DSL is %2")
                    .arg(matchedPackage->manifest().languageContract.text(), runningLanguage)};
    }
    if (!matchedPackage->manifest().engineRange.contains(runningEngine)) {
        return {RuleCatalogLookupStatus::IncompatibleEngine,
                matchedPackage,
                *matchedEntryPoint,
                QStringLiteral("Package requires engine %1, running engine is %2")
                    .arg(matchedPackage->manifest().engineRange.text(), runningEngine)};
    }

    return {RuleCatalogLookupStatus::Found, matchedPackage, *matchedEntryPoint, {}};
}

} // namespace streamview::rules
