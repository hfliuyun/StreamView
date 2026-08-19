#include <streamview/rules/structural_entry_runner.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/bounded_source_view.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/core/version.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/h264_rbsp_payload_transform_provider.h>
#include <streamview/rules/language_version.h>
#include <streamview/rules/payload_transform.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_execution_session.h>

#include <QFile>
#include <QString>
#include <QStringList>
#include <QtTest>

#include <memory>
#include <vector>

using namespace streamview::core;
using namespace streamview::rules;

namespace {

[[nodiscard]] std::vector<std::byte> toBytes(std::initializer_list<quint8> list) {
    std::vector<std::byte> result;
    result.reserve(list.size());
    for (const quint8 b : list) {
        result.push_back(static_cast<std::byte>(b));
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> toBytes(const std::vector<quint8>& list) {
    std::vector<std::byte> result;
    result.reserve(list.size());
    for (const quint8 b : list) {
        result.push_back(static_cast<std::byte>(b));
    }
    return result;
}

void appendFixedBits(std::vector<bool>& bits, quint64 value, std::size_t bitCount) {
    for (std::size_t index = bitCount; index != 0; --index) {
        bits.push_back(((value >> (index - 1)) & 1U) != 0);
    }
}

void appendUnsignedExpGolomb(std::vector<bool>& bits, quint64 value) {
    const quint64 codeNum = value + 1;
    std::size_t codeBits = 0;
    for (quint64 remaining = codeNum; remaining != 0; remaining >>= 1) {
        ++codeBits;
    }
    for (std::size_t i = 0; i < codeBits - 1; ++i) {
        bits.push_back(false);
    }
    for (std::size_t index = codeBits; index != 0; --index) {
        bits.push_back(((codeNum >> (index - 1)) & 1U) != 0);
    }
}

void appendSignedExpGolomb(std::vector<bool>& bits, qint64 value) {
    const quint64 codeNum = value <= 0 ? static_cast<quint64>(-value) * 2U
                                       : static_cast<quint64>(value) * 2U - 1U;
    appendUnsignedExpGolomb(bits, codeNum);
}

[[nodiscard]] std::vector<std::byte> standaloneSpsNal(quint64 profileIdc = 66, quint64 spsId = 0) {
    std::vector<bool> bits;
    appendFixedBits(bits, profileIdc, 8);
    appendFixedBits(bits, 0, 8);
    appendFixedBits(bits, profileIdc == 100 ? 31 : 30, 8);
    appendUnsignedExpGolomb(bits, spsId);
    if (profileIdc == 100) {
        appendUnsignedExpGolomb(bits, 1);
        appendUnsignedExpGolomb(bits, 0);
        appendUnsignedExpGolomb(bits, 0);
        appendFixedBits(bits, 0, 1);
        appendFixedBits(bits, 0, 1);
    }
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 1);
    appendFixedBits(bits, 0, 1);
    appendUnsignedExpGolomb(bits, 19);
    appendUnsignedExpGolomb(bits, 14);
    appendFixedBits(bits, 1, 1);
    appendFixedBits(bits, 1, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 1, 1); // rbsp_stop_one_bit
    while (bits.size() % 8 != 0) {
        bits.push_back(false);
    }
    std::vector<std::byte> result{static_cast<std::byte>(0x67)}; // NalUnitHeader: ref_idc=3, type=7
    for (std::size_t offset = 0; offset < bits.size(); offset += 8) {
        unsigned int byteVal = 0;
        for (std::size_t b = 0; b < 8; ++b) {
            byteVal = (byteVal << 1U) | static_cast<unsigned int>(bits.at(offset + b));
        }
        result.push_back(static_cast<std::byte>(byteVal));
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> standalonePpsNal(quint64 ppsId = 0, quint64 spsId = 0) {
    std::vector<bool> bits;
    appendUnsignedExpGolomb(bits, ppsId);
    appendUnsignedExpGolomb(bits, spsId);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 0);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 2);
    appendSignedExpGolomb(bits, 0);
    appendSignedExpGolomb(bits, 0);
    appendSignedExpGolomb(bits, 0);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 1, 1); // rbsp_stop_one_bit
    while (bits.size() % 8 != 0) {
        bits.push_back(false);
    }
    std::vector<std::byte> result{static_cast<std::byte>(0x68)}; // NalUnitHeader: ref_idc=3, type=8
    for (std::size_t offset = 0; offset < bits.size(); offset += 8) {
        unsigned int byteVal = 0;
        for (std::size_t b = 0; b < 8; ++b) {
            byteVal = (byteVal << 1U) | static_cast<unsigned int>(bits.at(offset + b));
        }
        result.push_back(static_cast<std::byte>(byteVal));
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> standaloneSpsNalWithSarEmulation() {
    std::vector<bool> bits;
    appendFixedBits(bits, 66, 8); // profile_idc
    appendFixedBits(bits, 0, 8);  // constraint_set_flags
    appendFixedBits(bits, 30, 8); // level_idc
    appendUnsignedExpGolomb(bits, 0); // sps_id = 0
    appendUnsignedExpGolomb(bits, 0); // log2_max_frame_num_minus4
    appendUnsignedExpGolomb(bits, 0); // pic_order_cnt_type
    appendUnsignedExpGolomb(bits, 0); // log2_max_pic_order_cnt_lsb_minus4
    appendUnsignedExpGolomb(bits, 1); // max_num_ref_frames
    appendFixedBits(bits, 0, 1);       // gaps_in_frame_num_value_allowed_flag
    appendUnsignedExpGolomb(bits, 19); // pic_width_in_mbs_minus1
    appendUnsignedExpGolomb(bits, 14); // pic_height_in_map_units_minus1
    appendFixedBits(bits, 1, 1);       // frame_mbs_only_flag
    appendFixedBits(bits, 1, 1);       // direct_8x8_inference_flag
    appendFixedBits(bits, 0, 1);       // frame_cropping_flag
    appendFixedBits(bits, 1, 1);       // vui_parameters_present_flag
    appendFixedBits(bits, 1, 1);       // aspect_ratio_info_present_flag
    appendFixedBits(bits, 255, 8);     // aspect_ratio_idc = 255
    appendFixedBits(bits, 0, 16);      // sar_width = 0 (0x0000)
    appendFixedBits(bits, 1, 16);      // sar_height = 1 (0x0001) -> produces 0x00 0x00 0x00 0x01
    appendFixedBits(bits, 0, 1);       // overscan_info_present_flag
    appendFixedBits(bits, 0, 1);       // video_signal_type_present_flag
    appendFixedBits(bits, 0, 1);       // chroma_loc_info_present_flag
    appendFixedBits(bits, 0, 1);       // timing_info_present_flag
    appendFixedBits(bits, 0, 1);       // nal_hrd_parameters_present_flag
    appendFixedBits(bits, 0, 1);       // vcl_hrd_parameters_present_flag
    appendFixedBits(bits, 0, 1);       // pic_struct_present_flag
    appendFixedBits(bits, 0, 1);       // bitstream_restriction_flag
    appendFixedBits(bits, 1, 1);       // rbsp_stop_one_bit
    while (bits.size() % 8 != 0) {
        bits.push_back(false);
    }
    std::vector<std::byte> rawPayload;
    for (std::size_t offset = 0; offset < bits.size(); offset += 8) {
        unsigned int byteVal = 0;
        for (std::size_t b = 0; b < 8; ++b) {
            byteVal = (byteVal << 1U) | static_cast<unsigned int>(bits.at(offset + b));
        }
        rawPayload.push_back(static_cast<std::byte>(byteVal));
    }
    std::vector<std::byte> ebsp;
    ebsp.push_back(static_cast<std::byte>(0x67)); // header
    int zeroCount = 0;
    for (const auto b : rawPayload) {
        const auto val = static_cast<quint8>(b);
        if (zeroCount == 2 && val <= 3) {
            ebsp.push_back(static_cast<std::byte>(0x03));
            zeroCount = 0;
        }
        ebsp.push_back(b);
        if (val == 0) {
            ++zeroCount;
        } else {
            zeroCount = 0;
        }
    }
    return ebsp;
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
        const auto count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.data() + offset, count, destination.data());
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
};

class FaultySource final : public RandomAccessSource {
public:
    FaultySource(std::vector<std::byte> data, quint64 failAtOffset)
        : data_(std::move(data)), failAtOffset_(failAtOffset) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return data_.size(); }
    [[nodiscard]] QString identity() const override { return QStringLiteral("faulty-source"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= sizeBytes()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const quint64 available = sizeBytes() - byteOffset;
        const std::size_t count = static_cast<std::size_t>(
            std::min(static_cast<quint64>(destination.size()), available));
        if (byteOffset + static_cast<quint64>(count) > failAtOffset_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("Injected I/O failure")};
        }
        for (std::size_t i = 0; i < count; ++i) {
            destination[i] = data_[static_cast<std::size_t>(byteOffset + i)];
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
    quint64 failAtOffset_ = 0;
};

enum class MalformedReadMode {
    IncompleteSuccess,
    OversizedSuccess,
};

class MalformedSource final : public RandomAccessSource {
public:
    explicit MalformedSource(MalformedReadMode mode) : mode_(mode) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return 2; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("malformed"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (mode_ == MalformedReadMode::OversizedSuccess) {
            return {SourceReadStatus::Complete, destination.size() + 1, {}};
        }
        return {SourceReadStatus::Complete, destination.size() - 1, {}};
    }

private:
    MalformedReadMode mode_;
};

[[nodiscard]] std::vector<std::byte> readFixtureBytes(const QString& relativePath) {
    const QString basePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/");
    QFile file(basePath + relativePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = file.readAll();
    const auto uSize = static_cast<std::size_t>(data.size());
    std::vector<std::byte> bytes(uSize);
    std::memcpy(bytes.data(), data.constData(), uSize);
    return bytes;
}

} // namespace

class StructuralEntryRunnerTest : public QObject {
    Q_OBJECT

private slots:
    void boundedSourceViewSingleSpan();
    void boundedSourceViewDisjointSpans();
    void boundedSourceViewOutOfBoundsAndEof();
    void boundedSourceViewPropagatesSourceError();
    void boundedSourceViewRejectsMalformedSuccessfulReads();
    void boundedSourceViewRejectsUnalignedSpans();

    void rejectsSequenceEntryKind();
    void rejectsOutOfRangeTargetIndex();
    void rejectsZeroOrUnalignedLogicalLength();
    void rejectsUnalignedSourceSpans();
    void executesLocalMinimalStructSuccess();
    void executesAcrossDisjointPhysicalSpansAndMapsFieldLocations();
    void handlesTruncatedSourceWithPartialTree();
    void handlesUnderlyingSourceError();
    void handlesCancellation();
    void handlesResourceLimits();
    void executesOfficialAacAscOnEsdsFixture();
    void executesCatalogResolvedAacAscOnEsdsFixture();
    void resolvesOfficialH264StandaloneNalEntry();
    void executesOfficialH264StandaloneSpsNal();
    void executesOfficialH264StandalonePpsNalWithContextImport();
    void executesOfficialH264StandalonePpsFailsWithoutSpsContext();
    void executesOfficialH264StandaloneTruncatedAndMalformedNal();
    void executesOfficialH264StandaloneNalWithEmulationPreventionAndExactCoordinates();
};

void StructuralEntryRunnerTest::boundedSourceViewSingleSpan() {
    MemorySource source(toBytes({0x10, 0x20, 0x30, 0x40, 0x50}));
    const auto span = SourceSpan::create(SourceBitAddress(8), 24); // bytes [1, 4) -> 0x20, 0x30, 0x40
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    BoundedSourceView view(source, *mapping, 3);
    QCOMPARE(view.sizeBytes(), quint64{3});
    QCOMPARE(view.identity(), source.identity());

    std::array<std::byte, 3> dest{};
    const auto readRes = view.readAt(0, dest);
    QCOMPARE(readRes.status, SourceReadStatus::Complete);
    QCOMPARE(readRes.bytesRead, std::size_t{3});
    QCOMPARE(static_cast<quint8>(dest[0]), 0x20);
    QCOMPARE(static_cast<quint8>(dest[1]), 0x30);
    QCOMPARE(static_cast<quint8>(dest[2]), 0x40);
}

void StructuralEntryRunnerTest::boundedSourceViewDisjointSpans() {
    // baseSource: 100 bytes of data
    std::vector<quint8> rawData(100, 0);
    rawData[10] = 0xAA;
    rawData[11] = 0xBB;
    rawData[50] = 0xCC;
    rawData[51] = 0xDD;
    MemorySource source(toBytes(rawData));

    const auto span1 = SourceSpan::create(SourceBitAddress(80), 16);  // bytes [10, 12) -> 0xAA, 0xBB
    const auto span2 = SourceSpan::create(SourceBitAddress(400), 16); // bytes [50, 52) -> 0xCC, 0xDD
    QVERIFY(span1.has_value());
    QVERIFY(span2.has_value());

    const auto mapping = SourceMapping::create(LogicalViewId(2), {*span1, *span2});
    QVERIFY(mapping.has_value());
    QCOMPARE(mapping->logicalBitLength(), quint64{32}); // 4 bytes total

    BoundedSourceView view(source, *mapping, 4);
    QCOMPARE(view.sizeBytes(), quint64{4});

    // Read across disjoint span boundary (e.g. 3 bytes starting from logical offset 1)
    std::array<std::byte, 3> dest{};
    const auto readRes = view.readAt(1, dest);
    QCOMPARE(readRes.status, SourceReadStatus::Complete);
    QCOMPARE(readRes.bytesRead, std::size_t{3});
    QCOMPARE(static_cast<quint8>(dest[0]), 0xBB); // logical byte 1 (span1 byte 1)
    QCOMPARE(static_cast<quint8>(dest[1]), 0xCC); // logical byte 2 (span2 byte 0)
    QCOMPARE(static_cast<quint8>(dest[2]), 0xDD); // logical byte 3 (span2 byte 1)
}

void StructuralEntryRunnerTest::boundedSourceViewOutOfBoundsAndEof() {
    MemorySource source(toBytes({0x01, 0x02, 0x03, 0x04}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 16); // 2 bytes
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(3), {*span});
    QVERIFY(mapping.has_value());

    BoundedSourceView view(source, *mapping, 2);

    // Read past sizeBytes
    std::array<std::byte, 2> dest{};
    const auto eofRes = view.readAt(2, dest);
    QCOMPARE(eofRes.status, SourceReadStatus::EndOfSource);
    QCOMPARE(eofRes.bytesRead, std::size_t{0});

    // Partial read at boundary
    const auto partialRes = view.readAt(1, dest);
    QCOMPARE(partialRes.status, SourceReadStatus::EndOfSource);
    QCOMPARE(partialRes.bytesRead, std::size_t{1});
    QCOMPARE(static_cast<quint8>(dest[0]), 0x02);
}

void StructuralEntryRunnerTest::boundedSourceViewPropagatesSourceError() {
    std::vector<quint8> rawData(100, 0);
    rawData[10] = 0xAA;
    rawData[11] = 0xBB;
    FaultySource source(toBytes(rawData), 50); // Fails while reading the second mapped span.
    const auto firstSpan = SourceSpan::create(SourceBitAddress(80), 16);
    const auto secondSpan = SourceSpan::create(SourceBitAddress(400), 16);
    QVERIFY(firstSpan.has_value());
    QVERIFY(secondSpan.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(4), {*firstSpan, *secondSpan});
    QVERIFY(mapping.has_value());

    BoundedSourceView view(source, *mapping, 4);
    std::array<std::byte, 4> dest{};
    const auto readRes = view.readAt(0, dest);
    QCOMPARE(readRes.status, SourceReadStatus::Error);
    QCOMPARE(readRes.bytesRead, std::size_t{2});
    QCOMPARE(readRes.errorMessage, QStringLiteral("Injected I/O failure"));
}

void StructuralEntryRunnerTest::boundedSourceViewRejectsMalformedSuccessfulReads() {
    const auto span = SourceSpan::create(SourceBitAddress(0), 16);
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(5), {*span});
    QVERIFY(mapping.has_value());
    std::array<std::byte, 2> destination{};

    MalformedSource incompleteSource(MalformedReadMode::IncompleteSuccess);
    BoundedSourceView incompleteView(incompleteSource, *mapping, 2);
    const auto incompleteResult = incompleteView.readAt(0, destination);
    QCOMPARE(incompleteResult.status, SourceReadStatus::Error);
    QCOMPARE(incompleteResult.bytesRead, std::size_t{1});
    QCOMPARE(incompleteResult.errorMessage,
             QStringLiteral("Bounded source view received an incomplete successful read"));

    MalformedSource oversizedSource(MalformedReadMode::OversizedSuccess);
    BoundedSourceView oversizedView(oversizedSource, *mapping, 2);
    const auto oversizedResult = oversizedView.readAt(0, destination);
    QCOMPARE(oversizedResult.status, SourceReadStatus::Error);
    QCOMPARE(oversizedResult.bytesRead, std::size_t{0});
    QCOMPARE(oversizedResult.errorMessage,
             QStringLiteral("Bounded source view received an oversized read"));
}

void StructuralEntryRunnerTest::boundedSourceViewRejectsUnalignedSpans() {
    MemorySource source(toBytes({0xFF, 0xFF}));
    // Non-byte-aligned span (bit start 3)
    const auto span = SourceSpan::create(SourceBitAddress(3), 8);
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(5), {*span});
    QVERIFY(mapping.has_value());

    BoundedSourceView view(source, *mapping, 1);
    std::array<std::byte, 1> dest{};
    const auto readRes = view.readAt(0, dest);
    QCOMPARE(readRes.status, SourceReadStatus::Error);
    QCOMPARE(readRes.errorMessage, QStringLiteral("Bounded source view span is not byte-aligned"));
}

void StructuralEntryRunnerTest::rejectsSequenceEntryKind() {
    const auto parsed = DslParser::parse(
        QStringLiteral("struct Header { bits<8> a; } @index(progressive) sequence<Header> items = scan(h264_start_code); entry items;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    MemorySource source(toBytes({0x01, 0x02}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 16);
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(result.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(result.execution.errorMessage,
             QStringLiteral("Structural entry runner requires DslEntryKind::Structure"));
    QVERIFY(!result.succeeded());
    QVERIFY(result.tree == nullptr);
}

void StructuralEntryRunnerTest::rejectsOutOfRangeTargetIndex() {
    const auto parsed = DslParser::parse(QStringLiteral("struct Header { bits<8> a; } entry Header;"));
    QVERIFY(parsed.succeeded());
    auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    // Corrupt targetIndex
    compiled.program->entry.targetIndex = 999;

    MemorySource source(toBytes({0x01}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 8);
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(result.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(result.execution.errorMessage,
             QStringLiteral("Structural entry targetIndex is out of range"));
}

void StructuralEntryRunnerTest::rejectsZeroOrUnalignedLogicalLength() {
    const auto parsed = DslParser::parse(QStringLiteral("struct Header { bits<8> a; } entry Header;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    MemorySource source(toBytes({0x01}));

    const auto zeroMapping = SourceMapping::create(LogicalViewId(1), {});
    QVERIFY(zeroMapping.has_value());
    const auto zeroResult = StructuralEntryRunner::execute(
        source, *zeroMapping, *compiled.program);
    QCOMPARE(zeroResult.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(zeroResult.execution.errorMessage,
             QStringLiteral("Source mapping logical length is zero"));
    QVERIFY(zeroResult.tree == nullptr);

    // Non-byte-aligned logical length (7 bits)
    const auto unalignedSpan = SourceSpan::create(SourceBitAddress(0), 7);
    const auto unalignedMapping = SourceMapping::create(LogicalViewId(1), {*unalignedSpan});
    QVERIFY(unalignedMapping.has_value());

    const auto unalignedRes = StructuralEntryRunner::execute(source, *unalignedMapping, *compiled.program);
    QCOMPARE(unalignedRes.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(unalignedRes.execution.errorMessage,
             QStringLiteral("Source mapping logical length is not byte-aligned"));
}

void StructuralEntryRunnerTest::rejectsUnalignedSourceSpans() {
    const auto parsed = DslParser::parse(QStringLiteral("struct Header { bits<8> a; } entry Header;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    MemorySource source(toBytes({0xFF, 0xFF}));

    // Unaligned start bit (bit 3, 8 bits long)
    const auto span = SourceSpan::create(SourceBitAddress(3), 8);
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto res = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(res.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(res.execution.errorMessage,
             QStringLiteral("Source mapping spans must be byte-aligned"));
}

void StructuralEntryRunnerTest::executesLocalMinimalStructSuccess() {
    const auto parsed = DslParser::parse(
        QStringLiteral("struct Minimal { bits<8> a; bits<16> b; } entry Minimal;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    MemorySource source(toBytes({0x12, 0x34, 0x56}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 24); // 3 bytes
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(10), {*span});
    QVERIFY(mapping.has_value());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QVERIFY(result.succeeded());
    QCOMPARE(result.execution.status, DslExecutionStatus::Materialized);
    QVERIFY(result.tree != nullptr);

    // Root region contains the decoded structure node
    const auto rootNode = result.tree->node(result.tree->rootId());
    QVERIFY(rootNode.has_value());
    QCOMPARE(rootNode->children().size(), std::size_t{1});

    const auto structNode = result.tree->node(rootNode->children().front());
    QVERIFY(structNode.has_value());
    QCOMPARE(structNode->name(), QStringLiteral("Minimal"));
    QCOMPARE(structNode->children().size(), std::size_t{2});

    const auto fieldA = result.tree->node(structNode->children()[0]);
    QVERIFY(fieldA.has_value());
    QCOMPARE(fieldA->name(), QStringLiteral("a"));
    QCOMPARE(fieldA->value().toULongLong(), quint64{0x12});
    QVERIFY(fieldA->location().has_value());
    QCOMPARE(fieldA->location()->logicalRange().start().bitOffset(), quint64{0});
    QCOMPARE(fieldA->location()->logicalRange().bitLength(), quint64{8});
    QCOMPARE(fieldA->location()->sourceSpans().size(), std::size_t{1});
    QCOMPARE(fieldA->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{0});
    QCOMPARE(fieldA->location()->sourceSpans().front().bitLength(), quint64{8});

    const auto fieldB = result.tree->node(structNode->children()[1]);
    QVERIFY(fieldB.has_value());
    QCOMPARE(fieldB->name(), QStringLiteral("b"));
    QCOMPARE(fieldB->value().toULongLong(), quint64{0x3456});
    QVERIFY(fieldB->location().has_value());
    QCOMPARE(fieldB->location()->logicalRange().start().bitOffset(), quint64{8});
    QCOMPARE(fieldB->location()->logicalRange().bitLength(), quint64{16});
    QCOMPARE(fieldB->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{8});
    QCOMPARE(fieldB->location()->sourceSpans().front().bitLength(), quint64{16});
}

void StructuralEntryRunnerTest::executesAcrossDisjointPhysicalSpansAndMapsFieldLocations() {
    // Root source with data at disjoint offsets:
    // Offset 100 (bits 800..808): 0xAA (field 'first')
    // Offset 200 (bits 1600..1616): 0xBB, 0xCC (field 'second')
    std::vector<quint8> rootData(300, 0);
    rootData[100] = 0xAA;
    rootData[200] = 0xBB;
    rootData[201] = 0xCC;
    MemorySource source(toBytes(rootData));

    const auto span1 = SourceSpan::create(SourceBitAddress(800), 8);   // byte 100
    const auto span2 = SourceSpan::create(SourceBitAddress(1600), 16); // bytes 200..201
    QVERIFY(span1.has_value());
    QVERIFY(span2.has_value());

    const auto mapping = SourceMapping::create(LogicalViewId(20), {*span1, *span2});
    QVERIFY(mapping.has_value());
    QCOMPARE(mapping->logicalBitLength(), quint64{24});

    const auto parsed = DslParser::parse(
        QStringLiteral("struct Disjoint { bits<8> first; bits<16> second; } entry Disjoint;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QVERIFY(result.succeeded());
    QCOMPARE(result.execution.status, DslExecutionStatus::Materialized);

    const auto rootNode = result.tree->node(result.tree->rootId());
    const auto structNode = result.tree->node(rootNode->children().front());
    QCOMPARE(structNode->name(), QStringLiteral("Disjoint"));

    // field 'first' mapped to root source bits [800, 808)
    const auto field1 = result.tree->node(structNode->children()[0]);
    QCOMPARE(field1->name(), QStringLiteral("first"));
    QCOMPARE(field1->value().toULongLong(), quint64{0xAA});
    QVERIFY(field1->location().has_value());
    QCOMPARE(field1->location()->sourceSpans().size(), std::size_t{1});
    QCOMPARE(field1->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{800});
    QCOMPARE(field1->location()->sourceSpans().front().bitLength(), quint64{8});

    // field 'second' mapped to root source bits [1600, 1616)
    const auto field2 = result.tree->node(structNode->children()[1]);
    QCOMPARE(field2->name(), QStringLiteral("second"));
    QCOMPARE(field2->value().toULongLong(), quint64{0xBBCC});
    QVERIFY(field2->location().has_value());
    QCOMPARE(field2->location()->sourceSpans().size(), std::size_t{1});
    QCOMPARE(field2->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1600});
    QCOMPARE(field2->location()->sourceSpans().front().bitLength(), quint64{16});
}

void StructuralEntryRunnerTest::handlesTruncatedSourceWithPartialTree() {
    const auto parsed = DslParser::parse(
        QStringLiteral("struct Large { bits<16> first; bits<16> second; } entry Large;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    // Only 2 bytes provided (for 'first'), 'second' is truncated
    MemorySource source(toBytes({0x11, 0x22}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 16);
    const auto mapping = SourceMapping::create(LogicalViewId(30), {*span});
    QVERIFY(mapping.has_value());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(result.execution.status, DslExecutionStatus::TruncatedSource);
    QVERIFY(!result.succeeded());
    QVERIFY(result.tree != nullptr);

    // Partial nodes are preserved
    const auto rootNode = result.tree->node(result.tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto structNode = result.tree->node(rootNode->children().front());
    QVERIFY(structNode.has_value());
    QCOMPARE(structNode->state(), MaterializationState::Invalid);
    QVERIFY(!structNode->diagnostics().empty());
    QCOMPARE(structNode->diagnostics().front().code, DiagnosticCode::TruncatedSource);
}

void StructuralEntryRunnerTest::handlesUnderlyingSourceError() {
    FaultySource source(toBytes({0x11, 0x22, 0x33, 0x44}), 2);
    const auto span = SourceSpan::create(SourceBitAddress(0), 32);
    const auto mapping = SourceMapping::create(LogicalViewId(40), {*span});
    QVERIFY(mapping.has_value());

    const auto parsed = DslParser::parse(
        QStringLiteral("struct FourBytes { bits<16> a; bits<16> b; } entry FourBytes;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(result.execution.status, DslExecutionStatus::SourceError);
    QVERIFY(!result.succeeded());
}

void StructuralEntryRunnerTest::handlesCancellation() {
    MemorySource source(toBytes({0x11, 0x22, 0x33, 0x44}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 32);
    const auto mapping = SourceMapping::create(LogicalViewId(50), {*span});
    QVERIFY(mapping.has_value());

    const auto parsed = DslParser::parse(
        QStringLiteral("struct CancelTarget { bits<32> a; } entry CancelTarget;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    CancellationSource cancelSource;
    (void)cancelSource.requestCancellation();

    StructuralExecutionOptions options;
    options.cancellation = cancelSource.token();

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program, options);
    QCOMPARE(result.execution.status, DslExecutionStatus::Cancelled);
    QVERIFY(!result.succeeded());
}

void StructuralEntryRunnerTest::handlesResourceLimits() {
    MemorySource source(toBytes({0x01, 0x02, 0x03, 0x04}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 32);
    const auto mapping = SourceMapping::create(LogicalViewId(60), {*span});
    QVERIFY(mapping.has_value());

    const auto parsed = DslParser::parse(
        QStringLiteral("struct MultiField { bits<8> a; bits<8> b; bits<8> c; bits<8> d; } entry MultiField;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    StructuralExecutionOptions options;
    options.limits.maximumInstructions = 2; // Exceeded during execution

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program, options);
    QCOMPARE(result.execution.status, DslExecutionStatus::ResourceLimit);
    QVERIFY(!result.succeeded());
}

void StructuralEntryRunnerTest::executesOfficialAacAscOnEsdsFixture() {
    // 1. Read real fixture mp4_p5h_mp4a_esds.mp4
    const auto fixtureBytes = readFixtureBytes(QStringLiteral("mp4_p5h_mp4a_esds.mp4"));
    QVERIFY(!fixtureBytes.empty());
    MemorySource rootSource(fixtureBytes);

    // In mp4_p5h_mp4a_esds.mp4, AudioSpecificConfig is 2 bytes at root byte offset 146 (0x12 0x10)
    // Bit address: 146 * 8 = 1168 bits. Bit length: 16 bits.
    const auto ascSpan = SourceSpan::create(SourceBitAddress(1168), 16);
    QVERIFY(ascSpan.has_value());
    const auto ascMapping = SourceMapping::create(LogicalViewId(100), {*ascSpan});
    QVERIFY(ascMapping.has_value());

    // 2. Read official org.streamview.aac aac_asc.svfmt rule
    const QString ascRulePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/src/rules/official/org.streamview.aac/src/aac_asc.svfmt");
    QFile ruleFile(ascRulePath);
    QVERIFY2(ruleFile.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(ruleFile.errorString()));
    const QString ruleSource = QString::fromUtf8(ruleFile.readAll());
    QVERIFY(!ruleSource.isEmpty());

    const auto parsed = DslParser::parse(ruleSource);
    QVERIFY2(parsed.succeeded(), "DSL parse failed");
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY2(compiled.succeeded(), "DSL compile failed");

    // 3. Execute StructuralEntryRunner on the official AudioSpecificConfig entrypoint
    const auto result = StructuralEntryRunner::execute(rootSource, *ascMapping, *compiled.program);
    QVERIFY2(result.succeeded(), qPrintable(result.execution.errorMessage));
    QCOMPARE(result.execution.status, DslExecutionStatus::Materialized);
    QVERIFY(result.tree != nullptr);

    // 4. Verify AST hierarchy and exact field values
    const auto rootNode = result.tree->node(result.tree->rootId());
    QVERIFY(rootNode.has_value());
    QCOMPARE(rootNode->children().size(), std::size_t{1});

    const auto ascNode = result.tree->node(rootNode->children().front());
    QVERIFY(ascNode.has_value());
    QCOMPARE(ascNode->name(), QStringLiteral("AudioSpecificConfig"));

    // Find and verify specific fields
    std::optional<AnalysisNode> audioObjectTypeNode;
    std::optional<AnalysisNode> samplingFreqIndexNode;
    std::optional<AnalysisNode> channelConfigNode;
    std::optional<AnalysisNode> frameLengthFlagNode;
    std::optional<AnalysisNode> dependsOnCoreCoderNode;
    std::optional<AnalysisNode> extensionFlagNode;
    QStringList childNames;

    for (const auto childId : ascNode->children()) {
        const auto child = result.tree->node(childId);
        if (!child) continue;
        childNames.push_back(child->name());
        if (child->name() == QStringLiteral("audio_object_type")) audioObjectTypeNode = child;
        else if (child->name() == QStringLiteral("sampling_frequency_index")) samplingFreqIndexNode = child;
        else if (child->name() == QStringLiteral("channel_configuration")) channelConfigNode = child;
        else if (child->name() == QStringLiteral("frame_length_flag")) frameLengthFlagNode = child;
        else if (child->name() == QStringLiteral("depends_on_core_coder")) dependsOnCoreCoderNode = child;
        else if (child->name() == QStringLiteral("extension_flag")) extensionFlagNode = child;
    }
    QCOMPARE(childNames,
             QStringList({QStringLiteral("audio_object_type"),
                          QStringLiteral("sampling_frequency_index"),
                          QStringLiteral("channel_configuration"),
                          QStringLiteral("frame_length_flag"),
                          QStringLiteral("depends_on_core_coder"),
                          QStringLiteral("extension_flag")}));

    // audio_object_type == 2 (AAC LC)
    QVERIFY(audioObjectTypeNode.has_value());
    QCOMPARE(audioObjectTypeNode->value().toULongLong(), quint64{2});
    QVERIFY(audioObjectTypeNode->location().has_value());
    QCOMPARE(audioObjectTypeNode->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1168});
    QCOMPARE(audioObjectTypeNode->location()->sourceSpans().front().bitLength(), quint64{5});

    // sampling_frequency_index == 4 (44100 Hz), starts at bit 1168 + 5 = 1173
    QVERIFY(samplingFreqIndexNode.has_value());
    QCOMPARE(samplingFreqIndexNode->value().toULongLong(), quint64{4});
    QVERIFY(samplingFreqIndexNode->location().has_value());
    QCOMPARE(samplingFreqIndexNode->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1173});
    QCOMPARE(samplingFreqIndexNode->location()->sourceSpans().front().bitLength(), quint64{4});

    // channel_configuration == 2 (stereo), starts at bit 1173 + 4 = 1177
    QVERIFY(channelConfigNode.has_value());
    QCOMPARE(channelConfigNode->value().toULongLong(), quint64{2});
    QVERIFY(channelConfigNode->location().has_value());
    QCOMPARE(channelConfigNode->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1177});
    QCOMPARE(channelConfigNode->location()->sourceSpans().front().bitLength(), quint64{4});

    // frame_length_flag == 0
    QVERIFY(frameLengthFlagNode.has_value());
    QCOMPARE(frameLengthFlagNode->value().toULongLong(), quint64{0});
    QVERIFY(frameLengthFlagNode->location().has_value());
    QCOMPARE(frameLengthFlagNode->location()->sourceSpans().front().start().absoluteBitOffset(),
             quint64{1181});
    QCOMPARE(frameLengthFlagNode->location()->sourceSpans().front().bitLength(), quint64{1});

    // depends_on_core_coder == 0
    QVERIFY(dependsOnCoreCoderNode.has_value());
    QCOMPARE(dependsOnCoreCoderNode->value().toULongLong(), quint64{0});
    QVERIFY(dependsOnCoreCoderNode->location().has_value());
    QCOMPARE(dependsOnCoreCoderNode->location()->sourceSpans().front().start().absoluteBitOffset(),
             quint64{1182});
    QCOMPARE(dependsOnCoreCoderNode->location()->sourceSpans().front().bitLength(), quint64{1});

    // extension_flag == 0
    QVERIFY(extensionFlagNode.has_value());
    QCOMPARE(extensionFlagNode->value().toULongLong(), quint64{0});
    QVERIFY(extensionFlagNode->location().has_value());
    QCOMPARE(extensionFlagNode->location()->sourceSpans().front().start().absoluteBitOffset(),
             quint64{1183});
    QCOMPARE(extensionFlagNode->location()->sourceSpans().front().bitLength(), quint64{1});
}

void StructuralEntryRunnerTest::executesCatalogResolvedAacAscOnEsdsFixture() {
    auto loaded = loadAacAdtsRulePackage();
    QVERIFY2(loaded.succeeded(), qPrintable(loaded.errorMessage));
    QVERIFY(loaded.package.has_value());
    QCOMPARE(loaded.package->identity().packageId(), QStringLiteral("org.streamview.aac"));
    QCOMPARE(loaded.package->identity().packageVersion(), QStringLiteral("0.1.4"));

    RulePackageCatalog catalog;
    const auto registered = catalog.registerPackage(std::move(*loaded.package));
    QVERIFY2(registered.succeeded(), qPrintable(registered.errorMessage));

    const auto resolved = catalog.resolveByFormat(
        u"audio.aac.asc", languageVersion(), streamview::core::version());
    QVERIFY2(resolved.succeeded(), qPrintable(resolved.errorMessage));
    QCOMPARE(resolved.status, RuleCatalogLookupStatus::Found);
    QCOMPARE(resolved.package->identity().packageId(), QStringLiteral("org.streamview.aac"));
    QCOMPARE(resolved.package->identity().packageVersion(), QStringLiteral("0.1.4"));
    QCOMPARE(resolved.entryPoint->id, QStringLiteral("asc"));
    QCOMPARE(resolved.entryPoint->format, QStringLiteral("audio.aac.asc"));
    QCOMPARE(resolved.entryPoint->sourcePath, QStringLiteral("src/aac_asc.svfmt"));
    QCOMPARE(resolved.entryPoint->depth, QStringLiteral("structural"));
    QVERIFY(!resolved.entryPoint->detector.has_value());

    const QByteArray* ruleSource =
        resolved.package->fileContents(resolved.entryPoint->sourcePath);
    QVERIFY(ruleSource != nullptr);
    const auto parsed = DslParser::parse(QString::fromUtf8(*ruleSource));
    QVERIFY2(parsed.succeeded(),
             parsed.diagnostics.empty() ? "" : qPrintable(parsed.diagnostics.front().message));
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY2(compiled.succeeded(),
             compiled.diagnostics.empty() ? ""
                                          : qPrintable(compiled.diagnostics.front().message));
    QCOMPARE(compiled.program->entry.kind, DslEntryKind::Structure);

    const auto fixtureBytes = readFixtureBytes(QStringLiteral("mp4_p5h_mp4a_esds.mp4"));
    QVERIFY(!fixtureBytes.empty());
    MemorySource rootSource(fixtureBytes);
    const auto ascSpan = SourceSpan::create(SourceBitAddress(1168), 16);
    QVERIFY(ascSpan.has_value());
    const auto ascMapping = SourceMapping::create(LogicalViewId(101), {*ascSpan});
    QVERIFY(ascMapping.has_value());

    const auto result =
        StructuralEntryRunner::execute(rootSource, *ascMapping, *compiled.program);
    QVERIFY2(result.succeeded(), qPrintable(result.execution.errorMessage));
    QCOMPARE(result.execution.status, DslExecutionStatus::Materialized);
    QVERIFY(result.tree != nullptr);

    const auto rootNode = result.tree->node(result.tree->rootId());
    QVERIFY(rootNode.has_value());
    QCOMPARE(rootNode->children().size(), std::size_t{1});
    const auto ascNode = result.tree->node(rootNode->children().front());
    QVERIFY(ascNode.has_value());
    QCOMPARE(ascNode->name(), QStringLiteral("AudioSpecificConfig"));

    QStringList childNames;
    for (const auto childId : ascNode->children()) {
        const auto child = result.tree->node(childId);
        QVERIFY(child.has_value());
        childNames.push_back(child->name());
    }
    QCOMPARE(childNames,
             QStringList({QStringLiteral("audio_object_type"),
                          QStringLiteral("sampling_frequency_index"),
                          QStringLiteral("channel_configuration"),
                          QStringLiteral("frame_length_flag"),
                          QStringLiteral("depends_on_core_coder"),
                          QStringLiteral("extension_flag")}));

    const auto firstField = result.tree->node(ascNode->children().front());
    QVERIFY(firstField.has_value());
    QCOMPARE(firstField->value().toULongLong(), quint64{2});
    QVERIFY(firstField->location().has_value());
    QCOMPARE(firstField->location()->sourceSpans().front().start().absoluteBitOffset(),
             quint64{1168});
    QCOMPARE(firstField->location()->sourceSpans().front().bitLength(), quint64{5});
}

void StructuralEntryRunnerTest::resolvesOfficialH264StandaloneNalEntry() {
    auto loaded = loadH264AnnexBRulePackage();
    QVERIFY2(loaded.succeeded(), qPrintable(loaded.errorMessage));
    QVERIFY(loaded.package.has_value());
    QCOMPARE(loaded.package->identity().packageId(), QStringLiteral("org.streamview.h264"));
    QCOMPARE(loaded.package->identity().packageVersion(), QStringLiteral("0.1.40"));

    RulePackageCatalog catalog;
    const auto registered = catalog.registerPackage(std::move(*loaded.package));
    QVERIFY2(registered.succeeded(), qPrintable(registered.errorMessage));

    const auto resolved = catalog.resolveByFormat(
        u"video.h264.nal", languageVersion(), streamview::core::version());
    QVERIFY2(resolved.succeeded(), qPrintable(resolved.errorMessage));
    QCOMPARE(resolved.status, RuleCatalogLookupStatus::Found);
    QCOMPARE(resolved.package->identity().packageId(), QStringLiteral("org.streamview.h264"));
    QCOMPARE(resolved.entryPoint->id, QStringLiteral("nal"));
    QCOMPARE(resolved.entryPoint->format, QStringLiteral("video.h264.nal"));
    QCOMPARE(resolved.entryPoint->sourcePath, QStringLiteral("src/h264_annex_b.svfmt"));
    QCOMPARE(resolved.entryPoint->target, std::optional<QString>(QStringLiteral("NalUnitHeader")));
}

void StructuralEntryRunnerTest::executesOfficialH264StandaloneSpsNal() {
    auto loaded = loadH264AnnexBRulePackage();
    QVERIFY2(loaded.succeeded(), qPrintable(loaded.errorMessage));
    RulePackageCatalog catalog;
    QVERIFY(catalog.registerPackage(std::move(*loaded.package)).succeeded());

    const auto resolved = catalog.resolveByFormat(
        u"video.h264.nal", languageVersion(), streamview::core::version());
    QVERIFY(resolved.succeeded());
    QCOMPARE(resolved.status, RuleCatalogLookupStatus::Found);

    const QByteArray* ruleSource =
        resolved.package->fileContents(resolved.entryPoint->sourcePath);
    QVERIFY(ruleSource != nullptr);
    const auto parsed = DslParser::parse(QString::fromUtf8(*ruleSource));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compileForTarget(
        parsed.program, resolved.entryPoint->target);
    QVERIFY(compiled.succeeded());

    // Single standalone SPS NAL: 0x67 header + SPS payload
    const auto spsBytes = standaloneSpsNal(66, 0); // Baseline, sps_id=0
    MemorySource source(spsBytes);
    const quint64 totalBits = spsBytes.size() * 8U;
    const auto headerSpan = SourceSpan::create(SourceBitAddress(0), 8);
    const auto payloadSpan = SourceSpan::create(SourceBitAddress(8), totalBits - 8);
    const auto enclosingSpan = SourceSpan::create(SourceBitAddress(0), totalBits);
    QVERIFY(headerSpan && payloadSpan && enclosingSpan);

    const auto headerMapping = SourceMapping::create(LogicalViewId(1), {*headerSpan});
    const auto payloadMapping = SourceMapping::create(LogicalViewId(2), {*payloadSpan});
    QVERIFY(headerMapping && payloadMapping);

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));

    auto tree = AnalysisTree::create(QStringLiteral("sps-tree"));
    QVERIFY(tree.has_value());

    RuleExecutionSession session(*compiled.program, 1);
    const auto headerIndex = *compiled.program->structureIndex(QStringLiteral("NalUnitHeader"));

    CompoundRuleExecutionRequest request;
    request.source = &source;
    request.headerMapping = &*headerMapping;
    request.headerStructureIndex = headerIndex;
    request.payloadMapping = &*payloadMapping;
    request.transformRegistry = &registry;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.enclosingSourceSpan = enclosingSpan;
    request.requireExactConsumption = true;
    request.autoDispatchPayload = true;

    const auto result = session.runCompound(request);
    QCOMPARE(result.status, RuleExecutionStatus::Materialized);
    QVERIFY(result.publishedDefinition.has_value());
    QCOMPARE(result.execution.selectedPayloadStructureIndex,
             compiled.program->structureIndex(QStringLiteral("SequenceParameterSetRbsp")));

    // Assert tree structure: root -> NalUnitHeader -> SequenceParameterSetRbsp
    const auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    QCOMPARE(rootNode->children().size(), std::size_t{1});

    const auto headerNode = tree->node(rootNode->children().front());
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->name(), QStringLiteral("NalUnitHeader"));
    QCOMPARE(headerNode->state(), MaterializationState::Materialized);

    // Find payload child under NalUnitHeader
    const auto spsChildIt = std::find_if(
        headerNode->children().begin(), headerNode->children().end(), [&](const auto id) {
            const auto node = tree->node(id);
            return node && node->name() == QStringLiteral("SequenceParameterSetRbsp");
        });
    QVERIFY(spsChildIt != headerNode->children().end());

    const auto spsNode = tree->node(*spsChildIt);
    QVERIFY(spsNode.has_value());
    QCOMPARE(spsNode->state(), MaterializationState::Materialized);

    // Assert SPS fields
    const auto findField = [&](const auto& parent, const QString& name) {
        const auto found = std::find_if(
            parent->children().begin(), parent->children().end(), [&](const auto id) {
                const auto node = tree->node(id);
                return node && node->name() == name;
            });
        return found == parent->children().end() ? std::nullopt : tree->node(*found);
    };

    const auto profileField = findField(spsNode, QStringLiteral("profile_idc"));
    QVERIFY(profileField.has_value());
    QCOMPARE(profileField->value().toULongLong(), quint64{66});
}

void StructuralEntryRunnerTest::executesOfficialH264StandalonePpsNalWithContextImport() {
    auto loaded = loadH264AnnexBRulePackage();
    QVERIFY(loaded.succeeded());
    RulePackageCatalog catalog;
    QVERIFY(catalog.registerPackage(std::move(*loaded.package)).succeeded());

    const auto resolved = catalog.resolveByFormat(
        u"video.h264.nal", languageVersion(), streamview::core::version());
    QVERIFY(resolved.succeeded());
    const QByteArray* ruleSource =
        resolved.package->fileContents(resolved.entryPoint->sourcePath);
    QVERIFY(ruleSource != nullptr);
    const auto parsed = DslParser::parse(QString::fromUtf8(*ruleSource));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compileForTarget(
        parsed.program, resolved.entryPoint->target);
    QVERIFY(compiled.succeeded());

    // Continuous stream with SPS (sps_id=0, profile_idc=66) followed by PPS (pps_id=0, sps_id=0)
    const auto spsBytes = standaloneSpsNal(66, 0);
    const auto ppsBytes = standalonePpsNal(0, 0);
    std::vector<std::byte> streamData;
    streamData.insert(streamData.end(), spsBytes.begin(), spsBytes.end());
    streamData.insert(streamData.end(), ppsBytes.begin(), ppsBytes.end());

    MemorySource source(streamData);
    const quint64 spsOffset = 0;
    const quint64 spsLen = spsBytes.size() * 8U;
    const quint64 ppsOffset = spsLen;
    const quint64 ppsLen = ppsBytes.size() * 8U;

    const auto spsHeaderSpan = SourceSpan::create(SourceBitAddress(spsOffset), 8);
    const auto spsPayloadSpan = SourceSpan::create(SourceBitAddress(spsOffset + 8), spsLen - 8);
    const auto spsEnclosing = SourceSpan::create(SourceBitAddress(spsOffset), spsLen);

    const auto ppsHeaderSpan = SourceSpan::create(SourceBitAddress(ppsOffset), 8);
    const auto ppsPayloadSpan = SourceSpan::create(SourceBitAddress(ppsOffset + 8), ppsLen - 8);
    const auto ppsEnclosing = SourceSpan::create(SourceBitAddress(ppsOffset), ppsLen);

    const auto spsHeaderMapping = SourceMapping::create(LogicalViewId(1), {*spsHeaderSpan});
    const auto spsPayloadMapping = SourceMapping::create(LogicalViewId(2), {*spsPayloadSpan});
    const auto ppsHeaderMapping = SourceMapping::create(LogicalViewId(3), {*ppsHeaderSpan});
    const auto ppsPayloadMapping = SourceMapping::create(LogicalViewId(4), {*ppsPayloadSpan});

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));

    auto tree = AnalysisTree::create(QStringLiteral("chain-tree"));
    QVERIFY(tree.has_value());

    RuleExecutionSession session(*compiled.program, 1);
    const auto headerIndex = *compiled.program->structureIndex(QStringLiteral("NalUnitHeader"));

    // 1. Run SPS
    CompoundRuleExecutionRequest spsReq;
    spsReq.source = &source;
    spsReq.headerMapping = &*spsHeaderMapping;
    spsReq.headerStructureIndex = headerIndex;
    spsReq.payloadMapping = &*spsPayloadMapping;
    spsReq.transformRegistry = &registry;
    spsReq.tree = &*tree;
    spsReq.parentId = tree->rootId();
    spsReq.enclosingSourceSpan = spsEnclosing;
    spsReq.requireExactConsumption = true;
    spsReq.autoDispatchPayload = true;

    const auto spsRes = session.runCompound(spsReq);
    QCOMPARE(spsRes.status, RuleExecutionStatus::Materialized);
    QVERIFY(spsRes.publishedDefinition.has_value());

    // 2. Run PPS in same session (imports SPS context)
    CompoundRuleExecutionRequest ppsReq;
    ppsReq.source = &source;
    ppsReq.headerMapping = &*ppsHeaderMapping;
    ppsReq.headerStructureIndex = headerIndex;
    ppsReq.payloadMapping = &*ppsPayloadMapping;
    ppsReq.transformRegistry = &registry;
    ppsReq.tree = &*tree;
    ppsReq.parentId = tree->rootId();
    ppsReq.enclosingSourceSpan = ppsEnclosing;
    ppsReq.requireExactConsumption = true;
    ppsReq.autoDispatchPayload = true;

    const auto ppsRes = session.runCompound(ppsReq);
    QCOMPARE(ppsRes.status, RuleExecutionStatus::Materialized);
    QVERIFY(ppsRes.publishedDefinition.has_value());
    QCOMPARE(ppsRes.execution.selectedPayloadStructureIndex,
             compiled.program->structureIndex(QStringLiteral("PictureParameterSetRbsp")));
}

void StructuralEntryRunnerTest::executesOfficialH264StandalonePpsFailsWithoutSpsContext() {
    auto loaded = loadH264AnnexBRulePackage();
    QVERIFY(loaded.succeeded());
    RulePackageCatalog catalog;
    QVERIFY(catalog.registerPackage(std::move(*loaded.package)).succeeded());

    const auto resolved = catalog.resolveByFormat(
        u"video.h264.nal", languageVersion(), streamview::core::version());
    QVERIFY(resolved.succeeded());
    const QByteArray* ruleSource =
        resolved.package->fileContents(resolved.entryPoint->sourcePath);
    QVERIFY(ruleSource != nullptr);
    const auto parsed = DslParser::parse(QString::fromUtf8(*ruleSource));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compileForTarget(
        parsed.program, resolved.entryPoint->target);
    QVERIFY(compiled.succeeded());

    // Standalone PPS NAL without any prior SPS
    const auto ppsBytes = standalonePpsNal(0, 0);
    MemorySource source(ppsBytes);
    const quint64 ppsLen = ppsBytes.size() * 8U;
    const auto ppsHeaderSpan = SourceSpan::create(SourceBitAddress(0), 8);
    const auto ppsPayloadSpan = SourceSpan::create(SourceBitAddress(8), ppsLen - 8);
    const auto ppsEnclosing = SourceSpan::create(SourceBitAddress(0), ppsLen);

    const auto ppsHeaderMapping = SourceMapping::create(LogicalViewId(1), {*ppsHeaderSpan});
    const auto ppsPayloadMapping = SourceMapping::create(LogicalViewId(2), {*ppsPayloadSpan});

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));

    auto tree = AnalysisTree::create(QStringLiteral("pps-isolated-tree"));
    QVERIFY(tree.has_value());

    RuleExecutionSession session(*compiled.program, 1);
    const auto headerIndex = *compiled.program->structureIndex(QStringLiteral("NalUnitHeader"));

    CompoundRuleExecutionRequest ppsReq;
    ppsReq.source = &source;
    ppsReq.headerMapping = &*ppsHeaderMapping;
    ppsReq.headerStructureIndex = headerIndex;
    ppsReq.payloadMapping = &*ppsPayloadMapping;
    ppsReq.transformRegistry = &registry;
    ppsReq.tree = &*tree;
    ppsReq.parentId = tree->rootId();
    ppsReq.enclosingSourceSpan = ppsEnclosing;
    ppsReq.requireExactConsumption = true;
    ppsReq.autoDispatchPayload = true;

    const auto ppsRes = session.runCompound(ppsReq);
    QCOMPARE(ppsRes.status, RuleExecutionStatus::DependencyUnavailable);
    QCOMPARE(ppsRes.publishedDefinition, std::nullopt);
    QCOMPARE(session.publishedDefinitionCount(), std::size_t(0));
}

void StructuralEntryRunnerTest::executesOfficialH264StandaloneTruncatedAndMalformedNal() {
    auto loaded = loadH264AnnexBRulePackage();
    QVERIFY(loaded.succeeded());
    RulePackageCatalog catalog;
    QVERIFY(catalog.registerPackage(std::move(*loaded.package)).succeeded());

    const auto resolved = catalog.resolveByFormat(
        u"video.h264.nal", languageVersion(), streamview::core::version());
    QVERIFY(resolved.succeeded());
    const QByteArray* ruleSource =
        resolved.package->fileContents(resolved.entryPoint->sourcePath);
    QVERIFY(ruleSource != nullptr);
    const auto parsed = DslParser::parse(QString::fromUtf8(*ruleSource));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compileForTarget(
        parsed.program, resolved.entryPoint->target);
    QVERIFY(compiled.succeeded());

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));

    const auto headerIndex = *compiled.program->structureIndex(QStringLiteral("NalUnitHeader"));

    // Case 1: Truncated SPS payload (only header byte 0x67 and 1 payload byte)
    {
        const auto truncBytes = toBytes({0x67, 0x42});
        MemorySource source(truncBytes);
        const auto headerSpan = SourceSpan::create(SourceBitAddress(0), 8);
        const auto payloadSpan = SourceSpan::create(SourceBitAddress(8), 8);
        const auto enclosing = SourceSpan::create(SourceBitAddress(0), 16);
        const auto hMap = SourceMapping::create(LogicalViewId(1), {*headerSpan});
        const auto pMap = SourceMapping::create(LogicalViewId(2), {*payloadSpan});

        auto tree = AnalysisTree::create(QStringLiteral("trunc-tree"));
        RuleExecutionSession session(*compiled.program, 1);

        CompoundRuleExecutionRequest req;
        req.source = &source;
        req.headerMapping = &*hMap;
        req.headerStructureIndex = headerIndex;
        req.payloadMapping = &*pMap;
        req.transformRegistry = &registry;
        req.tree = &*tree;
        req.parentId = tree->rootId();
        req.enclosingSourceSpan = enclosing;
        req.requireExactConsumption = true;
        req.autoDispatchPayload = true;

        const auto res = session.runCompound(req);
        QCOMPARE(res.status, RuleExecutionStatus::TruncatedSource);
        QCOMPARE(session.publishedDefinitionCount(), std::size_t(0));
    }

    // Case 2: Malformed EBSP sequence (00 00 03 04 -> invalid emulation prevention byte)
    {
        const auto malformedBytes = toBytes({0x67, 0x00, 0x00, 0x03, 0x04, 0x00, 0x00});
        MemorySource source(malformedBytes);
        const auto headerSpan = SourceSpan::create(SourceBitAddress(0), 8);
        const auto payloadSpan = SourceSpan::create(SourceBitAddress(8), malformedBytes.size() * 8U - 8);
        const auto enclosing = SourceSpan::create(SourceBitAddress(0), malformedBytes.size() * 8U);
        const auto hMap = SourceMapping::create(LogicalViewId(3), {*headerSpan});
        const auto pMap = SourceMapping::create(LogicalViewId(4), {*payloadSpan});

        auto tree = AnalysisTree::create(QStringLiteral("malformed-tree"));
        RuleExecutionSession session(*compiled.program, 1);

        CompoundRuleExecutionRequest req;
        req.source = &source;
        req.headerMapping = &*hMap;
        req.headerStructureIndex = headerIndex;
        req.payloadMapping = &*pMap;
        req.transformRegistry = &registry;
        req.tree = &*tree;
        req.parentId = tree->rootId();
        req.enclosingSourceSpan = enclosing;
        req.requireExactConsumption = true;
        req.autoDispatchPayload = true;

        const auto res = session.runCompound(req);
        QCOMPARE(res.status, RuleExecutionStatus::InvalidSyntax);
        QCOMPARE(session.publishedDefinitionCount(), std::size_t(0));
    }

    // Case 3: AUD NAL (type 9, 0x69)
    {
        const auto audBytes = toBytes({0x69, 0x10});
        MemorySource source(audBytes);
        const auto headerSpan = SourceSpan::create(SourceBitAddress(0), 8);
        const auto payloadSpan = SourceSpan::create(SourceBitAddress(8), 8);
        const auto enclosing = SourceSpan::create(SourceBitAddress(0), 16);
        const auto hMap = SourceMapping::create(LogicalViewId(5), {*headerSpan});
        const auto pMap = SourceMapping::create(LogicalViewId(6), {*payloadSpan});

        auto tree = AnalysisTree::create(QStringLiteral("aud-tree"));
        RuleExecutionSession session(*compiled.program, 1);

        CompoundRuleExecutionRequest req;
        req.source = &source;
        req.headerMapping = &*hMap;
        req.headerStructureIndex = headerIndex;
        req.payloadMapping = &*pMap;
        req.transformRegistry = &registry;
        req.tree = &*tree;
        req.parentId = tree->rootId();
        req.enclosingSourceSpan = enclosing;
        req.requireExactConsumption = true;
        req.autoDispatchPayload = true;

        const auto res = session.runCompound(req);
        QCOMPARE(res.status, RuleExecutionStatus::Materialized);
        QCOMPARE(res.execution.selectedPayloadStructureIndex,
                 compiled.program->structureIndex(QStringLiteral("AccessUnitDelimiterRbsp")));
    }
}

void StructuralEntryRunnerTest::executesOfficialH264StandaloneNalWithEmulationPreventionAndExactCoordinates() {
    auto loaded = loadH264AnnexBRulePackage();
    QVERIFY(loaded.succeeded());
    RulePackageCatalog catalog;
    QVERIFY(catalog.registerPackage(std::move(*loaded.package)).succeeded());

    const auto resolved = catalog.resolveByFormat(
        u"video.h264.nal", languageVersion(), streamview::core::version());
    QVERIFY(resolved.succeeded());
    const QByteArray* ruleSource =
        resolved.package->fileContents(resolved.entryPoint->sourcePath);
    QVERIFY(ruleSource != nullptr);
    const auto parsed = DslParser::parse(QString::fromUtf8(*ruleSource));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compileForTarget(
        parsed.program, resolved.entryPoint->target);
    QVERIFY(compiled.succeeded());

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));

    const auto spsBytes = standaloneSpsNalWithSarEmulation();
    MemorySource source(spsBytes);
    const quint64 totalBits = spsBytes.size() * 8U;
    const auto headerSpan = SourceSpan::create(SourceBitAddress(0), 8);
    const auto payloadSpan = SourceSpan::create(SourceBitAddress(8), totalBits - 8);
    const auto enclosing = SourceSpan::create(SourceBitAddress(0), totalBits);
    const auto hMap = SourceMapping::create(LogicalViewId(1), {*headerSpan});
    const auto pMap = SourceMapping::create(LogicalViewId(2), {*payloadSpan});

    auto tree = AnalysisTree::create(QStringLiteral("sps-emulation-tree"));
    QVERIFY(tree.has_value());

    RuleExecutionSession session(*compiled.program, 1);
    const auto headerIndex = *compiled.program->structureIndex(QStringLiteral("NalUnitHeader"));

    CompoundRuleExecutionRequest req;
    req.source = &source;
    req.headerMapping = &*hMap;
    req.headerStructureIndex = headerIndex;
    req.payloadMapping = &*pMap;
    req.transformRegistry = &registry;
    req.tree = &*tree;
    req.parentId = tree->rootId();
    req.enclosingSourceSpan = enclosing;
    req.requireExactConsumption = true;
    req.autoDispatchPayload = true;

    const auto res = session.runCompound(req);
    QCOMPARE(res.status, RuleExecutionStatus::Materialized);
    QVERIFY(res.publishedDefinition.has_value());
    QVERIFY(!res.execution.excludedSpans.empty());

    // Locate the SPS node and its SAR fields
    const auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto headerNode = tree->node(rootNode->children().front());
    QVERIFY(headerNode.has_value());

    const auto spsChildIt = std::find_if(
        headerNode->children().begin(), headerNode->children().end(), [&](const auto id) {
            const auto node = tree->node(id);
            return node && node->name() == QStringLiteral("SequenceParameterSetRbsp");
        });
    QVERIFY(spsChildIt != headerNode->children().end());
    const auto spsNode = tree->node(*spsChildIt);
    QVERIFY(spsNode.has_value());

    const auto findField = [&](const auto& parent, const QString& name) {
        const auto found = std::find_if(
            parent->children().begin(), parent->children().end(), [&](const auto id) {
                const auto node = tree->node(id);
                return node && node->name() == name;
            });
        return found == parent->children().end() ? std::nullopt : tree->node(*found);
    };

    const auto sarWidth = findField(spsNode, QStringLiteral("sar_width"));
    const auto sarHeight = findField(spsNode, QStringLiteral("sar_height"));
    QVERIFY(sarWidth.has_value());
    QVERIFY(sarHeight.has_value());
    QCOMPARE(sarWidth->value().toULongLong(), quint64{0});
    QCOMPARE(sarHeight->value().toULongLong(), quint64{1});

    // Excluded span verification: ensure 0x03 emulation prevention byte does not forge physical coordinates
    const auto& excluded = res.execution.excludedSpans.front();
    QCOMPARE(excluded.sourceSpan.bitLength(), quint64{8});
    const quint64 excludedOffset = excluded.sourceSpan.start().absoluteBitOffset();
    QVERIFY(excludedOffset >= 8 && excludedOffset < totalBits);

    // Logical lengths are exactly 16 bits each
    QCOMPARE(sarWidth->location()->logicalRange().bitLength(), quint64{16});
    QCOMPARE(sarHeight->location()->logicalRange().bitLength(), quint64{16});

    // The fields properly map across the source span containing the excluded 0x03 byte
    const quint64 sarWidthStart = sarWidth->location()->sourceSpans().front().start().absoluteBitOffset();
    const quint64 sarHeightEnd = sarHeight->location()->sourceSpans().back().endExclusive().absoluteBitOffset();
    QVERIFY(sarWidthStart < excludedOffset);
    QVERIFY(sarHeightEnd > excludedOffset);
}

QTEST_MAIN(StructuralEntryRunnerTest)
#include "structural_entry_runner_test.moc"
