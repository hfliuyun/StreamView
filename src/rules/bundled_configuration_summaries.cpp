#include <streamview/rules/configuration_summary.h>

#include <streamview/rules/aac_asc_configuration_summary_provider.h>

#include <QtGlobal>

#include <memory>

namespace streamview::rules {
namespace {

struct BundledConfigurationSummaries final {
    BundledConfigurationSummaries() {
        const bool registered =
            registry.registerProvider(std::make_shared<AacAscConfigurationSummaryProvider>());
        Q_ASSERT(registered);
        (void)registered;
    }

    ConfigurationSummaryRegistry registry;
};

} // namespace

const ConfigurationSummaryRegistry& bundledConfigurationSummaryRegistry() {
    static const BundledConfigurationSummaries summaries;
    return summaries.registry;
}

} // namespace streamview::rules
