#include <streamview/rules/riff_wave_detector.h>

#include <QObject>
#include <QTest>

#include <cstddef>
#include <vector>

using streamview::core::SourceSpan;
using streamview::rules::RiffWaveDetectionConfidence;
using streamview::rules::RiffWaveDetectionResult;
using streamview::rules::detectRiffWaveCandidate;

namespace {

constexpr quint32 kRiffMagic = 0x52494646U;   // 'RIFF'
constexpr quint32 kWaveForm = 0x57415645U;    // 'WAVE'
constexpr quint32 kFmtId = 0x666D7420U;       // 'fmt '
constexpr quint32 kDataId = 0x64617461U;      // 'data'

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

[[nodiscard]] std::vector<std::byte> makeFormHeader(quint32 riffSize) {
    std::vector<std::byte> data;
    appendU32Be(data, kRiffMagic);
    appendU32Le(data, riffSize);
    appendU32Be(data, kWaveForm);
    return data;
}

[[nodiscard]] std::vector<std::byte> makeChunk(quint32 id, quint32 size, std::byte fill = std::byte{0x00}) {
    std::vector<std::byte> data;
    appendU32Be(data, id);
    appendU32Le(data, size);
    data.insert(data.end(), size, fill);
    if ((size & 1U) != 0U) {
        data.push_back(std::byte{0x7E});
    }
    return data;
}

[[nodiscard]] quint64 bitOffsetOf(const RiffWaveDetectionResult& result, std::size_t index) {
    return result.candidate->evidence[index].chunkSpan->start().absoluteBitOffset();
}

} // namespace

class RiffWaveDetectorTest : public QObject {
    Q_OBJECT

private slots:
    void classifiesMinimalWaveAsStrong() {
        std::vector<std::byte> data = makeFormHeader(8U + 16U + 8U + 4U);
        const auto fmt = makeChunk(kFmtId, 16U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), fmt.begin(), fmt.end());
        data.insert(data.end(), payload.begin(), payload.end());

        const auto result = detectRiffWaveCandidate(data, static_cast<quint64>(data.size()));

        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, RiffWaveDetectionConfidence::Strong);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(3));
        QCOMPARE(result.inspectedByteCount, static_cast<quint64>(data.size()));
        QCOMPARE(result.sourceFullyInspected, true);

        QVERIFY(result.candidate->evidence[0].isFormHeader);
        QCOMPARE(result.candidate->evidence[0].chunkId, kRiffMagic);
        QCOMPARE(bitOffsetOf(result, 0), quint64(0));

        QVERIFY(!result.candidate->evidence[1].isFormHeader);
        QCOMPARE(result.candidate->evidence[1].chunkId, kFmtId);
        QCOMPARE(bitOffsetOf(result, 1), quint64(12 * 8));

        QCOMPARE(result.candidate->evidence[2].chunkId, kDataId);
        QCOMPARE(bitOffsetOf(result, 2), quint64((12 + 8 + 16) * 8));
    }

    void classifiesHeaderPlusOneChunkAsProbable() {
        std::vector<std::byte> data = makeFormHeader(8U + 16U);
        const auto fmt = makeChunk(kFmtId, 16U);
        data.insert(data.end(), fmt.begin(), fmt.end());

        const auto result = detectRiffWaveCandidate(data, static_cast<quint64>(data.size()));

        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, RiffWaveDetectionConfidence::Probable);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(2));
    }

    void classifiesHeaderOnlyAsWeak() {
        std::vector<std::byte> data = makeFormHeader(8U + 64U);
        data.insert(data.end(), 3, std::byte{0x11});

        const auto result = detectRiffWaveCandidate(data, static_cast<quint64>(data.size()));

        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, RiffWaveDetectionConfidence::Weak);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(1));
        QVERIFY(result.candidate->evidence[0].isFormHeader);
    }

    void returnsNoCandidateWithoutWaveFormMagic() {
        std::vector<std::byte> data = makeFormHeader(8U + 16U);
        data[8] = std::byte{0x41};   // 'A'
        data[9] = std::byte{0x56};   // 'V'
        data[10] = std::byte{0x49};  // 'I'
        data[11] = std::byte{0x20};  // ' '
        const auto fmt = makeChunk(kFmtId, 16U);
        data.insert(data.end(), fmt.begin(), fmt.end());

        const auto result = detectRiffWaveCandidate(data, static_cast<quint64>(data.size()));

        QVERIFY(!result.candidate.has_value());
    }

    void returnsNoCandidateForShortPrefix() {
        std::vector<std::byte> data = makeFormHeader(8U + 16U);
        data.resize(8);

        const auto result = detectRiffWaveCandidate(data, static_cast<quint64>(data.size()));

        QVERIFY(!result.candidate.has_value());
        QCOMPARE(result.inspectedByteCount, quint64(8));
    }

    void recordsHeaderOnlyEvidenceForChunkBeyondInspection() {
        // A large data chunk frames inside the source but extends past the
        // 64 KiB inspection window: its verified header is evidence.
        std::vector<std::byte> data = makeFormHeader(8U + 16U + 8U + 200000U);
        const auto fmt = makeChunk(kFmtId, 16U);
        const auto payload = makeChunk(kDataId, 200000U);
        data.insert(data.end(), fmt.begin(), fmt.end());
        data.insert(data.end(), payload.begin(), payload.end());

        const auto result = detectRiffWaveCandidate(data, static_cast<quint64>(data.size()));

        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.sourceFullyInspected, false);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(3));
        QVERIFY(!result.candidate->evidence[2].isFormHeader);
        QVERIFY(result.candidate->evidence[2].chunkSpan.has_value());
        // The span covers only the verified 8-byte chunk header.
        QCOMPARE(result.candidate->evidence[2].chunkSpan->bitLength(), quint64(8 * 8));
        QCOMPARE(result.candidate->confidence, RiffWaveDetectionConfidence::Strong);
    }

    void ignoresChunksTruncatedInSource() {
        // data declares more payload bytes than the source carries: no
        // evidence for it.
        std::vector<std::byte> data = makeFormHeader(8U + 64U);
        appendU32Be(data, kDataId);
        appendU32Le(data, 64U);
        data.insert(data.end(), 4, std::byte{0x42});

        const auto result = detectRiffWaveCandidate(data, static_cast<quint64>(data.size()));

        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, RiffWaveDetectionConfidence::Weak);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(1));
    }

    void weakCandidateForDegenerateFormSize() {
        std::vector<std::byte> data = makeFormHeader(2U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), payload.begin(), payload.end());

        const auto result = detectRiffWaveCandidate(data, static_cast<quint64>(data.size()));

        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, RiffWaveDetectionConfidence::Weak);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(1));
    }

    void stopsCollectingEvidenceAtTheFormEnd() {
        // Trailing bytes after the form end are outside the tiling and must
        // not add evidence.
        std::vector<std::byte> data = makeFormHeader(8U + 4U);
        const auto payload = makeChunk(kDataId, 4U);
        data.insert(data.end(), payload.begin(), payload.end());
        // Trailing bytes that frame a plausible extra chunk beyond the form.
        const auto extra = makeChunk(kFmtId, 16U);
        data.insert(data.end(), extra.begin(), extra.end());

        const auto result = detectRiffWaveCandidate(data, static_cast<quint64>(data.size()));

        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->evidence.size(), std::size_t(2));
        QCOMPARE(result.candidate->confidence, RiffWaveDetectionConfidence::Probable);
    }
};

QTEST_GUILESS_MAIN(RiffWaveDetectorTest)

#include "riff_wave_detector_test.moc"
