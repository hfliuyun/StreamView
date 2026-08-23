#include <streamview/rules/sample_payload_runner.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/sample_descriptor.h>
#include <streamview/core/source.h>
#include <streamview/core/version.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/h264_rbsp_payload_transform_provider.h>
#include <streamview/rules/language_version.h>
#include <streamview/rules/payload_transform.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_execution_session.h>

#include <QString>
#include <QtTest>

#include <algorithm>
#include <memory>
#include <optional>
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

[[nodiscard]] std::vector<std::byte> packBits(const std::vector<bool>& bits) {
    std::vector<std::byte> result;
    for (std::size_t offset = 0; offset < bits.size(); offset += 8) {
        unsigned int byteVal = 0;
        for (std::size_t b = 0; b < 8; ++b) {
            byteVal = (byteVal << 1U) | static_cast<unsigned int>(bits.at(offset + b));
        }
        result.push_back(static_cast<std::byte>(byteVal));
    }
    return result;
}

/// Baseline SPS NAL (header 0x67) with no emulation prevention bytes.
[[nodiscard]] std::vector<std::byte> standaloneSpsNal(quint64 spsId = 0) {
    std::vector<bool> bits;
    appendFixedBits(bits, 66, 8); // profile_idc
    appendFixedBits(bits, 0, 8);  // constraint_set_flags
    appendFixedBits(bits, 30, 8); // level_idc
    appendUnsignedExpGolomb(bits, spsId);
    appendUnsignedExpGolomb(bits, 0);  // log2_max_frame_num_minus4
    appendUnsignedExpGolomb(bits, 0);  // pic_order_cnt_type
    appendUnsignedExpGolomb(bits, 0);  // log2_max_pic_order_cnt_lsb_minus4
    appendUnsignedExpGolomb(bits, 1);  // max_num_ref_frames
    appendFixedBits(bits, 0, 1);       // gaps_in_frame_num_value_allowed_flag
    appendUnsignedExpGolomb(bits, 19); // pic_width_in_mbs_minus1
    appendUnsignedExpGolomb(bits, 14); // pic_height_in_map_units_minus1
    appendFixedBits(bits, 1, 1);       // frame_mbs_only_flag
    appendFixedBits(bits, 1, 1);       // direct_8x8_inference_flag
    appendFixedBits(bits, 0, 1);       // frame_cropping_flag
    appendFixedBits(bits, 0, 1);       // vui_parameters_present_flag
    appendFixedBits(bits, 1, 1);       // rbsp_stop_one_bit
    while (bits.size() % 8 != 0) {
        bits.push_back(false);
    }
    std::vector<std::byte> result{static_cast<std::byte>(0x67)};
    const auto payload = packBits(bits);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

/// Baseline PPS NAL (header 0x68) referring to `spsId`.
[[nodiscard]] std::vector<std::byte> standalonePpsNal(quint64 ppsId = 0, quint64 spsId = 0) {
    std::vector<bool> bits;
    appendUnsignedExpGolomb(bits, ppsId);
    appendUnsignedExpGolomb(bits, spsId);
    appendFixedBits(bits, 0, 1); // entropy_coding_mode_flag
    appendFixedBits(bits, 0, 1); // bottom_field_pic_order_in_frame_present_flag
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
    std::vector<std::byte> result{static_cast<std::byte>(0x68)};
    const auto payload = packBits(bits);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

/// SPS NAL whose SAR fields force an emulation prevention byte into the EBSP, so
/// the RBSP transform must report an excluded span.
[[nodiscard]] std::vector<std::byte> standaloneSpsNalWithSarEmulation() {
    std::vector<bool> bits;
    appendFixedBits(bits, 66, 8);
    appendFixedBits(bits, 0, 8);
    appendFixedBits(bits, 30, 8);
    appendUnsignedExpGolomb(bits, 0);
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
    appendFixedBits(bits, 1, 1);   // vui_parameters_present_flag
    appendFixedBits(bits, 1, 1);   // aspect_ratio_info_present_flag
    appendFixedBits(bits, 255, 8); // aspect_ratio_idc = extended SAR
    appendFixedBits(bits, 0, 16);  // sar_width
    appendFixedBits(bits, 1, 16);  // sar_height -> 0x00 0x00 0x00 0x01
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 1, 1); // rbsp_stop_one_bit
    while (bits.size() % 8 != 0) {
        bits.push_back(false);
    }
    const auto rawPayload = packBits(bits);
    std::vector<std::byte> ebsp{static_cast<std::byte>(0x67)};
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

/// Concatenates `units` with big-endian length prefixes of `prefixBytes` bytes,
/// which is how AVC samples are laid out inside a container.
[[nodiscard]] std::vector<std::byte>
lengthPrefixedSample(const std::vector<std::vector<std::byte>>& units, quint32 prefixBytes) {
    std::vector<std::byte> out;
    for (const auto& unit : units) {
        const auto length = static_cast<quint64>(unit.size());
        for (quint32 index = prefixBytes; index != 0; --index) {
            out.push_back(static_cast<std::byte>((length >> ((index - 1) * 8U)) & 0xFFU));
        }
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
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

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
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
        const auto count = static_cast<std::size_t>(
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

[[nodiscard]] std::optional<SourceSpan> byteSpan(quint64 startByte, quint64 byteLength) {
    return SourceSpan::create(SourceBitAddress(startByte * 8U), byteLength * 8U);
}

[[nodiscard]] SampleDescriptor sampleFor(std::vector<SourceSpan> spans) {
    SampleDescriptor sample;
    sample.trackId = 1;
    sample.sampleIndex = 0;
    sample.sampleDescriptionIndex = 1;
    sample.sourceSpans = std::move(spans);
    sample.dts = 0;
    sample.pts = 0;
    sample.duration = 3000;
    sample.timescale = 90000;
    sample.isSyncSample = true;
    return sample;
}

struct H264Fixture final {
    std::optional<DslTypedProgram> program;
    quint32 headerIndex = 0;
};

/// Compiles the official H.264 NAL entry point once per test that needs it.
[[nodiscard]] H264Fixture loadH264NalProgram() {
    H264Fixture fixture;
    auto loaded = loadH264AnnexBRulePackage();
    if (!loaded.succeeded()) {
        return fixture;
    }
    RulePackageCatalog catalog;
    if (!catalog.registerPackage(std::move(*loaded.package)).succeeded()) {
        return fixture;
    }
    const auto resolved = catalog.resolveByFormat(u"video.h264.nal", languageVersion(),
                                                  streamview::core::version());
    if (!resolved.succeeded()) {
        return fixture;
    }
    const QByteArray* ruleSource = resolved.package->fileContents(resolved.entryPoint->sourcePath);
    if (ruleSource == nullptr) {
        return fixture;
    }
    const auto parsed = DslParser::parse(QString::fromUtf8(*ruleSource));
    if (!parsed.succeeded()) {
        return fixture;
    }
    auto compiled = DslCompiler::compileForTarget(parsed.program, resolved.entryPoint->target);
    if (!compiled.succeeded()) {
        return fixture;
    }
    const auto headerIndex = compiled.program->structureIndex(QStringLiteral("NalUnitHeader"));
    if (!headerIndex) {
        return fixture;
    }
    fixture.headerIndex = *headerIndex;
    fixture.program = std::move(*compiled.program);
    return fixture;
}

[[nodiscard]] std::optional<AnalysisNode> findNamedChild(const AnalysisTree& tree, AnalysisNodeId parentId,
                                                    const QString& name) {
    const auto parent = tree.node(parentId);
    if (!parent) {
        return std::nullopt;
    }
    for (const auto childId : parent->children()) {
        const auto child = tree.node(childId);
        if (child && child->name() == name) {
            return child;
        }
    }
    return std::nullopt;
}

} // namespace

class SamplePayloadRunnerTest : public QObject {
    Q_OBJECT

private slots:
    void framesSingleUnitWithFourBytePrefix();
    void framesMultipleUnitsWithFourBytePrefix();
    void framesUnitsWithOneBytePrefix();
    void framesUnitsWithTwoBytePrefix();
    void framesUnitsAcrossDiscontiguousSampleSpans();
    void framesOpaqueAccessUnitAsSingleUnit();
    void rejectsUnsupportedPrefixLength();
    void rejectsZeroLengthUnit();
    void rejectsUnalignedSampleMapping();
    void rejectsZeroMaximumUnits();
    void reportsTruncatedSampleWhenUnitExceedsBoundary();
    void reportsTruncatedSampleWhenPrefixIsIncomplete();
    void enforcesMaximumUnitBudget();
    void honoursCancellationDuringFraming();
    void reportsSourceErrorFromFaultySource();

    void rejectsRunRequestWithoutSourceOrTree();
    void rejectsSampleWithoutSourceSpans();
    void reportsSampleMetadataFields();
    void runsSingleNalSampleThroughOfficialH264Rule();
    void recordsUnitLengthPrefixAsSyntaxField();
    void runsMultiNalSampleSharingParameterSetContext();
    void preservesRbspExcludedSpansPerUnit();
    void isolatesSingleUnitFailureWithoutFailingSample();
    void marksSampleNodeWhenFramingFails();
    void honoursCancellationBeforeExecution();
    void buildsAccessUnitEnvelopeWithConfigurationReference();
    void reportsDependencyUnavailableWithoutConfiguration();
    void mapsAccessUnitSpansOntoDiscontiguousSample();
};

void SamplePayloadRunnerTest::framesSingleUnitWithFourBytePrefix() {
    const auto nal = standaloneSpsNal();
    const auto bytes = lengthPrefixedSample({nal}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = SamplePayloadFramer::frame(source, *mapping, {});
    QVERIFY2(result.framed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{1});
    QCOMPARE(result.units.front().unitIndex, quint64{0});
    QCOMPARE(result.units.front().prefixByteOffset, quint64{0});
    QCOMPARE(result.units.front().prefixByteLength, quint32{4});
    QCOMPARE(result.units.front().payloadByteOffset, quint64{4});
    QCOMPARE(result.units.front().payloadByteLength, static_cast<quint64>(nal.size()));
    QCOMPARE(result.framedByteCount, static_cast<quint64>(bytes.size()));
}

void SamplePayloadRunnerTest::framesMultipleUnitsWithFourBytePrefix() {
    const auto aud = toBytes({0x09, 0x10});
    const auto sps = standaloneSpsNal();
    const auto pps = standalonePpsNal();
    const auto bytes = lengthPrefixedSample({aud, sps, pps}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = SamplePayloadFramer::frame(source, *mapping, {});
    QVERIFY2(result.framed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{3});
    QCOMPARE(result.units[0].payloadByteLength, static_cast<quint64>(aud.size()));
    QCOMPARE(result.units[1].payloadByteLength, static_cast<quint64>(sps.size()));
    QCOMPARE(result.units[2].payloadByteLength, static_cast<quint64>(pps.size()));
    QCOMPARE(result.units[1].payloadByteOffset, quint64{4 + aud.size() + 4});
    QCOMPARE(result.framedByteCount, static_cast<quint64>(bytes.size()));
}

void SamplePayloadRunnerTest::framesUnitsWithOneBytePrefix() {
    const auto first = toBytes({0x09, 0x10});
    const auto second = toBytes({0x67, 0x42, 0x00, 0x1E});
    const auto bytes = lengthPrefixedSample({first, second}, 1);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    SamplePayloadFramingOptions options;
    options.prefixLengthBytes = 1;
    const auto result = SamplePayloadFramer::frame(source, *mapping, options);
    QVERIFY2(result.framed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{2});
    QCOMPARE(result.units[0].prefixByteLength, quint32{1});
    QCOMPARE(result.units[0].payloadByteOffset, quint64{1});
    QCOMPARE(result.units[0].payloadByteLength, quint64{2});
    QCOMPARE(result.units[1].prefixByteOffset, quint64{3});
    QCOMPARE(result.units[1].payloadByteLength, quint64{4});
}

void SamplePayloadRunnerTest::framesUnitsWithTwoBytePrefix() {
    const auto first = standaloneSpsNal();
    const auto second = standalonePpsNal();
    const auto bytes = lengthPrefixedSample({first, second}, 2);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    SamplePayloadFramingOptions options;
    options.prefixLengthBytes = 2;
    const auto result = SamplePayloadFramer::frame(source, *mapping, options);
    QVERIFY2(result.framed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{2});
    QCOMPARE(result.units[0].prefixByteLength, quint32{2});
    QCOMPARE(result.units[0].payloadByteLength, static_cast<quint64>(first.size()));
    QCOMPARE(result.units[1].payloadByteLength, static_cast<quint64>(second.size()));
}

void SamplePayloadRunnerTest::framesUnitsAcrossDiscontiguousSampleSpans() {
    // Physical bytes 0..3 and 8..11 form one logical 8-byte sample.
    std::vector<std::byte> raw(16, static_cast<std::byte>(0xAA));
    raw[0] = static_cast<std::byte>(3);
    raw[1] = static_cast<std::byte>(0x67);
    raw[2] = static_cast<std::byte>(0x42);
    raw[3] = static_cast<std::byte>(0x1E);
    raw[8] = static_cast<std::byte>(3);
    raw[9] = static_cast<std::byte>(0x68);
    raw[10] = static_cast<std::byte>(0xCE);
    raw[11] = static_cast<std::byte>(0x3C);
    MemorySource source(raw);

    const auto firstSpan = byteSpan(0, 4);
    const auto secondSpan = byteSpan(8, 4);
    QVERIFY(firstSpan && secondSpan);
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*firstSpan, *secondSpan});
    QVERIFY(mapping.has_value());
    QCOMPARE(mapping->logicalBitLength(), quint64{64});

    SamplePayloadFramingOptions options;
    options.prefixLengthBytes = 1;
    const auto result = SamplePayloadFramer::frame(source, *mapping, options);
    QVERIFY2(result.framed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{2});
    QCOMPARE(result.units[1].prefixByteOffset, quint64{4});
    QCOMPARE(result.units[1].payloadByteOffset, quint64{5});
    QCOMPARE(result.units[1].payloadByteLength, quint64{3});

    // The second unit's payload must resolve back to physical bytes 9..11.
    const auto range = LogicalRange::create(LogicalBitAddress(LogicalViewId(1), 5U * 8U), 3U * 8U);
    QVERIFY(range.has_value());
    const auto located = mapping->locate(*range);
    QVERIFY(located.has_value());
    QCOMPARE(located->sourceSpans().size(), std::size_t{1});
    QCOMPARE(located->sourceSpans().front().start().absoluteBitOffset(), quint64{9U * 8U});
}

void SamplePayloadRunnerTest::framesOpaqueAccessUnitAsSingleUnit() {
    const auto bytes = toBytes({0x21, 0x1A, 0x4B, 0x7F, 0x00, 0x11});
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    SamplePayloadFramingOptions options;
    options.framing = SamplePayloadFraming::OpaqueAccessUnit;
    const auto result = SamplePayloadFramer::frame(source, *mapping, options);
    QVERIFY2(result.framed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{1});
    QCOMPARE(result.units.front().prefixByteLength, quint32{0});
    QCOMPARE(result.units.front().payloadByteOffset, quint64{0});
    QCOMPARE(result.units.front().payloadByteLength, static_cast<quint64>(bytes.size()));
}

void SamplePayloadRunnerTest::rejectsUnsupportedPrefixLength() {
    const auto bytes = toBytes({0x00, 0x00, 0x00, 0x02, 0x67, 0x42});
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    for (const quint32 illegal : {quint32{0}, quint32{3}, quint32{5}, quint32{8}}) {
        SamplePayloadFramingOptions options;
        options.prefixLengthBytes = illegal;
        const auto result = SamplePayloadFramer::frame(source, *mapping, options);
        QVERIFY(!result.framed());
        QCOMPARE(result.status, SamplePayloadFramingStatus::UnsupportedFraming);
    }
}

void SamplePayloadRunnerTest::rejectsZeroLengthUnit() {
    const auto bytes = toBytes({0x00, 0x00, 0x00, 0x00, 0x67, 0x42});
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = SamplePayloadFramer::frame(source, *mapping, {});
    QVERIFY(!result.framed());
    QCOMPARE(result.status, SamplePayloadFramingStatus::InvalidUnitLength);
}

void SamplePayloadRunnerTest::rejectsUnalignedSampleMapping() {
    const auto bytes = toBytes({0x00, 0x00, 0x00, 0x02, 0x67, 0x42});
    MemorySource source(bytes);

    // Unaligned start bit.
    const auto shifted = SourceSpan::create(SourceBitAddress(4), 32);
    QVERIFY(shifted.has_value());
    const auto shiftedMapping = SourceMapping::create(LogicalViewId(1), {*shifted});
    QVERIFY(shiftedMapping.has_value());
    const auto shiftedResult = SamplePayloadFramer::frame(source, *shiftedMapping, {});
    QVERIFY(!shiftedResult.framed());
    QCOMPARE(shiftedResult.status, SamplePayloadFramingStatus::InvalidRequest);

    // Length that is not a whole number of bytes.
    const auto partial = SourceSpan::create(SourceBitAddress(0), 12);
    QVERIFY(partial.has_value());
    const auto partialMapping = SourceMapping::create(LogicalViewId(1), {*partial});
    QVERIFY(partialMapping.has_value());
    const auto partialResult = SamplePayloadFramer::frame(source, *partialMapping, {});
    QVERIFY(!partialResult.framed());
    QCOMPARE(partialResult.status, SamplePayloadFramingStatus::InvalidRequest);
}

void SamplePayloadRunnerTest::rejectsZeroMaximumUnits() {
    const auto bytes = lengthPrefixedSample({standaloneSpsNal()}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    SamplePayloadFramingOptions options;
    options.maximumUnits = 0;
    const auto result = SamplePayloadFramer::frame(source, *mapping, options);
    QVERIFY(!result.framed());
    QCOMPARE(result.status, SamplePayloadFramingStatus::InvalidRequest);
}

void SamplePayloadRunnerTest::reportsTruncatedSampleWhenUnitExceedsBoundary() {
    // Declares 16 bytes but only 2 follow the prefix.
    const auto bytes = toBytes({0x00, 0x00, 0x00, 0x10, 0x67, 0x42});
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = SamplePayloadFramer::frame(source, *mapping, {});
    QVERIFY(!result.framed());
    QCOMPARE(result.status, SamplePayloadFramingStatus::TruncatedSample);
    QVERIFY(result.units.empty());
}

void SamplePayloadRunnerTest::reportsTruncatedSampleWhenPrefixIsIncomplete() {
    // One complete 2-byte unit, then a stray single byte that cannot hold a
    // 4-byte prefix.
    const auto bytes = toBytes({0x00, 0x00, 0x00, 0x02, 0x67, 0x42, 0x00});
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = SamplePayloadFramer::frame(source, *mapping, {});
    QVERIFY(!result.framed());
    QCOMPARE(result.status, SamplePayloadFramingStatus::TruncatedSample);
}

void SamplePayloadRunnerTest::enforcesMaximumUnitBudget() {
    const auto unit = toBytes({0x09, 0x10});
    const auto bytes = lengthPrefixedSample({unit, unit, unit, unit}, 1);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    SamplePayloadFramingOptions options;
    options.prefixLengthBytes = 1;
    options.maximumUnits = 2;
    const auto result = SamplePayloadFramer::frame(source, *mapping, options);
    QVERIFY(!result.framed());
    QCOMPARE(result.status, SamplePayloadFramingStatus::ResourceLimit);
}

void SamplePayloadRunnerTest::honoursCancellationDuringFraming() {
    const auto bytes = lengthPrefixedSample({standaloneSpsNal()}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    CancellationSource cancellation;
    QVERIFY(cancellation.requestCancellation());
    SamplePayloadFramingOptions options;
    options.cancellation = cancellation.token();
    const auto result = SamplePayloadFramer::frame(source, *mapping, options);
    QVERIFY(!result.framed());
    QCOMPARE(result.status, SamplePayloadFramingStatus::Cancelled);
}

void SamplePayloadRunnerTest::reportsSourceErrorFromFaultySource() {
    const auto bytes = lengthPrefixedSample({standaloneSpsNal()}, 4);
    FaultySource source(bytes, 0);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = SamplePayloadFramer::frame(source, *mapping, {});
    QVERIFY(!result.framed());
    QCOMPARE(result.status, SamplePayloadFramingStatus::SourceError);
}

void SamplePayloadRunnerTest::rejectsRunRequestWithoutSourceOrTree() {
    const auto bytes = lengthPrefixedSample({standaloneSpsNal()}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});
    auto tree = AnalysisTree::create(QStringLiteral("no-source"));
    QVERIFY(tree.has_value());

    SamplePayloadRunRequest missingSource;
    missingSource.sample = &sample;
    missingSource.tree = &*tree;
    missingSource.parentId = tree->rootId();
    QCOMPARE(SamplePayloadRunner::run(missingSource).status,
             SamplePayloadRunStatus::InvalidRequest);

    SamplePayloadRunRequest missingTree;
    missingTree.source = &source;
    missingTree.sample = &sample;
    QCOMPARE(SamplePayloadRunner::run(missingTree).status, SamplePayloadRunStatus::InvalidRequest);

    SamplePayloadRunRequest missingSample;
    missingSample.source = &source;
    missingSample.tree = &*tree;
    missingSample.parentId = tree->rootId();
    QCOMPARE(SamplePayloadRunner::run(missingSample).status,
             SamplePayloadRunStatus::InvalidRequest);
}

void SamplePayloadRunnerTest::rejectsSampleWithoutSourceSpans() {
    const auto bytes = lengthPrefixedSample({standaloneSpsNal()}, 4);
    MemorySource source(bytes);
    const auto sample = sampleFor({});
    auto tree = AnalysisTree::create(QStringLiteral("no-spans"));
    QVERIFY(tree.has_value());

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.tree = &*tree;
    request.parentId = tree->rootId();

    const auto result = SamplePayloadRunner::run(request);
    QCOMPARE(result.status, SamplePayloadRunStatus::InvalidRequest);
    QVERIFY(!result.sampleNode.has_value());
}

void SamplePayloadRunnerTest::reportsSampleMetadataFields() {
    const auto payload = toBytes({0x21, 0x1A, 0x4B, 0x7F});
    MemorySource source(payload);
    const auto span = byteSpan(0, payload.size());
    QVERIFY(span.has_value());
    auto sample = sampleFor({*span});
    sample.sampleIndex = 7;
    sample.dts = 12000;
    sample.pts = 15000;
    sample.duration = 1024;
    sample.timescale = 48000;
    sample.isSyncSample = false;

    auto tree = AnalysisTree::create(QStringLiteral("metadata"));
    QVERIFY(tree.has_value());
    const auto configSpec = [] {
        AnalysisNodeSpec spec;
        spec.kind = AnalysisNodeKind::Structure;
        spec.name = QStringLiteral("AudioSpecificConfig");
        return spec;
    }();
    const auto configId = tree->appendChild(tree->rootId(), configSpec);
    QVERIFY(configId.has_value());

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.framing = SamplePayloadFraming::OpaqueAccessUnit;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.configurationNode = configId;

    const auto result = SamplePayloadRunner::run(request);
    QVERIFY2(result.executed(), qPrintable(result.errorMessage));
    QVERIFY(result.sampleNode.has_value());

    const auto sampleIndexField =
        findNamedChild(*tree, *result.sampleNode, QStringLiteral("sample_index"));
    QVERIFY(sampleIndexField.has_value());
    QCOMPARE(sampleIndexField->value().toULongLong(), quint64{7});
    const auto dtsField = findNamedChild(*tree, *result.sampleNode, QStringLiteral("dts"));
    QVERIFY(dtsField.has_value());
    QCOMPARE(dtsField->value().toLongLong(), qint64{12000});
    const auto ptsField = findNamedChild(*tree, *result.sampleNode, QStringLiteral("pts"));
    QVERIFY(ptsField.has_value());
    QCOMPARE(ptsField->value().toLongLong(), qint64{15000});
    const auto durationField = findNamedChild(*tree, *result.sampleNode, QStringLiteral("duration"));
    QVERIFY(durationField.has_value());
    QCOMPARE(durationField->value().toULongLong(), quint64{1024});
    const auto timescaleField = findNamedChild(*tree, *result.sampleNode, QStringLiteral("timescale"));
    QVERIFY(timescaleField.has_value());
    QCOMPARE(timescaleField->value().toUInt(), quint32{48000});
    const auto syncField = findNamedChild(*tree, *result.sampleNode, QStringLiteral("is_sync_sample"));
    QVERIFY(syncField.has_value());
    QCOMPARE(syncField->value().toBool(), false);
    const auto lengthField = findNamedChild(*tree, *result.sampleNode, QStringLiteral("byte_length"));
    QVERIFY(lengthField.has_value());
    QCOMPARE(lengthField->value().toULongLong(), static_cast<quint64>(payload.size()));
}

void SamplePayloadRunnerTest::runsSingleNalSampleThroughOfficialH264Rule() {
    auto fixture = loadH264NalProgram();
    QVERIFY(fixture.program.has_value());

    const auto nal = standaloneSpsNal();
    const auto bytes = lengthPrefixedSample({nal}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));
    auto tree = AnalysisTree::create(QStringLiteral("single-nal"));
    QVERIFY(tree.has_value());
    RuleExecutionSession session(*fixture.program, 1);

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.executionSession = &session;
    request.unitStructureIndex = fixture.headerIndex;
    request.transformRegistry = &registry;

    const auto result = SamplePayloadRunner::run(request);
    QVERIFY2(result.executed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{1});
    QCOMPARE(result.executedUnitCount(), quint64{1});
    QCOMPARE(result.units.front().executionStatus, RuleExecutionStatus::Materialized);
    QCOMPARE(result.framedByteCount, static_cast<quint64>(bytes.size()));

    QVERIFY(result.units.front().unitNode.has_value());
    const auto headerNode =
        findNamedChild(*tree, *result.units.front().unitNode, QStringLiteral("NalUnitHeader"));
    QVERIFY(headerNode.has_value());
    QCOMPARE(headerNode->state(), MaterializationState::Materialized);

    const auto spsNode = findNamedChild(*tree, result.units.front().structureNode.value(),
                                   QStringLiteral("SequenceParameterSetRbsp"));
    QVERIFY(spsNode.has_value());
    const auto profileField = findNamedChild(*tree, spsNode->id(), QStringLiteral("profile_idc"));
    QVERIFY(profileField.has_value());
    QCOMPARE(profileField->value().toULongLong(), quint64{66});
}

void SamplePayloadRunnerTest::recordsUnitLengthPrefixAsSyntaxField() {
    auto fixture = loadH264NalProgram();
    QVERIFY(fixture.program.has_value());

    const auto nal = standaloneSpsNal();
    const auto bytes = lengthPrefixedSample({nal}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));
    auto tree = AnalysisTree::create(QStringLiteral("prefix-field"));
    QVERIFY(tree.has_value());
    RuleExecutionSession session(*fixture.program, 1);

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.executionSession = &session;
    request.unitStructureIndex = fixture.headerIndex;
    request.transformRegistry = &registry;

    const auto result = SamplePayloadRunner::run(request);
    QVERIFY2(result.executed(), qPrintable(result.errorMessage));
    QVERIFY(result.units.front().unitNode.has_value());

    const auto prefixField =
        findNamedChild(*tree, *result.units.front().unitNode, QStringLiteral("unit_length"));
    QVERIFY(prefixField.has_value());
    QCOMPARE(prefixField->value().toULongLong(), static_cast<quint64>(nal.size()));
    QVERIFY(prefixField->location().has_value());
    QCOMPARE(prefixField->location()->logicalRange().bitLength(), quint64{32});
    QCOMPARE(prefixField->location()->sourceSpans().size(), std::size_t{1});
    QCOMPARE(prefixField->location()->sourceSpans().front().start().absoluteBitOffset(),
             quint64{0});
}

void SamplePayloadRunnerTest::runsMultiNalSampleSharingParameterSetContext() {
    auto fixture = loadH264NalProgram();
    QVERIFY(fixture.program.has_value());

    const auto sps = standaloneSpsNal();
    const auto pps = standalonePpsNal();
    const auto bytes = lengthPrefixedSample({sps, pps}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));
    auto tree = AnalysisTree::create(QStringLiteral("multi-nal"));
    QVERIFY(tree.has_value());
    RuleExecutionSession session(*fixture.program, 1);

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.executionSession = &session;
    request.unitStructureIndex = fixture.headerIndex;
    request.transformRegistry = &registry;

    const auto result = SamplePayloadRunner::run(request);
    QVERIFY2(result.executed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{2});
    QCOMPARE(result.executedUnitCount(), quint64{2});

    // The SPS published a definition that the PPS in the same sample consumed.
    QVERIFY(session.publishedDefinitionCount() >= 1);

    QVERIFY(result.units[0].structureNode.has_value());
    QVERIFY(result.units[1].structureNode.has_value());
    const auto spsNode = findNamedChild(*tree, *result.units[0].structureNode,
                                   QStringLiteral("SequenceParameterSetRbsp"));
    QVERIFY(spsNode.has_value());
    const auto ppsNode = findNamedChild(*tree, *result.units[1].structureNode,
                                   QStringLiteral("PictureParameterSetRbsp"));
    QVERIFY(ppsNode.has_value());
    QCOMPARE(ppsNode->state(), MaterializationState::Materialized);

    // Both units hang off one sample node, in bitstream order.
    const auto sampleNode = tree->node(*result.sampleNode);
    QVERIFY(sampleNode.has_value());
    QCOMPARE(result.units[0].unitNode.has_value(), true);
    QCOMPARE(result.units[1].unitNode.has_value(), true);
    QVERIFY(*result.units[0].unitNode < *result.units[1].unitNode);
}

void SamplePayloadRunnerTest::preservesRbspExcludedSpansPerUnit() {
    auto fixture = loadH264NalProgram();
    QVERIFY(fixture.program.has_value());

    const auto nal = standaloneSpsNalWithSarEmulation();
    const auto bytes = lengthPrefixedSample({nal}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));
    auto tree = AnalysisTree::create(QStringLiteral("excluded-spans"));
    QVERIFY(tree.has_value());
    RuleExecutionSession session(*fixture.program, 1);

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.executionSession = &session;
    request.unitStructureIndex = fixture.headerIndex;
    request.transformRegistry = &registry;

    const auto result = SamplePayloadRunner::run(request);
    QVERIFY2(result.executed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{1});
    QCOMPARE(result.units.front().executionStatus, RuleExecutionStatus::Materialized);

    // The emulation prevention byte stays a separate record mapped to the root
    // source, offset past the 4-byte length prefix.
    QCOMPARE(result.units.front().excludedSpans.size(), std::size_t{1});
    const auto& excluded = result.units.front().excludedSpans.front();
    QCOMPARE(excluded.sourceSpan.bitLength(), quint64{8});
    QVERIFY(excluded.sourceSpan.start().absoluteBitOffset() >= quint64{4U * 8U});
    QVERIFY(excluded.sourceSpan.start().absoluteBitOffset() <
            static_cast<quint64>(bytes.size()) * 8U);
}

void SamplePayloadRunnerTest::isolatesSingleUnitFailureWithoutFailingSample() {
    auto fixture = loadH264NalProgram();
    QVERIFY(fixture.program.has_value());

    // A well-formed SPS followed by an SPS header whose RBSP stops after one
    // byte, so only the second unit can fail.
    const auto good = standaloneSpsNal();
    const auto broken = toBytes({0x67, 0x42});
    const auto bytes = lengthPrefixedSample({good, broken}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));
    auto tree = AnalysisTree::create(QStringLiteral("isolated-failure"));
    QVERIFY(tree.has_value());
    RuleExecutionSession session(*fixture.program, 1);

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.executionSession = &session;
    request.unitStructureIndex = fixture.headerIndex;
    request.transformRegistry = &registry;

    const auto result = SamplePayloadRunner::run(request);
    // The sample still executes: one bad NAL does not take the sample down.
    QVERIFY2(result.executed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{2});
    QCOMPARE(result.executedUnitCount(), quint64{1});
    QVERIFY(result.units[0].executed());
    QVERIFY(!result.units[1].executed());
    QVERIFY(result.units[1].executionStatus == RuleExecutionStatus::TruncatedSource ||
            result.units[1].executionStatus == RuleExecutionStatus::InvalidSyntax);

    // The failure is recorded on the failing unit's node, not on the sample.
    const auto sampleNode = tree->node(*result.sampleNode);
    QVERIFY(sampleNode.has_value());
    QCOMPARE(sampleNode->state(), MaterializationState::Materialized);
    QVERIFY(result.units[1].unitNode.has_value());
    const auto failedNode = tree->node(*result.units[1].unitNode);
    QVERIFY(failedNode.has_value());
    QVERIFY(failedNode->state() != MaterializationState::Materialized);
    QVERIFY(!failedNode->diagnostics().empty());
}

