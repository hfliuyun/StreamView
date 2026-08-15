#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>

#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <vector>

using streamview::core::CancellationSource;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::rules::AacAdtsAnalysisBatch;
using streamview::rules::AacAdtsAnalysisStatus;
using streamview::rules::AacAdtsAnalyzer;

namespace {

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
        const std::size_t count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(count),
                    destination.begin());
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
};

[[nodiscard]] std::vector<std::byte> makeAdtsFrame(quint16 frameLength,
                                                    bool protectionAbsent = true,
                                                    quint8 profile = 1,
                                                    quint8 samplingFreqIndex = 4,
                                                    quint8 channelConfig = 2,
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
        ((frameLength & 0x07U) << 5U) | 0x1FU)};
    frame[6] = std::byte{0xFC};
    if (!protectionAbsent) {
        frame[7] = std::byte{0x12};
        frame[8] = std::byte{0x34};
    }
    return frame;
}

} // namespace

class AacAdtsAnalyzerTest : public QObject {
    Q_OBJECT

private slots:
    void createsAnalyzerAndRunsBatchAnalysis() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(150, true);
        const auto f2 = makeAdtsFrame(200, true);
        const auto f3 = makeAdtsFrame(180, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = AacAdtsAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));
        QCOMPARE(analyzer->ruleIdentity().packageIdentity().packageId(), QStringLiteral("org.streamview.aac"));
        QCOMPARE(analyzer->ruleIdentity().entryPointId(), QStringLiteral("adts-stream"));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.frameNodes.size(), std::size_t(3));
        QVERIFY(analyzer->finished());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->children().size(), std::size_t(3));

        const auto node1 = analyzer->tree().node(batch.frameNodes[0]);
        QVERIFY(node1.has_value());
        QCOMPARE(node1->name(), QStringLiteral("adts_frame"));
        QVERIFY(node1->location().has_value());
        QCOMPARE(node1->location()->logicalRange().bitLength(), quint64(150 * 8));
        QCOMPARE(node1->children().size(), std::size_t(2));

        const auto headerChild = analyzer->tree().node(node1->children()[0]);
        QVERIFY(headerChild.has_value());
        QCOMPARE(headerChild->name(), QStringLiteral("header"));
        QVERIFY(headerChild->location().has_value());
        QCOMPARE(headerChild->location()->logicalRange().bitLength(), quint64(7 * 8));

        const auto payloadChild = analyzer->tree().node(node1->children()[1]);
        QVERIFY(payloadChild.has_value());
        QCOMPARE(payloadChild->name(), QStringLiteral("raw_data_block"));
        QVERIFY(payloadChild->location().has_value());
        QCOMPARE(payloadChild->location()->logicalRange().bitLength(), quint64(143 * 8));
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
        auto analyzer = AacAdtsAnalyzer::create(source);
        QVERIFY(analyzer.has_value());

        const auto b1 = analyzer->analyzeBatch(1);
        QCOMPARE(b1.status, AacAdtsAnalysisStatus::InProgress);
        QCOMPARE(b1.frameNodes.size(), std::size_t(1));

        const auto b2 = analyzer->analyzeBatch(1);
        QCOMPARE(b2.status, AacAdtsAnalysisStatus::InProgress);
        QCOMPARE(b2.frameNodes.size(), std::size_t(1));

        const auto b3 = analyzer->analyzeBatch(1);
        QCOMPARE(b3.status, AacAdtsAnalysisStatus::Complete);
        QCOMPARE(b3.frameNodes.size(), std::size_t(1));
        QVERIFY(analyzer->finished());
    }

    void respectsCancellationAndResumes() {
        std::vector<std::byte> stream;
        for (int i = 0; i < 50; ++i) {
            const auto f = makeAdtsFrame(100, true);
            stream.insert(stream.end(), f.begin(), f.end());
        }

        const MemorySource source(std::move(stream));
        CancellationSource cancelSource;
        auto analyzer = AacAdtsAnalyzer::create(source, nullptr, cancelSource.token());
        QVERIFY(analyzer.has_value());

        (void)cancelSource.requestCancellation();
        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, AacAdtsAnalysisStatus::Cancelled);

        CancellationSource newCancelSource;
        QVERIFY(analyzer->resumeAfterCancellation(newCancelSource.token()));
        const auto resumeBatch = analyzer->analyzeBatch();
        QCOMPARE(resumeBatch.status, AacAdtsAnalysisStatus::Complete);
        QVERIFY(analyzer->finished());
    }

    void handlesInvalidBatchSize() {
        const MemorySource source(std::vector<std::byte>{std::byte{0x00}});
        auto analyzer = AacAdtsAnalyzer::create(source);
        QVERIFY(analyzer.has_value());

        const auto b1 = analyzer->analyzeBatch(0, 100);
        QCOMPARE(b1.status, AacAdtsAnalysisStatus::InvalidBatchSize);
        const auto b2 = analyzer->analyzeBatch(10, 0);
        QCOMPARE(b2.status, AacAdtsAnalysisStatus::InvalidBatchSize);
    }
};

QTEST_MAIN(AacAdtsAnalyzerTest)
#include "aac_adts_analyzer_test.moc"
