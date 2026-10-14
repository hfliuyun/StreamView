#include <streamview/rules/wav_riff_analyzer.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>

#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <vector>

using streamview::core::CancellationSource;
using streamview::core::MaterializationState;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::rules::WavRiffAnalysisBatch;
using streamview::rules::WavRiffAnalysisStatus;
using streamview::rules::WavRiffAnalyzer;

namespace {

constexpr quint32 kRiffMagic = 0x52494646U;   // 'RIFF'
constexpr quint32 kWaveForm = 0x57415645U;    // 'WAVE'
constexpr quint32 kFmtId = 0x666D7420U;       // 'fmt '
constexpr quint32 kDataId = 0x64617461U;      // 'data'
constexpr quint32 kFactId = 0x66616374U;      // 'fact'

class MemorySource final : public RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> data, bool failReads = false)
        : data_(std::move(data)), failReads_(failReads) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (failReads_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("Simulated I/O read failure")};
        }
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
                count, {}};
    }

private:
    std::vector<std::byte> data_;
    bool failReads_ = false;
};

void appendU16Le(std::vector<std::byte>& data, quint32 value) {
    data.push_back(static_cast<std::byte>(value & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

void appendU32Le(std::vector<std::byte>& data, quint32 value) {
    data.push_back(static_cast<std::byte>(value & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
}

void appendU32Be(std::vector<std::byte>& data, quint32 value) {
    data.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
    data.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::byte>(value & 0xFF));
}

void appendChunk(std::vector<std::byte>& data, quint32 id, const std::vector<std::byte>& payload) {
    appendU32Be(data, id);
    appendU32Le(data, static_cast<quint32>(payload.size()));
    data.insert(data.end(), payload.begin(), payload.end());
    if ((payload.size() & 1U) != 0U) {
        data.push_back(std::byte{0x7E});
    }
}

[[nodiscard]] std::vector<std::byte> pcmFmtChunk() {
    std::vector<std::byte> payload;
    appendU16Le(payload, 1);      // audio_format: PCM
    appendU16Le(payload, 1);      // num_channels
    appendU32Le(payload, 8000);   // sample_rate
    appendU32Le(payload, 8000);   // byte_rate
    appendU16Le(payload, 1);      // block_align
    appendU16Le(payload, 8);      // bits_per_sample
    return payload;
}

[[nodiscard]] std::vector<std::byte> minimalPcmWave() {
    std::vector<std::byte> chunks;
    appendChunk(chunks, kFmtId, pcmFmtChunk());
    appendChunk(chunks, kDataId, {std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}});

    std::vector<std::byte> data;
    appendU32Be(data, kRiffMagic);
    appendU32Le(data, static_cast<quint32>(4 + chunks.size()));
    appendU32Be(data, kWaveForm);
    data.insert(data.end(), chunks.begin(), chunks.end());
    return data;
}

[[nodiscard]] std::vector<std::byte> extensibleFmtPayload() {
    std::vector<std::byte> payload;
    appendU16Le(payload, 0xFFFE);  // audio_format
    appendU16Le(payload, 2);       // num_channels
    appendU32Le(payload, 48000);   // sample_rate
    appendU32Le(payload, 384000);  // byte_rate
    appendU16Le(payload, 8);       // block_align
    appendU16Le(payload, 32);      // bits_per_sample
    appendU16Le(payload, 22);      // extensible_cb_size
    appendU16Le(payload, 32);      // valid_bits_per_sample
    appendU32Le(payload, 3);       // channel_mask
    appendU32Le(payload, 1);       // subformat GUID Data1 (PCM subformat)
    for (int index = 0; index < 12; ++index) {
        payload.push_back(static_cast<std::byte>(index));
    }
    return payload;
}

[[nodiscard]] std::optional<streamview::core::AnalysisNode>
childByName(const streamview::core::AnalysisTree& tree,
            const streamview::core::AnalysisNode& parent,
            const QString& name) {
    for (const auto childId : parent.children()) {
        const auto child = tree.node(childId);
        if (child && child->name() == name) {
            return child;
        }
    }
    return std::nullopt;
}

} // namespace

class WavRiffAnalyzerTest : public QObject {
    Q_OBJECT

private slots:
    void createsAnalyzerFromBundledPackageAndDecodesFieldsViaDsl() {
        const MemorySource source(minimalPcmWave());
        QString error;
        auto analyzer = WavRiffAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));
        QCOMPARE(analyzer->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.wav"));
        QCOMPARE(analyzer->ruleIdentity().entryPointId(), QStringLiteral("wave"));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.recordNodes.size(), std::size_t(3));
        QVERIFY(analyzer->finished());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
        QCOMPARE(root->children().size(), std::size_t(3));

        // Record 0: the RIFF form header
        const auto formNode = analyzer->tree().node(batch.recordNodes[0]);
        QVERIFY(formNode.has_value());
        QCOMPARE(formNode->name(), QStringLiteral("riff_form_header"));
        QCOMPARE(formNode->state(), MaterializationState::Materialized);
        QVERIFY(formNode->diagnostics().empty());
        QCOMPARE(formNode->children().size(), std::size_t(1));

        const auto formStruct = analyzer->tree().node(formNode->children()[0]);
        QVERIFY(formStruct.has_value());
        QCOMPARE(formStruct->name(), QStringLiteral("RiffRecord"));
        const std::vector<QString> expectedFormNames = {
            QStringLiteral("fourcc"),
            QStringLiteral("riff_size"),
            QStringLiteral("wave_id"),
        };
        QCOMPARE(formStruct->children().size(), expectedFormNames.size());
        for (std::size_t i = 0; i < expectedFormNames.size(); ++i) {
            const auto child = analyzer->tree().node(formStruct->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedFormNames[i]);
        }
        QCOMPARE(analyzer->tree().node(formStruct->children()[0])->value().toULongLong(),
                 quint64(kRiffMagic));
        QCOMPARE(analyzer->tree().node(formStruct->children()[1])->value().toULongLong(),
                 quint64(minimalPcmWave().size() - 8));
        QCOMPARE(analyzer->tree().node(formStruct->children()[2])->value().toULongLong(),
                 quint64(kWaveForm));

        // Record 1: the fmt chunk
        const auto fmtNode = analyzer->tree().node(batch.recordNodes[1]);
        QVERIFY(fmtNode.has_value());
        QCOMPARE(fmtNode->name(), QStringLiteral("chunk[1]"));
        QVERIFY(fmtNode->diagnostics().empty());
        QCOMPARE(fmtNode->children().size(), std::size_t(1));
        const auto fmtStruct = analyzer->tree().node(fmtNode->children()[0]);
        QVERIFY(fmtStruct.has_value());
        const std::vector<QString> expectedFmtNames = {
            QStringLiteral("fourcc"),
            QStringLiteral("chunk_size"),
            QStringLiteral("audio_format"),
            QStringLiteral("num_channels"),
            QStringLiteral("sample_rate"),
            QStringLiteral("byte_rate"),
            QStringLiteral("block_align"),
            QStringLiteral("bits_per_sample"),
            QStringLiteral("fmt_tail"),
        };
        QCOMPARE(fmtStruct->children().size(), expectedFmtNames.size());
        for (std::size_t i = 0; i < expectedFmtNames.size(); ++i) {
            const auto child = analyzer->tree().node(fmtStruct->children()[i]);
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedFmtNames[i]);
        }
        QCOMPARE(analyzer->tree().node(fmtStruct->children()[2])->value().toULongLong(), quint64(1));
        QCOMPARE(analyzer->tree().node(fmtStruct->children()[3])->value().toULongLong(), quint64(1));
        QCOMPARE(analyzer->tree().node(fmtStruct->children()[4])->value().toULongLong(), quint64(8000));
        QCOMPARE(analyzer->tree().node(fmtStruct->children()[5])->value().toULongLong(), quint64(8000));
        QCOMPARE(analyzer->tree().node(fmtStruct->children()[6])->value().toULongLong(), quint64(1));
        QCOMPARE(analyzer->tree().node(fmtStruct->children()[7])->value().toULongLong(), quint64(8));

        // Record 2: the data chunk with an opaque lazy payload
        const auto dataNode = analyzer->tree().node(batch.recordNodes[2]);
        QVERIFY(dataNode.has_value());
        QCOMPARE(dataNode->name(), QStringLiteral("chunk[2]"));
        QVERIFY(dataNode->diagnostics().empty());
        QCOMPARE(dataNode->children().size(), std::size_t(1));
        const auto dataStruct = analyzer->tree().node(dataNode->children()[0]);
        QVERIFY(dataStruct.has_value());
        const auto payload = childByName(analyzer->tree(), *dataStruct,
                                         QStringLiteral("chunk_payload"));
        QVERIFY(payload.has_value());
    }

    void decodesExtensibleFmtFields() {
        std::vector<std::byte> chunks;
        appendChunk(chunks, kFmtId, extensibleFmtPayload());
        appendChunk(chunks, kDataId, std::vector<std::byte>(32, std::byte{0x00}));

        std::vector<std::byte> data;
        appendU32Be(data, kRiffMagic);
        appendU32Le(data, static_cast<quint32>(4 + chunks.size()));
        appendU32Be(data, kWaveForm);
        data.insert(data.end(), chunks.begin(), chunks.end());

        const MemorySource source(std::move(data));
        QString error;
        auto analyzer = WavRiffAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.recordNodes.size(), std::size_t(3));

        const auto fmtNode = analyzer->tree().node(batch.recordNodes[1]);
        QVERIFY(fmtNode.has_value());
        QVERIFY(fmtNode->diagnostics().empty());
        const auto fmtStruct = analyzer->tree().node(fmtNode->children()[0]);
        QVERIFY(fmtStruct.has_value());

        const auto cbSize = childByName(analyzer->tree(), *fmtStruct,
                                        QStringLiteral("extensible_cb_size"));
        QVERIFY(cbSize.has_value());
        QCOMPARE(cbSize->value().toULongLong(), quint64(22));
        const auto validBits = childByName(analyzer->tree(), *fmtStruct,
                                           QStringLiteral("valid_bits_per_sample"));
        QVERIFY(validBits.has_value());
        QCOMPARE(validBits->value().toULongLong(), quint64(32));
        const auto channelMask = childByName(analyzer->tree(), *fmtStruct,
                                             QStringLiteral("channel_mask"));
        QVERIFY(channelMask.has_value());
        QCOMPARE(channelMask->value().toULongLong(), quint64(3));
        const auto guidData1 = childByName(analyzer->tree(), *fmtStruct,
                                           QStringLiteral("subformat_guid_data1"));
        QVERIFY(guidData1.has_value());
        QCOMPARE(guidData1->value().toULongLong(), quint64(1));
        // Fixed arrays flatten into per-element nodes (ADR-0067 style names).
        for (int index = 0; index < 12; ++index) {
            const auto element = childByName(
                analyzer->tree(), *fmtStruct,
                QStringLiteral("subformat_guid_tail[%1]").arg(index));
            QVERIFY2(element.has_value(),
                     qPrintable(QStringLiteral("missing subformat_guid_tail[%1]").arg(index)));
            QCOMPARE(element->value().toULongLong(), quint64(index));
        }
    }

    void decodesCanonicalFactSampleLength() {
        std::vector<std::byte> chunks;
        appendChunk(chunks, kFmtId, pcmFmtChunk());
        appendChunk(chunks, kFactId, {std::byte{0x20}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}});
        appendChunk(chunks, kDataId, std::vector<std::byte>(4, std::byte{0x00}));

        std::vector<std::byte> data;
        appendU32Be(data, kRiffMagic);
        appendU32Le(data, static_cast<quint32>(4 + chunks.size()));
        appendU32Be(data, kWaveForm);
        data.insert(data.end(), chunks.begin(), chunks.end());

        const MemorySource source(std::move(data));
        QString error;
        auto analyzer = WavRiffAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.recordNodes.size(), std::size_t(4));

        const auto factNode = analyzer->tree().node(batch.recordNodes[2]);
        QVERIFY(factNode.has_value());
        QVERIFY(factNode->diagnostics().empty());
        const auto factStruct = analyzer->tree().node(factNode->children()[0]);
        QVERIFY(factStruct.has_value());
        const auto sampleLength = childByName(analyzer->tree(), *factStruct,
                                              QStringLiteral("sample_length"));
        QVERIFY(sampleLength.has_value());
        QCOMPARE(sampleLength->value().toULongLong(), quint64(32));
    }

    void isolatesTruncatedDataChunkWithDiagnostic() {
        std::vector<std::byte> chunks;
        appendChunk(chunks, kFmtId, pcmFmtChunk());
        // data claims 64 payload bytes; only 8 are present.
        appendU32Be(chunks, kDataId);
        appendU32Le(chunks, 64U);
        chunks.insert(chunks.end(), 8, std::byte{0x42});

        std::vector<std::byte> data;
        appendU32Be(data, kRiffMagic);
        appendU32Le(data, static_cast<quint32>(4 + chunks.size()));
        appendU32Be(data, kWaveForm);
        data.insert(data.end(), chunks.begin(), chunks.end());

        const MemorySource source(std::move(data));
        QString error;
        auto analyzer = WavRiffAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.recordNodes.size(), std::size_t(3));

        const auto dataNode = analyzer->tree().node(batch.recordNodes[2]);
        QVERIFY(dataNode.has_value());
        QCOMPARE(dataNode->state(), MaterializationState::Invalid);
        QVERIFY(!dataNode->diagnostics().empty());
        QCOMPARE(dataNode->diagnostics().front().code,
                 streamview::core::DiagnosticCode::TruncatedSource);

        // The root materializes with partial results; the valid prefix stays.
        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QVERIFY(analyzer->tree().hasPartialResults());
    }

    void marksRootInvalidWhenSourceIsNotRiff() {
        std::vector<std::byte> data(64, std::byte{0x11});
        const MemorySource source(std::move(data));
        QString error;
        auto analyzer = WavRiffAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.recordNodes.size(), std::size_t(0));

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
        QVERIFY(!root->diagnostics().empty());
        QVERIFY(root->diagnostics().front().message.contains(
            QStringLiteral("No RIFF/WAVE record was found")));
    }

    void resumesAfterCancellation() {
        CancellationSource cancellationSource;
        (void)cancellationSource.requestCancellation();

        const MemorySource source(minimalPcmWave());
        QString error;
        auto analyzer = WavRiffAnalyzer::create(source, &error, cancellationSource.token());
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto cancelled = analyzer->analyzeBatch();
        QCOMPARE(cancelled.status, WavRiffAnalysisStatus::Cancelled);
        QVERIFY(analyzer->finished());

        CancellationSource freshSource;
        QVERIFY(analyzer->resumeAfterCancellation(freshSource.token(), &error));
        const auto resumed = analyzer->analyzeBatch();
        QVERIFY(resumed.complete());
        QCOMPARE(resumed.recordNodes.size(), std::size_t(3));
    }

    void propagatesSourceErrors() {
        const MemorySource source(minimalPcmWave(), true);
        QString error;
        auto analyzer = WavRiffAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, WavRiffAnalysisStatus::SourceError);
        QVERIFY(batch.errorMessage.contains(QStringLiteral("Simulated I/O read failure")));
        QVERIFY(analyzer->finished());

        const auto next = analyzer->analyzeBatch();
        QCOMPARE(next.status, WavRiffAnalysisStatus::SourceError);
    }
};

QTEST_GUILESS_MAIN(WavRiffAnalyzerTest)

#include "wav_riff_analyzer_test.moc"
