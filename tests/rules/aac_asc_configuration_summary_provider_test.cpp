#include <streamview/rules/aac_asc_configuration_summary_provider.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/configuration_summary.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/structural_entry_runner.h>

#include <QFile>
#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace streamview::rules {

namespace {

using namespace streamview::core;

class MemorySource final : public RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> data) : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= data_.size()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const auto count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.data() + offset, count, destination.data());
        return {count == destination.size() ? SourceReadStatus::Complete
                                           : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
};

[[nodiscard]] std::vector<std::byte> readFixtureBytes(const QString& relativePath) {
    const QString basePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/");
    QFile file(basePath + relativePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = file.readAll();
    const auto uSize = static_cast<std::size_t>(data.size());
    std::vector<std::byte> bytes(uSize);
    std::memcpy(bytes.data(), data.constData(), uSize);
    return bytes;
}

/// Compile the official ASC rule once. The provider must summarize what the
/// real rule package decodes, so these tests never hand-build an ASC tree with
/// invented field names: a rename in the rule has to fail them.
[[nodiscard]] const DslTypedProgram* officialAscProgram() {
    static const std::optional<DslTypedProgram> program = []() -> std::optional<DslTypedProgram> {
        const QString rulePath = QStringLiteral(
            STREAMVIEW_SOURCE_DIR "/src/rules/official/org.streamview.aac/src/aac_asc.svfmt");
        QFile ruleFile(rulePath);
        if (!ruleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return std::nullopt;
        }
        const auto parsed = DslParser::parse(QString::fromUtf8(ruleFile.readAll()));
        if (!parsed.succeeded()) {
            return std::nullopt;
        }
        const auto compiled = DslCompiler::compile(parsed.program);
        if (!compiled.succeeded() || !compiled.program.has_value()) {
            return std::nullopt;
        }
        return compiled.program;
    }();
    return program ? &*program : nullptr;
}

/// A decoded ASC tree plus the source that backs it.
///
/// The tree borrows nothing from the source once execution finishes, but the
/// runner needs the source alive during execution, so both are returned
/// together and outlive the assertions.
struct DecodedAsc final {
    std::unique_ptr<MemorySource> source;
    std::shared_ptr<AnalysisTree> tree;
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
};

[[nodiscard]] DecodedAsc decodeAsc(std::vector<std::byte> ascBytes) {
    DecodedAsc decoded;
    const auto bitLength = static_cast<quint64>(ascBytes.size()) * 8U;
    decoded.source = std::make_unique<MemorySource>(std::move(ascBytes));
    const auto span = SourceSpan::create(SourceBitAddress(0), bitLength);
    if (!span) {
        return decoded;
    }
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    if (!mapping) {
        return decoded;
    }
    const DslTypedProgram* program = officialAscProgram();
    if (program == nullptr) {
        return decoded;
    }
    auto result = StructuralEntryRunner::execute(*decoded.source, *mapping, *program);
    decoded.status = result.execution.status;
    decoded.tree = std::move(result.tree);
    return decoded;
}

/// Pack `values` (each a bit count plus a value) into whole bytes, zero-padded.
[[nodiscard]] std::vector<std::byte>
packBits(const std::vector<std::pair<std::size_t, quint64>>& values) {
    std::vector<bool> bits;
    for (const auto& [bitCount, value] : values) {
        for (std::size_t index = bitCount; index != 0; --index) {
            bits.push_back(((value >> (index - 1)) & 1U) != 0);
        }
    }
    while (bits.size() % 8 != 0) {
        bits.push_back(false);
    }
    std::vector<std::byte> bytes;
    for (std::size_t offset = 0; offset < bits.size(); offset += 8) {
        unsigned int packed = 0;
        for (std::size_t bit = 0; bit < 8; ++bit) {
            packed = (packed << 1U) | static_cast<unsigned int>(bits.at(offset + bit));
        }
        bytes.push_back(static_cast<std::byte>(packed));
    }
    return bytes;
}

/// A general-audio ASC: AOT, frequency index, channel configuration, then the
/// three GASpecificConfig flags the rule requires to complete.
[[nodiscard]] std::vector<std::byte>
gaAscBytes(quint64 audioObjectType, quint64 frequencyIndex, quint64 channelConfiguration) {
    return packBits({{5, audioObjectType},
                     {4, frequencyIndex},
                     {4, channelConfiguration},
                     {1, 0},
                     {1, 0},
                     {1, 0}});
}

[[nodiscard]] ConfigurationSummaryResult summarize(const DecodedAsc& decoded) {
    const AacAscConfigurationSummaryProvider provider;
    return provider.format({.configurationTree = decoded.tree.get(),
                            .configurationNode = decoded.tree->rootId(),
                            .targetFormat = QStringLiteral("audio.aac.asc")});
}

} // namespace

