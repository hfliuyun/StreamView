#include <streamview/rules/format_selection.h>

#include <QFile>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <vector>

using streamview::rules::AacAdtsCandidate;
using streamview::rules::AacAdtsDetectionEvidence;
using streamview::rules::AacAdtsDetectionResult;
using streamview::rules::DetectedFormat;
using streamview::rules::DetectedFormatReason;
using streamview::rules::H264AnnexBDetectionResult;
using streamview::rules::Mp4Candidate;
using streamview::rules::Mp4DetectionEvidence;
using streamview::rules::Mp4DetectionResult;
using streamview::rules::detectAacAdtsCandidate;
using streamview::rules::detectH264AnnexBCandidate;
using streamview::rules::detectMp4Candidate;
using streamview::rules::selectFormatFromDetection;

namespace {

void appendBigEndianU32(std::vector<std::byte>& out, quint32 value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFU));
    out.push_back(static_cast<std::byte>(value & 0xFFU));
}

void appendFourCharCode(std::vector<std::byte>& out, const char (&code)[5]) {
    for (int index = 0; index < 4; ++index) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(code[index])));
    }
}

void appendBox(std::vector<std::byte>& out,
               quint32 declaredSize,
               const char (&code)[5],
               std::byte filler,
               quint32 bodyBytes) {
    appendBigEndianU32(out, declaredSize);
    appendFourCharCode(out, code);
    for (quint32 index = 0; index < bodyBytes; ++index) {
        out.push_back(filler);
    }
}

void appendStartCodeFourByte(std::vector<std::byte>& out) {
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x01});
}

void appendNalUnit(std::vector<std::byte>& out, quint8 header, std::byte filler, int payloadBytes) {
    appendStartCodeFourByte(out);
    out.push_back(static_cast<std::byte>(header));
    for (int index = 0; index < payloadBytes; ++index) {
        out.push_back(filler);
    }
}

void appendAdtsFrame(std::vector<std::byte>& out,
                     quint16 frameLength,
                     bool protectionAbsent = true,
                     quint8 profile = 1,
                     quint8 samplingFreqIndex = 4,
                     quint8 channelConfig = 2,
                     std::byte fillByte = std::byte{0xAB}) {
    const std::size_t headerLen = protectionAbsent ? 7U : 9U;
    if (frameLength < headerLen) {
        return;
    }
    const std::size_t start = out.size();
    out.resize(start + frameLength, fillByte);
    // Byte 0: 0xFF
    out[start] = std::byte{0xFF};
    // Byte 1: 0xF0 | (id=0 << 3) | (layer=0 << 1) | (protectionAbsent ? 1 : 0)
    out[start + 1] = std::byte{static_cast<quint8>(0xF0U | (protectionAbsent ? 1U : 0U))};
    // Byte 2: (profile << 6) | (samplingFreqIndex << 2) | (private_bit=0) | (channelConfig >> 2)
    out[start + 2] = std::byte{static_cast<quint8>(
        ((profile & 0x03U) << 6U) |
        ((samplingFreqIndex & 0x0FU) << 2U) |
        ((channelConfig >> 2U) & 0x01U))};
    // Byte 3: ((channelConfig & 0x03) << 6) | (copy=0) | (home=0) | (copy_id=0) | (copy_start=0) | (frameLength >> 11)
    out[start + 3] = std::byte{static_cast<quint8>(
        ((channelConfig & 0x03U) << 6U) |
        ((frameLength >> 11U) & 0x03U))};
    // Byte 4: (frameLength >> 3) & 0xFF
    out[start + 4] = std::byte{static_cast<quint8>((frameLength >> 3U) & 0xFFU)};
    // Byte 5: ((frameLength & 0x07) << 5) | (buffer_fullness=0x7FF >> 6)
    out[start + 5] = std::byte{static_cast<quint8>(
        ((frameLength & 0x07U) << 5U) | 0x1FU)};
    // Byte 6: ((buffer_fullness=0x7FF & 0x3F) << 2) | (raw_blocks=0)
    out[start + 6] = std::byte{0xFC};
    if (!protectionAbsent) {
        out[start + 7] = std::byte{0x12};
        out[start + 8] = std::byte{0x34};
    }
}

