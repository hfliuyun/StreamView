#include <streamview/rules/dsl.h>
#include <streamview/rules/h264_annex_b_analyzer.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <vector>

using streamview::core::AnalysisNodeKind;
using streamview::core::CancellationSource;
using streamview::core::MaterializationState;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::rules::DslParser;
using streamview::rules::H264AnnexBAnalysisStatus;
using streamview::rules::H264AnnexBAnalyzer;
using streamview::rules::h264AnnexBRuleSource;

namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

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

class FailAfterFirstReadSource final : public RandomAccessSource {
public:
    [[nodiscard]] quint64 sizeBytes() const noexcept override { return 4; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("failing-memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (readCount_ != 0) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected read failure")};
        }
        ++readCount_;
        const auto data = bytes({0x00, 0x00, 0x01, 0x65});
        const auto offset = static_cast<std::size_t>(byteOffset);
        const std::size_t count = std::min(destination.size(), data.size() - offset);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(count),
                    destination.begin());
        return {SourceReadStatus::Complete, count, {}};
    }

private:
    mutable std::size_t readCount_ = 0;
};

class CancellingSource final : public RandomAccessSource {
public:
    explicit CancellingSource(CancellationSource& cancellation)
        : data_(2048, std::byte{0xFF}), cancellation_(&cancellation) {
        const auto first = bytes({0x00, 0x00, 0x01, 0x65});
        std::copy(first.begin(), first.end(), data_.begin());
        const auto second = bytes({0x00, 0x00, 0x01, 0x41});
        std::copy(second.begin(), second.end(), data_.begin() + 10);
    }

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("cancelling-memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (!cancellationRequested_) {
            cancellationRequested_ = true;
            (void)cancellation_->requestCancellation();
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
    CancellationSource* cancellation_ = nullptr;
    mutable bool cancellationRequested_ = false;
};

} // namespace

class H264AnnexBAnalyzerTest final : public QObject {
    Q_OBJECT

private slots:
    void loadsBundledRule() {
        QString errorMessage;
        const QString source = h264AnnexBRuleSource(&errorMessage);

        QVERIFY2(!source.isEmpty(), qPrintable(errorMessage));
        const auto parsed = DslParser::parse(source);
        QVERIFY(parsed.succeeded());
        QCOMPARE(parsed.program.structs.size(), std::size_t(1));
        QCOMPARE(parsed.program.scans.size(), std::size_t(1));
        QCOMPARE(parsed.program.entry.targetName, QStringLiteral("nal_units"));
    }

