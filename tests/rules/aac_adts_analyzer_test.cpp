#include <streamview/rules/aac_adts_analyzer.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>

#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/core/version.h>
#include <streamview/rules/language_version.h>
#include <streamview/rules/rule_catalog.h>

#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace {

[[nodiscard]] std::optional<streamview::core::SourceMapping> mappingForBytes(quint64 byteCount) {
    const auto span = streamview::core::SourceSpan::create(
        streamview::core::SourceBitAddress(0), byteCount * 8U);
    if (!span) {
        return std::nullopt;
    }
    return streamview::core::SourceMapping::create(
        streamview::core::LogicalViewId(1), {*span});
}


class MemorySource final : public streamview::core::RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> data) : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("memory"); }

    [[nodiscard]] streamview::core::SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {streamview::core::SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= data_.size()) {
            return {streamview::core::SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const auto count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.data() + offset, count, destination.data());
        return {count == destination.size()
                    ? streamview::core::SourceReadStatus::Complete
                    : streamview::core::SourceReadStatus::EndOfSource,
                count, {}};
    }

private:
    std::vector<std::byte> data_;
};

[[nodiscard]] std::vector<std::byte> makeAdtsFrame(quint16 frameLength,
                                                    bool protectionAbsent = true,
                                                    quint8 profile = 1,
                                                    quint8 samplingFreqIndex = 4,
                                                    quint8 channelConfig = 2,
                                                    quint16 bufferFullness = 0x7FF,
                                                    quint8 numRawBlocks = 0,
                                                    quint16 crcValue = 0x1234,
                                                    std::byte fillByte = std::byte{0xAB}) {
    const std::size_t headerLen = protectionAbsent ? 7U : 9U;
    std::vector<std::byte> frame(frameLength, fillByte);
    if (frameLength < headerLen) {
        return frame;
    }
    frame[0] = std::byte{0xFF};
    frame[1] = std::byte{static_cast<quint8>(0xF0U | (protectionAbsent ? 1U : 0U))};
    frame[2] = std::byte{static_cast<quint8>(
        ((profile & 0x03U) << 6U) |
        ((samplingFreqIndex & 0x0FU) << 2U) |
        ((channelConfig >> 2U) & 0x01U))};
    frame[3] = std::byte{static_cast<quint8>(
        ((channelConfig & 0x03U) << 6U) |
        ((frameLength >> 11U) & 0x03U))};
    frame[4] = std::byte{static_cast<quint8>((frameLength >> 3U) & 0xFFU)};
    frame[5] = std::byte{static_cast<quint8>(
        ((frameLength & 0x07U) << 5U) | ((bufferFullness >> 6U) & 0x1FU))};
    frame[6] = std::byte{static_cast<quint8>(
        ((bufferFullness & 0x3FU) << 2U) | (numRawBlocks & 0x03U))};
    if (!protectionAbsent) {
        frame[7] = std::byte{static_cast<quint8>((crcValue >> 8U) & 0xFFU)};
        frame[8] = std::byte{static_cast<quint8>(crcValue & 0xFFU)};
    }
    return frame;
}

} // namespace