void SamplePayloadRunnerTest::marksSampleNodeWhenFramingFails() {
    auto fixture = loadH264NalProgram();
    QVERIFY(fixture.program.has_value());

    // Declares 64 bytes inside a 6-byte sample.
    const auto bytes = toBytes({0x00, 0x00, 0x00, 0x40, 0x67, 0x42});
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));
    auto tree = AnalysisTree::create(QStringLiteral("framing-failure"));
    QVERIFY(tree.has_value());
    RuleExecutionSession session(*fixture.program, 1);

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.executionSession = &session;
    request.unitStructureIndex = fixture.headerIndex;
    request.transformRegistry = &registry;

    const auto result = SamplePayloadRunner::run(request);
    QCOMPARE(result.status, SamplePayloadRunStatus::TruncatedSample);
    QVERIFY(result.units.empty());

    // The sample node exists so the failure has somewhere to live.
    QVERIFY(result.sampleNode.has_value());
    const auto sampleNode = tree->node(*result.sampleNode);
    QVERIFY(sampleNode.has_value());
    QCOMPARE(sampleNode->state(), MaterializationState::Invalid);
    QCOMPARE(sampleNode->diagnostics().size(), std::size_t{1});
    QCOMPARE(sampleNode->diagnostics().front().code, DiagnosticCode::TruncatedSource);
    QVERIFY(sampleNode->diagnostics().front().location.has_value());
}