    void materializesStartCodesAndNalHeadersInBatches() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0xAA,
                                   0x00, 0x00, 0x00, 0x01, 0x41}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto firstBatch = analyzer->analyzeBatch(1);
        QCOMPARE(firstBatch.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(firstBatch.nalUnitNodes.size(), std::size_t(1));

        const auto firstNal = analyzer->tree().node(firstBatch.nalUnitNodes.front());
        QVERIFY(firstNal.has_value());
        QCOMPARE(firstNal->kind(), AnalysisNodeKind::Region);
        QCOMPARE(firstNal->state(), MaterializationState::Materialized);
        QCOMPARE(firstNal->children().size(), std::size_t(2));

        const auto firstStartCode = analyzer->tree().node(firstNal->children().at(0));
        QVERIFY(firstStartCode.has_value());
        QCOMPARE(firstStartCode->name(), QStringLiteral("start_code"));
        QVERIFY(firstStartCode->location().has_value());
        QCOMPARE(firstStartCode->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(0));
        QCOMPARE(firstStartCode->location()->sourceSpans().front().bitLength(), quint64(24));

        const auto firstHeader = analyzer->tree().node(firstNal->children().at(1));
        QVERIFY(firstHeader.has_value());
        QCOMPARE(firstHeader->name(), QStringLiteral("NalUnitHeader"));
        QCOMPARE(firstHeader->children().size(), std::size_t(3));
        const auto forbidden = analyzer->tree().node(firstHeader->children().at(0));
        const auto referenceIdc = analyzer->tree().node(firstHeader->children().at(1));
        const auto unitType = analyzer->tree().node(firstHeader->children().at(2));
        QVERIFY(forbidden.has_value());
        QVERIFY(referenceIdc.has_value());
        QVERIFY(unitType.has_value());
        QCOMPARE(forbidden->value().toULongLong(), quint64(0));
        QCOMPARE(referenceIdc->value().toULongLong(), quint64(3));
        QCOMPARE(unitType->value().toULongLong(), quint64(5));
        QCOMPARE(forbidden->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(24));
        QCOMPARE(referenceIdc->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(25));
        QCOMPARE(unitType->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(27));

        const auto secondBatch = analyzer->analyzeBatch(1);
        QCOMPARE(secondBatch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(secondBatch.nalUnitNodes.size(), std::size_t(1));
        const auto secondNal = analyzer->tree().node(secondBatch.nalUnitNodes.front());
        QVERIFY(secondNal.has_value());
        const auto secondStartCode = analyzer->tree().node(secondNal->children().at(0));
        QVERIFY(secondStartCode.has_value());
        QVERIFY(secondStartCode->location().has_value());
        QCOMPARE(secondStartCode->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(40));
        QCOMPARE(secondStartCode->location()->sourceSpans().front().bitLength(), quint64(32));
        const auto secondHeader = analyzer->tree().node(secondNal->children().at(1));
        QVERIFY(secondHeader.has_value());
        const auto secondReferenceIdc = analyzer->tree().node(secondHeader->children().at(1));
        const auto secondUnitType = analyzer->tree().node(secondHeader->children().at(2));
        QCOMPARE(secondReferenceIdc->value().toULongLong(), quint64(2));
        QCOMPARE(secondUnitType->value().toULongLong(), quint64(1));

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
    }

    void retainsTheInvalidForbiddenBitAsAPartialResult() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0xE5}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(analyzer->tree().hasPartialResults());

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->diagnostics().size(), std::size_t(1));
        QCOMPARE(nal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);

        const auto header = analyzer->tree().node(nal->children().at(1));
        QVERIFY(header.has_value());
        QCOMPARE(header->state(), MaterializationState::Invalid);
        QCOMPARE(header->children().size(), std::size_t(1));
        const auto forbidden = analyzer->tree().node(header->children().front());
        QVERIFY(forbidden.has_value());
        QCOMPARE(forbidden->value().toULongLong(), quint64(1));
        QCOMPARE(forbidden->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(24));

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
    }

    void publishesAnEmptyFinalNalUnitAsTruncated() {
        MemorySource source(bytes({0x00, 0x00, 0x01}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(analyzer->tree().hasPartialResults());

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->children().size(), std::size_t(2));
        QCOMPARE(nal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::TruncatedSource);
        QVERIFY(nal->diagnostics().front().location.has_value());
        QCOMPARE(nal->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(24));

        const auto startCode = analyzer->tree().node(nal->children().at(0));
        const auto header = analyzer->tree().node(nal->children().at(1));
        QVERIFY(startCode.has_value());
        QVERIFY(header.has_value());
        QCOMPARE(startCode->location()->sourceSpans().front().bitLength(), quint64(24));
        QCOMPARE(header->name(), QStringLiteral("NalUnitHeader"));
        QCOMPARE(header->state(), MaterializationState::Invalid);
        QVERIFY(header->children().empty());
        QCOMPARE(header->diagnostics().front().code,
                 streamview::core::DiagnosticCode::TruncatedSource);

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
    }

    void retainsPublishedNodesWhenHeaderReadingFails() {
        FailAfterFirstReadSource source;
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::SourceError);
        QCOMPARE(batch.errorMessage, QStringLiteral("injected read failure"));
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(analyzer->tree().hasPartialResults());

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::SourceError);
        QVERIFY(nal->diagnostics().front().location.has_value());
        QCOMPARE(nal->diagnostics()
                     .front()
                     .location->sourceSpans()
                     .front()
                     .start()
                     .absoluteBitOffset(),
                 quint64(24));
        QCOMPARE(nal->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(1));

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
        QCOMPARE(root->diagnostics().front().code,
                 streamview::core::DiagnosticCode::SourceError);
    }

    void cancellationKeepsTheLastPublishedBatch() {
        CancellationSource cancellation;
        CancellingSource source(cancellation);
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(analyzer->finished());

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Cancelled);
        QCOMPARE(root->diagnostics().front().code,
                 streamview::core::DiagnosticCode::Cancelled);
    }

    void reportsInputWithoutAStartCodeAsInvalid() {
        MemorySource source(bytes({0x12, 0x34, 0x56}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QVERIFY(batch.nalUnitNodes.empty());
        QVERIFY(analyzer->tree().hasPartialResults());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
        QCOMPARE(root->diagnostics().size(), std::size_t(1));
        QCOMPARE(root->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QVERIFY(root->diagnostics().front().message.contains(QStringLiteral("start code")));
    }
};

QTEST_GUILESS_MAIN(H264AnnexBAnalyzerTest)

#include "h264_annex_b_analyzer_test.moc"
