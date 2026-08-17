#include <streamview/rules/mp4_box_detector.h>

#include <QObject>
#include <QTest>

#include <cstddef>
#include <vector>

using streamview::rules::detectMp4Candidate;
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

    void handlesPrefixSmallerThanSource() {
        std::vector<std::byte> data;
        for (int i = 0; i < 4; ++i) {
            const auto b = makeNormalBox(32, 0x66726565);
            data.insert(data.end(), b.begin(), b.end());
        }

        const auto result = detectMp4Candidate(data, 100'000U);
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, Mp4DetectionConfidence::Strong);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(4));
        QCOMPARE(result.inspectedByteCount, quint64(data.size()));
        QVERIFY(!result.sourceFullyInspected);
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