void SamplePayloadRunnerTest::honoursCancellationBeforeExecution() {
    auto fixture = loadH264NalProgram();
    QVERIFY(fixture.program.has_value());

    const auto bytes = lengthPrefixedSample({standaloneSpsNal()}, 4);
    MemorySource source(bytes);
    const auto span = byteSpan(0, bytes.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});

    PayloadTransformRegistry registry;
    QVERIFY(registry.registerProvider(std::make_shared<H264RbspPayloadTransformProvider>()));
    auto tree = AnalysisTree::create(QStringLiteral("cancelled"));
    QVERIFY(tree.has_value());
    RuleExecutionSession session(*fixture.program, 1);

    CancellationSource cancellation;
    QVERIFY(cancellation.requestCancellation());

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.executionSession = &session;
    request.unitStructureIndex = fixture.headerIndex;
    request.transformRegistry = &registry;
    request.options.cancellation = cancellation.token();

    const auto result = SamplePayloadRunner::run(request);
    QCOMPARE(result.status, SamplePayloadRunStatus::Cancelled);
    QVERIFY(!result.sampleNode.has_value());

    // Nothing was appended to the tree.
    const auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    QVERIFY(rootNode->children().empty());
}

void SamplePayloadRunnerTest::buildsAccessUnitEnvelopeWithConfigurationReference() {
    const auto payload = toBytes({0x21, 0x1A, 0x4B, 0x7F, 0x00, 0x11, 0x22, 0x33});
    MemorySource source(payload);
    const auto span = byteSpan(0, payload.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});

    auto tree = AnalysisTree::create(QStringLiteral("access-unit"));
    QVERIFY(tree.has_value());
    AnalysisNodeSpec configSpec;
    configSpec.kind = AnalysisNodeKind::Structure;
    configSpec.name = QStringLiteral("AudioSpecificConfig");
    const auto configId = tree->appendChild(tree->rootId(), configSpec);
    QVERIFY(configId.has_value());

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.framing = SamplePayloadFraming::OpaqueAccessUnit;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.configurationNode = configId;
    request.configurationSummary = QStringLiteral("AAC LC, 48000 Hz, stereo");

    const auto result = SamplePayloadRunner::run(request);
    QVERIFY2(result.executed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{1});
    QVERIFY(result.sampleNode.has_value());

    // The access unit is one compressed record covering the whole sample.
    const auto payloadNode = findNamedChild(*tree, *result.sampleNode, QStringLiteral("access_unit"));
    QVERIFY(payloadNode.has_value());
    QCOMPARE(payloadNode->kind(), AnalysisNodeKind::CompressedPayload);
    QCOMPARE(payloadNode->state(), MaterializationState::Materialized);
    QVERIFY(payloadNode->location().has_value());
    QCOMPARE(payloadNode->location()->logicalRange().bitLength(),
             static_cast<quint64>(payload.size()) * 8U);
    QCOMPARE(payloadNode->location()->sourceSpans().size(), std::size_t{1});
    QCOMPARE(payloadNode->location()->sourceSpans().front().start().absoluteBitOffset(),
             quint64{0});

    const auto configField =
        findNamedChild(*tree, *result.sampleNode, QStringLiteral("configuration_node"));
    QVERIFY(configField.has_value());
    QCOMPARE(configField->value().toULongLong(), configId->value());
    const auto summaryField =
        findNamedChild(*tree, *result.sampleNode, QStringLiteral("configuration_summary"));
    QVERIFY(summaryField.has_value());
    QCOMPARE(summaryField->value().toString(), QStringLiteral("AAC LC, 48000 Hz, stereo"));

    // No ADTS header is fabricated: the payload is the only child carrying bytes.
    QCOMPARE(result.units.front().payloadSpans.size(), std::size_t{1});
    QCOMPARE(result.units.front().payloadSpans.front().bitLength(),
             static_cast<quint64>(payload.size()) * 8U);
}

