#include <streamview/rules/aac_asc_configuration_summary_provider.h>

#include <QStringList>
#include <QVariant>

#include <optional>

namespace streamview::rules {
namespace {

/// ISO/IEC 14496-3:2019 Table 1.18 sampling frequency index.
///
/// Indices 13 and 14 are reserved and 15 means the frequency is carried
/// explicitly, so neither is in this table: both are answered elsewhere rather
/// than resolved to a frequency here.
[[nodiscard]] std::optional<quint32> standardSamplingFrequency(quint64 index) {
    switch (index) {
    case 0:
        return 96000U;
    case 1:
        return 88200U;
    case 2:
        return 64000U;
    case 3:
        return 48000U;
    case 4:
        return 44100U;
    case 5:
        return 32000U;
    case 6:
        return 24000U;
    case 7:
        return 22050U;
    case 8:
        return 16000U;
    case 9:
        return 12000U;
    case 10:
        return 11025U;
    case 11:
        return 8000U;
    case 12:
        return 7350U;
    default:
        return std::nullopt;
    }
}

/// ISO/IEC 14496-3:2019 Table 1.17 audio object type.
///
/// Covers the types the official ASC rule decodes plus the two it explicitly
/// refuses, since their object type is still a decoded fact even when their
/// specific configuration is not. Anything else is named numerically.
[[nodiscard]] QString audioObjectTypeName(quint64 objectType) {
    switch (objectType) {
    case 1:
        return QStringLiteral("AAC Main");
    case 2:
        return QStringLiteral("AAC LC");
    case 3:
        return QStringLiteral("AAC SSR");
    case 4:
        return QStringLiteral("AAC LTP");
    case 5:
        return QStringLiteral("SBR");
    case 6:
        return QStringLiteral("AAC Scalable");
    case 7:
        return QStringLiteral("TwinVQ");
    case 29:
        return QStringLiteral("PS");
    default:
        return QStringLiteral("Audio object type %1").arg(objectType);
    }
}

/// ISO/IEC 14496-3:2019 Table 1.19 channel configuration.
///
/// Index 0 means the channels come from a Program Config Element and 8..15 are
/// reserved, so neither is named here.
[[nodiscard]] std::optional<QString> channelConfigurationName(quint64 configuration) {
    switch (configuration) {
    case 1:
        return QStringLiteral("mono");
    case 2:
        return QStringLiteral("stereo");
    case 3:
        return QStringLiteral("3 channels");
    case 4:
        return QStringLiteral("4 channels");
    case 5:
        return QStringLiteral("5 channels");
    case 6:
        return QStringLiteral("5.1");
    case 7:
        return QStringLiteral("7.1");
    default:
        return std::nullopt;
    }
}

/// The first field of every AudioSpecificConfig, and therefore the marker that
/// tells a structure carrying decoded fields apart from a node merely named
/// after one.
[[nodiscard]] QString audioObjectTypeField() {
    return QStringLiteral("audio_object_type");
}

[[nodiscard]] bool hasChildNamed(const core::AnalysisTree& tree, const core::AnalysisNode& node,
                                 const QString& childName) {
    for (const auto childId : node.children()) {
        const auto child = tree.node(childId);
        if (child && child->name() == childName) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<core::AnalysisNode>
findConfigurationStructure(const core::AnalysisTree& tree, core::AnalysisNodeId nodeId) {
    const auto node = tree.node(nodeId);
    if (!node) {
        return std::nullopt;
    }
    // Match on the presence of the fields, not on the structure's name: a
    // sub-format tree's root is named after its entry structure, so the root and
    // the structure below it share the name `AudioSpecificConfig`. Matching by
    // name would resolve to whichever came first and then find no fields under
    // it. Presence of the mandatory first field distinguishes them reliably.
    if (hasChildNamed(tree, *node, audioObjectTypeField())) {
        return node;
    }
    // Callers forward whatever node they hold, which for a sub-format execution
    // is that root. One level down covers it without turning this into an
    // unbounded search of a tree the provider does not own.
    for (const auto childId : node->children()) {
        const auto child = tree.node(childId);
        if (child && hasChildNamed(tree, *child, audioObjectTypeField())) {
            return child;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<quint64>
fieldValue(const core::AnalysisTree& tree, const core::AnalysisNode& structure,
           const QString& fieldName) {
    for (const auto childId : structure.children()) {
        const auto child = tree.node(childId);
        if (!child || child->name() != fieldName) {
            continue;
        }
        bool ok = false;
        const auto value = child->value().toULongLong(&ok);
        if (!ok) {
            return std::nullopt;
        }
        return static_cast<quint64>(value);
    }
    return std::nullopt;
}

[[nodiscard]] ConfigurationSummaryResult unavailable(const QString& message) {
    ConfigurationSummaryResult result;
    result.status = ConfigurationSummaryStatus::DependencyUnavailable;
    result.errorMessage = message;
    return result;
}

} // namespace

ConfigurationSummaryResult AacAscConfigurationSummaryProvider::format(
    const ConfigurationSummaryRequest& request) const {
    if (request.configurationTree == nullptr) {
        ConfigurationSummaryResult result;
        result.status = ConfigurationSummaryStatus::InvalidRequest;
        result.errorMessage = QStringLiteral("No configuration tree supplied");
        return result;
    }

    const auto structure =
        findConfigurationStructure(*request.configurationTree, request.configurationNode);
    if (!structure) {
        return unavailable(QStringLiteral("No decoded AudioSpecificConfig at the configuration node"));
    }

    const auto objectType =
        fieldValue(*request.configurationTree, *structure, audioObjectTypeField());
    if (!objectType) {
        return unavailable(QStringLiteral("AudioSpecificConfig declares no audio_object_type"));
    }
    QString objectTypeName;
    if (*objectType == 31) {
        // The escape value defers the real object type to a 6-bit extension,
        // so the summary needs that extension rather than the escape itself.
        const auto extension = fieldValue(
            *request.configurationTree, *structure, QStringLiteral("audio_object_type_ext"));
        if (!extension) {
            return unavailable(
                QStringLiteral("Escaped audio object type declares no audio_object_type_ext"));
        }
        objectTypeName = audioObjectTypeName(32 + *extension);
    } else if (*objectType == 0) {
        // Table 1.17 defines 0 as NULL, which names no configuration at all.
        return unavailable(QStringLiteral("AudioSpecificConfig declares the NULL audio object type"));
    } else {
        objectTypeName = audioObjectTypeName(*objectType);
    }

    const auto frequencyIndex = fieldValue(
        *request.configurationTree, *structure, QStringLiteral("sampling_frequency_index"));
    if (!frequencyIndex) {
        return unavailable(
            QStringLiteral("AudioSpecificConfig declares no sampling_frequency_index"));
    }
    quint64 samplingFrequency = 0;
    if (*frequencyIndex == 15) {
        const auto explicitFrequency = fieldValue(
            *request.configurationTree, *structure, QStringLiteral("sampling_frequency"));
        if (!explicitFrequency) {
            return unavailable(QStringLiteral(
                "AudioSpecificConfig selects an explicit sampling frequency but declares none"));
        }
        if (*explicitFrequency == 0) {
            return unavailable(
                QStringLiteral("AudioSpecificConfig declares a zero explicit sampling frequency"));
        }
        samplingFrequency = *explicitFrequency;
    } else {
        const auto standard = standardSamplingFrequency(*frequencyIndex);
        if (!standard) {
            return unavailable(QStringLiteral("Sampling frequency index %1 is reserved")
                                   .arg(*frequencyIndex));
        }
        samplingFrequency = *standard;
    }

    const auto channelConfiguration = fieldValue(
        *request.configurationTree, *structure, QStringLiteral("channel_configuration"));
    if (!channelConfiguration) {
        return unavailable(
            QStringLiteral("AudioSpecificConfig declares no channel_configuration"));
    }
    QString channelName;
    if (*channelConfiguration == 0) {
        // A zero configuration is a decoded fact, not a missing one: the channel
        // map lives in the Program Config Element. Naming its element counts is
        // a wider read than this summary needs, so it reports the custom map
        // rather than a channel count it has not derived.
        channelName = QStringLiteral("custom channel configuration");
    } else {
        const auto named = channelConfigurationName(*channelConfiguration);
        if (!named) {
            return unavailable(QStringLiteral("Channel configuration %1 is reserved")
                                   .arg(*channelConfiguration));
        }
        channelName = *named;
    }

    ConfigurationSummaryResult result;
    result.status = ConfigurationSummaryStatus::Formatted;
    result.summary = QStringList{objectTypeName,
                                 QStringLiteral("%1 Hz").arg(samplingFrequency),
                                 channelName}
                         .join(QStringLiteral(", "));
    return result;
}

} // namespace streamview::rules