[[nodiscard]] std::vector<std::byte> readFixture(const QString& relativePath) {
    QFile file(QStringLiteral(STREAMVIEW_SOURCE_DIR "/") + relativePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray contents = file.readAll();
    std::vector<std::byte> out;
    out.reserve(static_cast<std::size_t>(contents.size()));
    for (const char byte : contents) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    }
    return out;
}

[[nodiscard]] streamview::rules::FormatSelection selectFor(
    const std::vector<std::byte>& bytes, quint64 declaredSourceSize) {
    const std::span<const std::byte> prefix(bytes.data(), bytes.size());
    const Mp4DetectionResult mp4 = detectMp4Candidate(prefix, declaredSourceSize);
    const AacAdtsDetectionResult aac = detectAacAdtsCandidate(prefix, declaredSourceSize);
    const H264AnnexBDetectionResult h264 =
        detectH264AnnexBCandidate(prefix, declaredSourceSize);
    return selectFormatFromDetection(mp4, aac, h264);
}

[[nodiscard]] streamview::rules::FormatSelection selectFor(const std::vector<std::byte>& bytes) {
    return selectFor(bytes, static_cast<quint64>(bytes.size()));
}

} // namespace

class FormatSelectionTest final : public QObject {
    Q_OBJECT

private slots:
    void resolvesAmbiguityTowardTheContainerAndMarksIt() {
        // The one case no FourCC-free rule can settle in both directions: bytes
        // that open with a real start code *and* tile as boxes across the whole
        // window. Pinning it to "container wins, loudly marked" keeps the
        // decision reviewable instead of presenting a coincidence as a fact.
        // Size 0x0105 makes the first four bytes read as a three-byte start
        // code followed by 0x05, a well-formed NAL header. A second start code
        // lives *inside* the box body so the tiling stays intact while the
        // pattern detector still reaches Strong.
        constexpr quint32 firstBoxSize = 0x0105U;
        std::vector<std::byte> bytes;
        appendBigEndianU32(bytes, firstBoxSize);
        appendFourCharCode(bytes, "free");
        for (quint32 index = 0; index < firstBoxSize - 8U; ++index) {
            if (index == 100U) {
                bytes.push_back(std::byte{0x00});
                bytes.push_back(std::byte{0x00});
                bytes.push_back(std::byte{0x01});
                bytes.push_back(std::byte{0x67});
                index += 3U;
                continue;
            }
            bytes.push_back(std::byte{0xEE});
        }
        QCOMPARE(bytes.size(), static_cast<std::size_t>(firstBoxSize));
        appendBox(bytes, 24U, "ftyp", std::byte{0x00}, 16U);
        appendBox(bytes, 40U, "mdat", std::byte{0x33}, 32U);

        const auto h264 = detectH264AnnexBCandidate({bytes.data(), bytes.size()}, bytes.size());
        const auto mp4 = detectMp4Candidate({bytes.data(), bytes.size()}, bytes.size());
        QVERIFY2(mp4.candidate.has_value(), "the fixture must tile as boxes");
        QVERIFY2(h264.candidate.has_value(), "the fixture must also look like Annex B");

        // Asserted unconditionally: a guarded check would pass vacuously if the
        // fixture stopped reaching Strong, and the ambiguity branch would go
        // unexercised without any test turning red.
        QCOMPARE(h264.candidate->confidence,
                 streamview::rules::H264AnnexBDetectionConfidence::Strong);
        QCOMPARE(mp4.candidate->confidence, streamview::rules::Mp4DetectionConfidence::Strong);

        const auto selection = selectFor(bytes);
        QCOMPARE(selection.format, DetectedFormat::Mp4Isobmff);
        QCOMPARE(selection.reason,
                 DetectedFormatReason::AmbiguousContainerVersusElementaryStream);
        QVERIFY(selection.ambiguous());
    }

