#include <streamview/rules/mp4_box_detector.h>

#include <QObject>
#include <QTest>

#include <cstddef>
#include <vector>

using streamview::rules::detectMp4Candidate;
using streamview::rules::mp4DetectionProbeSizeBytes;
using streamview::rules::Mp4DetectionConfidence;

namespace {

[[nodiscard]] std::vector<std::byte> makeNormalBox(quint32 size, quint32 type, std::byte fill = std::byte{0x00}) {
    std::vector<std::byte> box(size >= 8 ? size : 8, fill);
    box[0] = static_cast<std::byte>((size >> 24) & 0xFF);
    box[1] = static_cast<std::byte>((size >> 16) & 0xFF);
    box[2] = static_cast<std::byte>((size >> 8) & 0xFF);
    box[3] = static_cast<std::byte>(size & 0xFF);
    box[4] = static_cast<std::byte>((type >> 24) & 0xFF);
    box[5] = static_cast<std::byte>((type >> 16) & 0xFF);
    box[6] = static_cast<std::byte>((type >> 8) & 0xFF);
    box[7] = static_cast<std::byte>(type & 0xFF);
    return box;
}

} // namespace

class Mp4BoxDetectorTest : public QObject {
    Q_OBJECT

private slots:
    void detectsStrongCandidateWithThreeOrMoreBoxes() {
        std::vector<std::byte> data;
        const auto b1 = makeNormalBox(32, 0x66747970); // ftyp
        const auto b2 = makeNormalBox(8,  0x66726565); // free
        const auto b3 = makeNormalBox(48, 0x6D6F6F76); // moov
        data.insert(data.end(), b1.begin(), b1.end());
        data.insert(data.end(), b2.begin(), b2.end());
        data.insert(data.end(), b3.begin(), b3.end());

        const auto result = detectMp4Candidate(data, data.size());
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Strong);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(3));
        QCOMPARE(result.inspectedByteCount, quint64(data.size()));
        QVERIFY(result.sourceFullyInspected);

        // Check evidence
        QCOMPARE(result.candidate->evidence[0].boxOffset, quint64(0));
        QCOMPARE(result.candidate->evidence[0].declaredBoxSize, quint64(32));
        QCOMPARE(result.candidate->evidence[1].boxOffset, quint64(32));
        QCOMPARE(result.candidate->evidence[1].declaredBoxSize, quint64(8));
        QCOMPARE(result.candidate->evidence[2].boxOffset, quint64(40));
        QCOMPARE(result.candidate->evidence[2].declaredBoxSize, quint64(48));
    }

    void detectsProbableCandidateWithTwoBoxes() {
        std::vector<std::byte> data;
        const auto b1 = makeNormalBox(32, 0x66747970);
        const auto b2 = makeNormalBox(24, 0x6D6F6F76);
        data.insert(data.end(), b1.begin(), b1.end());
        data.insert(data.end(), b2.begin(), b2.end());

        const auto result = detectMp4Candidate(data, data.size());
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Probable);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(2));
    }

    void detectsWeakCandidateWithOneBox() {
        const auto b = makeNormalBox(32, 0x66747970);
        const auto result = detectMp4Candidate(b, b.size());
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Weak);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(1));
    }

    void detectsStrongWithThreeCompleteBoxesAndTrailingTruncated() {
        std::vector<std::byte> data;
        const auto b1 = makeNormalBox(32, 0x66747970);
        const auto b2 = makeNormalBox(8,  0x66726565);
        const auto b3 = makeNormalBox(48, 0x6D6F6F76);
        data.insert(data.end(), b1.begin(), b1.end());
        data.insert(data.end(), b2.begin(), b2.end());
        data.insert(data.end(), b3.begin(), b3.end());

        // Append 4th box that claims size 100, but only 20 bytes exist in source
        const auto b4 = makeNormalBox(100, 0x6D646174);
        data.insert(data.end(), b4.begin(), b4.begin() + 20);

        const auto result = detectMp4Candidate(data, data.size());
        QVERIFY(result.candidate.has_value());
        // Trailing truncated box does not downgrade prior complete chain (still Strong with 3 complete boxes)
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Strong);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(3));
    }

    void rejectsTruncatedFirstBox() {
        // First box claims size 100, but only 20 bytes exist
        const auto b = makeNormalBox(100, 0x66747970);
        std::vector<std::byte> truncated(b.begin(), b.begin() + 20);

        const auto result = detectMp4Candidate(truncated, truncated.size());
        QVERIFY(!result.candidate.has_value());
    }

    void detectsSizeZeroTerminalBox() {
        std::vector<std::byte> data;
        const auto b = makeNormalBox(0, 0x6D646174);
        data.insert(data.end(), b.begin(), b.end());
        data.insert(data.end(), 40, std::byte{0x77});

        const auto result = detectMp4Candidate(data, data.size());
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Weak);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(1));
        QCOMPARE(result.candidate->evidence[0].declaredBoxSize, quint64(0));
    }

    void sizeZeroTerminalOnlyEvidenceWhenSourceFullyInspected() {
        // When source size > probe size, size 0 box is NOT fully verified
        std::vector<std::byte> prefix = makeNormalBox(0, 0x6D646174);
        prefix.insert(prefix.end(), 100, std::byte{0x77});

        // Case A: sourceSizeBytes matches prefix -> sourceFullyInspected = true -> Weak candidate
        const auto resFull = detectMp4Candidate(prefix, prefix.size());
        QVERIFY(resFull.candidate.has_value());
        QCOMPARE(resFull.candidate->confidence, Mp4DetectionConfidence::Weak);

        // Case B: sourceSizeBytes is 200'000 (larger than probe 64KiB) -> sourceFullyInspected = false -> no candidate
        const auto resPartial = detectMp4Candidate(prefix, 200'000U);
        QVERIFY(!resPartial.candidate.has_value());
    }

    void stopsAtMalformedSize() {
        std::vector<std::byte> data;
        const auto b1 = makeNormalBox(32, 0x66747970);
        const auto b2 = makeNormalBox(4,  0x6D6F6F76); // size 4 is malformed
        data.insert(data.end(), b1.begin(), b1.end());
        data.insert(data.end(), b2.begin(), b2.end());

        const auto result = detectMp4Candidate(data, data.size());
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Weak);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(1));
    }

    void neverReadsPastSourceSizeBytesWhenPrefixIsLarger() {
        // Prefix contains 3 valid boxes (96 bytes total), but sourceSizeBytes is declared as 32
        std::vector<std::byte> prefix;
        for (int i = 0; i < 3; ++i) {
            const auto b = makeNormalBox(32, 0x66726565);
            prefix.insert(prefix.end(), b.begin(), b.end());
        }

        const auto result = detectMp4Candidate(prefix, 32U);
        QVERIFY(result.candidate.has_value());
        // Only 1 box falls inside sourceSizeBytes = 32
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Weak);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(1));
        QCOMPARE(result.inspectedByteCount, quint64(32));
        QVERIFY(result.sourceFullyInspected);
    }

    void clampsInspectedByteCountToProbeSize() {
        // Prefix is 128 KiB, sourceSizeBytes is 200 KiB
        std::vector<std::byte> prefix(128U * 1024U, std::byte{0x00});
        // Populate boxes inside prefix
        for (std::size_t offset = 0; offset + 64 <= prefix.size(); offset += 64) {
            const auto b = makeNormalBox(64, 0x66726565);
            std::copy(b.begin(), b.end(), prefix.begin() + static_cast<std::ptrdiff_t>(offset));
        }

        const auto result = detectMp4Candidate(prefix, 200U * 1024U);
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.inspectedByteCount, mp4DetectionProbeSizeBytes()); // strictly clamped to 64 KiB
        QVERIFY(!result.sourceFullyInspected);
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Strong);
    }

    void rejectsBoxWhoseBodyExtendsPastInspectedByteCount() {
        // 2 complete boxes in first 65500 bytes
        std::vector<std::byte> prefix(64U * 1024U, std::byte{0x00});
        const auto b1 = makeNormalBox(32768, 0x66747970);
        const auto b2 = makeNormalBox(32732, 0x6D6F6F76);
        std::copy(b1.begin(), b1.end(), prefix.begin());
        std::copy(b2.begin(), b2.end(), prefix.begin() + 32768);
        // At offset 65500 (36 bytes before 65536), place a box claiming size 1000 (ends at 66500, beyond 65536)
        const auto b3 = makeNormalBox(1000, 0x6D646174);
        std::copy_n(b3.begin(), 36, prefix.begin() + 65500);

        const auto result = detectMp4Candidate(prefix, 100U * 1024U);
        QVERIFY(result.candidate.has_value());
        // Only b1 and b2 are complete and verified within probe -> Probable (2 boxes)
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Probable);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(2));
    }

    void allEvidenceSpansEndWithinInspectedByteCount() {
        std::vector<std::byte> prefix;
        for (int i = 0; i < 5; ++i) {
            const auto b = makeNormalBox(32, 0x66726565);
            prefix.insert(prefix.end(), b.begin(), b.end());
        }

        const auto result = detectMp4Candidate(prefix, prefix.size());
        QVERIFY(result.candidate.has_value());
        for (const auto& ev : result.candidate->evidence) {
            QVERIFY(ev.boxSpan.has_value());
            QVERIFY(ev.boxSpan->endExclusive().byteOffset() <= result.inspectedByteCount);
        }
    }

    void trailingIncompleteBoxDoesNotDowngradeConfidence() {
        std::vector<std::byte> prefix;
        for (int i = 0; i < 3; ++i) {
            const auto b = makeNormalBox(32, 0x66726565);
            prefix.insert(prefix.end(), b.begin(), b.end());
        }
        // Append incomplete 4th box
        const auto b4 = makeNormalBox(50, 0x6D646174);
        prefix.insert(prefix.end(), b4.begin(), b4.begin() + 10);

        const auto result = detectMp4Candidate(prefix, prefix.size());
        QVERIFY(result.candidate.has_value());
        // 3 complete boxes -> Strong
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Strong);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(3));
    }

    void handlesEmptyInput() {
        std::vector<std::byte> empty;
        const auto result = detectMp4Candidate(empty, 0);
        QVERIFY(!result.candidate.has_value());
        QCOMPARE(result.inspectedByteCount, quint64(0));
        QVERIFY(result.sourceFullyInspected);
    }
};

QTEST_MAIN(Mp4BoxDetectorTest)
#include "mp4_box_detector_test.moc"