class AacAscConfigurationSummaryProviderTest final : public QObject {
    Q_OBJECT

private slots:
    void claimsTheAscTargetFormat() {
        const AacAscConfigurationSummaryProvider provider;
        // The value must match the annotation the MP4 esds rule declares, since
        // that string is what resolves this provider at runtime.
        QCOMPARE(provider.targetFormat(), QStringLiteral("audio.aac.asc"));
    }

    void summarizesRealEsdsFixtureConfiguration() {
        // The ASC in this fixture is 2 bytes at root byte offset 146; the same
        // span StructuralEntryRunnerTest::executesOfficialAacAscOnEsdsFixture uses.
        const auto fixtureBytes = readFixtureBytes(QStringLiteral("mp4_p5h_mp4a_esds.mp4"));
        QVERIFY(!fixtureBytes.empty());
        const MemorySource rootSource(fixtureBytes);
        const auto ascSpan = SourceSpan::create(SourceBitAddress(1168), 16);
        QVERIFY(ascSpan.has_value());
        const auto ascMapping = SourceMapping::create(LogicalViewId(100), {*ascSpan});
        QVERIFY(ascMapping.has_value());
        const DslTypedProgram* program = officialAscProgram();
        QVERIFY(program != nullptr);

        const auto executed =
            StructuralEntryRunner::execute(rootSource, *ascMapping, *program);
        QVERIFY2(executed.succeeded(), qUtf8Printable(executed.execution.errorMessage));

        const AacAscConfigurationSummaryProvider provider;
        const auto result = provider.format({.configurationTree = executed.tree.get(),
                                             .configurationNode = executed.tree->rootId(),
                                             .targetFormat = QStringLiteral("audio.aac.asc")});
        QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
        QVERIFY(result.formatted());
        // AOT 2, frequency index 4, channel configuration 2 on the wire.
        QCOMPARE(result.summary, QStringLiteral("AAC LC, 44100 Hz, stereo"));
    }

    void acceptsTheConfigurationStructureItself() {
        const auto decoded = decodeAsc(gaAscBytes(2, 4, 2));
        QCOMPARE(decoded.status, DslExecutionStatus::Materialized);
        QVERIFY(decoded.tree != nullptr);
        const auto root = decoded.tree->node(decoded.tree->rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->children().size(), std::size_t{1});

        // Callers may hold either the sub-format tree's root or the structure
        // itself, so both must resolve to the same summary.
        const AacAscConfigurationSummaryProvider provider;
        const auto result = provider.format({.configurationTree = decoded.tree.get(),
                                             .configurationNode = root->children().front(),
                                             .targetFormat = QStringLiteral("audio.aac.asc")});
        QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
        QCOMPARE(result.summary, QStringLiteral("AAC LC, 44100 Hz, stereo"));
    }

    void namesEveryStandardSamplingFrequency() {
        const std::vector<std::pair<quint64, QString>> expected = {
            {0, QStringLiteral("96000 Hz")}, {1, QStringLiteral("88200 Hz")},
            {2, QStringLiteral("64000 Hz")}, {3, QStringLiteral("48000 Hz")},
            {4, QStringLiteral("44100 Hz")}, {5, QStringLiteral("32000 Hz")},
            {6, QStringLiteral("24000 Hz")}, {7, QStringLiteral("22050 Hz")},
            {8, QStringLiteral("16000 Hz")}, {9, QStringLiteral("12000 Hz")},
            {10, QStringLiteral("11025 Hz")}, {11, QStringLiteral("8000 Hz")},
            {12, QStringLiteral("7350 Hz")}};
        for (const auto& [index, frequency] : expected) {
            const auto decoded = decodeAsc(gaAscBytes(2, index, 1));
            QCOMPARE(decoded.status, DslExecutionStatus::Materialized);
            const auto result = summarize(decoded);
            QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
            QCOMPARE(result.summary,
                     QStringLiteral("AAC LC, %1, mono").arg(frequency));
        }
    }