    void prefersTheContainerForRealAvcInMp4() {
        const auto bytes = readFixture(QStringLiteral("tests/fixtures/mp4_p5j4_avc_multi_nal.mp4"));
        QVERIFY(!bytes.empty());

        const auto h264 = detectH264AnnexBCandidate({bytes.data(), bytes.size()}, bytes.size());
        QVERIFY2(h264.candidate.has_value() &&
                     h264.candidate->confidence ==
                         streamview::rules::H264AnnexBDetectionConfidence::Strong,
                 "the defect requires the H.264 detector to reach Strong on this container");

        const auto selection = selectFor(bytes);
        QCOMPARE(selection.format, DetectedFormat::Mp4Isobmff);
        QCOMPARE(selection.reason, DetectedFormatReason::Mp4SubsumesPatternEvidence);
    }

    void prefersTheContainerForATwoTrackMovie() {
        const auto bytes = readFixture(QStringLiteral("tests/fixtures/mp4_p5j4_two_tracks.mp4"));
        QVERIFY(!bytes.empty());

        const auto selection = selectFor(bytes);
        QCOMPARE(selection.format, DetectedFormat::Mp4Isobmff);
        QCOMPARE(selection.reason, DetectedFormatReason::Mp4SubsumesPatternEvidence);
    }

    void keepsRecognizingContainersThatNeverTrippedTheDefect() {
        for (const auto* relative : {"tests/fixtures/mp4_p5j4_aac_opaque.mp4",
                                     "tests/fixtures/mp4_p5h_avc1_avcC.mp4",
                                     "tests/fixtures/mp4_p5h_mp4a_esds.mp4"}) {
            const auto bytes = readFixture(QString::fromLatin1(relative));
            QVERIFY2(!bytes.empty(), relative);
            const auto selection = selectFor(bytes);
            QCOMPARE(selection.format, DetectedFormat::Mp4Isobmff);
        }
    }

    void selectsAnnexBForAGenuineElementaryStream() {
        std::vector<std::byte> bytes;
        appendNalUnit(bytes, 0x67U, std::byte{0x42}, 20);
        appendNalUnit(bytes, 0x68U, std::byte{0xCE}, 8);
        appendNalUnit(bytes, 0x65U, std::byte{0x9A}, 40);

        const auto selection = selectFor(bytes);
        QCOMPARE(selection.format, DetectedFormat::H264AnnexB);
        QCOMPARE(selection.reason, DetectedFormatReason::H264AnchoredStartCodes);
    }

    void selectsAnnexBWhenLeadingZeroBytesPrecedeTheFirstStartCode() {
        // Annex B allows leading zero bytes, so anchoring must not demand that
        // the start code begin exactly at offset 0.
        std::vector<std::byte> bytes;
        bytes.push_back(std::byte{0x00});
        appendNalUnit(bytes, 0x67U, std::byte{0x42}, 20);
        appendNalUnit(bytes, 0x68U, std::byte{0xCE}, 8);
        appendNalUnit(bytes, 0x65U, std::byte{0x9A}, 40);

        const auto selection = selectFor(bytes);
        QCOMPARE(selection.format, DetectedFormat::H264AnnexB);
        QCOMPARE(selection.reason, DetectedFormatReason::H264AnchoredStartCodes);
    }

    void lettingAnnexBWinRequiresEvidenceOutsideTheContainerTiling() {
        std::vector<std::byte> bytes;
        appendBox(bytes, 24U, "ftyp", std::byte{0x00}, 16U);
        appendNalUnit(bytes, 0x67U, std::byte{0x42}, 20);
        appendNalUnit(bytes, 0x68U, std::byte{0xCE}, 8);
        appendNalUnit(bytes, 0x65U, std::byte{0x9A}, 40);

        const auto mp4 = detectMp4Candidate({bytes.data(), bytes.size()}, bytes.size());
        QVERIFY(mp4.candidate.has_value());
        QCOMPARE(mp4.candidate->confidence, streamview::rules::Mp4DetectionConfidence::Weak);

        const auto selection = selectFor(bytes);
        QCOMPARE(selection.format, DetectedFormat::H264AnnexB);
        QCOMPARE(selection.reason, DetectedFormatReason::H264UnanchoredStartCodes);
    }

