#include <streamview/rules/aac_adts_detector.h>

#include <QObject>
#include <QTest>

#include <cstddef>
#include <vector>

namespace {

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
    // Byte 0: 0xFF
    frame[0] = std::byte{0xFF};
    // Byte 1: 0xF0 | (id=0 << 3) | (layer=0 << 1) | (protectionAbsent ? 1 : 0)
    frame[1] = std::byte{static_cast<quint8>(0xF0U | (protectionAbsent ? 1U : 0U))};
    // Byte 2: (profile << 6) | (samplingFreqIndex << 2) | (private_bit=0) | (channelConfig >> 2)
    frame[2] = std::byte{static_cast<quint8>(
        ((profile & 0x03U) << 6U) |
        ((samplingFreqIndex & 0x0FU) << 2U) |
        ((channelConfig >> 2U) & 0x01U))};
    // Byte 3: ((channelConfig & 0x03) << 6) | (copy=0) | (home=0) | (copy_id=0) | (copy_start=0) | (frameLength >> 11)
    frame[3] = std::byte{static_cast<quint8>(
        ((channelConfig & 0x03U) << 6U) |
        ((frameLength >> 11U) & 0x03U))};
    // Byte 4: (frameLength >> 3) & 0xFF
    frame[4] = std::byte{static_cast<quint8>((frameLength >> 3U) & 0xFFU)};
    // Byte 5: ((frameLength & 0x07) << 5) | (buffer_fullness=0x7FF >> 6)
    frame[5] = std::byte{static_cast<quint8>(
        ((frameLength & 0x07U) << 5U) | 0x1FU)};
    // Byte 6: ((buffer_fullness=0x7FF & 0x3F) << 2) | (raw_blocks=0)
    frame[6] = std::byte{0xFC};
    if (!protectionAbsent) {
        frame[7] = std::byte{0x12};
        frame[8] = std::byte{0x34};
    }
    return frame;
}

} // namespace

class AacAdtsDetectorTest : public QObject {
    Q_OBJECT

private slots:
    void detectsCleanAdtsStreamWithStrongConfidence() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(150, true);
        const auto f2 = makeAdtsFrame(200, true);
        const auto f3 = makeAdtsFrame(180, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const auto result = streamview::rules::detectAacAdtsCandidate(
            stream, static_cast<quint64>(stream.size()));
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence,
                 streamview::rules::AacAdtsDetectionConfidence::Strong);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(3));
        QCOMPARE(result.candidate->evidence[0].aacFrameLength, quint16(150));
        QCOMPARE(result.candidate->evidence[0].profile, quint8(1));
        QCOMPARE(result.candidate->evidence[0].samplingFrequencyIndex, quint8(4));
        QCOMPARE(result.candidate->evidence[0].channelConfiguration, quint8(2));
    }

    void detectsShortTwoFrameStreamWithProbableOrStrong() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(120, false);
        const auto f2 = makeAdtsFrame(160, false);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());

        const auto result = streamview::rules::detectAacAdtsCandidate(
            stream, static_cast<quint64>(stream.size()));
        QVERIFY(result.candidate.has_value());
        QVERIFY(result.candidate->confidence ==
                    streamview::rules::AacAdtsDetectionConfidence::Strong ||
                result.candidate->confidence ==
                    streamview::rules::AacAdtsDetectionConfidence::Probable);
        QCOMPARE(result.candidate->evidence.size(), std::size_t(2));
        QCOMPARE(result.candidate->evidence[0].crcPresent, true);
        QCOMPARE(result.candidate->evidence[1].crcPresent, true);
    }

    void detectsSingleFrameWithWeakOrProbable() {
        const auto stream = makeAdtsFrame(150, true);
        const auto result = streamview::rules::detectAacAdtsCandidate(
            stream, static_cast<quint64>(stream.size()));
        QVERIFY(result.candidate.has_value());
        QVERIFY(result.candidate->confidence ==
                    streamview::rules::AacAdtsDetectionConfidence::Weak ||
                result.candidate->confidence ==
                    streamview::rules::AacAdtsDetectionConfidence::Probable);
    }

    void detectsAdtsStreamWithGarbagePrefix() {
        std::vector<std::byte> stream(64, std::byte{0x55});
        const auto f1 = makeAdtsFrame(100, true);
        const auto f2 = makeAdtsFrame(100, true);
        const auto f3 = makeAdtsFrame(100, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        const auto result = streamview::rules::detectAacAdtsCandidate(
            stream, static_cast<quint64>(stream.size()));
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence,
                 streamview::rules::AacAdtsDetectionConfidence::Strong);
    }

    void rejectsPseudoSourceStartingWith0xFFFWithoutLengthChain() {
        // Starts with 0xFF 0xF1 0x50 0x80 0x10 0x1F 0xFC (declares length 32)
        // followed by random non-ADTS data at byte 32
        std::vector<std::byte> stream(128, std::byte{0x00});
        stream[0] = std::byte{0xFF};
        stream[1] = std::byte{0xF1};
        stream[2] = std::byte{0x50};
        stream[3] = std::byte{0x80};
        stream[4] = std::byte{0x10}; // length = 32
        stream[5] = std::byte{0x1F};
        stream[6] = std::byte{0xFC};
        // Byte 32 is 0x00 (not syncword)

        const auto result = streamview::rules::detectAacAdtsCandidate(
            stream, static_cast<quint64>(stream.size()));
        // Must NOT classify as Strong
        if (result.candidate.has_value()) {
            QVERIFY(result.candidate->confidence !=
                    streamview::rules::AacAdtsDetectionConfidence::Strong);
        }
    }

    void rejectsNonAdtsData() {
        const std::vector<std::byte> empty;
        const auto r1 = streamview::rules::detectAacAdtsCandidate(empty, 0);
        QVERIFY(!r1.candidate.has_value());

        const std::vector<std::byte> zeroes(1024, std::byte{0x00});
        const auto r2 = streamview::rules::detectAacAdtsCandidate(
            zeroes, static_cast<quint64>(zeroes.size()));
        QVERIFY(!r2.candidate.has_value());
    }
};

QTEST_MAIN(AacAdtsDetectorTest)
#include "aac_adts_detector_test.moc"
