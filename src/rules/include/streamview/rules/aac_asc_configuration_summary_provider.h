#pragma once

#include <streamview/rules/configuration_summary.h>

namespace streamview::rules {

/// Summarizes a decoded `AudioSpecificConfig` for ADR-0105 section 5's
/// access-unit envelope: audio object type, sampling frequency, channels.
///
/// The provider reads an already-decoded tree and never touches raw bytes. ASC
/// decoding belongs to the official `org.streamview.aac` rule package, so this
/// provider adds naming, not a second decoder.
///
/// Reporting rule: the summary never states a fact the wire does not support.
/// A field that is absent, or whose value is reserved and therefore names
/// nothing, yields `DependencyUnavailable`. A value that is real but has no
/// short human name is reported numerically, because the number is itself the
/// wire fact rather than a guess.
class AacAscConfigurationSummaryProvider final : public ConfigurationSummaryProvider {
public:
    [[nodiscard]] QString targetFormat() const override {
        return QStringLiteral("audio.aac.asc");
    }

    [[nodiscard]] ConfigurationSummaryResult format(
        const ConfigurationSummaryRequest& request) const override;
};

} // namespace streamview::rules