    void reportsNoFormatForBytesWithoutAnyStructuralSignature() {
        std::vector<std::byte> bytes(512, std::byte{0xAB});

        const auto selection = selectFor(bytes);
        QCOMPARE(selection.format, DetectedFormat::None);
        QCOMPARE(selection.reason, DetectedFormatReason::NoCandidate);
        QVERIFY(!selection.decided());
    }

    void containedPatternEvidenceCannotOutrankAContainerTiling() {
        // Direct statement of the invariant, independent of any one fixture:
        // when the tiling covers the whole inspected range, no start code can be
        // independent of it, so the container must win.
        const auto bytes = readFixture(QStringLiteral("tests/fixtures/mp4_p5j4_two_tracks.mp4"));
        QVERIFY(!bytes.empty());

        const auto mp4 = detectMp4Candidate({bytes.data(), bytes.size()}, bytes.size());
        QVERIFY(mp4.candidate.has_value());
        quint64 coverageEnd = 0;
        for (const auto& evidence : mp4.candidate->evidence) {
            QVERIFY(evidence.boxSpan.has_value());
            coverageEnd = std::max(
                coverageEnd, evidence.boxSpan->endExclusive().absoluteBitOffset() / 8U);
        }
        QCOMPARE(coverageEnd, static_cast<quint64>(bytes.size()));

        const auto h264 = detectH264AnnexBCandidate({bytes.data(), bytes.size()}, bytes.size());
        QVERIFY(h264.candidate.has_value());
        for (const auto& evidence : h264.candidate->evidence) {
            if (!evidence.startCode.has_value()) {
                continue;
            }
            QVERIFY(evidence.startCode->start().absoluteBitOffset() / 8U < coverageEnd);
        }

        QCOMPARE(selectFor(bytes).format, DetectedFormat::Mp4Isobmff);
    }

    void ignoresACandidateThatDidNotReachStrongConfidence() {
        std::vector<std::byte> bytes;
        appendBox(bytes, 24U, "ftyp", std::byte{0x00}, 16U);
        appendBox(bytes, 16U, "free", std::byte{0x00}, 8U);

        const auto mp4 = detectMp4Candidate({bytes.data(), bytes.size()}, bytes.size());
        QVERIFY(mp4.candidate.has_value());
        QCOMPARE(mp4.candidate->confidence, streamview::rules::Mp4DetectionConfidence::Probable);

        const auto selection = selectFor(bytes);
        QCOMPARE(selection.format, DetectedFormat::None);
    }

    void unanchoredAacPatternCannotOutrankPartialContainerTiling() {
        // Regression test for P1-1: three complete boxes (ftyp, free, moov)
        // establish an MP4 Strong candidate covering the first 72 bytes. A fourth
        // box declares a size extending past the inspection window/source size,
        // so the tiling breaks at byte 72 (partial tiling). In the visible body of
        // the fourth box (starting after byte 72), a chain of 3 self-consistent ADTS
        // frames makes the AAC detector reach Strong.
        // Because the ADTS syncwords are deep inside the source (> 72), they are
        // not anchored at offset 0. With AAC anchoring, the container wins with
        // Mp4SubsumesPatternEvidence; without AAC anchoring, unanchored AAC would veto.
        std::vector<std::byte> bytes;
        appendBox(bytes, 24U, "ftyp", std::byte{0x00}, 16U);
        appendBox(bytes, 16U, "free", std::byte{0x00}, 8U);
        appendBox(bytes, 32U, "moov", std::byte{0x00}, 24U);
        QCOMPARE(bytes.size(), 72U);

        // Fourth box: declared size 5000 exceeds source size
        appendBigEndianU32(bytes, 5000U);
        appendFourCharCode(bytes, "mdat");
        appendAdtsFrame(bytes, 50U, true);
        appendAdtsFrame(bytes, 50U, true);
        appendAdtsFrame(bytes, 50U, true);

        const auto declaredSourceSize = static_cast<quint64>(bytes.size());
        const auto mp4 = detectMp4Candidate({bytes.data(), bytes.size()}, declaredSourceSize);
        const auto aac = detectAacAdtsCandidate({bytes.data(), bytes.size()}, declaredSourceSize);

        QVERIFY(mp4.candidate.has_value());
        QCOMPARE(mp4.candidate->confidence, streamview::rules::Mp4DetectionConfidence::Strong);
        QVERIFY(aac.candidate.has_value());
        QCOMPARE(aac.candidate->confidence, streamview::rules::AacAdtsDetectionConfidence::Strong);

        const auto selection = selectFor(bytes, declaredSourceSize);
        QCOMPARE(selection.format, DetectedFormat::Mp4Isobmff);
        QCOMPARE(selection.reason, DetectedFormatReason::Mp4SubsumesPatternEvidence);
        QVERIFY(!selection.ambiguous());
    }

