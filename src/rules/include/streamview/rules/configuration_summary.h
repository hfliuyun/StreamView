#pragma once

#include <streamview/core/analysis_model.h>

#include <QString>
#include <QtGlobal>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace streamview::rules {

enum class ConfigurationSummaryStatus : quint8 {
    /// A summary was produced from the decoded configuration.
    Formatted,
    /// The request itself is unusable: no tree, unknown node, or empty target format.
    InvalidRequest,
    /// No provider claims this target format, so StreamView cannot summarize it yet.
    UnsupportedFormat,
    /// A provider claimed the format but the decoded configuration is missing a
    /// field it needs, or carries a value it cannot name. ADR-0105 section 5
    /// requires reporting this rather than guessing at a configuration.
    DependencyUnavailable,
};

/// A decoded configuration sub-tree awaiting a human-readable summary.
///
/// The caller supplies the tree produced by executing the configuration's
/// sub-format plus the node that owns the decoded fields, and names the target
/// format that produced it. Callers never name the configuration's structure or
/// its fields: which node holds the fields and what they mean is the provider's
/// knowledge, so no codec string is needed outside the rules layer.
struct ConfigurationSummaryRequest final {
    const core::AnalysisTree* configurationTree = nullptr;
    /// Either the structure holding the decoded fields or an ancestor of it,
    /// such as the sub-format tree's root. Providers accept both so the caller
    /// can forward a runner result without knowing the structure's name.
    core::AnalysisNodeId configurationNode{};
    QString targetFormat;
};

struct ConfigurationSummaryResult final {
    ConfigurationSummaryStatus status = ConfigurationSummaryStatus::InvalidRequest;
    QString summary;
    QString errorMessage;

    [[nodiscard]] bool formatted() const noexcept {
        return status == ConfigurationSummaryStatus::Formatted && !summary.isEmpty();
    }
};

class ConfigurationSummaryProvider {
public:
    virtual ~ConfigurationSummaryProvider() = default;

    /// The target format this provider summarizes, matching the
    /// `@target_format` annotation value whose execution produced the tree.
    [[nodiscard]] virtual QString targetFormat() const = 0;
    [[nodiscard]] virtual ConfigurationSummaryResult format(
        const ConfigurationSummaryRequest& request) const = 0;
};

/// Maps a target format to the provider that can summarize its configuration.
///
/// Unlike `PayloadTransformRegistry` this registry has no built-in fallback
/// provider. A transform can sensibly default to identity; a summary cannot
/// default to anything, because inventing one is the guess ADR-0105 section 5
/// prohibits. An unclaimed format is therefore reported as `UnsupportedFormat`,
/// and `reset()` empties the registry instead of restoring built-ins.
class ConfigurationSummaryRegistry final {
public:
    [[nodiscard]] static ConfigurationSummaryRegistry& instance();

    /// Register a provider. Returns false if the provider is null, its target
    /// format is empty, or that format is already claimed.
    bool registerProvider(std::shared_ptr<const ConfigurationSummaryProvider> provider);

    /// Unregister the provider claiming `targetFormat`. Returns false if none did.
    bool unregisterProvider(const QString& targetFormat);

    /// Look up the provider claiming `targetFormat`. Returns nullptr if none does.
    [[nodiscard]] std::shared_ptr<const ConfigurationSummaryProvider>
    findProvider(const QString& targetFormat) const;

    /// Format `request` with whichever provider claims its target format.
    ///
    /// Resolution and its failure statuses live here so callers never turn an
    /// unclaimed or unreadable configuration into a summary of their own.
    [[nodiscard]] ConfigurationSummaryResult format(
        const ConfigurationSummaryRequest& request) const;

    /// Drop every registered provider.
    void reset();

private:
    mutable std::mutex mutex_;
    std::unordered_map<QString, std::shared_ptr<const ConfigurationSummaryProvider>> providers_;
};

} // namespace streamview::rules