    void reportsExplicitSamplingFrequency() {
        // Index 15 carries the frequency in the following 24 bits rather than
        // naming a table row, so the summary must read the explicit value.
        const auto ascBytes = packBits({{5, 2}, {4, 15}, {24, 96000}, {4, 2}, {1, 0}, {1, 0}, {1, 0}});
        const auto decoded = decodeAsc(ascBytes);
        QCOMPARE(decoded.status, DslExecutionStatus::Materialized);
        const auto result = summarize(decoded);
        QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
        QCOMPARE(result.summary, QStringLiteral("AAC LC, 96000 Hz, stereo"));
    }

    void reportsDependencyUnavailableForReservedSamplingFrequencyIndex() {
        // 13 and 14 are reserved: they name no frequency, and ADR-0105 section 5
        // requires reporting that rather than substituting a plausible rate.
        for (const quint64 index : {quint64{13}, quint64{14}}) {
            const auto decoded = decodeAsc(gaAscBytes(2, index, 2));
            QCOMPARE(decoded.status, DslExecutionStatus::Materialized);
            const auto result = summarize(decoded);
            QCOMPARE(result.status, ConfigurationSummaryStatus::DependencyUnavailable);
            QVERIFY(!result.formatted());
            QVERIFY(result.summary.isEmpty());
        }
    }

    void namesEveryStandardChannelConfiguration() {
        const std::vector<std::pair<quint64, QString>> expected = {
            {1, QStringLiteral("mono")},       {2, QStringLiteral("stereo")},
            {3, QStringLiteral("3 channels")}, {4, QStringLiteral("4 channels")},
            {5, QStringLiteral("5 channels")}, {6, QStringLiteral("5.1")},
            {7, QStringLiteral("7.1")}};
        for (const auto& [configuration, name] : expected) {
            const auto decoded = decodeAsc(gaAscBytes(2, 4, configuration));
            QCOMPARE(decoded.status, DslExecutionStatus::Materialized);
            const auto result = summarize(decoded);
            QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
            QCOMPARE(result.summary, QStringLiteral("AAC LC, 44100 Hz, %1").arg(name));
        }
    }

    void reportsCustomChannelConfigurationForProgramConfigElement() {
        // channel_configuration 0 is a decoded fact, not a missing one: the map
        // lives in the PCE. The summary says so instead of inventing a count.
        // A minimal all-zero PCE is enough, since the summary stops here.
        const auto ascBytes = packBits({{5, 2},
                                        {4, 4},
                                        {4, 0},
                                        {1, 0},
                                        {1, 0},
                                        {1, 0},
                                        // PCE: tag, object type, frequency index, element counts
                                        {4, 0},
                                        {2, 0},
                                        {4, 4},
                                        {4, 0},
                                        {4, 0},
                                        {4, 0},
                                        {2, 0},
                                        {3, 0},
                                        {4, 0},
                                        // mono / stereo / matrix mixdown absent
                                        {1, 0},
                                        {1, 0},
                                        {1, 0},
                                        // byte alignment then an empty comment field
                                        {2, 0},
                                        {8, 0}});
        const auto decoded = decodeAsc(ascBytes);
        QCOMPARE(decoded.status, DslExecutionStatus::Materialized);
        const auto result = summarize(decoded);
        QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
        QCOMPARE(result.summary,
                 QStringLiteral("AAC LC, 44100 Hz, custom channel configuration"));
    }

    void reportsDependencyUnavailableForReservedChannelConfiguration() {
        // 8..15 are reserved. Two representative values keep the loop short.
        for (const quint64 configuration : {quint64{8}, quint64{15}}) {
            const auto decoded = decodeAsc(gaAscBytes(2, 4, configuration));
            QCOMPARE(decoded.status, DslExecutionStatus::Materialized);
            const auto result = summarize(decoded);
            QCOMPARE(result.status, ConfigurationSummaryStatus::DependencyUnavailable);
            QVERIFY(result.summary.isEmpty());
        }
    }

    void namesGeneralAudioObjectTypes() {
        const std::vector<std::pair<quint64, QString>> expected = {
            {1, QStringLiteral("AAC Main")},
            {2, QStringLiteral("AAC LC")},
            {3, QStringLiteral("AAC SSR")},
            {4, QStringLiteral("AAC LTP")},
            {6, QStringLiteral("AAC Scalable")},
            {7, QStringLiteral("TwinVQ")}};
        for (const auto& [objectType, name] : expected) {
            const auto decoded = decodeAsc(gaAscBytes(objectType, 4, 2));
            QCOMPARE(decoded.status, DslExecutionStatus::Materialized);
            const auto result = summarize(decoded);
            QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
            QCOMPARE(result.summary, QStringLiteral("%1, 44100 Hz, stereo").arg(name));
        }
    }