    void unanchoredH264PatternCannotOutrankPartialContainerTiling() {
        // Regression test for P1-2: three complete boxes (ftyp, free, moov)
        // establish an MP4 Strong candidate with coverageEnd == 72. A fourth
        // box declared size extends beyond the window, breaking tiling at byte 72.
        // In its visible body (starting at byte 80), two valid H.264 NAL units
        // make the H.264 detector reach Strong.
        // Because the start codes are at byte >= 80, h264Contained is FALSE.
        // This is the single-variable load-bearing test for h264Anchored:
        // when h264Anchored is removed, this test turns RED!
        std::vector<std::byte> bytes;
        appendBox(bytes, 24U, "ftyp", std::byte{0x00}, 16U);
        appendBox(bytes, 16U, "free", std::byte{0x00}, 8U);
        appendBox(bytes, 32U, "moov", std::byte{0x00}, 24U);
        QCOMPARE(bytes.size(), 72U);

        // Fourth box: declared size 5000 exceeds source size
        appendBigEndianU32(bytes, 5000U);
        appendFourCharCode(bytes, "mdat");
        appendNalUnit(bytes, 0x67U, std::byte{0x42}, 20);
        appendNalUnit(bytes, 0x68U, std::byte{0xCE}, 8);

        const auto declaredSourceSize = static_cast<quint64>(bytes.size());
        const auto mp4 = detectMp4Candidate({bytes.data(), bytes.size()}, declaredSourceSize);
        const auto h264 = detectH264AnnexBCandidate({bytes.data(), bytes.size()}, declaredSourceSize);

        QVERIFY(mp4.candidate.has_value());
        QCOMPARE(mp4.candidate->confidence, streamview::rules::Mp4DetectionConfidence::Strong);
        QVERIFY(h264.candidate.has_value());
        QCOMPARE(h264.candidate->confidence, streamview::rules::H264AnnexBDetectionConfidence::Strong);

        const auto selection = selectFor(bytes, declaredSourceSize);
        QCOMPARE(selection.format, DetectedFormat::Mp4Isobmff);
        QCOMPARE(selection.reason, DetectedFormatReason::Mp4SubsumesPatternEvidence);
        QVERIFY(!selection.ambiguous());
    }

    void annexBWithFourLeadingZeroBytesIsNotAnchored() {
        // Annex B allows up to 3 leading zero bytes (maximumAnnexBLeadingZeroBytes == 3).
        // If a start code is preceded by 4 leading zero bytes, it starts at offset 4,
        // which exceeds the threshold and is treated as unanchored.
        std::vector<std::byte> bytes;
        bytes.push_back(std::byte{0x00});
        bytes.push_back(std::byte{0x00});
        bytes.push_back(std::byte{0x00});
        bytes.push_back(std::byte{0x00});
        appendNalUnit(bytes, 0x67U, std::byte{0x42}, 20);
        appendNalUnit(bytes, 0x68U, std::byte{0xCE}, 8);

        const auto selection = selectFor(bytes);
        QCOMPARE(selection.format, DetectedFormat::H264AnnexB);
        QCOMPARE(selection.reason, DetectedFormatReason::H264UnanchoredStartCodes);
    }