class AacAdtsAnalyzerTest : public QObject {
    Q_OBJECT

private slots:
    void createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(120, true, 1, 4, 2, 0x100, 0);
        const auto f2 = makeAdtsFrame(150, false, 0, 3, 1, 0x7FF, 0, 0x1234);
        const auto f3 = makeAdtsFrame(100, true, 2, 5, 6, 0x200, 0);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));
        QCOMPARE(analyzer->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.aac"));
        QCOMPARE(analyzer->ruleIdentity().entryPointId(), QStringLiteral("adts"));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.frameNodes.size(), std::size_t(3));
        QVERIFY(analyzer->finished());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), streamview::core::MaterializationState::Materialized);
        QCOMPARE(root->children().size(), std::size_t(3));

        // Frame 0: CBR, protection_absent == 1 (7-byte header)
        const auto node0 = analyzer->tree().node(batch.frameNodes[0]);
        QVERIFY(node0.has_value());
        QCOMPARE(node0->name(), QStringLiteral("adts_frame[0]"));
        QCOMPARE(node0->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node0->diagnostics().empty());
        QCOMPARE(node0->children().size(), std::size_t(1));

        const auto header0 = analyzer->tree().node(node0->children()[0]);
        QVERIFY(header0.has_value());
        QCOMPARE(header0->name(), QStringLiteral("AdtsHeader"));
        QCOMPARE(header0->children().size(), std::size_t(18));

        const std::vector<QString> expectedNames0 = {
            QStringLiteral("syncword"),
            QStringLiteral("id"),
            QStringLiteral("layer"),
            QStringLiteral("protection_absent"),
            QStringLiteral("profile"),
            QStringLiteral("sampling_frequency_index"),
            QStringLiteral("private_bit"),
            QStringLiteral("channel_configuration"),
            QStringLiteral("original_copy"),
            QStringLiteral("home"),
            QStringLiteral("copyright_identification_bit"),
            QStringLiteral("copyright_identification_start"),
            QStringLiteral("aac_frame_length"),
            QStringLiteral("adts_buffer_fullness"),
            QStringLiteral("number_of_raw_data_blocks_in_frame"),
            QStringLiteral("minimum_frame_length"),
            QStringLiteral("raw_data_block_bytes"),
            QStringLiteral("raw_data_block")
        };

        for (std::size_t i = 0; i < expectedNames0.size(); ++i) {
            const auto child = analyzer->tree().node(header0->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames0[i]);
        }

        // Verify values of frame 0
        QCOMPARE(analyzer->tree().node(header0->children()[0])->value().toULongLong(), quint64(4095));
        QCOMPARE(analyzer->tree().node(header0->children()[1])->value().toULongLong(), quint64(0));
        QCOMPARE(analyzer->tree().node(header0->children()[2])->value().toULongLong(), quint64(0));
        QCOMPARE(analyzer->tree().node(header0->children()[3])->value().toULongLong(), quint64(1));
        QCOMPARE(analyzer->tree().node(header0->children()[4])->value().toULongLong(), quint64(1));
        QCOMPARE(analyzer->tree().node(header0->children()[5])->value().toULongLong(), quint64(4));
        QCOMPARE(analyzer->tree().node(header0->children()[6])->value().toULongLong(), quint64(0));
        QCOMPARE(analyzer->tree().node(header0->children()[7])->value().toULongLong(), quint64(2));
        QCOMPARE(analyzer->tree().node(header0->children()[12])->value().toULongLong(), quint64(120));
        QCOMPARE(analyzer->tree().node(header0->children()[13])->value().toULongLong(), quint64(256));
        QCOMPARE(analyzer->tree().node(header0->children()[14])->value().toULongLong(), quint64(0));
        QCOMPARE(analyzer->tree().node(header0->children()[15])->value().toULongLong(), quint64(7));

        // Frame 1: VBR, protection_absent == 0 (9-byte header with crc_check)
        const auto node1 = analyzer->tree().node(batch.frameNodes[1]);
        QVERIFY(node1.has_value());
        QCOMPARE(node1->name(), QStringLiteral("adts_frame[1]"));
        QCOMPARE(node1->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node1->diagnostics().empty());

        const auto header1 = analyzer->tree().node(node1->children()[0]);
        QVERIFY(header1.has_value());
        QCOMPARE(header1->children().size(), std::size_t(19));

        const std::vector<QString> expectedNames1 = {
            QStringLiteral("syncword"),
            QStringLiteral("id"),
            QStringLiteral("layer"),
            QStringLiteral("protection_absent"),
            QStringLiteral("profile"),
            QStringLiteral("sampling_frequency_index"),
            QStringLiteral("private_bit"),
            QStringLiteral("channel_configuration"),
            QStringLiteral("original_copy"),
            QStringLiteral("home"),
            QStringLiteral("copyright_identification_bit"),
            QStringLiteral("copyright_identification_start"),
            QStringLiteral("aac_frame_length"),
            QStringLiteral("adts_buffer_fullness"),
            QStringLiteral("number_of_raw_data_blocks_in_frame"),
            QStringLiteral("crc_check"),
            QStringLiteral("minimum_frame_length"),
            QStringLiteral("raw_data_block_bytes"),
            QStringLiteral("raw_data_block")
        };

        for (std::size_t i = 0; i < expectedNames1.size(); ++i) {
            const auto child = analyzer->tree().node(header1->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames1[i]);
        }

        QCOMPARE(analyzer->tree().node(header1->children()[3])->value().toULongLong(), quint64(0));
        QCOMPARE(analyzer->tree().node(header1->children()[12])->value().toULongLong(), quint64(150));
        QCOMPARE(analyzer->tree().node(header1->children()[13])->value().toULongLong(), quint64(0x7FF));
        QCOMPARE(analyzer->tree().node(header1->children()[15])->value().toULongLong(), quint64(0x1234));
        QCOMPARE(analyzer->tree().node(header1->children()[16])->value().toULongLong(), quint64(9));
    }

    void failsToCreateAnalyzerWhenResolvedRuleIsInvalid() {
        const MemorySource source(std::vector<std::byte>{std::byte{0x00}});
        streamview::rules::RuleCatalogLookupResult invalidResolved;
        invalidResolved.status = streamview::rules::RuleCatalogLookupStatus::MissingContent;
        invalidResolved.errorMessage = QStringLiteral("Missing rule package content");

        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, invalidResolved, &error);
        QVERIFY(!analyzer.has_value());
        QCOMPARE(error, QStringLiteral("Missing rule package content"));
    }

    void isolatesMultipleRawDataBlocksViolationAndContinues() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(100, true, 1, 4, 2, 0x7FF, 0);
        const auto f2 = makeAdtsFrame(120, true, 1, 4, 2, 0x7FF, 1); // violation: num_blocks = 1
        const auto f3 = makeAdtsFrame(100, true, 1, 4, 2, 0x7FF, 0);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.status, streamview::rules::AacAdtsAnalysisStatus::Complete);
        QCOMPARE(batch.frameNodes.size(), std::size_t(3));
        QVERIFY(analyzer->finished());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), streamview::core::MaterializationState::Materialized);

        // Frame 0: Materialized
        const auto node0 = analyzer->tree().node(batch.frameNodes[0]);
        QVERIFY(node0.has_value());
        QCOMPARE(node0->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node0->diagnostics().empty());

        // Frame 1: Invalid due to number_of_raw_data_blocks_in_frame @equals(0) failure
        const auto node1 = analyzer->tree().node(batch.frameNodes[1]);
        QVERIFY(node1.has_value());
        QCOMPARE(node1->state(), streamview::core::MaterializationState::Invalid);
        QVERIFY(!node1->diagnostics().empty());
        QCOMPARE(node1->diagnostics().front().code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(node1->diagnostics().front().severity, streamview::core::DiagnosticSeverity::Error);

        // Frame 2: Materialized (subsequent frame correctly decoded and materialized)
        const auto node2 = analyzer->tree().node(batch.frameNodes[2]);
        QVERIFY(node2.has_value());
        QCOMPARE(node2->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node2->diagnostics().empty());
    }

    void handlesHeaderTruncationWithCrcPresent() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(100, true);
        const auto f2 = makeAdtsFrame(100, false); // header length 9 (CRC present)
        stream.insert(stream.end(), f1.begin(), f1.end());
        // Truncate frame 2 to only 8 bytes (less than 9 required)
        stream.insert(stream.end(), f2.begin(), f2.begin() + 8);

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.frameNodes.size(), std::size_t(2));
        QVERIFY(analyzer->finished());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), streamview::core::MaterializationState::Materialized);

        const auto node0 = analyzer->tree().node(batch.frameNodes[0]);
        QVERIFY(node0.has_value());
        QCOMPARE(node0->state(), streamview::core::MaterializationState::Materialized);

        const auto node1 = analyzer->tree().node(batch.frameNodes[1]);
        QVERIFY(node1.has_value());
        QCOMPARE(node1->state(), streamview::core::MaterializationState::Invalid);
        QVERIFY(!node1->diagnostics().empty());
        QCOMPARE(node1->diagnostics().front().code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(node1->diagnostics().front().severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(node1->diagnostics().front().message, QStringLiteral("Unable to read complete syntax field"));
    }

    void handlesPayloadTruncationAtEof() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(100, true);
        const auto f2 = makeAdtsFrame(200, true); // declares 200 bytes
        stream.insert(stream.end(), f1.begin(), f1.end());
        // Truncate frame 2 payload to only 50 bytes total
        stream.insert(stream.end(), f2.begin(), f2.begin() + 50);

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.frameNodes.size(), std::size_t(2));
        QVERIFY(analyzer->finished());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), streamview::core::MaterializationState::Materialized);

        const auto node0 = analyzer->tree().node(batch.frameNodes[0]);
        QVERIFY(node0.has_value());
        QCOMPARE(node0->state(), streamview::core::MaterializationState::Materialized);

        const auto node1 = analyzer->tree().node(batch.frameNodes[1]);
        QVERIFY(node1.has_value());
        QCOMPARE(node1->state(), streamview::core::MaterializationState::Invalid);
        QVERIFY(!node1->diagnostics().empty());
        QCOMPARE(node1->diagnostics().front().code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(node1->diagnostics().front().severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(node1->diagnostics().front().message, QStringLiteral("Lazy byte region exceeds the available source range"));

        // Header structure node inside truncated frame is invalid due to truncated lazy payload
        QCOMPARE(node1->children().size(), std::size_t(1));
        const auto header1 = analyzer->tree().node(node1->children()[0]);
        QVERIFY(header1.has_value());
        QCOMPARE(header1->name(), QStringLiteral("AdtsHeader"));
        QCOMPARE(header1->state(), streamview::core::MaterializationState::Invalid);
    }

    /**
     * @brief Pins the known capability boundary / narrowed behavior when an ADTS rule
     * package does NOT declare a payload region (e.g. lacks @lazy raw_data_block).
     *
     * In this scenario, the scanner flags record.truncated == true, but the DSL VM
     * successfully decodes all declared header fields within the available frame mapping.
     * Because format-specific diagnostics are no longer synthesized in C++, the trailing
     * truncated frame region node transitions to Materialized with zero diagnostics, with its
     * logical range clamped to available bytes in the source (30 bytes * 8 = 240 bits).
     *
     * This test intentionally documents and pins this narrowed capability boundary.
     */
    void materializesTruncatedTrailingFrameWhenRuleLacksPayloadDeclaration() {
        const QByteArray toml = QByteArrayLiteral(
            "manifest-version = 1\n\n"
            "[package]\n"
            "id = \"org.custom.aac\"\n"
            "version = \"0.1.0\"\n"
            "authors = [\"Custom Author\"]\n"
            "license = \"MIT\"\n"
            "dependencies = []\n\n"
            "[compatibility]\n"
            "language = \"0.1\"\n"
            "engine = \">=0.1.0 <0.2.0\"\n\n"
            "[[entrypoints]]\n"
            "id = \"adts\"\n"
            "format = \"audio.aac.adts\"\n"
            "source = \"src/adts.svfmt\"\n"
            "profiles = [\"lc\"]\n"
            "depth = \"structural\"\n");

        const QByteArray svfmt = QByteArrayLiteral(
            "struct AdtsHeader {\n"
            "    bits<12> syncword @equals(4095);\n"
            "    bits<1> id;\n"
            "    bits<2> layer;\n"
            "    bits<1> protection_absent;\n"
            "    bits<2> profile;\n"
            "    bits<4> sampling_frequency_index;\n"
            "    bits<1> private_bit;\n"
            "    bits<3> channel_configuration;\n"
            "    bits<1> original_copy;\n"
            "    bits<1> home;\n"
            "    bits<1> copyright_identification_bit;\n"
            "    bits<1> copyright_identification_start;\n"
            "    bits<13> aac_frame_length;\n"
            "    bits<11> adts_buffer_fullness;\n"
            "    bits<2> number_of_raw_data_blocks_in_frame;\n"
            "}\n\n"
            "@index(progressive) sequence<AdtsHeader> frames = scan(adts_frame);\n"
            "entry frames;\n");

        std::vector<streamview::rules::RulePackageFile> files{
            {QStringLiteral("rule.toml"), toml},
            {QStringLiteral("src/adts.svfmt"), svfmt}};
        auto pkgRes = streamview::rules::RulePackage::fromFiles(std::move(files));
        QVERIFY(pkgRes.succeeded());
        QVERIFY(pkgRes.package.has_value());

        const auto pkgIdentity = pkgRes.package->identity();
        streamview::rules::RulePackageCatalog catalog;
        const auto reg = catalog.registerPackage(std::move(*pkgRes.package));
        QVERIFY(reg.succeeded());

        const auto resolved = catalog.resolve(
            pkgIdentity,
            QStringLiteral("adts"),
            streamview::rules::languageVersion(),
            streamview::core::version());
        QVERIFY2(resolved.succeeded(), qPrintable(resolved.errorMessage));

        // Stream: Frame 0 (50 bytes complete) + Frame 1 (declares 100 bytes, truncated to 30 bytes)
        const auto f0 = makeAdtsFrame(50, true);
        const auto f1Full = makeAdtsFrame(100, true);
        std::vector<std::byte> stream(f0.begin(), f0.end());
        stream.insert(stream.end(), f1Full.begin(), f1Full.begin() + 30);

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, resolved, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.frameNodes.size(), std::size_t(2));

        // Frame 0: Fully materialized complete frame
        const auto node0 = analyzer->tree().node(batch.frameNodes[0]);
        QVERIFY(node0.has_value());
        QCOMPARE(node0->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node0->diagnostics().empty());
        QVERIFY(node0->location().has_value());
        QCOMPARE(node0->location()->logicalRange().bitLength(), quint64(50U * 8U));

        // Frame 1 (truncated trailing frame): Known capability boundary pins Materialized with 0 diagnostics
        // and logicalRange bitLength clamped to available bytes (30 bytes * 8 = 240 bits)
        const auto node1 = analyzer->tree().node(batch.frameNodes[1]);
        QVERIFY(node1.has_value());
        QCOMPARE(node1->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node1->diagnostics().empty());
        QVERIFY(node1->location().has_value());
        QCOMPARE(node1->location()->logicalRange().bitLength(), quint64(30U * 8U));

        const auto header1 = analyzer->tree().node(node1->children()[0]);
        QVERIFY(header1.has_value());
        QCOMPARE(header1->state(), streamview::core::MaterializationState::Materialized);
        QCOMPARE(header1->children().size(), std::size_t(15));
        QVERIFY(header1->diagnostics().empty());
    }

    void handlesTrailingGarbageSmallerThanHeader() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(150, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        // 4 trailing bytes (< 7 bytes header)
        stream.push_back(std::byte{0xFF});
        stream.push_back(std::byte{0xF1});
        stream.push_back(std::byte{0x00});
        stream.push_back(std::byte{0x00});

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.frameNodes.size(), std::size_t(1));
        QVERIFY(analyzer->finished());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), streamview::core::MaterializationState::Materialized);
    }

    void resynchronizesAcrossCorruptedByteSpan() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(100, true);
        const auto f2 = makeAdtsFrame(100, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        // Corrupt gap of 20 bytes
        for (int i = 0; i < 20; ++i) {
            stream.push_back(std::byte{0xAA});
        }
        stream.insert(stream.end(), f2.begin(), f2.end());

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.frameNodes.size(), std::size_t(2));
        QVERIFY(analyzer->finished());

        const auto node0 = analyzer->tree().node(batch.frameNodes[0]);
        QVERIFY(node0.has_value());
        QCOMPARE(node0->state(), streamview::core::MaterializationState::Materialized);

        const auto node1 = analyzer->tree().node(batch.frameNodes[1]);
        QVERIFY(node1.has_value());
        QCOMPARE(node1->state(), streamview::core::MaterializationState::Materialized);
    }

    void respectsBatchLimits() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(100, true);
        const auto f2 = makeAdtsFrame(100, true);
        const auto f3 = makeAdtsFrame(100, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto b1 = analyzer->analyzeBatch(1);
        QCOMPARE(b1.status, streamview::rules::AacAdtsAnalysisStatus::InProgress);
        QCOMPARE(b1.frameNodes.size(), std::size_t(1));

        const auto b2 = analyzer->analyzeBatch(1);
        QCOMPARE(b2.status, streamview::rules::AacAdtsAnalysisStatus::InProgress);
        QCOMPARE(b2.frameNodes.size(), std::size_t(1));

        const auto b3 = analyzer->analyzeBatch(1);
        QCOMPARE(b3.status, streamview::rules::AacAdtsAnalysisStatus::Complete);
        QCOMPARE(b3.frameNodes.size(), std::size_t(1));
        QVERIFY(analyzer->finished());
    }

    void respectsCancellationAndResumes() {
        std::vector<std::byte> stream;
        for (int i = 0; i < 10; ++i) {
            const auto f = makeAdtsFrame(100, true);
            stream.insert(stream.end(), f.begin(), f.end());
        }

        const MemorySource source(std::move(stream));
        streamview::core::CancellationSource cancelSource;
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(
            source, &error, cancelSource.token());
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        (void)cancelSource.requestCancellation();
        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, streamview::rules::AacAdtsAnalysisStatus::Cancelled);

        QVERIFY(analyzer->resumeAfterCancellation());
        const auto resumeBatch = analyzer->analyzeBatch();
        QCOMPARE(resumeBatch.status, streamview::rules::AacAdtsAnalysisStatus::Complete);
        QVERIFY(analyzer->finished());
    }

    void handlesInvalidBatchSize() {
        const MemorySource source(std::vector<std::byte>{std::byte{0x00}});
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY(analyzer.has_value());

        const auto b1 = analyzer->analyzeBatch(0, 100);
        QCOMPARE(b1.status, streamview::rules::AacAdtsAnalysisStatus::InvalidBatchSize);

        const auto b2 = analyzer->analyzeBatch(10, 0);
        QCOMPARE(b2.status, streamview::rules::AacAdtsAnalysisStatus::InvalidBatchSize);
    }

    void resolvesAscEntryPointFromBundledRulePackage() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        QVERIFY(loaded.package.has_value());
        QCOMPARE(loaded.package->identity().packageId(), QStringLiteral("org.streamview.aac"));
        QCOMPARE(loaded.package->identity().packageVersion(), QStringLiteral("0.1.3"));

        streamview::rules::RulePackageCatalog catalog;
        const auto reg = catalog.registerPackage(streamview::rules::RulePackage(*loaded.package));
        QVERIFY(reg.succeeded());

        const auto resolvedAsc = catalog.resolve(
            loaded.package->identity(),
            QStringLiteral("asc"),
            streamview::rules::languageVersion(),
            streamview::core::version());
        QVERIFY2(resolvedAsc.succeeded(), qPrintable(resolvedAsc.errorMessage));
        QVERIFY(resolvedAsc.entryPoint.has_value());
        QCOMPARE(resolvedAsc.entryPoint->id, QStringLiteral("asc"));
        QCOMPARE(resolvedAsc.entryPoint->format, QStringLiteral("audio.aac.asc"));
        QCOMPARE(resolvedAsc.entryPoint->depth, QStringLiteral("structural"));
        QCOMPARE(resolvedAsc.entryPoint->sourcePath, QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(!resolvedAsc.entryPoint->detector.has_value());

        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        QVERIFY2(parsed.succeeded(), parsed.diagnostics.empty() ? "" : qPrintable(parsed.diagnostics.front().message));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(), compiled.diagnostics.empty() ? "" : qPrintable(compiled.diagnostics.front().message));
    }

    void decodesAscCase1Baseline() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<std::byte> raw = {std::byte{0x11}, std::byte{0x80}, std::byte{0x00}, std::byte{0xC0}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x5A}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}, std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B}, std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}, std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B}, std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}, std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}, std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}, std::byte{0x4D}, std::byte{0x4E}, std::byte{0x4F}, std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57}, std::byte{0x58}, std::byte{0x59}};
        const MemorySource source(raw);
        auto tree = streamview::core::AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const auto mapping = mappingForBytes(source.sizeBytes());
        QVERIFY(mapping.has_value());
        streamview::core::BitReader reader(source, *mapping);

        const auto result = streamview::rules::DslExecutor::decodeStruct(
            *compiled.program,
            QStringLiteral("AudioSpecificConfig"),
            reader,
            *mapping,
            0,
            *tree,
            tree->rootId());
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::Materialized);
        QVERIFY(result.structureNode.has_value());

        const auto node = tree->node(*result.structureNode);
        QVERIFY(node.has_value());
        QCOMPARE(node->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node->diagnostics().empty());

        const std::vector<QString> expectedNames = {
            QStringLiteral("audio_object_type"),
            QStringLiteral("sampling_frequency_index"),
            QStringLiteral("channel_configuration"),
            QStringLiteral("frame_length_flag"),
            QStringLiteral("depends_on_core_coder"),
            QStringLiteral("extension_flag"),
            QStringLiteral("element_instance_tag"),
            QStringLiteral("object_type"),
            QStringLiteral("pce_sampling_frequency_index"),
            QStringLiteral("num_front_channel_elements"),
            QStringLiteral("num_side_channel_elements"),
            QStringLiteral("num_back_channel_elements"),
            QStringLiteral("num_lfe_channel_elements"),
            QStringLiteral("num_assoc_data_elements"),
            QStringLiteral("num_valid_cc_elements"),
            QStringLiteral("mono_mixdown_present"),
            QStringLiteral("stereo_mixdown_present"),
            QStringLiteral("matrix_mixdown_idx_present"),
            QStringLiteral("pce_total_bits"),
            QStringLiteral("pce_rem"),
            QStringLiteral("pce_alignment_bits"),
            QStringLiteral("byte_alignment_zero_bit[0]"),
            QStringLiteral("byte_alignment_zero_bit[1]"),
            QStringLiteral("byte_alignment_zero_bit[2]"),
            QStringLiteral("byte_alignment_zero_bit[3]"),
            QStringLiteral("byte_alignment_zero_bit[4]"),
            QStringLiteral("byte_alignment_zero_bit[5]"),
            QStringLiteral("comment_field_bytes"),
            QStringLiteral("comment_field_data[0]"),
            QStringLiteral("comment_field_data[1]"),
            QStringLiteral("comment_field_data[2]"),
            QStringLiteral("comment_field_data[3]"),
            QStringLiteral("comment_field_data[4]"),
            QStringLiteral("comment_field_data[5]"),
            QStringLiteral("comment_field_data[6]"),
            QStringLiteral("comment_field_data[7]"),
            QStringLiteral("comment_field_data[8]"),
            QStringLiteral("comment_field_data[9]"),
            QStringLiteral("comment_field_data[10]"),
            QStringLiteral("comment_field_data[11]"),
            QStringLiteral("comment_field_data[12]"),
            QStringLiteral("comment_field_data[13]"),
            QStringLiteral("comment_field_data[14]"),
            QStringLiteral("comment_field_data[15]"),
            QStringLiteral("comment_field_data[16]"),
            QStringLiteral("comment_field_data[17]"),
            QStringLiteral("comment_field_data[18]"),
            QStringLiteral("comment_field_data[19]"),
            QStringLiteral("comment_field_data[20]"),
            QStringLiteral("comment_field_data[21]"),
            QStringLiteral("comment_field_data[22]"),
            QStringLiteral("comment_field_data[23]"),
            QStringLiteral("comment_field_data[24]"),
            QStringLiteral("comment_field_data[25]"),
            QStringLiteral("comment_field_data[26]"),
            QStringLiteral("comment_field_data[27]"),
            QStringLiteral("comment_field_data[28]"),
            QStringLiteral("comment_field_data[29]"),
            QStringLiteral("comment_field_data[30]"),
            QStringLiteral("comment_field_data[31]"),
            QStringLiteral("comment_field_data[32]"),
            QStringLiteral("comment_field_data[33]"),
            QStringLiteral("comment_field_data[34]"),
            QStringLiteral("comment_field_data[35]"),
            QStringLiteral("comment_field_data[36]"),
            QStringLiteral("comment_field_data[37]"),
            QStringLiteral("comment_field_data[38]"),
            QStringLiteral("comment_field_data[39]"),
            QStringLiteral("comment_field_data[40]"),
            QStringLiteral("comment_field_data[41]"),
            QStringLiteral("comment_field_data[42]"),
            QStringLiteral("comment_field_data[43]"),
            QStringLiteral("comment_field_data[44]"),
            QStringLiteral("comment_field_data[45]"),
            QStringLiteral("comment_field_data[46]"),
            QStringLiteral("comment_field_data[47]"),
            QStringLiteral("comment_field_data[48]"),
            QStringLiteral("comment_field_data[49]"),
            QStringLiteral("comment_field_data[50]"),
            QStringLiteral("comment_field_data[51]"),
            QStringLiteral("comment_field_data[52]"),
            QStringLiteral("comment_field_data[53]"),
            QStringLiteral("comment_field_data[54]"),
            QStringLiteral("comment_field_data[55]"),
            QStringLiteral("comment_field_data[56]"),
            QStringLiteral("comment_field_data[57]"),
            QStringLiteral("comment_field_data[58]"),
            QStringLiteral("comment_field_data[59]"),
            QStringLiteral("comment_field_data[60]"),
            QStringLiteral("comment_field_data[61]"),
            QStringLiteral("comment_field_data[62]"),
            QStringLiteral("comment_field_data[63]"),
            QStringLiteral("comment_field_data[64]"),
            QStringLiteral("comment_field_data[65]"),
            QStringLiteral("comment_field_data[66]"),
            QStringLiteral("comment_field_data[67]"),
            QStringLiteral("comment_field_data[68]"),
            QStringLiteral("comment_field_data[69]"),
            QStringLiteral("comment_field_data[70]"),
            QStringLiteral("comment_field_data[71]"),
            QStringLiteral("comment_field_data[72]"),
            QStringLiteral("comment_field_data[73]"),
            QStringLiteral("comment_field_data[74]"),
            QStringLiteral("comment_field_data[75]"),
            QStringLiteral("comment_field_data[76]"),
            QStringLiteral("comment_field_data[77]"),
            QStringLiteral("comment_field_data[78]"),
            QStringLiteral("comment_field_data[79]"),
            QStringLiteral("comment_field_data[80]"),
            QStringLiteral("comment_field_data[81]"),
            QStringLiteral("comment_field_data[82]"),
            QStringLiteral("comment_field_data[83]"),
            QStringLiteral("comment_field_data[84]"),
            QStringLiteral("comment_field_data[85]"),
            QStringLiteral("comment_field_data[86]"),
            QStringLiteral("comment_field_data[87]"),
            QStringLiteral("comment_field_data[88]"),
            QStringLiteral("comment_field_data[89]"),
        };
        QCOMPARE(node->children().size(), expectedNames.size());
        for (std::size_t i = 0; i < expectedNames.size(); ++i) {
            const auto child = tree->node(node->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames[i]);
            if (child->name() == QStringLiteral("comment_field_bytes")) {
                QVERIFY(child->location().has_value());
                QCOMPARE(child->location()->logicalRange().start().bitOffset() / 8, quint64(7));
                QCOMPARE(child->value().toULongLong(), quint64(90));
            }
        }
    }

    void decodesAscCase2CoreCoderDelay() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<std::byte> raw = {std::byte{0x11}, std::byte{0x82}, std::byte{0xD5}, std::byte{0xE0}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x5A}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}, std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B}, std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}, std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B}, std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}, std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}, std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}, std::byte{0x4D}, std::byte{0x4E}, std::byte{0x4F}, std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57}, std::byte{0x58}, std::byte{0x59}};
        const MemorySource source(raw);
        auto tree = streamview::core::AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const auto mapping = mappingForBytes(source.sizeBytes());
        QVERIFY(mapping.has_value());
        streamview::core::BitReader reader(source, *mapping);

        const auto result = streamview::rules::DslExecutor::decodeStruct(
            *compiled.program,
            QStringLiteral("AudioSpecificConfig"),
            reader,
            *mapping,
            0,
            *tree,
            tree->rootId());
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::Materialized);
        QVERIFY(result.structureNode.has_value());

        const auto node = tree->node(*result.structureNode);
        QVERIFY(node.has_value());
        QCOMPARE(node->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node->diagnostics().empty());

        const std::vector<QString> expectedNames = {
            QStringLiteral("audio_object_type"),
            QStringLiteral("sampling_frequency_index"),
            QStringLiteral("channel_configuration"),
            QStringLiteral("frame_length_flag"),
            QStringLiteral("depends_on_core_coder"),
            QStringLiteral("core_coder_delay"),
            QStringLiteral("extension_flag"),
            QStringLiteral("element_instance_tag"),
            QStringLiteral("object_type"),
            QStringLiteral("pce_sampling_frequency_index"),
            QStringLiteral("num_front_channel_elements"),
            QStringLiteral("num_side_channel_elements"),
            QStringLiteral("num_back_channel_elements"),
            QStringLiteral("num_lfe_channel_elements"),
            QStringLiteral("num_assoc_data_elements"),
            QStringLiteral("num_valid_cc_elements"),
            QStringLiteral("mono_mixdown_present"),
            QStringLiteral("stereo_mixdown_present"),
            QStringLiteral("matrix_mixdown_idx_present"),
            QStringLiteral("pce_total_bits"),
            QStringLiteral("pce_rem"),
            QStringLiteral("pce_alignment_bits"),
            QStringLiteral("comment_field_bytes"),
            QStringLiteral("comment_field_data[0]"),
            QStringLiteral("comment_field_data[1]"),
            QStringLiteral("comment_field_data[2]"),
            QStringLiteral("comment_field_data[3]"),
            QStringLiteral("comment_field_data[4]"),
            QStringLiteral("comment_field_data[5]"),
            QStringLiteral("comment_field_data[6]"),
            QStringLiteral("comment_field_data[7]"),
            QStringLiteral("comment_field_data[8]"),
            QStringLiteral("comment_field_data[9]"),
            QStringLiteral("comment_field_data[10]"),
            QStringLiteral("comment_field_data[11]"),
            QStringLiteral("comment_field_data[12]"),
            QStringLiteral("comment_field_data[13]"),
            QStringLiteral("comment_field_data[14]"),
            QStringLiteral("comment_field_data[15]"),
            QStringLiteral("comment_field_data[16]"),
            QStringLiteral("comment_field_data[17]"),
            QStringLiteral("comment_field_data[18]"),
            QStringLiteral("comment_field_data[19]"),
            QStringLiteral("comment_field_data[20]"),
            QStringLiteral("comment_field_data[21]"),
            QStringLiteral("comment_field_data[22]"),
            QStringLiteral("comment_field_data[23]"),
            QStringLiteral("comment_field_data[24]"),
            QStringLiteral("comment_field_data[25]"),
            QStringLiteral("comment_field_data[26]"),
            QStringLiteral("comment_field_data[27]"),
            QStringLiteral("comment_field_data[28]"),
            QStringLiteral("comment_field_data[29]"),
            QStringLiteral("comment_field_data[30]"),
            QStringLiteral("comment_field_data[31]"),
            QStringLiteral("comment_field_data[32]"),
            QStringLiteral("comment_field_data[33]"),
            QStringLiteral("comment_field_data[34]"),
            QStringLiteral("comment_field_data[35]"),
            QStringLiteral("comment_field_data[36]"),
            QStringLiteral("comment_field_data[37]"),
            QStringLiteral("comment_field_data[38]"),
            QStringLiteral("comment_field_data[39]"),
            QStringLiteral("comment_field_data[40]"),
            QStringLiteral("comment_field_data[41]"),
            QStringLiteral("comment_field_data[42]"),
            QStringLiteral("comment_field_data[43]"),
            QStringLiteral("comment_field_data[44]"),
            QStringLiteral("comment_field_data[45]"),
            QStringLiteral("comment_field_data[46]"),
            QStringLiteral("comment_field_data[47]"),
            QStringLiteral("comment_field_data[48]"),
            QStringLiteral("comment_field_data[49]"),
            QStringLiteral("comment_field_data[50]"),
            QStringLiteral("comment_field_data[51]"),
            QStringLiteral("comment_field_data[52]"),
            QStringLiteral("comment_field_data[53]"),
            QStringLiteral("comment_field_data[54]"),
            QStringLiteral("comment_field_data[55]"),
            QStringLiteral("comment_field_data[56]"),
            QStringLiteral("comment_field_data[57]"),
            QStringLiteral("comment_field_data[58]"),
            QStringLiteral("comment_field_data[59]"),
            QStringLiteral("comment_field_data[60]"),
            QStringLiteral("comment_field_data[61]"),
            QStringLiteral("comment_field_data[62]"),
            QStringLiteral("comment_field_data[63]"),
            QStringLiteral("comment_field_data[64]"),
            QStringLiteral("comment_field_data[65]"),
            QStringLiteral("comment_field_data[66]"),
            QStringLiteral("comment_field_data[67]"),
            QStringLiteral("comment_field_data[68]"),
            QStringLiteral("comment_field_data[69]"),
            QStringLiteral("comment_field_data[70]"),
            QStringLiteral("comment_field_data[71]"),
            QStringLiteral("comment_field_data[72]"),
            QStringLiteral("comment_field_data[73]"),
            QStringLiteral("comment_field_data[74]"),
            QStringLiteral("comment_field_data[75]"),
            QStringLiteral("comment_field_data[76]"),
            QStringLiteral("comment_field_data[77]"),
            QStringLiteral("comment_field_data[78]"),
            QStringLiteral("comment_field_data[79]"),
            QStringLiteral("comment_field_data[80]"),
            QStringLiteral("comment_field_data[81]"),
            QStringLiteral("comment_field_data[82]"),
            QStringLiteral("comment_field_data[83]"),
            QStringLiteral("comment_field_data[84]"),
            QStringLiteral("comment_field_data[85]"),
            QStringLiteral("comment_field_data[86]"),
            QStringLiteral("comment_field_data[87]"),
            QStringLiteral("comment_field_data[88]"),
            QStringLiteral("comment_field_data[89]"),
        };
        QCOMPARE(node->children().size(), expectedNames.size());
        for (std::size_t i = 0; i < expectedNames.size(); ++i) {
            const auto child = tree->node(node->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames[i]);
            if (child->name() == QStringLiteral("core_coder_delay")) {
                QCOMPARE(child->value().toULongLong(), quint64(0x1ABC));
            }
            if (child->name() == QStringLiteral("comment_field_bytes")) {
                QVERIFY(child->location().has_value());
                QCOMPARE(child->location()->logicalRange().start().bitOffset() / 8, quint64(8));
                QCOMPARE(child->value().toULongLong(), quint64(90));
            }
        }
    }

    void decodesAscCase3ExtendedAudioObjectType() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<std::byte> raw = {std::byte{0xF8}, std::byte{0x46}, std::byte{0x00}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x5A}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}, std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B}, std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}, std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B}, std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}, std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}, std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}, std::byte{0x4D}, std::byte{0x4E}, std::byte{0x4F}, std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57}, std::byte{0x58}, std::byte{0x59}};
        const MemorySource source(raw);
        auto tree = streamview::core::AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const auto mapping = mappingForBytes(source.sizeBytes());
        QVERIFY(mapping.has_value());
        streamview::core::BitReader reader(source, *mapping);

        const auto result = streamview::rules::DslExecutor::decodeStruct(
            *compiled.program,
            QStringLiteral("AudioSpecificConfig"),
            reader,
            *mapping,
            0,
            *tree,
            tree->rootId());
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::Materialized);
        QVERIFY(result.structureNode.has_value());

        const auto node = tree->node(*result.structureNode);
        QVERIFY(node.has_value());
        QCOMPARE(node->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node->diagnostics().empty());

        const std::vector<QString> expectedNames = {
            QStringLiteral("audio_object_type"),
            QStringLiteral("audio_object_type_ext"),
            QStringLiteral("sampling_frequency_index"),
            QStringLiteral("channel_configuration"),
            QStringLiteral("frame_length_flag"),
            QStringLiteral("depends_on_core_coder"),
            QStringLiteral("extension_flag"),
            QStringLiteral("element_instance_tag"),
            QStringLiteral("object_type"),
            QStringLiteral("pce_sampling_frequency_index"),
            QStringLiteral("num_front_channel_elements"),
            QStringLiteral("num_side_channel_elements"),
            QStringLiteral("num_back_channel_elements"),
            QStringLiteral("num_lfe_channel_elements"),
            QStringLiteral("num_assoc_data_elements"),
            QStringLiteral("num_valid_cc_elements"),
            QStringLiteral("mono_mixdown_present"),
            QStringLiteral("stereo_mixdown_present"),
            QStringLiteral("matrix_mixdown_idx_present"),
            QStringLiteral("pce_total_bits"),
            QStringLiteral("pce_rem"),
            QStringLiteral("pce_alignment_bits"),
            QStringLiteral("comment_field_bytes"),
            QStringLiteral("comment_field_data[0]"),
            QStringLiteral("comment_field_data[1]"),
            QStringLiteral("comment_field_data[2]"),
            QStringLiteral("comment_field_data[3]"),
            QStringLiteral("comment_field_data[4]"),
            QStringLiteral("comment_field_data[5]"),
            QStringLiteral("comment_field_data[6]"),
            QStringLiteral("comment_field_data[7]"),
            QStringLiteral("comment_field_data[8]"),
            QStringLiteral("comment_field_data[9]"),
            QStringLiteral("comment_field_data[10]"),
            QStringLiteral("comment_field_data[11]"),
            QStringLiteral("comment_field_data[12]"),
            QStringLiteral("comment_field_data[13]"),
            QStringLiteral("comment_field_data[14]"),
            QStringLiteral("comment_field_data[15]"),
            QStringLiteral("comment_field_data[16]"),
            QStringLiteral("comment_field_data[17]"),
            QStringLiteral("comment_field_data[18]"),
            QStringLiteral("comment_field_data[19]"),
            QStringLiteral("comment_field_data[20]"),
            QStringLiteral("comment_field_data[21]"),
            QStringLiteral("comment_field_data[22]"),
            QStringLiteral("comment_field_data[23]"),
            QStringLiteral("comment_field_data[24]"),
            QStringLiteral("comment_field_data[25]"),
            QStringLiteral("comment_field_data[26]"),
            QStringLiteral("comment_field_data[27]"),
            QStringLiteral("comment_field_data[28]"),
            QStringLiteral("comment_field_data[29]"),
            QStringLiteral("comment_field_data[30]"),
            QStringLiteral("comment_field_data[31]"),
            QStringLiteral("comment_field_data[32]"),
            QStringLiteral("comment_field_data[33]"),
            QStringLiteral("comment_field_data[34]"),
            QStringLiteral("comment_field_data[35]"),
            QStringLiteral("comment_field_data[36]"),
            QStringLiteral("comment_field_data[37]"),
            QStringLiteral("comment_field_data[38]"),
            QStringLiteral("comment_field_data[39]"),
            QStringLiteral("comment_field_data[40]"),
            QStringLiteral("comment_field_data[41]"),
            QStringLiteral("comment_field_data[42]"),
            QStringLiteral("comment_field_data[43]"),
            QStringLiteral("comment_field_data[44]"),
            QStringLiteral("comment_field_data[45]"),
            QStringLiteral("comment_field_data[46]"),
            QStringLiteral("comment_field_data[47]"),
            QStringLiteral("comment_field_data[48]"),
            QStringLiteral("comment_field_data[49]"),
            QStringLiteral("comment_field_data[50]"),
            QStringLiteral("comment_field_data[51]"),
            QStringLiteral("comment_field_data[52]"),
            QStringLiteral("comment_field_data[53]"),
            QStringLiteral("comment_field_data[54]"),
            QStringLiteral("comment_field_data[55]"),
            QStringLiteral("comment_field_data[56]"),
            QStringLiteral("comment_field_data[57]"),
            QStringLiteral("comment_field_data[58]"),
            QStringLiteral("comment_field_data[59]"),
            QStringLiteral("comment_field_data[60]"),
            QStringLiteral("comment_field_data[61]"),
            QStringLiteral("comment_field_data[62]"),
            QStringLiteral("comment_field_data[63]"),
            QStringLiteral("comment_field_data[64]"),
            QStringLiteral("comment_field_data[65]"),
            QStringLiteral("comment_field_data[66]"),
            QStringLiteral("comment_field_data[67]"),
            QStringLiteral("comment_field_data[68]"),
            QStringLiteral("comment_field_data[69]"),
            QStringLiteral("comment_field_data[70]"),
            QStringLiteral("comment_field_data[71]"),
            QStringLiteral("comment_field_data[72]"),
            QStringLiteral("comment_field_data[73]"),
            QStringLiteral("comment_field_data[74]"),
            QStringLiteral("comment_field_data[75]"),
            QStringLiteral("comment_field_data[76]"),
            QStringLiteral("comment_field_data[77]"),
            QStringLiteral("comment_field_data[78]"),
            QStringLiteral("comment_field_data[79]"),
            QStringLiteral("comment_field_data[80]"),
            QStringLiteral("comment_field_data[81]"),
            QStringLiteral("comment_field_data[82]"),
            QStringLiteral("comment_field_data[83]"),
            QStringLiteral("comment_field_data[84]"),
            QStringLiteral("comment_field_data[85]"),
            QStringLiteral("comment_field_data[86]"),
            QStringLiteral("comment_field_data[87]"),
            QStringLiteral("comment_field_data[88]"),
            QStringLiteral("comment_field_data[89]"),
        };
        QCOMPARE(node->children().size(), expectedNames.size());
        for (std::size_t i = 0; i < expectedNames.size(); ++i) {
            const auto child = tree->node(node->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames[i]);
            if (child->name() == QStringLiteral("audio_object_type_ext")) {
                QCOMPARE(child->value().toULongLong(), quint64(2));
            }
            if (child->name() == QStringLiteral("comment_field_bytes")) {
                QVERIFY(child->location().has_value());
                QCOMPARE(child->location()->logicalRange().start().bitOffset() / 8, quint64(7));
                QCOMPARE(child->value().toULongLong(), quint64(90));
            }
        }
    }

    void decodesAscCase4ExplicitSamplingFrequency() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<std::byte> raw = {std::byte{0x17}, std::byte{0x80}, std::byte{0x56}, std::byte{0x22}, std::byte{0x00}, std::byte{0x00}, std::byte{0xC0}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x5A}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}, std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B}, std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}, std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B}, std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}, std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}, std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}, std::byte{0x4D}, std::byte{0x4E}, std::byte{0x4F}, std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57}, std::byte{0x58}, std::byte{0x59}};
        const MemorySource source(raw);
        auto tree = streamview::core::AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const auto mapping = mappingForBytes(source.sizeBytes());
        QVERIFY(mapping.has_value());
        streamview::core::BitReader reader(source, *mapping);

        const auto result = streamview::rules::DslExecutor::decodeStruct(
            *compiled.program,
            QStringLiteral("AudioSpecificConfig"),
            reader,
            *mapping,
            0,
            *tree,
            tree->rootId());
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::Materialized);
        QVERIFY(result.structureNode.has_value());

        const auto node = tree->node(*result.structureNode);
        QVERIFY(node.has_value());
        QCOMPARE(node->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node->diagnostics().empty());

        const std::vector<QString> expectedNames = {
            QStringLiteral("audio_object_type"),
            QStringLiteral("sampling_frequency_index"),
            QStringLiteral("sampling_frequency"),
            QStringLiteral("channel_configuration"),
            QStringLiteral("frame_length_flag"),
            QStringLiteral("depends_on_core_coder"),
            QStringLiteral("extension_flag"),
            QStringLiteral("element_instance_tag"),
            QStringLiteral("object_type"),
            QStringLiteral("pce_sampling_frequency_index"),
            QStringLiteral("num_front_channel_elements"),
            QStringLiteral("num_side_channel_elements"),
            QStringLiteral("num_back_channel_elements"),
            QStringLiteral("num_lfe_channel_elements"),
            QStringLiteral("num_assoc_data_elements"),
            QStringLiteral("num_valid_cc_elements"),
            QStringLiteral("mono_mixdown_present"),
            QStringLiteral("stereo_mixdown_present"),
            QStringLiteral("matrix_mixdown_idx_present"),
            QStringLiteral("pce_total_bits"),
            QStringLiteral("pce_rem"),
            QStringLiteral("pce_alignment_bits"),
            QStringLiteral("byte_alignment_zero_bit[0]"),
            QStringLiteral("byte_alignment_zero_bit[1]"),
            QStringLiteral("byte_alignment_zero_bit[2]"),
            QStringLiteral("byte_alignment_zero_bit[3]"),
            QStringLiteral("byte_alignment_zero_bit[4]"),
            QStringLiteral("byte_alignment_zero_bit[5]"),
            QStringLiteral("comment_field_bytes"),
            QStringLiteral("comment_field_data[0]"),
            QStringLiteral("comment_field_data[1]"),
            QStringLiteral("comment_field_data[2]"),
            QStringLiteral("comment_field_data[3]"),
            QStringLiteral("comment_field_data[4]"),
            QStringLiteral("comment_field_data[5]"),
            QStringLiteral("comment_field_data[6]"),
            QStringLiteral("comment_field_data[7]"),
            QStringLiteral("comment_field_data[8]"),
            QStringLiteral("comment_field_data[9]"),
            QStringLiteral("comment_field_data[10]"),
            QStringLiteral("comment_field_data[11]"),
            QStringLiteral("comment_field_data[12]"),
            QStringLiteral("comment_field_data[13]"),
            QStringLiteral("comment_field_data[14]"),
            QStringLiteral("comment_field_data[15]"),
            QStringLiteral("comment_field_data[16]"),
            QStringLiteral("comment_field_data[17]"),
            QStringLiteral("comment_field_data[18]"),
            QStringLiteral("comment_field_data[19]"),
            QStringLiteral("comment_field_data[20]"),
            QStringLiteral("comment_field_data[21]"),
            QStringLiteral("comment_field_data[22]"),
            QStringLiteral("comment_field_data[23]"),
            QStringLiteral("comment_field_data[24]"),
            QStringLiteral("comment_field_data[25]"),
            QStringLiteral("comment_field_data[26]"),
            QStringLiteral("comment_field_data[27]"),
            QStringLiteral("comment_field_data[28]"),
            QStringLiteral("comment_field_data[29]"),
            QStringLiteral("comment_field_data[30]"),
            QStringLiteral("comment_field_data[31]"),
            QStringLiteral("comment_field_data[32]"),
            QStringLiteral("comment_field_data[33]"),
            QStringLiteral("comment_field_data[34]"),
            QStringLiteral("comment_field_data[35]"),
            QStringLiteral("comment_field_data[36]"),
            QStringLiteral("comment_field_data[37]"),
            QStringLiteral("comment_field_data[38]"),
            QStringLiteral("comment_field_data[39]"),
            QStringLiteral("comment_field_data[40]"),
            QStringLiteral("comment_field_data[41]"),
            QStringLiteral("comment_field_data[42]"),
            QStringLiteral("comment_field_data[43]"),
            QStringLiteral("comment_field_data[44]"),
            QStringLiteral("comment_field_data[45]"),
            QStringLiteral("comment_field_data[46]"),
            QStringLiteral("comment_field_data[47]"),
            QStringLiteral("comment_field_data[48]"),
            QStringLiteral("comment_field_data[49]"),
            QStringLiteral("comment_field_data[50]"),
            QStringLiteral("comment_field_data[51]"),
            QStringLiteral("comment_field_data[52]"),
            QStringLiteral("comment_field_data[53]"),
            QStringLiteral("comment_field_data[54]"),
            QStringLiteral("comment_field_data[55]"),
            QStringLiteral("comment_field_data[56]"),
            QStringLiteral("comment_field_data[57]"),
            QStringLiteral("comment_field_data[58]"),
            QStringLiteral("comment_field_data[59]"),
            QStringLiteral("comment_field_data[60]"),
            QStringLiteral("comment_field_data[61]"),
            QStringLiteral("comment_field_data[62]"),
            QStringLiteral("comment_field_data[63]"),
            QStringLiteral("comment_field_data[64]"),
            QStringLiteral("comment_field_data[65]"),
            QStringLiteral("comment_field_data[66]"),
            QStringLiteral("comment_field_data[67]"),
            QStringLiteral("comment_field_data[68]"),
            QStringLiteral("comment_field_data[69]"),
            QStringLiteral("comment_field_data[70]"),
            QStringLiteral("comment_field_data[71]"),
            QStringLiteral("comment_field_data[72]"),
            QStringLiteral("comment_field_data[73]"),
            QStringLiteral("comment_field_data[74]"),
            QStringLiteral("comment_field_data[75]"),
            QStringLiteral("comment_field_data[76]"),
            QStringLiteral("comment_field_data[77]"),
            QStringLiteral("comment_field_data[78]"),
            QStringLiteral("comment_field_data[79]"),
            QStringLiteral("comment_field_data[80]"),
            QStringLiteral("comment_field_data[81]"),
            QStringLiteral("comment_field_data[82]"),
            QStringLiteral("comment_field_data[83]"),
            QStringLiteral("comment_field_data[84]"),
            QStringLiteral("comment_field_data[85]"),
            QStringLiteral("comment_field_data[86]"),
            QStringLiteral("comment_field_data[87]"),
            QStringLiteral("comment_field_data[88]"),
            QStringLiteral("comment_field_data[89]"),
        };
        QCOMPARE(node->children().size(), expectedNames.size());
        for (std::size_t i = 0; i < expectedNames.size(); ++i) {
            const auto child = tree->node(node->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames[i]);
            if (child->name() == QStringLiteral("sampling_frequency")) {
                QCOMPARE(child->value().toULongLong(), quint64(44100));
            }
            if (child->name() == QStringLiteral("comment_field_bytes")) {
                QVERIFY(child->location().has_value());
                QCOMPARE(child->location()->logicalRange().start().bitOffset() / 8, quint64(10));
                QCOMPARE(child->value().toULongLong(), quint64(90));
            }
        }
    }

    void decodesAscCase5MultichannelFrontAndLfe() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<std::byte> raw = {std::byte{0x11}, std::byte{0x80}, std::byte{0x00}, std::byte{0xC8}, std::byte{0x01}, std::byte{0x00}, std::byte{0x20}, std::byte{0x10}, std::byte{0x5A}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}, std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B}, std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}, std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B}, std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}, std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}, std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}, std::byte{0x4D}, std::byte{0x4E}, std::byte{0x4F}, std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57}, std::byte{0x58}, std::byte{0x59}};
        const MemorySource source(raw);
        auto tree = streamview::core::AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const auto mapping = mappingForBytes(source.sizeBytes());
        QVERIFY(mapping.has_value());
        streamview::core::BitReader reader(source, *mapping);

        const auto result = streamview::rules::DslExecutor::decodeStruct(
            *compiled.program,
            QStringLiteral("AudioSpecificConfig"),
            reader,
            *mapping,
            0,
            *tree,
            tree->rootId());
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::Materialized);
        QVERIFY(result.structureNode.has_value());

        const auto node = tree->node(*result.structureNode);
        QVERIFY(node.has_value());
        QCOMPARE(node->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node->diagnostics().empty());

        const std::vector<QString> expectedNames = {
            QStringLiteral("audio_object_type"),
            QStringLiteral("sampling_frequency_index"),
            QStringLiteral("channel_configuration"),
            QStringLiteral("frame_length_flag"),
            QStringLiteral("depends_on_core_coder"),
            QStringLiteral("extension_flag"),
            QStringLiteral("element_instance_tag"),
            QStringLiteral("object_type"),
            QStringLiteral("pce_sampling_frequency_index"),
            QStringLiteral("num_front_channel_elements"),
            QStringLiteral("num_side_channel_elements"),
            QStringLiteral("num_back_channel_elements"),
            QStringLiteral("num_lfe_channel_elements"),
            QStringLiteral("num_assoc_data_elements"),
            QStringLiteral("num_valid_cc_elements"),
            QStringLiteral("mono_mixdown_present"),
            QStringLiteral("stereo_mixdown_present"),
            QStringLiteral("matrix_mixdown_idx_present"),
            QStringLiteral("front_element_is_cpe[0]"),
            QStringLiteral("front_element_tag_select[0]"),
            QStringLiteral("front_element_is_cpe[1]"),
            QStringLiteral("front_element_tag_select[1]"),
            QStringLiteral("lfe_element_tag_select[0]"),
            QStringLiteral("pce_total_bits"),
            QStringLiteral("pce_rem"),
            QStringLiteral("pce_alignment_bits"),
            QStringLiteral("comment_field_bytes"),
            QStringLiteral("comment_field_data[0]"),
            QStringLiteral("comment_field_data[1]"),
            QStringLiteral("comment_field_data[2]"),
            QStringLiteral("comment_field_data[3]"),
            QStringLiteral("comment_field_data[4]"),
            QStringLiteral("comment_field_data[5]"),
            QStringLiteral("comment_field_data[6]"),
            QStringLiteral("comment_field_data[7]"),
            QStringLiteral("comment_field_data[8]"),
            QStringLiteral("comment_field_data[9]"),
            QStringLiteral("comment_field_data[10]"),
            QStringLiteral("comment_field_data[11]"),
            QStringLiteral("comment_field_data[12]"),
            QStringLiteral("comment_field_data[13]"),
            QStringLiteral("comment_field_data[14]"),
            QStringLiteral("comment_field_data[15]"),
            QStringLiteral("comment_field_data[16]"),
            QStringLiteral("comment_field_data[17]"),
            QStringLiteral("comment_field_data[18]"),
            QStringLiteral("comment_field_data[19]"),
            QStringLiteral("comment_field_data[20]"),
            QStringLiteral("comment_field_data[21]"),
            QStringLiteral("comment_field_data[22]"),
            QStringLiteral("comment_field_data[23]"),
            QStringLiteral("comment_field_data[24]"),
            QStringLiteral("comment_field_data[25]"),
            QStringLiteral("comment_field_data[26]"),
            QStringLiteral("comment_field_data[27]"),
            QStringLiteral("comment_field_data[28]"),
            QStringLiteral("comment_field_data[29]"),
            QStringLiteral("comment_field_data[30]"),
            QStringLiteral("comment_field_data[31]"),
            QStringLiteral("comment_field_data[32]"),
            QStringLiteral("comment_field_data[33]"),
            QStringLiteral("comment_field_data[34]"),
            QStringLiteral("comment_field_data[35]"),
            QStringLiteral("comment_field_data[36]"),
            QStringLiteral("comment_field_data[37]"),
            QStringLiteral("comment_field_data[38]"),
            QStringLiteral("comment_field_data[39]"),
            QStringLiteral("comment_field_data[40]"),
            QStringLiteral("comment_field_data[41]"),
            QStringLiteral("comment_field_data[42]"),
            QStringLiteral("comment_field_data[43]"),
            QStringLiteral("comment_field_data[44]"),
            QStringLiteral("comment_field_data[45]"),
            QStringLiteral("comment_field_data[46]"),
            QStringLiteral("comment_field_data[47]"),
            QStringLiteral("comment_field_data[48]"),
            QStringLiteral("comment_field_data[49]"),
            QStringLiteral("comment_field_data[50]"),
            QStringLiteral("comment_field_data[51]"),
            QStringLiteral("comment_field_data[52]"),
            QStringLiteral("comment_field_data[53]"),
            QStringLiteral("comment_field_data[54]"),
            QStringLiteral("comment_field_data[55]"),
            QStringLiteral("comment_field_data[56]"),
            QStringLiteral("comment_field_data[57]"),
            QStringLiteral("comment_field_data[58]"),
            QStringLiteral("comment_field_data[59]"),
            QStringLiteral("comment_field_data[60]"),
            QStringLiteral("comment_field_data[61]"),
            QStringLiteral("comment_field_data[62]"),
            QStringLiteral("comment_field_data[63]"),
            QStringLiteral("comment_field_data[64]"),
            QStringLiteral("comment_field_data[65]"),
            QStringLiteral("comment_field_data[66]"),
            QStringLiteral("comment_field_data[67]"),
            QStringLiteral("comment_field_data[68]"),
            QStringLiteral("comment_field_data[69]"),
            QStringLiteral("comment_field_data[70]"),
            QStringLiteral("comment_field_data[71]"),
            QStringLiteral("comment_field_data[72]"),
            QStringLiteral("comment_field_data[73]"),
            QStringLiteral("comment_field_data[74]"),
            QStringLiteral("comment_field_data[75]"),
            QStringLiteral("comment_field_data[76]"),
            QStringLiteral("comment_field_data[77]"),
            QStringLiteral("comment_field_data[78]"),
            QStringLiteral("comment_field_data[79]"),
            QStringLiteral("comment_field_data[80]"),
            QStringLiteral("comment_field_data[81]"),
            QStringLiteral("comment_field_data[82]"),
            QStringLiteral("comment_field_data[83]"),
            QStringLiteral("comment_field_data[84]"),
            QStringLiteral("comment_field_data[85]"),
            QStringLiteral("comment_field_data[86]"),
            QStringLiteral("comment_field_data[87]"),
            QStringLiteral("comment_field_data[88]"),
            QStringLiteral("comment_field_data[89]"),
        };
        QCOMPARE(node->children().size(), expectedNames.size());
        for (std::size_t i = 0; i < expectedNames.size(); ++i) {
            const auto child = tree->node(node->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames[i]);
            if (child->name() == QStringLiteral("comment_field_bytes")) {
                QVERIFY(child->location().has_value());
                QCOMPARE(child->location()->logicalRange().start().bitOffset() / 8, quint64(8));
                QCOMPARE(child->value().toULongLong(), quint64(90));
            }
        }
    }

    void decodesAscCase6AllMixdownPresent() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<std::byte> raw = {std::byte{0x11}, std::byte{0x80}, std::byte{0x00}, std::byte{0xC0}, std::byte{0x00}, std::byte{0x01}, std::byte{0x19}, std::byte{0x58}, std::byte{0x5A}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}, std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B}, std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}, std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B}, std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}, std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}, std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}, std::byte{0x4D}, std::byte{0x4E}, std::byte{0x4F}, std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57}, std::byte{0x58}, std::byte{0x59}};
        const MemorySource source(raw);
        auto tree = streamview::core::AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const auto mapping = mappingForBytes(source.sizeBytes());
        QVERIFY(mapping.has_value());
        streamview::core::BitReader reader(source, *mapping);

        const auto result = streamview::rules::DslExecutor::decodeStruct(
            *compiled.program,
            QStringLiteral("AudioSpecificConfig"),
            reader,
            *mapping,
            0,
            *tree,
            tree->rootId());
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::Materialized);
        QVERIFY(result.structureNode.has_value());

        const auto node = tree->node(*result.structureNode);
        QVERIFY(node.has_value());
        QCOMPARE(node->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node->diagnostics().empty());

        const std::vector<QString> expectedNames = {
            QStringLiteral("audio_object_type"),
            QStringLiteral("sampling_frequency_index"),
            QStringLiteral("channel_configuration"),
            QStringLiteral("frame_length_flag"),
            QStringLiteral("depends_on_core_coder"),
            QStringLiteral("extension_flag"),
            QStringLiteral("element_instance_tag"),
            QStringLiteral("object_type"),
            QStringLiteral("pce_sampling_frequency_index"),
            QStringLiteral("num_front_channel_elements"),
            QStringLiteral("num_side_channel_elements"),
            QStringLiteral("num_back_channel_elements"),
            QStringLiteral("num_lfe_channel_elements"),
            QStringLiteral("num_assoc_data_elements"),
            QStringLiteral("num_valid_cc_elements"),
            QStringLiteral("mono_mixdown_present"),
            QStringLiteral("mono_mixdown_element_number"),
            QStringLiteral("stereo_mixdown_present"),
            QStringLiteral("stereo_mixdown_element_number"),
            QStringLiteral("matrix_mixdown_idx_present"),
            QStringLiteral("matrix_mixdown_idx"),
            QStringLiteral("pseudo_surround_enable"),
            QStringLiteral("pce_total_bits"),
            QStringLiteral("pce_rem"),
            QStringLiteral("pce_alignment_bits"),
            QStringLiteral("byte_alignment_zero_bit[0]"),
            QStringLiteral("byte_alignment_zero_bit[1]"),
            QStringLiteral("byte_alignment_zero_bit[2]"),
            QStringLiteral("comment_field_bytes"),
            QStringLiteral("comment_field_data[0]"),
            QStringLiteral("comment_field_data[1]"),
            QStringLiteral("comment_field_data[2]"),
            QStringLiteral("comment_field_data[3]"),
            QStringLiteral("comment_field_data[4]"),
            QStringLiteral("comment_field_data[5]"),
            QStringLiteral("comment_field_data[6]"),
            QStringLiteral("comment_field_data[7]"),
            QStringLiteral("comment_field_data[8]"),
            QStringLiteral("comment_field_data[9]"),
            QStringLiteral("comment_field_data[10]"),
            QStringLiteral("comment_field_data[11]"),
            QStringLiteral("comment_field_data[12]"),
            QStringLiteral("comment_field_data[13]"),
            QStringLiteral("comment_field_data[14]"),
            QStringLiteral("comment_field_data[15]"),
            QStringLiteral("comment_field_data[16]"),
            QStringLiteral("comment_field_data[17]"),
            QStringLiteral("comment_field_data[18]"),
            QStringLiteral("comment_field_data[19]"),
            QStringLiteral("comment_field_data[20]"),
            QStringLiteral("comment_field_data[21]"),
            QStringLiteral("comment_field_data[22]"),
            QStringLiteral("comment_field_data[23]"),
            QStringLiteral("comment_field_data[24]"),
            QStringLiteral("comment_field_data[25]"),
            QStringLiteral("comment_field_data[26]"),
            QStringLiteral("comment_field_data[27]"),
            QStringLiteral("comment_field_data[28]"),
            QStringLiteral("comment_field_data[29]"),
            QStringLiteral("comment_field_data[30]"),
            QStringLiteral("comment_field_data[31]"),
            QStringLiteral("comment_field_data[32]"),
            QStringLiteral("comment_field_data[33]"),
            QStringLiteral("comment_field_data[34]"),
            QStringLiteral("comment_field_data[35]"),
            QStringLiteral("comment_field_data[36]"),
            QStringLiteral("comment_field_data[37]"),
            QStringLiteral("comment_field_data[38]"),
            QStringLiteral("comment_field_data[39]"),
            QStringLiteral("comment_field_data[40]"),
            QStringLiteral("comment_field_data[41]"),
            QStringLiteral("comment_field_data[42]"),
            QStringLiteral("comment_field_data[43]"),
            QStringLiteral("comment_field_data[44]"),
            QStringLiteral("comment_field_data[45]"),
            QStringLiteral("comment_field_data[46]"),
            QStringLiteral("comment_field_data[47]"),
            QStringLiteral("comment_field_data[48]"),
            QStringLiteral("comment_field_data[49]"),
            QStringLiteral("comment_field_data[50]"),
            QStringLiteral("comment_field_data[51]"),
            QStringLiteral("comment_field_data[52]"),
            QStringLiteral("comment_field_data[53]"),
            QStringLiteral("comment_field_data[54]"),
            QStringLiteral("comment_field_data[55]"),
            QStringLiteral("comment_field_data[56]"),
            QStringLiteral("comment_field_data[57]"),
            QStringLiteral("comment_field_data[58]"),
            QStringLiteral("comment_field_data[59]"),
            QStringLiteral("comment_field_data[60]"),
            QStringLiteral("comment_field_data[61]"),
            QStringLiteral("comment_field_data[62]"),
            QStringLiteral("comment_field_data[63]"),
            QStringLiteral("comment_field_data[64]"),
            QStringLiteral("comment_field_data[65]"),
            QStringLiteral("comment_field_data[66]"),
            QStringLiteral("comment_field_data[67]"),
            QStringLiteral("comment_field_data[68]"),
            QStringLiteral("comment_field_data[69]"),
            QStringLiteral("comment_field_data[70]"),
            QStringLiteral("comment_field_data[71]"),
            QStringLiteral("comment_field_data[72]"),
            QStringLiteral("comment_field_data[73]"),
            QStringLiteral("comment_field_data[74]"),
            QStringLiteral("comment_field_data[75]"),
            QStringLiteral("comment_field_data[76]"),
            QStringLiteral("comment_field_data[77]"),
            QStringLiteral("comment_field_data[78]"),
            QStringLiteral("comment_field_data[79]"),
            QStringLiteral("comment_field_data[80]"),
            QStringLiteral("comment_field_data[81]"),
            QStringLiteral("comment_field_data[82]"),
            QStringLiteral("comment_field_data[83]"),
            QStringLiteral("comment_field_data[84]"),
            QStringLiteral("comment_field_data[85]"),
            QStringLiteral("comment_field_data[86]"),
            QStringLiteral("comment_field_data[87]"),
            QStringLiteral("comment_field_data[88]"),
            QStringLiteral("comment_field_data[89]"),
        };
        QCOMPARE(node->children().size(), expectedNames.size());
        for (std::size_t i = 0; i < expectedNames.size(); ++i) {
            const auto child = tree->node(node->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames[i]);
            if (child->name() == QStringLiteral("comment_field_bytes")) {
                QVERIFY(child->location().has_value());
                QCOMPARE(child->location()->logicalRange().start().bitOffset() / 8, quint64(8));
                QCOMPARE(child->value().toULongLong(), quint64(90));
            }
        }
    }

    void decodesAscCase7MultichannelSideBackAssocCc() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<std::byte> raw = {std::byte{0x11}, std::byte{0x80}, std::byte{0x00}, std::byte{0xC0}, std::byte{0x44}, std::byte{0x22}, std::byte{0x21}, std::byte{0x00}, std::byte{0x00}, std::byte{0x5A}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}, std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B}, std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}, std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B}, std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}, std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}, std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}, std::byte{0x4D}, std::byte{0x4E}, std::byte{0x4F}, std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57}, std::byte{0x58}, std::byte{0x59}};
        const MemorySource source(raw);
        auto tree = streamview::core::AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const auto mapping = mappingForBytes(source.sizeBytes());
        QVERIFY(mapping.has_value());
        streamview::core::BitReader reader(source, *mapping);

        const auto result = streamview::rules::DslExecutor::decodeStruct(
            *compiled.program,
            QStringLiteral("AudioSpecificConfig"),
            reader,
            *mapping,
            0,
            *tree,
            tree->rootId());
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::Materialized);
        QVERIFY(result.structureNode.has_value());

        const auto node = tree->node(*result.structureNode);
        QVERIFY(node.has_value());
        QCOMPARE(node->state(), streamview::core::MaterializationState::Materialized);
        QVERIFY(node->diagnostics().empty());

        const std::vector<QString> expectedNames = {
            QStringLiteral("audio_object_type"),
            QStringLiteral("sampling_frequency_index"),
            QStringLiteral("channel_configuration"),
            QStringLiteral("frame_length_flag"),
            QStringLiteral("depends_on_core_coder"),
            QStringLiteral("extension_flag"),
            QStringLiteral("element_instance_tag"),
            QStringLiteral("object_type"),
            QStringLiteral("pce_sampling_frequency_index"),
            QStringLiteral("num_front_channel_elements"),
            QStringLiteral("num_side_channel_elements"),
            QStringLiteral("num_back_channel_elements"),
            QStringLiteral("num_lfe_channel_elements"),
            QStringLiteral("num_assoc_data_elements"),
            QStringLiteral("num_valid_cc_elements"),
            QStringLiteral("mono_mixdown_present"),
            QStringLiteral("stereo_mixdown_present"),
            QStringLiteral("matrix_mixdown_idx_present"),
            QStringLiteral("side_element_is_cpe[0]"),
            QStringLiteral("side_element_tag_select[0]"),
            QStringLiteral("back_element_is_cpe[0]"),
            QStringLiteral("back_element_tag_select[0]"),
            QStringLiteral("assoc_data_element_tag_select[0]"),
            QStringLiteral("cc_element_is_ind_sw[0]"),
            QStringLiteral("valid_cc_element_tag_select[0]"),
            QStringLiteral("pce_total_bits"),
            QStringLiteral("pce_rem"),
            QStringLiteral("pce_alignment_bits"),
            QStringLiteral("byte_alignment_zero_bit[0]"),
            QStringLiteral("byte_alignment_zero_bit[1]"),
            QStringLiteral("byte_alignment_zero_bit[2]"),
            QStringLiteral("comment_field_bytes"),
            QStringLiteral("comment_field_data[0]"),
            QStringLiteral("comment_field_data[1]"),
            QStringLiteral("comment_field_data[2]"),
            QStringLiteral("comment_field_data[3]"),
            QStringLiteral("comment_field_data[4]"),
            QStringLiteral("comment_field_data[5]"),
            QStringLiteral("comment_field_data[6]"),
            QStringLiteral("comment_field_data[7]"),
            QStringLiteral("comment_field_data[8]"),
            QStringLiteral("comment_field_data[9]"),
            QStringLiteral("comment_field_data[10]"),
            QStringLiteral("comment_field_data[11]"),
            QStringLiteral("comment_field_data[12]"),
            QStringLiteral("comment_field_data[13]"),
            QStringLiteral("comment_field_data[14]"),
            QStringLiteral("comment_field_data[15]"),
            QStringLiteral("comment_field_data[16]"),
            QStringLiteral("comment_field_data[17]"),
            QStringLiteral("comment_field_data[18]"),
            QStringLiteral("comment_field_data[19]"),
            QStringLiteral("comment_field_data[20]"),
            QStringLiteral("comment_field_data[21]"),
            QStringLiteral("comment_field_data[22]"),
            QStringLiteral("comment_field_data[23]"),
            QStringLiteral("comment_field_data[24]"),
            QStringLiteral("comment_field_data[25]"),
            QStringLiteral("comment_field_data[26]"),
            QStringLiteral("comment_field_data[27]"),
            QStringLiteral("comment_field_data[28]"),
            QStringLiteral("comment_field_data[29]"),
            QStringLiteral("comment_field_data[30]"),
            QStringLiteral("comment_field_data[31]"),
            QStringLiteral("comment_field_data[32]"),
            QStringLiteral("comment_field_data[33]"),
            QStringLiteral("comment_field_data[34]"),
            QStringLiteral("comment_field_data[35]"),
            QStringLiteral("comment_field_data[36]"),
            QStringLiteral("comment_field_data[37]"),
            QStringLiteral("comment_field_data[38]"),
            QStringLiteral("comment_field_data[39]"),
            QStringLiteral("comment_field_data[40]"),
            QStringLiteral("comment_field_data[41]"),
            QStringLiteral("comment_field_data[42]"),
            QStringLiteral("comment_field_data[43]"),
            QStringLiteral("comment_field_data[44]"),
            QStringLiteral("comment_field_data[45]"),
            QStringLiteral("comment_field_data[46]"),
            QStringLiteral("comment_field_data[47]"),
            QStringLiteral("comment_field_data[48]"),
            QStringLiteral("comment_field_data[49]"),
            QStringLiteral("comment_field_data[50]"),
            QStringLiteral("comment_field_data[51]"),
            QStringLiteral("comment_field_data[52]"),
            QStringLiteral("comment_field_data[53]"),
            QStringLiteral("comment_field_data[54]"),
            QStringLiteral("comment_field_data[55]"),
            QStringLiteral("comment_field_data[56]"),
            QStringLiteral("comment_field_data[57]"),
            QStringLiteral("comment_field_data[58]"),
            QStringLiteral("comment_field_data[59]"),
            QStringLiteral("comment_field_data[60]"),
            QStringLiteral("comment_field_data[61]"),
            QStringLiteral("comment_field_data[62]"),
            QStringLiteral("comment_field_data[63]"),
            QStringLiteral("comment_field_data[64]"),
            QStringLiteral("comment_field_data[65]"),
            QStringLiteral("comment_field_data[66]"),
            QStringLiteral("comment_field_data[67]"),
            QStringLiteral("comment_field_data[68]"),
            QStringLiteral("comment_field_data[69]"),
            QStringLiteral("comment_field_data[70]"),
            QStringLiteral("comment_field_data[71]"),
            QStringLiteral("comment_field_data[72]"),
            QStringLiteral("comment_field_data[73]"),
            QStringLiteral("comment_field_data[74]"),
            QStringLiteral("comment_field_data[75]"),
            QStringLiteral("comment_field_data[76]"),
            QStringLiteral("comment_field_data[77]"),
            QStringLiteral("comment_field_data[78]"),
            QStringLiteral("comment_field_data[79]"),
            QStringLiteral("comment_field_data[80]"),
            QStringLiteral("comment_field_data[81]"),
            QStringLiteral("comment_field_data[82]"),
            QStringLiteral("comment_field_data[83]"),
            QStringLiteral("comment_field_data[84]"),
            QStringLiteral("comment_field_data[85]"),
            QStringLiteral("comment_field_data[86]"),
            QStringLiteral("comment_field_data[87]"),
            QStringLiteral("comment_field_data[88]"),
            QStringLiteral("comment_field_data[89]"),
        };
        QCOMPARE(node->children().size(), expectedNames.size());
        for (std::size_t i = 0; i < expectedNames.size(); ++i) {
            const auto child = tree->node(node->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames[i]);
            if (child->name() == QStringLiteral("comment_field_bytes")) {
                QVERIFY(child->location().has_value());
                QCOMPARE(child->location()->logicalRange().start().bitOffset() / 8, quint64(9));
                QCOMPARE(child->value().toULongLong(), quint64(90));
            }
        }
    }

    void rejectsAscCase8NonzeroAlignmentBit() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<std::byte> raw = {std::byte{0x11}, std::byte{0x80}, std::byte{0x00}, std::byte{0xC0}, std::byte{0x00}, std::byte{0x00}, std::byte{0x20}, std::byte{0x5A}, std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}, std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B}, std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}, std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B}, std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}, std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48}, std::byte{0x49}, std::byte{0x4A}, std::byte{0x4B}, std::byte{0x4C}, std::byte{0x4D}, std::byte{0x4E}, std::byte{0x4F}, std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57}, std::byte{0x58}, std::byte{0x59}};
        const MemorySource source(raw);
        auto tree = streamview::core::AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const auto mapping = mappingForBytes(source.sizeBytes());
        QVERIFY(mapping.has_value());
        streamview::core::BitReader reader(source, *mapping);

        const auto result = streamview::rules::DslExecutor::decodeStruct(
            *compiled.program,
            QStringLiteral("AudioSpecificConfig"),
            reader,
            *mapping,
            0,
            *tree,
            tree->rootId());
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::InvalidSyntax);
        QVERIFY(result.structureNode.has_value());

        const auto node = tree->node(*result.structureNode);
        QVERIFY(node.has_value());
        QCOMPARE(node->state(), streamview::core::MaterializationState::Invalid);
        QCOMPARE(node->diagnostics().size(), std::size_t(1));
        QCOMPARE(node->diagnostics()[0].code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(node->diagnostics()[0].message, QStringLiteral("Field value violates @equals constraint"));
    }

    void rejectsAscCase9PrematureTruncation() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        const auto* ascSource = loaded.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        QVERIFY(ascSource != nullptr);
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*ascSource));
        const auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<std::byte> raw = {std::byte{0x11}, std::byte{0x80}, std::byte{0x00}};
        const MemorySource source(raw);
        auto tree = streamview::core::AnalysisTree::create(QStringLiteral("Root"));
        QVERIFY(tree.has_value());
        const auto mapping = mappingForBytes(source.sizeBytes());
        QVERIFY(mapping.has_value());
        streamview::core::BitReader reader(source, *mapping);

        const auto result = streamview::rules::DslExecutor::decodeStruct(
            *compiled.program,
            QStringLiteral("AudioSpecificConfig"),
            reader,
            *mapping,
            0,
            *tree,
            tree->rootId());
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::TruncatedSource);
        QVERIFY(result.structureNode.has_value());

        const auto node = tree->node(*result.structureNode);
        QVERIFY(node.has_value());
        QCOMPARE(node->state(), streamview::core::MaterializationState::Invalid);
        QCOMPARE(node->diagnostics().size(), std::size_t(1));
        QCOMPARE(node->diagnostics()[0].code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(node->diagnostics()[0].message, QStringLiteral("Unable to read complete syntax field"));
    }

    void bundledAacAdtsRuleResolvesAdtsWithoutRegression() {
        const auto loaded = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(loaded.succeeded());
        QVERIFY(loaded.package.has_value());

        streamview::rules::RulePackageCatalog catalog;
        const auto reg = catalog.registerPackage(streamview::rules::RulePackage(*loaded.package));
        QVERIFY(reg.succeeded());

        const auto resolvedAdts = catalog.resolve(
            loaded.package->identity(),
            QStringLiteral("adts"),
            streamview::rules::languageVersion(),
            streamview::core::version());
        QVERIFY2(resolvedAdts.succeeded(), qPrintable(resolvedAdts.errorMessage));
        QVERIFY(resolvedAdts.entryPoint.has_value());
        QCOMPARE(resolvedAdts.entryPoint->id, QStringLiteral("adts"));
        QCOMPARE(resolvedAdts.entryPoint->format, QStringLiteral("audio.aac.adts"));
        QCOMPARE(resolvedAdts.entryPoint->depth, QStringLiteral("adts-frame"));
        QVERIFY(resolvedAdts.entryPoint->detector.has_value());
        QCOMPARE(*resolvedAdts.entryPoint->detector, QStringLiteral("aac-adts"));

        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(100, true);
        const auto f2 = makeAdtsFrame(100, true);
        const auto f3 = makeAdtsFrame(100, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));
        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, streamview::rules::AacAdtsAnalysisStatus::Complete);
        QCOMPARE(batch.frameNodes.size(), std::size_t(3));
        QVERIFY(analyzer->finished());
    }

};

QTEST_MAIN(AacAdtsAnalyzerTest)
#include "aac_adts_analyzer_test.moc"