void SamplePayloadRunnerTest::reportsDependencyUnavailableWithoutConfiguration() {
    const auto payload = toBytes({0x21, 0x1A, 0x4B, 0x7F});
    MemorySource source(payload);
    const auto span = byteSpan(0, payload.size());
    QVERIFY(span.has_value());
    const auto sample = sampleFor({*span});

    auto tree = AnalysisTree::create(QStringLiteral("missing-config"));
    QVERIFY(tree.has_value());

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.framing = SamplePayloadFraming::OpaqueAccessUnit;
    request.tree = &*tree;
    request.parentId = tree->rootId();

    const auto result = SamplePayloadRunner::run(request);
    QCOMPARE(result.status, SamplePayloadRunStatus::DependencyUnavailable);
    QVERIFY(result.units.empty());

    // Nothing is guessed: the run stops before any node is created.
    const auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    QVERIFY(rootNode->children().empty());
}

void SamplePayloadRunnerTest::mapsAccessUnitSpansOntoDiscontiguousSample() {
    std::vector<std::byte> raw(24, static_cast<std::byte>(0x5A));
    MemorySource source(raw);
    const auto firstSpan = byteSpan(2, 4);
    const auto secondSpan = byteSpan(16, 4);
    QVERIFY(firstSpan && secondSpan);
    const auto sample = sampleFor({*firstSpan, *secondSpan});

    auto tree = AnalysisTree::create(QStringLiteral("discontiguous-au"));
    QVERIFY(tree.has_value());
    AnalysisNodeSpec configSpec;
    configSpec.kind = AnalysisNodeKind::Structure;
    configSpec.name = QStringLiteral("AudioSpecificConfig");
    const auto configId = tree->appendChild(tree->rootId(), configSpec);
    QVERIFY(configId.has_value());

    SamplePayloadRunRequest request;
    request.source = &source;
    request.sample = &sample;
    request.framing = SamplePayloadFraming::OpaqueAccessUnit;
    request.tree = &*tree;
    request.parentId = tree->rootId();
    request.configurationNode = configId;

    const auto result = SamplePayloadRunner::run(request);
    QVERIFY2(result.executed(), qPrintable(result.errorMessage));
    QCOMPARE(result.units.size(), std::size_t{1});

    // Both physical fragments survive: the envelope does not flatten the sample
    // into one contiguous span.
    QCOMPARE(result.units.front().payloadSpans.size(), std::size_t{2});
    QCOMPARE(result.units.front().payloadSpans[0].start().absoluteBitOffset(), quint64{2U * 8U});
    QCOMPARE(result.units.front().payloadSpans[0].bitLength(), quint64{4U * 8U});
    QCOMPARE(result.units.front().payloadSpans[1].start().absoluteBitOffset(), quint64{16U * 8U});
    QCOMPARE(result.units.front().payloadSpans[1].bitLength(), quint64{4U * 8U});

    const auto lengthField = findNamedChild(*tree, *result.sampleNode, QStringLiteral("byte_length"));
    QVERIFY(lengthField.has_value());
    QCOMPARE(lengthField->value().toULongLong(), quint64{8});
}

QTEST_MAIN(SamplePayloadRunnerTest)
#include "sample_payload_runner_test.moc"
