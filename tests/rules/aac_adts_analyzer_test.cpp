#include <streamview/rules/aac_adts_analyzer.h>

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
        QCOMPARE(header0->children().size(), std::size_t(16));

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
            QStringLiteral("minimum_frame_length")
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
        QCOMPARE(header1->children().size(), std::size_t(17));

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
            QStringLiteral("minimum_frame_length")
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
        QCOMPARE(node1->diagnostics().front().severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(node1->diagnostics().front().message, QStringLiteral("ADTS frame payload is truncated at EOF"));

        // Header structure node inside truncated frame is fully materialized
        QCOMPARE(node1->children().size(), std::size_t(1));
        const auto header1 = analyzer->tree().node(node1->children()[0]);
        QVERIFY(header1.has_value());
        QCOMPARE(header1->name(), QStringLiteral("AdtsHeader"));
        QCOMPARE(header1->state(), streamview::core::MaterializationState::Materialized);
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
};

QTEST_MAIN(AacAdtsAnalyzerTest)
#include "aac_adts_analyzer_test.moc"
