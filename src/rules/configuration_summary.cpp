#include <streamview/rules/configuration_summary.h>

#include <utility>

namespace streamview::rules {

ConfigurationSummaryRegistry& ConfigurationSummaryRegistry::instance() {
    static ConfigurationSummaryRegistry registry;
    return registry;
}

bool ConfigurationSummaryRegistry::registerProvider(
    std::shared_ptr<const ConfigurationSummaryProvider> provider) {
    if (!provider) {
        return false;
    }
    const QString format = provider->targetFormat().trimmed();
    if (format.isEmpty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (providers_.find(format) != providers_.end()) {
        return false;
    }
    providers_.emplace(format, std::move(provider));
    return true;
}

bool ConfigurationSummaryRegistry::unregisterProvider(const QString& targetFormat) {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.erase(targetFormat.trimmed()) > 0;
}

std::shared_ptr<const ConfigurationSummaryProvider>
ConfigurationSummaryRegistry::findProvider(const QString& targetFormat) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = providers_.find(targetFormat.trimmed());
    if (it != providers_.end()) {
        return it->second;
    }
    return nullptr;
}

ConfigurationSummaryResult ConfigurationSummaryRegistry::format(
    const ConfigurationSummaryRequest& request) const {
    ConfigurationSummaryResult result;
    if (request.configurationTree == nullptr) {
        result.status = ConfigurationSummaryStatus::InvalidRequest;
        result.errorMessage = QStringLiteral("No configuration tree supplied");
        return result;
    }
    if (!request.configurationTree->node(request.configurationNode).has_value()) {
        result.status = ConfigurationSummaryStatus::InvalidRequest;
        result.errorMessage = QStringLiteral("Configuration node %1 is not in the supplied tree")
                                  .arg(request.configurationNode.value());
        return result;
    }
    const QString format = request.targetFormat.trimmed();
    if (format.isEmpty()) {
        result.status = ConfigurationSummaryStatus::InvalidRequest;
        result.errorMessage = QStringLiteral("No target format supplied");
        return result;
    }

    const auto provider = findProvider(format);
    if (!provider) {
        result.status = ConfigurationSummaryStatus::UnsupportedFormat;
        result.errorMessage =
            QStringLiteral("No configuration summary provider for target format '%1'").arg(format);
        return result;
    }
    return provider->format(request);
}

void ConfigurationSummaryRegistry::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_.clear();
}

} // namespace streamview::rules