    void summarizesConfigurationsWhoseSpecificConfigIsUnsupported() {
        // The rule stops before SBR and PS specific configs, so their trees end
        // after channel_configuration. Those three fields are still decoded
        // facts, so the summary reports them rather than failing with the
        // payload the rule declined to interpret.
        const std::vector<std::pair<quint64, QString>> expected = {
            {5, QStringLiteral("SBR")}, {29, QStringLiteral("PS")}};
        for (const auto& [objectType, name] : expected) {
            const auto ascBytes = packBits({{5, objectType}, {4, 4}, {4, 2}});
            const auto decoded = decodeAsc(ascBytes);
            QCOMPARE(decoded.status, DslExecutionStatus::Unsupported);
            QVERIFY(decoded.tree != nullptr);
            const auto result = summarize(decoded);
            QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
            QCOMPARE(result.summary, QStringLiteral("%1, 44100 Hz, stereo").arg(name));
        }
    }

    void namesEscapedAudioObjectTypeFromItsExtension() {
        // AOT 31 defers the real type to a 6-bit extension biased by 32, so the
        // summary must name 32 + ext rather than the escape value itself.
        const auto ascBytes = packBits({{5, 31}, {6, 10}, {4, 4}, {4, 2}});
        const auto decoded = decodeAsc(ascBytes);
        QVERIFY(decoded.tree != nullptr);
        const auto result = summarize(decoded);
        QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
        QCOMPARE(result.summary, QStringLiteral("Audio object type 42, 44100 Hz, stereo"));
    }

    void reportsDependencyUnavailableForNullAudioObjectType() {
        // Table 1.17 defines 0 as NULL, which describes no configuration.
        const auto decoded = decodeAsc(gaAscBytes(0, 4, 2));
        QVERIFY(decoded.tree != nullptr);
        const auto result = summarize(decoded);
        QCOMPARE(result.status, ConfigurationSummaryStatus::DependencyUnavailable);
        QVERIFY(result.summary.isEmpty());
    }

    void reportsDependencyUnavailableWhenNoConfigurationWasDecoded() {
        // ADR-0105 section 5: a missing configuration is reported, never guessed.
        auto tree = AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const AacAscConfigurationSummaryProvider provider;
        const auto result = provider.format({.configurationTree = &*tree,
                                             .configurationNode = tree->rootId(),
                                             .targetFormat = QStringLiteral("audio.aac.asc")});
        QCOMPARE(result.status, ConfigurationSummaryStatus::DependencyUnavailable);
        QVERIFY(result.summary.isEmpty());
        QVERIFY(!result.errorMessage.isEmpty());
    }

    void reportsInvalidRequestWithoutTree() {
        const AacAscConfigurationSummaryProvider provider;
        const auto result = provider.format(
            {.configurationTree = nullptr, .targetFormat = QStringLiteral("audio.aac.asc")});
        QCOMPARE(result.status, ConfigurationSummaryStatus::InvalidRequest);
    }

    void bundledRegistryResolvesTheAscProvider() {
        // P5j-4 must reach this provider without naming its class or its codec,
        // so the bundled registry has to resolve the target format on its own.
        const auto& registry = bundledConfigurationSummaryRegistry();
        const auto provider = registry.findProvider(QStringLiteral("audio.aac.asc"));
        QVERIFY(provider != nullptr);

        const auto decoded = decodeAsc(gaAscBytes(2, 3, 2));
        QCOMPARE(decoded.status, DslExecutionStatus::Materialized);
        const auto result = registry.format({.configurationTree = decoded.tree.get(),
                                             .configurationNode = decoded.tree->rootId(),
                                             .targetFormat = QStringLiteral("audio.aac.asc")});
        QVERIFY2(result.status == ConfigurationSummaryStatus::Formatted,
                 qUtf8Printable(result.errorMessage));
        // The exact string SamplePayloadRunnerTest already pins as a runner input.
        QCOMPARE(result.summary, QStringLiteral("AAC LC, 48000 Hz, stereo"));
    }
};

} // namespace streamview::rules

QTEST_MAIN(streamview::rules::AacAscConfigurationSummaryProviderTest)
#include "aac_asc_configuration_summary_provider_test.moc"
