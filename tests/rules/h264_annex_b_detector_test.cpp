#include <streamview/rules/h264_annex_b_detector.h>

#include <QTest>

#include <cstddef>
#include <initializer_list>
#include <vector>

using streamview::rules::H264AnnexBDetectionConfidence;
using streamview::rules::detectH264AnnexBCandidate;
using streamview::rules::h264AnnexBDetectionProbeSizeBytes;

namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

} // namespace

class H264AnnexBDetectorTest final : public QObject {
    Q_OBJECT

private slots:
    void recommendsAnnexBFromAStartCodeAndNalHeader() {
        const auto sourcePrefix = bytes({0x00, 0x00, 0x01, 0x65});

        const auto result = detectH264AnnexBCandidate(sourcePrefix, sourcePrefix.size());

        QCOMPARE(result.inspectedByteCount, quint64{4});
        QVERIFY(result.sourceFullyInspected);
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, H264AnnexBDetectionConfidence::Probable);
        QCOMPARE(result.candidate->evidence.size(), std::size_t{1});

        const auto& evidence = result.candidate->evidence.front();
        QVERIFY(evidence.startCode.has_value());
        QCOMPARE(evidence.startCode->start().absoluteBitOffset(), quint64{0});
        QCOMPARE(evidence.startCode->bitLength(), quint64{24});
        QVERIFY(evidence.nalUnitHeader.has_value());
        QCOMPARE(evidence.nalUnitHeader->start().absoluteBitOffset(), quint64{24});
        QCOMPARE(evidence.nalUnitHeader->bitLength(), quint64{8});
        QCOMPARE(evidence.nalUnitType, std::optional<quint8>{5});
        QVERIFY(evidence.forbiddenZeroBitIsZero);
    }

    void raisesConfidenceFromIndependentNalHeaders() {
        const auto sourcePrefix =
            bytes({0x00, 0x00, 0x01, 0x67, 0xAA, 0x00, 0x00, 0x01, 0x68});

        const auto result = detectH264AnnexBCandidate(sourcePrefix, sourcePrefix.size());

        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, H264AnnexBDetectionConfidence::Strong);
        QCOMPARE(result.candidate->evidence.size(), std::size_t{2});
        QCOMPARE(result.candidate->evidence[0].nalUnitType, std::optional<quint8>{7});
        QCOMPARE(result.candidate->evidence[1].nalUnitType, std::optional<quint8>{8});
        QCOMPARE(result.candidate->evidence[1].startCode->start().byteOffset(), quint64{5});
    }

    void keepsInvalidOrUnspecifiedNalHeadersAtWeakConfidence() {
        const auto sourcePrefix =
            bytes({0x00, 0x00, 0x01, 0xE5, 0x00, 0x00, 0x01, 0x18});

        const auto result = detectH264AnnexBCandidate(sourcePrefix, sourcePrefix.size());

        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, H264AnnexBDetectionConfidence::Weak);
        QCOMPARE(result.candidate->evidence.size(), std::size_t{2});
        QVERIFY(!result.candidate->evidence[0].forbiddenZeroBitIsZero);
        QCOMPARE(result.candidate->evidence[1].nalUnitType, std::optional<quint8>{24});
    }

    void neverInspectsBeyondTheDeclaredProbeBudget() {
        const quint64 probeSize = h264AnnexBDetectionProbeSizeBytes();
        QCOMPARE(probeSize, quint64{64U * 1024U});
        std::vector<std::byte> sourcePrefix(static_cast<std::size_t>(probeSize) + 4U,
                                            std::byte{0xFF});
        sourcePrefix[static_cast<std::size_t>(probeSize)] = std::byte{0x00};
        sourcePrefix[static_cast<std::size_t>(probeSize) + 1U] = std::byte{0x00};
        sourcePrefix[static_cast<std::size_t>(probeSize) + 2U] = std::byte{0x01};
        sourcePrefix[static_cast<std::size_t>(probeSize) + 3U] = std::byte{0x65};

        const auto result = detectH264AnnexBCandidate(sourcePrefix, sourcePrefix.size());

        QCOMPARE(result.inspectedByteCount, probeSize);
        QVERIFY(!result.sourceFullyInspected);
        QVERIFY(!result.candidate.has_value());
    }

    void retainsATruncatedFourByteStartCodeAsWeakEvidence() {
        const auto sourcePrefix = bytes({0x00, 0x00, 0x00, 0x01});

        const auto result = detectH264AnnexBCandidate(sourcePrefix, sourcePrefix.size());

        QVERIFY(result.sourceFullyInspected);
        QVERIFY(result.candidate.has_value());
        QCOMPARE(result.candidate->confidence, H264AnnexBDetectionConfidence::Weak);
        QCOMPARE(result.candidate->evidence.size(), std::size_t{1});
        QCOMPARE(result.candidate->evidence.front().startCode->bitLength(), quint64{32});
        QVERIFY(!result.candidate->evidence.front().nalUnitHeader.has_value());
        QVERIFY(!result.candidate->evidence.front().nalUnitType.has_value());
    }

    void reportsNoCandidateForBytesWithoutAStructuralSignature() {
        const auto sourcePrefix = bytes({0x12, 0x00, 0x00, 0x02, 0x34});

        const auto result = detectH264AnnexBCandidate(sourcePrefix, sourcePrefix.size());

        QCOMPARE(result.inspectedByteCount, quint64{5});
        QVERIFY(result.sourceFullyInspected);
        QVERIFY(!result.candidate.has_value());
    }
};

QTEST_GUILESS_MAIN(H264AnnexBDetectorTest)

#include "h264_annex_b_detector_test.moc"