    void anchoredAacCollisionProducesAmbiguousSelection() {
        // Direct test of the AAC ambiguity branch: when MP4 is Strong,
        // H.264 is None, and AAC is Strong AND anchored at offset 0,
        // the collision between the container tiling and the anchored AAC stream
        // resolves to Mp4Isobmff with AmbiguousContainerVersusElementaryStream.
        Mp4DetectionResult mp4;
        Mp4Candidate mp4Cand;
        mp4Cand.confidence = streamview::rules::Mp4DetectionConfidence::Strong;
        for (quint64 offset = 0; offset < 300; offset += 100) {
            Mp4DetectionEvidence ev;
            ev.boxSpan = streamview::core::SourceSpan::create(
                streamview::core::SourceBitAddress(offset * 8U), 100 * 8U);
            ev.boxOffset = offset;
            ev.declaredBoxSize = 100;
            mp4Cand.evidence.push_back(ev);
        }
        mp4.candidate = mp4Cand;
        mp4.inspectedByteCount = 300;

        AacAdtsDetectionResult aac;
        AacAdtsCandidate aacCand;
        aacCand.confidence = streamview::rules::AacAdtsDetectionConfidence::Strong;
        AacAdtsDetectionEvidence aacEv;
        aacEv.syncword = streamview::core::SourceSpan::create(
            streamview::core::SourceBitAddress(0), 16);
        aacCand.evidence.push_back(aacEv);
        aac.candidate = aacCand;

        H264AnnexBDetectionResult h264;

        const auto selection = selectFormatFromDetection(mp4, aac, h264);
        QCOMPARE(selection.format, DetectedFormat::Mp4Isobmff);
        QCOMPARE(selection.reason,
                 DetectedFormatReason::AmbiguousContainerVersusElementaryStream);
        QVERIFY(selection.ambiguous());
    }

    void largeFaststartMovieWithAvcPayloadResolvesToContainer() {
        // Real-scale synthetic test for Task P5j-4e:
        // Layout: ftyp (32) + moov (2000) + 500 MB mdat.
        // Inside mdat body (within 64 KiB probe window, starting at byte 2040),
        // we place two valid H.264 NAL units so detectH264AnnexBCandidate reaches Strong.
        // detectMp4Candidate reaches Strong because the mdat header is verified within the window.
        // mp4CoverageEnd is 2040 (mdat header end, body excluded).
        // Since H.264 start codes are at byte >= 2040, h264Contained is FALSE.
        // But because h264Anchored is FALSE (ftyp at offset 0), h264Independent is FALSE.
        // selectFormatFromDetection selects Mp4Isobmff with Mp4SubsumesPatternEvidence.
        const quint64 declaredSourceSize = 500U * 1024U * 1024U;
        std::vector<std::byte> bytes;
        appendBox(bytes, 32U, "ftyp", std::byte{0x00}, 24U);
        appendBox(bytes, 2000U, "moov", std::byte{0x00}, 1992U);
        QCOMPARE(bytes.size(), 2032U);

        // mdat header claiming 500 MB - 2032
        const quint32 mdatSize = static_cast<quint32>(declaredSourceSize - 2032U);
        appendBigEndianU32(bytes, mdatSize);
        appendFourCharCode(bytes, "mdat");
        QCOMPARE(bytes.size(), 2040U);

        // AVC NAL units in mdat body
        appendNalUnit(bytes, 0x67U, std::byte{0x42}, 20);
        appendNalUnit(bytes, 0x68U, std::byte{0xCE}, 8);

        const auto mp4 = detectMp4Candidate({bytes.data(), bytes.size()}, declaredSourceSize);
        const auto h264 = detectH264AnnexBCandidate({bytes.data(), bytes.size()}, declaredSourceSize);

        QVERIFY(mp4.candidate.has_value());
        QCOMPARE(mp4.candidate->confidence, streamview::rules::Mp4DetectionConfidence::Strong);
        QCOMPARE(mp4.candidate->evidence.size(), std::size_t(3));
        QVERIFY(h264.candidate.has_value());
        QCOMPARE(h264.candidate->confidence, streamview::rules::H264AnnexBDetectionConfidence::Strong);

        const auto selection = selectFor(bytes, declaredSourceSize);
        QCOMPARE(selection.format, DetectedFormat::Mp4Isobmff);
        QCOMPARE(selection.reason, DetectedFormatReason::Mp4SubsumesPatternEvidence);
        QVERIFY(!selection.ambiguous());
    }
};

QTEST_MAIN(FormatSelectionTest)
#include "format_selection_test.moc"
