#include <streamview/core/source_pager.h>
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
using streamview::core::SourcePager;
using streamview::rules::DslParser;
using streamview::rules::H264AnnexBAnalysisBatch;
using streamview::rules::H264AnnexBAnalysisStatus;
using streamview::rules::H264AnnexBAnalyzer;
using streamview::rules::H264EbspRbspMapLimits;
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
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        ++readCount_;
        const auto data = bytes({0x00, 0x00, 0x01, 0x65});
        if (byteOffset >= data.size()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
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

class FailAtOffsetSource final : public RandomAccessSource {
public:
    FailAtOffsetSource(std::vector<std::byte> data, quint64 failingOffset)
        : data_(std::move(data)), failingOffset_(failingOffset) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("offset-failing-memory");
    }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (byteOffset == failingOffset_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected payload read failure")};
        }
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
    quint64 failingOffset_ = 0;
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

class ArmedCancellingSource final : public RandomAccessSource {
public:
    explicit ArmedCancellingSource(std::vector<std::byte> data) : data_(std::move(data)) {}

    void arm(quint64 byteOffset, CancellationSource& cancellation) noexcept {
        cancellationOffset_ = byteOffset;
        cancellation_ = &cancellation;
        armed_ = true;
    }

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("armed-cancelling-memory");
    }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (armed_ && byteOffset == cancellationOffset_) {
            armed_ = false;
            (void)cancellation_->requestCancellation();
        }
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
    mutable CancellationSource* cancellation_ = nullptr;
    mutable quint64 cancellationOffset_ = 0;
    mutable bool armed_ = false;
};

class HundredGigabyteSparseSource final : public RandomAccessSource {
public:
    HundredGigabyteSparseSource() : prefix_(1040, std::byte{0xFF}) {
        const auto put = [this](std::size_t offset,
                                std::initializer_list<unsigned int> values) {
            const auto valueBytes = bytes(values);
            std::copy(valueBytes.begin(),
                      valueBytes.end(),
                      prefix_.begin() + static_cast<std::ptrdiff_t>(offset));
        };
        put(0, {0x00, 0x00, 0x01, 0x65, 0x12});
        put(10, {0x00, 0x00, 0x01, 0x6E});
        put(1030, {0x00, 0x00, 0x01, 0x41});
    }

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return 100ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("hundred-gigabyte-sparse");
    }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (byteOffset >= sizeBytes()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto count = static_cast<std::size_t>(std::min<quint64>(
            sizeBytes() - byteOffset, static_cast<quint64>(destination.size())));
        maximumRequestSize_ = std::max(maximumRequestSize_, count);
        highestReadEnd_ = std::max(highestReadEnd_, byteOffset + static_cast<quint64>(count));
        for (std::size_t index = 0; index < count; ++index) {
            const quint64 absolute = byteOffset + static_cast<quint64>(index);
            destination[index] = absolute < static_cast<quint64>(prefix_.size())
                                     ? prefix_.at(static_cast<std::size_t>(absolute))
                                     : std::byte{0xFF};
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

    [[nodiscard]] std::size_t maximumRequestSize() const noexcept {
        return maximumRequestSize_;
    }
    [[nodiscard]] quint64 highestReadEnd() const noexcept { return highestReadEnd_; }

private:
    std::vector<std::byte> prefix_;
    mutable std::size_t maximumRequestSize_ = 0;
    mutable quint64 highestReadEnd_ = 0;
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
        QCOMPARE(parsed.program.structs.size(), std::size_t(4));
        QCOMPARE(parsed.program.structs.at(0).name, QStringLiteral("NalUnitHeader"));
        QCOMPARE(parsed.program.structs.at(1).name,
                 QStringLiteral("AccessUnitDelimiterRbsp"));
        QCOMPARE(parsed.program.structs.at(1).items.size(), std::size_t(2));
        QCOMPARE(parsed.program.structs.at(1).items.at(1).kind,
                 streamview::rules::DslStructItemKind::RbspTrailingBits);
        QCOMPARE(parsed.program.structs.at(2).name,
                 QStringLiteral("SequenceParameterSetRbsp"));
        QCOMPARE(parsed.program.structs.at(3).name,
                 QStringLiteral("PictureParameterSetRbsp"));
        QCOMPARE(parsed.program.structs.at(3).items.back().kind,
                 streamview::rules::DslStructItemKind::RbspTrailingBits);
        QCOMPARE(parsed.program.scans.size(), std::size_t(1));
        QCOMPARE(parsed.program.entry.targetName, QStringLiteral("nal_units"));
        QVERIFY(parsed.program.payloadDispatch.has_value());
        const auto& dispatch = *parsed.program.payloadDispatch;
        QCOMPARE(dispatch.viewKind, QStringLiteral("rbsp"));
        QCOMPARE(dispatch.sequenceName, QStringLiteral("nal_units"));
        QCOMPARE(dispatch.controllerFieldName, QStringLiteral("nal_unit_type"));
        QCOMPARE(dispatch.cases.size(), std::size_t(5));
    }

    void decodesTheSupportedBaselineSequenceParameterSetPayload() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->children().size(), std::size_t(1));
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->name(), QStringLiteral("SequenceParameterSetRbsp"));
        QCOMPARE(sps->state(), MaterializationState::Materialized);
        QCOMPARE(sps->metadata().specification->clause, QStringLiteral("7.3.2.1.1"));

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto profile = fieldNamed(QStringLiteral("profile_idc"));
        const auto level = fieldNamed(QStringLiteral("level_idc"));
        const auto spsId = fieldNamed(QStringLiteral("seq_parameter_set_id"));
        const auto picOrderType = fieldNamed(QStringLiteral("pic_order_cnt_type"));
        const auto width = fieldNamed(QStringLiteral("pic_width_in_mbs_minus1"));
        const auto height = fieldNamed(QStringLiteral("pic_height_in_map_units_minus1"));
        QVERIFY(profile.has_value());
        QVERIFY(level.has_value());
        QVERIFY(spsId.has_value());
        QVERIFY(picOrderType.has_value());
        QVERIFY(width.has_value());
        QVERIFY(height.has_value());
        QCOMPARE(profile->value().toULongLong(), quint64(66));
        QCOMPARE(level->value().toULongLong(), quint64(30));
        QCOMPARE(spsId->value().toULongLong(), quint64(0));
        QCOMPARE(picOrderType->value().toULongLong(), quint64(0));
        QCOMPARE(width->value().toULongLong(), quint64(19));
        QCOMPARE(height->value().toULongLong(), quint64(14));
        QCOMPARE(profile->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));

        const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(stop.has_value());
        QCOMPARE(stop->name(), QStringLiteral("rbsp_stop_one_bit"));
        QCOMPARE(stop->value().toULongLong(), quint64(1));
    }

    void decodesTheSupportedHighSequenceParameterSetSubset() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x64, 0x00, 0x1f, 0xac, 0xe8, 0x14, 0x1f, 0x90}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto profile = fieldNamed(QStringLiteral("profile_idc"));
        const auto chroma = fieldNamed(QStringLiteral("chroma_format_idc"));
        const auto depth = fieldNamed(QStringLiteral("bit_depth_luma_minus8"));
        const auto scaling = fieldNamed(QStringLiteral("seq_scaling_matrix_present_flag"));
        QVERIFY(profile.has_value());
        QVERIFY(chroma.has_value());
        QVERIFY(depth.has_value());
        QVERIFY(scaling.has_value());
        QCOMPARE(profile->value().toULongLong(), quint64(100));
        QCOMPARE(chroma->value().toULongLong(), quint64(1));
        QCOMPARE(depth->value().toULongLong(), quint64(0));
        QCOMPARE(scaling->value().toULongLong(), quint64(0));
    }

    void warnsOnOutOfRangeSequenceParameterSetFrameNumberWrap() {
        // log2_max_frame_num_minus4 carries ue(13), one above the 7.4.2.1.1 maximum,
        // which shifts every following field six bits later without invalidating them.
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0x8e, 0xd0, 0x28, 0x3f, 0x20}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Materialized);
        QVERIFY(sps->diagnostics().empty());

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto frameNum = fieldNamed(QStringLiteral("log2_max_frame_num_minus4"));
        QVERIFY(frameNum.has_value());
        QCOMPARE(frameNum->state(), MaterializationState::Materialized);
        QCOMPARE(frameNum->value().toULongLong(), quint64(13));
        QCOMPARE(frameNum->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = frameNum->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Field value is above its @range maximum"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("SequenceParameterSetRbsp.log2_max_frame_num_minus4"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(57));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(7));

        // Every field after the violation keeps its conformant value and stays clean.
        const auto lsb = fieldNamed(QStringLiteral("log2_max_pic_order_cnt_lsb_minus4"));
        const auto width = fieldNamed(QStringLiteral("pic_width_in_mbs_minus1"));
        const auto height = fieldNamed(QStringLiteral("pic_height_in_map_units_minus1"));
        const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(lsb.has_value());
        QVERIFY(width.has_value());
        QVERIFY(height.has_value());
        QVERIFY(stop.has_value());
        QCOMPARE(lsb->value().toULongLong(), quint64(0));
        QVERIFY(lsb->diagnostics().empty());
        QCOMPARE(width->value().toULongLong(), quint64(19));
        QCOMPARE(height->value().toULongLong(), quint64(14));
        QCOMPARE(stop->value().toULongLong(), quint64(1));
    }

    void decodesTheMinimalVideoUsabilityInformationCore() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xd0, 0x04}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto vuiPresent = fieldNamed(QStringLiteral("vui_parameters_present_flag"));
        const auto aspectPresent = fieldNamed(QStringLiteral("aspect_ratio_info_present_flag"));
        const auto nalHrd = fieldNamed(QStringLiteral("nal_hrd_parameters_present_flag"));
        const auto vclHrd = fieldNamed(QStringLiteral("vcl_hrd_parameters_present_flag"));
        const auto picStruct = fieldNamed(QStringLiteral("pic_struct_present_flag"));
        const auto restriction = fieldNamed(QStringLiteral("bitstream_restriction_flag"));
        const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(vuiPresent.has_value());
        QVERIFY(aspectPresent.has_value());
        QVERIFY(nalHrd.has_value());
        QVERIFY(vclHrd.has_value());
        QVERIFY(picStruct.has_value());
        QVERIFY(restriction.has_value());
        QVERIFY(stop.has_value());
        QCOMPARE(vuiPresent->value().toULongLong(), quint64(1));
        QCOMPARE(aspectPresent->value().toULongLong(), quint64(0));
        QCOMPARE(nalHrd->value().toULongLong(), quint64(0));
        QCOMPARE(vclHrd->value().toULongLong(), quint64(0));
        QCOMPARE(picStruct->value().toULongLong(), quint64(0));
        QCOMPARE(restriction->value().toULongLong(), quint64(0));
        QCOMPARE(stop->value().toULongLong(), quint64(1));
        QVERIFY(!fieldNamed(QStringLiteral("aspect_ratio_idc")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("motion_vectors_over_pic_boundaries_flag"))
                     .has_value());
    }

    void decodesTheSupportedVideoUsabilityInformationBranches() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xdf,
            0xf8, 0x00, 0x20, 0x00, 0x1f, 0xb8, 0x08, 0x08, 0x0d, 0x92, 0x00,
            0x00, 0x07, 0xd2, 0x00, 0x01, 0xd4, 0xc1, 0x3b, 0x41, 0x10, 0x83,
            0x2c,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const std::vector expectedUnsigned{
            std::pair{QStringLiteral("aspect_ratio_info_present_flag"), quint64(1)},
            std::pair{QStringLiteral("aspect_ratio_idc"), quint64(255)},
            std::pair{QStringLiteral("sar_width"), quint64(4)},
            std::pair{QStringLiteral("sar_height"), quint64(3)},
            std::pair{QStringLiteral("overscan_info_present_flag"), quint64(1)},
            std::pair{QStringLiteral("overscan_appropriate_flag"), quint64(1)},
            std::pair{QStringLiteral("video_signal_type_present_flag"), quint64(1)},
            std::pair{QStringLiteral("video_format"), quint64(5)},
            std::pair{QStringLiteral("video_full_range_flag"), quint64(1)},
            std::pair{QStringLiteral("colour_description_present_flag"), quint64(1)},
            std::pair{QStringLiteral("colour_primaries"), quint64(1)},
            std::pair{QStringLiteral("transfer_characteristics"), quint64(1)},
            std::pair{QStringLiteral("matrix_coefficients"), quint64(1)},
            std::pair{QStringLiteral("chroma_loc_info_present_flag"), quint64(1)},
            std::pair{QStringLiteral("chroma_sample_loc_type_top_field"), quint64(2)},
            std::pair{QStringLiteral("chroma_sample_loc_type_bottom_field"), quint64(3)},
            std::pair{QStringLiteral("timing_info_present_flag"), quint64(1)},
            std::pair{QStringLiteral("num_units_in_tick"), quint64(1001)},
            std::pair{QStringLiteral("time_scale"), quint64(60000)},
            std::pair{QStringLiteral("fixed_frame_rate_flag"), quint64(1)},
            std::pair{QStringLiteral("nal_hrd_parameters_present_flag"), quint64(0)},
            std::pair{QStringLiteral("vcl_hrd_parameters_present_flag"), quint64(0)},
            std::pair{QStringLiteral("pic_struct_present_flag"), quint64(1)},
            std::pair{QStringLiteral("bitstream_restriction_flag"), quint64(1)},
            std::pair{QStringLiteral("motion_vectors_over_pic_boundaries_flag"), quint64(1)},
            std::pair{QStringLiteral("max_bytes_per_pic_denom"), quint64(2)},
            std::pair{QStringLiteral("max_bits_per_mb_denom"), quint64(1)},
            std::pair{QStringLiteral("log2_max_mv_length_horizontal"), quint64(16)},
            std::pair{QStringLiteral("log2_max_mv_length_vertical"), quint64(15)},
            std::pair{QStringLiteral("max_num_reorder_frames"), quint64(2)},
            std::pair{QStringLiteral("max_dec_frame_buffering"), quint64(4)},
            std::pair{QStringLiteral("rbsp_stop_one_bit"), quint64(1)},
        };
        for (const auto& [name, expected] : expectedUnsigned) {
            const auto field = fieldNamed(name);
            QVERIFY2(field.has_value(), qPrintable(name));
            QCOMPARE(field->value().toULongLong(), expected);
            QVERIFY(field->diagnostics().empty());
        }
        const auto aspect = fieldNamed(QStringLiteral("aspect_ratio_idc"));
        const auto chroma = fieldNamed(QStringLiteral("chroma_sample_loc_type_top_field"));
        QVERIFY(aspect.has_value());
        QVERIFY(chroma.has_value());
        QCOMPARE(aspect->metadata().specification->clause, QStringLiteral("E.1.1"));
        QCOMPARE(chroma->metadata().specification->clause, QStringLiteral("E.2.1"));
        QCOMPARE(aspect->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(85));
        QCOMPARE(aspect->location()->sourceSpans().front().bitLength(), quint64(8));
        QVERIFY(!fieldNamed(QStringLiteral("low_delay_hrd_flag")).has_value());
    }

    void decodesTheSupportedHypotheticalReferenceDecoderBranches() {
        struct HrdCase {
            std::vector<unsigned int> values;
            std::vector<std::pair<QString, quint64>> expectedFields;
            std::vector<QString> absentFields;
            quint64 lowDelayHrdFlag;
        };
        const std::vector cases{
            HrdCase{
                {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a,
                 0x0f, 0xd0, 0x51, 0xa5, 0x59, 0x17, 0xb5, 0x68, 0xd0},
                {
                    {QStringLiteral("nal_hrd_cpb_cnt_minus1"), quint64(1)},
                    {QStringLiteral("nal_hrd_bit_rate_scale"), quint64(3)},
                    {QStringLiteral("nal_hrd_cpb_size_scale"), quint64(4)},
                    {QStringLiteral("nal_hrd_cpb_count"), quint64(2)},
                    {QStringLiteral("nal_hrd_bit_rate_value_minus1[0]"), quint64(0)},
                    {QStringLiteral("nal_hrd_cpb_size_value_minus1[0]"), quint64(1)},
                    {QStringLiteral("nal_hrd_cbr_flag[0]"), quint64(1)},
                    {QStringLiteral("nal_hrd_bit_rate_value_minus1[1]"), quint64(2)},
                    {QStringLiteral("nal_hrd_cpb_size_value_minus1[1]"), quint64(3)},
                    {QStringLiteral("nal_hrd_cbr_flag[1]"), quint64(0)},
                    {QStringLiteral("nal_hrd_initial_cpb_removal_delay_length_minus1"),
                     quint64(23)},
                    {QStringLiteral("nal_hrd_cpb_removal_delay_length_minus1"), quint64(22)},
                    {QStringLiteral("nal_hrd_dpb_output_delay_length_minus1"), quint64(21)},
                    {QStringLiteral("nal_hrd_time_offset_length"), quint64(20)},
                },
                {QStringLiteral("vcl_hrd_cpb_cnt_minus1")},
                quint64(1),
            },
            HrdCase{
                {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a,
                 0x0f, 0xd0, 0x31, 0x22, 0x9a, 0xa5, 0xb1, 0xa2},
                {
                    {QStringLiteral("vcl_hrd_cpb_cnt_minus1"), quint64(0)},
                    {QStringLiteral("vcl_hrd_bit_rate_scale"), quint64(1)},
                    {QStringLiteral("vcl_hrd_cpb_size_scale"), quint64(2)},
                    {QStringLiteral("vcl_hrd_cpb_count"), quint64(1)},
                    {QStringLiteral("vcl_hrd_bit_rate_value_minus1[0]"), quint64(4)},
                    {QStringLiteral("vcl_hrd_cpb_size_value_minus1[0]"), quint64(5)},
                    {QStringLiteral("vcl_hrd_cbr_flag[0]"), quint64(1)},
                    {QStringLiteral("vcl_hrd_initial_cpb_removal_delay_length_minus1"),
                     quint64(10)},
                    {QStringLiteral("vcl_hrd_cpb_removal_delay_length_minus1"), quint64(11)},
                    {QStringLiteral("vcl_hrd_dpb_output_delay_length_minus1"), quint64(12)},
                    {QStringLiteral("vcl_hrd_time_offset_length"), quint64(13)},
                },
                {QStringLiteral("nal_hrd_cpb_cnt_minus1")},
                quint64(0),
            },
            HrdCase{
                {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f,
                 0xd0, 0x64, 0xa9, 0x8e, 0x84, 0xaa, 0x99, 0xc8, 0x59, 0x8e,
                 0x5b, 0x1a, 0xe9},
                {
                    {QStringLiteral("nal_hrd_cpb_cnt_minus1"), quint64(0)},
                    {QStringLiteral("nal_hrd_bit_rate_scale"), quint64(2)},
                    {QStringLiteral("nal_hrd_cpb_size_scale"), quint64(5)},
                    {QStringLiteral("nal_hrd_bit_rate_value_minus1[0]"), quint64(1)},
                    {QStringLiteral("nal_hrd_cpb_size_value_minus1[0]"), quint64(2)},
                    {QStringLiteral("vcl_hrd_cpb_cnt_minus1"), quint64(1)},
                    {QStringLiteral("vcl_hrd_bit_rate_scale"), quint64(6)},
                    {QStringLiteral("vcl_hrd_cpb_size_scale"), quint64(7)},
                    {QStringLiteral("vcl_hrd_bit_rate_value_minus1[1]"), quint64(5)},
                    {QStringLiteral("vcl_hrd_cpb_size_value_minus1[1]"), quint64(6)},
                    {QStringLiteral("vcl_hrd_initial_cpb_removal_delay_length_minus1"),
                     quint64(11)},
                    {QStringLiteral("vcl_hrd_time_offset_length"), quint64(14)},
                },
                {},
                quint64(1),
            },
        };

        for (const auto& hrdCase : cases) {
            std::vector<std::byte> sourceBytes;
            sourceBytes.reserve(hrdCase.values.size());
            for (const unsigned int value : hrdCase.values) {
                sourceBytes.push_back(static_cast<std::byte>(value));
            }
            MemorySource source(std::move(sourceBytes));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto sps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(sps.has_value());
            QCOMPARE(sps->state(), MaterializationState::Materialized);
            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    sps->children().begin(), sps->children().end(), [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == sps->children().end() ? std::nullopt
                                                    : analyzer->tree().node(*found);
            };
            for (const auto& [name, expected] : hrdCase.expectedFields) {
                const auto field = fieldNamed(name);
                QVERIFY2(field.has_value(), qPrintable(name));
                QCOMPARE(field->value().toULongLong(), expected);
                QVERIFY(field->diagnostics().empty());
            }
            for (const auto& name : hrdCase.absentFields) {
                QVERIFY2(!fieldNamed(name).has_value(), qPrintable(name));
            }
            const auto anyHrd = fieldNamed(QStringLiteral("hrd_parameters_present"));
            const auto lowDelay = fieldNamed(QStringLiteral("low_delay_hrd_flag"));
            const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
            QVERIFY(anyHrd.has_value());
            QVERIFY(lowDelay.has_value());
            QVERIFY(stop.has_value());
            QCOMPARE(anyHrd->value().toBool(), true);
            QVERIFY(!anyHrd->location().has_value());
            QCOMPARE(lowDelay->value().toULongLong(), hrdCase.lowDelayHrdFlag);
            QCOMPARE(stop->value().toULongLong(), quint64(1));
        }

        MemorySource metadataSource(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a,
            0x0f, 0xd0, 0x51, 0xa5, 0x59, 0x17, 0xb5, 0x68, 0xd0,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(metadataSource, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));
        const auto batch = analyzer->analyzeBatch();
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        const auto count = std::find_if(
            sps->children().begin(), sps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("nal_hrd_cpb_cnt_minus1");
            });
        QVERIFY(count != sps->children().end());
        const auto countNode = analyzer->tree().node(*count);
        QVERIFY(countNode.has_value());
        QCOMPARE(countNode->metadata().specification->clause, QStringLiteral("E.2.2"));
        QCOMPARE(countNode->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(90));
    }

    void warnsOnOutOfRangeVideoUsabilityInformationValues() {
        struct RangeCase {
            std::vector<unsigned int> values;
            QString fieldName;
            quint64 value;
            quint64 bitLength;
        };
        const std::vector cases{
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd1, 0x3c, 0x10},
                      QStringLiteral("chroma_sample_loc_type_top_field"), quint64(6), quint64(5)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd1, 0x9c, 0x10},
                      QStringLiteral("chroma_sample_loc_type_bottom_field"),
                      quint64(6), quint64(5)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd0, 0x0c, 0x25, 0xf8},
                      QStringLiteral("max_bytes_per_pic_denom"), quint64(17), quint64(9)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd0, 0x0e, 0x12, 0xf8},
                      QStringLiteral("max_bits_per_mb_denom"), quint64(17), quint64(9)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd0, 0x0f, 0x09, 0x78},
                      QStringLiteral("log2_max_mv_length_horizontal"), quint64(17), quint64(9)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd0, 0x0f, 0x84, 0xb8},
                      QStringLiteral("log2_max_mv_length_vertical"), quint64(17), quint64(9)},
        };

        for (const auto& rangeCase : cases) {
            std::vector<std::byte> sourceBytes;
            sourceBytes.reserve(rangeCase.values.size());
            for (const unsigned int value : rangeCase.values) {
                sourceBytes.push_back(static_cast<std::byte>(value));
            }
            MemorySource source(std::move(sourceBytes));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto sps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(sps.has_value());
            QCOMPARE(sps->state(), MaterializationState::Materialized);
            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    sps->children().begin(), sps->children().end(), [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == sps->children().end() ? std::nullopt
                                                    : analyzer->tree().node(*found);
            };
            const auto field = fieldNamed(rangeCase.fieldName);
            QVERIFY2(field.has_value(), qPrintable(rangeCase.fieldName));
            QCOMPARE(field->value().toULongLong(), rangeCase.value);
            QCOMPARE(field->diagnostics().size(), std::size_t(1));
            const auto& diagnostic = field->diagnostics().front();
            QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
            QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
            QCOMPARE(diagnostic.fieldPath,
                     QStringLiteral("SequenceParameterSetRbsp.") + rangeCase.fieldName);
            QVERIFY(diagnostic.location.has_value());
            QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(),
                     rangeCase.bitLength);
            const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
            QVERIFY(stop.has_value());
            QCOMPARE(stop->value().toULongLong(), quint64(1));
        }
    }

    void rejectsOutOfRangeHypotheticalReferenceDecoderScheduleCountAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                   0xf4, 0x0a, 0x0f, 0xd0, 0x41, 0x08, 0x07,
                                   0x00, 0x00, 0x01, 0x41}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Invalid);
        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto count = fieldNamed(QStringLiteral("nal_hrd_cpb_cnt_minus1"));
        const auto scale = fieldNamed(QStringLiteral("nal_hrd_bit_rate_scale"));
        const auto computedCount = fieldNamed(QStringLiteral("nal_hrd_cpb_count"));
        QVERIFY(count.has_value());
        QVERIFY(scale.has_value());
        QVERIFY(computedCount.has_value());
        QCOMPARE(count->value().toULongLong(), quint64(32));
        QCOMPARE(computedCount->value().toULongLong(), quint64(33));
        QCOMPARE(count->diagnostics().size(), std::size_t(1));
        const auto& warning = count->diagnostics().front();
        QCOMPARE(warning.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(warning.severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(warning.fieldPath,
                 QStringLiteral("SequenceParameterSetRbsp.nal_hrd_cpb_cnt_minus1"));
        QVERIFY(warning.location.has_value());
        QCOMPARE(warning.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(90));
        QCOMPARE(warning.location->sourceSpans().front().bitLength(), quint64(11));
        QCOMPARE(sps->diagnostics().size(), std::size_t(1));
        const auto& error = sps->diagnostics().front();
        QCOMPARE(error.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(error.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(error.fieldPath,
                 QStringLiteral("SequenceParameterSetRbsp.nal_hrd_cpb_count"));
        QVERIFY(!error.location.has_value());
        QVERIFY(!fieldNamed(QStringLiteral("nal_hrd_bit_rate_value_minus1[0]")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("vcl_hrd_parameters_present_flag")).has_value());

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsTruncatedSequenceParameterSetVuiAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xd8,
                                   0x00, 0x00, 0x01, 0x41}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Invalid);
        const auto vui = std::find_if(
            sps->children().begin(), sps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("vui_parameters_present_flag");
            });
        QVERIFY(vui != sps->children().end());
        const auto vuiNode = analyzer->tree().node(*vui);
        QVERIFY(vuiNode.has_value());
        QCOMPARE(vuiNode->value().toULongLong(), quint64(1));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsUnsupportedSequenceParameterSetPictureOrderCountType() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xd2, 0x05, 0x07, 0xe4}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Invalid);
        const auto picOrder = std::find_if(
            sps->children().begin(), sps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("pic_order_cnt_type");
            });
        QVERIFY(picOrder != sps->children().end());
        const auto picOrderNode = analyzer->tree().node(*picOrder);
        QVERIFY(picOrderNode.has_value());
        QCOMPARE(picOrderNode->value().toULongLong(), quint64(1));
        QCOMPARE(sps->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
    }

    void rejectsInvalidSequenceParameterSetProfileAndReservedBits() {
        const auto rejectedField = [](std::initializer_list<unsigned int> payload,
                                      const QString& expectedName) {
            std::vector<unsigned int> values{0x00, 0x00, 0x01, 0x67};
            values.insert(values.end(), payload.begin(), payload.end());
            return std::pair<std::vector<unsigned int>, QString>{std::move(values), expectedName};
        };
        const std::vector cases{
            rejectedField({0x65}, QStringLiteral("profile_idc")),
            rejectedField({0x42, 0x01}, QStringLiteral("reserved_zero_2bits")),
        };

        for (const auto& [values, expectedName] : cases) {
            std::vector<std::byte> sourceBytes;
            sourceBytes.reserve(values.size());
            for (const unsigned int value : values) {
                sourceBytes.push_back(static_cast<std::byte>(value));
            }
            MemorySource source(std::move(sourceBytes));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Invalid);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto sps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(sps.has_value());
            QCOMPARE(sps->state(), MaterializationState::Invalid);
            const auto field = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == expectedName;
                });
            QVERIFY(field != sps->children().end());
        }
    }

    void decodesTheSupportedPictureParameterSetBaseSyntax() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                                   0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->children().size(), std::size_t(1));
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->name(), QStringLiteral("PictureParameterSetRbsp"));
        QCOMPARE(pps->state(), MaterializationState::Materialized);
        QCOMPARE(pps->metadata().specification->clause, QStringLiteral("7.3.2.2"));
        QCOMPARE(pps->children().size(), std::size_t(23));

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                pps->children().begin(), pps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == pps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto ppsId = fieldNamed(QStringLiteral("pic_parameter_set_id"));
        const auto spsId = fieldNamed(QStringLiteral("seq_parameter_set_id"));
        const auto sliceGroups = fieldNamed(QStringLiteral("num_slice_groups_minus1"));
        const auto weightedBipred = fieldNamed(QStringLiteral("weighted_bipred_idc"));
        const auto qp = fieldNamed(QStringLiteral("pic_init_qp_minus26"));
        const auto deblocking =
            fieldNamed(QStringLiteral("deblocking_filter_control_present_flag"));
        const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(ppsId.has_value());
        QVERIFY(spsId.has_value());
        QVERIFY(sliceGroups.has_value());
        QVERIFY(weightedBipred.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(deblocking.has_value());
        QVERIFY(stop.has_value());
        QCOMPARE(ppsId->value().toULongLong(), quint64(0));
        QCOMPARE(ppsId->metadata().specification->clause, QStringLiteral("7.4.2.2"));
        QVERIFY(ppsId->metadata().description.contains(QStringLiteral("0 to 255")));
        QCOMPARE(spsId->value().toULongLong(), quint64(0));
        QCOMPARE(sliceGroups->value().toULongLong(), quint64(0));
        QCOMPARE(weightedBipred->value().toULongLong(), quint64(0));
        QCOMPARE(weightedBipred->metadata().typeName, QStringLiteral("WeightedBipredIdc"));
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QCOMPARE(deblocking->value().toULongLong(), quint64(1));
        QCOMPARE(stop->value().toULongLong(), quint64(1));
        QCOMPARE(ppsId->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(120));
        QCOMPARE(ppsId->location()->sourceSpans().front().bitLength(), quint64(1));
    }

    void decodesNonDefaultPictureParameterSetValues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0x7d, 0x02, 0x83, 0xf2,
                                   0x00, 0x00, 0x01, 0x68,
                                   0x23, 0xed, 0x66, 0x8a, 0xe0}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                pps->children().begin(), pps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == pps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        QCOMPARE(fieldNamed(QStringLiteral("pic_parameter_set_id"))->value().toULongLong(),
                 quint64(3));
        QCOMPARE(fieldNamed(QStringLiteral("seq_parameter_set_id"))->value().toULongLong(),
                 quint64(2));
        QCOMPARE(fieldNamed(QStringLiteral("entropy_coding_mode_flag"))->value().toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("bottom_field_pic_order_in_frame_present_flag"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("num_ref_idx_l0_default_active_minus1"))
                     ->value()
                     .toULongLong(),
                 quint64(2));
        QCOMPARE(fieldNamed(QStringLiteral("num_ref_idx_l1_default_active_minus1"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("weighted_bipred_idc"))->value().toULongLong(),
                 quint64(2));
        QCOMPARE(fieldNamed(QStringLiteral("pic_init_qp_minus26"))->value().toLongLong(),
                 qlonglong(-1));
        QCOMPARE(fieldNamed(QStringLiteral("pic_init_qs_minus26"))->value().toLongLong(),
                 qlonglong(1));
        QCOMPARE(fieldNamed(QStringLiteral("chroma_qp_index_offset"))->value().toLongLong(),
                 qlonglong(-2));
        QCOMPARE(fieldNamed(QStringLiteral("constrained_intra_pred_flag"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("redundant_pic_cnt_present_flag"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
    }

    void warnsOnOutOfRangePictureParameterSetIdentifier() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                                   0x00, 0x00, 0x01, 0x68,
                                   0x00, 0x80, 0xce, 0x3c, 0x80}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Materialized);
        const auto ppsId = analyzer->tree().node(pps->children().front());
        QVERIFY(ppsId.has_value());
        QCOMPARE(ppsId->name(), QStringLiteral("pic_parameter_set_id"));
        QCOMPARE(ppsId->value().toULongLong(), quint64(256));
        QCOMPARE(ppsId->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = ppsId->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("PictureParameterSetRbsp.pic_parameter_set_id"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(120));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(17));
    }

    void rejectsPictureParameterSetWithoutPriorSequenceParameterSetAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
                                   0x00, 0x00, 0x01, 0x09, 0x50}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto ppsNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(ppsNal.has_value());
        QCOMPARE(ppsNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(ppsNal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Materialized);
        QCOMPARE(pps->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = pps->diagnostics().front();
        QCOMPARE(diagnostic.code,
                 streamview::core::DiagnosticCode::DependencyUnavailable);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("PictureParameterSetRbsp.seq_parameter_set_id"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(33));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsUnsupportedPictureParameterSetSliceGroupsAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x68, 0xc5,
                                   0x00, 0x00, 0x01, 0x41}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Invalid);
        const auto sliceGroups = std::find_if(
            pps->children().begin(), pps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("num_slice_groups_minus1");
            });
        QVERIFY(sliceGroups != pps->children().end());
        QCOMPARE(analyzer->tree().node(*sliceGroups)->value().toULongLong(), quint64(1));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsReservedPictureParameterSetValuesAndExtensions() {
        const std::vector cases{
            std::pair{std::vector<unsigned int>{0xce, 0xfc, 0x80},
                      QStringLiteral("weighted_bipred_idc")},
            std::pair{std::vector<unsigned int>{0xce, 0x3c, 0x30},
                      QStringLiteral("rbsp_stop_one_bit")},
        };

        for (const auto& [payload, expectedName] : cases) {
            std::vector<std::byte> sourceBytes = bytes({0x00, 0x00, 0x01, 0x68});
            for (const unsigned int value : payload) {
                sourceBytes.push_back(static_cast<std::byte>(value));
            }
            MemorySource source(std::move(sourceBytes));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Invalid);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto pps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(pps.has_value());
            QCOMPARE(pps->state(), MaterializationState::Invalid);
            const auto field = std::find_if(
                pps->children().begin(), pps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == expectedName;
                });
            QVERIFY(field != pps->children().end());
        }
    }

    void decodesTheAccessUnitDelimiterPayload() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        QCOMPARE(nal->children().size(), std::size_t(3));

        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->children().size(), std::size_t(1));

        const auto aud = analyzer->tree().node(rbsp->children().front());
        QVERIFY(aud.has_value());
        QCOMPARE(aud->name(), QStringLiteral("AccessUnitDelimiterRbsp"));
        QCOMPARE(aud->state(), MaterializationState::Materialized);
        QCOMPARE(aud->children().size(), std::size_t(6));
        QCOMPARE(aud->metadata().specification->clause, QStringLiteral("7.3.2.4"));

        const auto primaryPicType = analyzer->tree().node(aud->children().at(0));
        QVERIFY(primaryPicType.has_value());
        QCOMPARE(primaryPicType->name(), QStringLiteral("primary_pic_type"));
        QCOMPARE(primaryPicType->kind(), AnalysisNodeKind::SyntaxField);
        QCOMPARE(primaryPicType->value().toULongLong(), quint64(2));
        QCOMPARE(primaryPicType->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(primaryPicType->location()->sourceSpans().front().bitLength(), quint64(3));

        const auto stopBit = analyzer->tree().node(aud->children().at(1));
        QVERIFY(stopBit.has_value());
        QCOMPARE(stopBit->name(), QStringLiteral("rbsp_stop_one_bit"));
        QCOMPARE(stopBit->value().toULongLong(), quint64(1));
        QCOMPARE(stopBit->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(35));
        QCOMPARE(stopBit->metadata().specification->clause, QStringLiteral("7.3.2.11"));

        for (std::size_t index = 0; index < 4; ++index) {
            const auto alignment = analyzer->tree().node(aud->children().at(2 + index));
            QVERIFY(alignment.has_value());
            QCOMPARE(alignment->name(),
                     QStringLiteral("rbsp_alignment_zero_bit[%1]").arg(index));
            QCOMPARE(alignment->value().toULongLong(), quint64(0));
            QCOMPARE(
                alignment->location()->sourceSpans().front().start().absoluteBitOffset(),
                quint64(36 + index));
            QCOMPARE(alignment->location()->sourceSpans().front().bitLength(), quint64(1));
        }
    }

    void reportsAHeaderOnlyAccessUnitDelimiterAsTruncated() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->children().size(), std::size_t(3));

        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->diagnostics().size(), std::size_t(1));
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::TruncatedSource);
    }

    void materializesHeaderOnlyEndOfSequenceAndEndOfStream() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x0A, 0x00, 0x00, 0x01, 0x0B}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        for (const auto nodeId : batch.nalUnitNodes) {
            const auto nal = analyzer->tree().node(nodeId);
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            QCOMPARE(nal->children().size(), std::size_t(3));
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
            QCOMPARE(rbsp->state(), MaterializationState::Materialized);
            QVERIFY(rbsp->children().empty());
            QVERIFY(rbsp->diagnostics().empty());
        }
    }

    void rejectsANonEmptyEndOfSequencePayload() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x0A, 0x80,
                                   0x00, 0x00, 0x01, 0x09, 0x50}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QVERIFY(rbsp->children().empty());
        QCOMPARE(rbsp->diagnostics().size(), std::size_t(1));
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QVERIFY(rbsp->diagnostics().front().message.contains(QStringLiteral("empty RBSP")));
        QCOMPARE(rbsp->location()->logicalRange().bitLength(), quint64(8));

        const auto following = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(following.has_value());
        QCOMPARE(following->state(), MaterializationState::Materialized);
        const auto followingRbsp = analyzer->tree().node(following->children().at(2));
        QVERIFY(followingRbsp.has_value());
        QCOMPARE(followingRbsp->children().size(), std::size_t(1));
    }

    void rejectsAnAccessUnitDelimiterWithAClearedStopBit() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09, 0x40}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->children().size(), std::size_t(1));

        const auto aud = analyzer->tree().node(rbsp->children().front());
        QVERIFY(aud.has_value());
        QCOMPARE(aud->state(), MaterializationState::Invalid);
        QCOMPARE(aud->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(aud->children().size(), std::size_t(2));
        const auto retained = analyzer->tree().node(aud->children().front());
        QVERIFY(retained.has_value());
        QCOMPARE(retained->name(), QStringLiteral("primary_pic_type"));
        QCOMPARE(retained->value().toULongLong(), quint64(2));
    }

    void rejectsNonzeroAccessUnitDelimiterPaddingAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09, 0x54,
                                   0x00, 0x00, 0x01, 0x41}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto malformedNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(malformedNal.has_value());
        QCOMPARE(malformedNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(malformedNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto aud = analyzer->tree().node(rbsp->children().front());
        QVERIFY(aud.has_value());
        QCOMPARE(aud->state(), MaterializationState::Invalid);
        QCOMPARE(aud->children().size(), std::size_t(4));
        const auto nonzeroPadding = analyzer->tree().node(aud->children().at(3));
        QVERIFY(nonzeroPadding.has_value());
        QCOMPARE(nonzeroPadding->name(), QStringLiteral("rbsp_alignment_zero_bit[1]"));
        QCOMPARE(nonzeroPadding->value().toULongLong(), quint64(1));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsAnAccessUnitDelimiterWithUndeclaredTrailingBits() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09, 0x50, 0x55}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->diagnostics().size(), std::size_t(1));
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QVERIFY(rbsp->diagnostics().front().message.contains(QStringLiteral("8 undeclared")));

        const auto aud = analyzer->tree().node(rbsp->children().front());
        QVERIFY(aud.has_value());
        QCOMPARE(aud->state(), MaterializationState::Materialized);
        QCOMPARE(aud->children().size(), std::size_t(6));
    }

    void keepsUndispatchedNalUnitPayloadsUninterpreted() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0xAA, 0x00, 0x00, 0x01, 0x41}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));

        const auto withPayload = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(withPayload.has_value());
        QCOMPARE(withPayload->state(), MaterializationState::Materialized);
        QCOMPARE(withPayload->children().size(), std::size_t(3));
        const auto rbsp = analyzer->tree().node(withPayload->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QVERIFY(rbsp->children().empty());

        const auto headerOnly = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(headerOnly.has_value());
        QCOMPARE(headerOnly->state(), MaterializationState::Materialized);
        QCOMPARE(headerOnly->children().size(), std::size_t(2));
    }

    void rejectsInvalidMapperLimitsBeforeAnalysis() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0x12}));
        H264EbspRbspMapLimits limits;
        limits.maximumMappingSegments = 0;
        QString errorMessage;

        const auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, std::nullopt, limits);

        QVERIFY(!analyzer.has_value());
        QVERIFY(errorMessage.contains(QStringLiteral("greater than zero")));
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
        QVERIFY(firstBatch.progressiveIndexUpdate.has_value());
        QCOMPARE(firstBatch.progressiveIndexUpdate->firstRecordIndex, quint64{0});
        QCOMPARE(firstBatch.progressiveIndexUpdate->records.size(), std::size_t{1});
        QCOMPARE(firstBatch.progressiveIndexUpdate->indexedThroughByteOffset, quint64{5});
        QVERIFY(!firstBatch.progressiveIndexUpdate->endOfSource);

        const auto firstNal = analyzer->tree().node(firstBatch.nalUnitNodes.front());
        QVERIFY(firstNal.has_value());
        QCOMPARE(firstNal->kind(), AnalysisNodeKind::Region);
        QCOMPARE(firstNal->state(), MaterializationState::Materialized);
        QCOMPARE(firstNal->children().size(), std::size_t(3));

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
        QVERIFY(forbidden->metadata().specification.has_value());
        QCOMPARE(forbidden->metadata().specification->standard,
                 QStringLiteral("ITU-T H.264"));
        QCOMPARE(forbidden->metadata().specification->clause, QStringLiteral("7.3.1"));
        QVERIFY(!forbidden->metadata().description.isEmpty());
        QVERIFY(!referenceIdc->metadata().description.isEmpty());
        QVERIFY(!unitType->metadata().description.isEmpty());
        QCOMPARE(forbidden->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(24));
        QCOMPARE(referenceIdc->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(25));
        QCOMPARE(unitType->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(27));

        const auto firstRbsp = analyzer->tree().node(firstNal->children().at(2));
        QVERIFY(firstRbsp.has_value());
        QCOMPARE(firstRbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(firstRbsp->state(), MaterializationState::Materialized);
        QVERIFY(firstRbsp->location().has_value());
        QCOMPARE(firstRbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(firstRbsp->location()->sourceSpans().front().bitLength(), quint64(8));

        const auto secondBatch = analyzer->analyzeBatch(1);
        QCOMPARE(secondBatch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(secondBatch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(secondBatch.progressiveIndexUpdate.has_value());
        QCOMPARE(secondBatch.progressiveIndexUpdate->firstRecordIndex, quint64{1});
        QCOMPARE(secondBatch.progressiveIndexUpdate->records.size(), std::size_t{1});
        QCOMPARE(secondBatch.progressiveIndexUpdate->indexedThroughByteOffset, quint64{10});
        QVERIFY(secondBatch.progressiveIndexUpdate->endOfSource);
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

    void mapsRbspPayloadAndPublishesExcludedBytes() {
        MemorySource source(bytes(
            {0x00, 0x00, 0x01, 0x65, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        QCOMPARE(nal->children().size(), std::size_t(5));

        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->kind(), AnalysisNodeKind::Region);
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QVERIFY(rbsp->location().has_value());
        QCOMPARE(rbsp->location()->logicalRange().bitLength(), quint64(24));
        QCOMPARE(rbsp->location()->sourceSpans().size(), std::size_t(2));
        QCOMPARE(rbsp->location()->sourceSpans().at(0).start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().at(0).bitLength(), quint64(16));
        QCOMPARE(rbsp->location()->sourceSpans().at(1).start().absoluteBitOffset(),
                 quint64(56));
        QCOMPARE(rbsp->location()->sourceSpans().at(1).bitLength(), quint64(8));
        QCOMPARE(rbsp->metadata().specification->clause, QStringLiteral("7.3.1, 7.4.1"));

        const auto excluded = analyzer->tree().node(nal->children().at(3));
        QVERIFY(excluded.has_value());
        QCOMPARE(excluded->name(), QStringLiteral("emulation_prevention_three_byte[0]"));
        QCOMPARE(excluded->kind(), AnalysisNodeKind::Region);
        QCOMPARE(excluded->state(), MaterializationState::Materialized);
        QCOMPARE(excluded->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(48));
        QCOMPARE(excluded->location()->sourceSpans().front().bitLength(), quint64(8));
        const auto trailing = analyzer->tree().node(nal->children().at(4));
        QVERIFY(trailing.has_value());
        QCOMPARE(trailing->name(), QStringLiteral("trailing_zero_8bits"));
        QCOMPARE(trailing->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(64));
        QCOMPARE(trailing->location()->sourceSpans().front().bitLength(), quint64(16));
    }

    void publishesTrailingZerosAfterMappedPayload() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0x12, 0x00, 0x00}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->children().size(), std::size_t(4));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        const auto trailing = analyzer->tree().node(nal->children().at(3));
        QVERIFY(rbsp.has_value());
        QVERIFY(trailing.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().front().bitLength(), quint64(8));
        QCOMPARE(trailing->name(), QStringLiteral("trailing_zero_8bits"));
        QCOMPARE(trailing->state(), MaterializationState::Materialized);
        QCOMPARE(trailing->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(40));
        QCOMPARE(trailing->location()->sourceSpans().front().bitLength(), quint64(16));
    }

    void keepsEbspConformanceIssuesLocalAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0x00, 0x00,
                                   0x03, 0x04, 0x00, 0x00, 0x01, 0x41}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto firstNal = analyzer->tree().node(batch.nalUnitNodes.at(0));
        const auto secondNal = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(firstNal.has_value());
        QVERIFY(secondNal.has_value());
        QCOMPARE(firstNal->state(), MaterializationState::Invalid);
        QCOMPARE(firstNal->diagnostics().size(), std::size_t(1));
        QCOMPARE(firstNal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QVERIFY(firstNal->diagnostics().front().message.contains(QStringLiteral("00 00 03")));
        QCOMPARE(firstNal->diagnostics()
                     .front()
                     .location->sourceSpans()
                     .front()
                     .start()
                     .absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(firstNal->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(32));
        const auto rbsp = analyzer->tree().node(firstNal->children().at(2));
        const auto excluded = analyzer->tree().node(firstNal->children().at(3));
        QVERIFY(rbsp.has_value());
        QVERIFY(excluded.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(excluded->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(48));
        QCOMPARE(secondNal->state(), MaterializationState::Materialized);
        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
    }

    void boundsRbspMappingAcrossAnalysisBatches() {
        MemorySource source(
            bytes({0x00, 0x00, 0x01, 0x65, 0x12, 0x34, 0x56, 0x78, 0x9A}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto first = analyzer->analyzeBatch(1, 64, 2);
        QCOMPARE(first.status, H264AnnexBAnalysisStatus::InProgress);
        QVERIFY(first.nalUnitNodes.empty());
        const auto rootWhilePending = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootWhilePending.has_value());
        QCOMPARE(rootWhilePending->children().size(), std::size_t(1));
        const auto pendingNal = analyzer->tree().node(rootWhilePending->children().front());
        QVERIFY(pendingNal.has_value());
        QCOMPARE(pendingNal->state(), MaterializationState::Indexing);
        QCOMPARE(pendingNal->children().size(), std::size_t(2));

        const auto second = analyzer->analyzeBatch(1, 64, 2);
        QCOMPARE(second.status, H264AnnexBAnalysisStatus::InProgress);
        QVERIFY(second.nalUnitNodes.empty());
        const auto third = analyzer->analyzeBatch(1, 64, 2);
        QCOMPARE(third.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(third.nalUnitNodes.size(), std::size_t(1));
        QCOMPARE(third.nalUnitNodes.front(), pendingNal->id());
        const auto completedNal = analyzer->tree().node(third.nalUnitNodes.front());
        QVERIFY(completedNal.has_value());
        QCOMPARE(completedNal->state(), MaterializationState::Materialized);
        QCOMPARE(completedNal->children().size(), std::size_t(3));
    }

    void cancellationRetainsTheCommittedRbspPrefix() {
        CancellationSource cancellation;
        MemorySource source(
            bytes({0x00, 0x00, 0x01, 0x65, 0x12, 0x34, 0x56, 0x78, 0x9A}));
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto first = analyzer->analyzeBatch(1, 64, 2);
        QCOMPARE(first.status, H264AnnexBAnalysisStatus::InProgress);
        QVERIFY(first.nalUnitNodes.empty());
        QVERIFY(cancellation.requestCancellation());

        const auto cancelled = analyzer->analyzeBatch(1, 64, 2);

        QCOMPARE(cancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(cancelled.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(cancelled.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Cancelled);
        QCOMPARE(nal->children().size(), std::size_t(3));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Cancelled);
        QVERIFY(rbsp->location().has_value());
        QCOMPARE(rbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().front().bitLength(), quint64(16));
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::Cancelled);
    }

    void leavesExtensionHeaderPayloadUninterpreted() {
        for (const unsigned int nalUnitType : {14U, 20U, 21U}) {
            MemorySource source(bytes({0x00,
                                       0x00,
                                       0x01,
                                       0x60U | nalUnitType,
                                       0x00,
                                       0x00,
                                       0x03,
                                       0x02,
                                       0x00,
                                       0x00}));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            QCOMPARE(nal->children().size(), std::size_t(3));
            const auto header = analyzer->tree().node(nal->children().at(1));
            const auto trailing = analyzer->tree().node(nal->children().at(2));
            QVERIFY(header.has_value());
            QVERIFY(trailing.has_value());
            const auto type = analyzer->tree().node(header->children().at(2));
            QVERIFY(type.has_value());
            QCOMPARE(type->value().toULongLong(), quint64(nalUnitType));
            QCOMPARE(trailing->name(), QStringLiteral("trailing_zero_8bits"));
            QCOMPARE(trailing->location()->sourceSpans().front().start().absoluteBitOffset(),
                     quint64(64));
            QCOMPARE(trailing->location()->sourceSpans().front().bitLength(), quint64(16));
        }
    }

    void exposesBoundedScanProgressAndRejectsInvalidWorkBudget() {
        MemorySource source(std::vector<std::byte>(20, std::byte{0x12}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto invalid = analyzer->analyzeBatch(1, 0);
        QCOMPARE(invalid.status, H264AnnexBAnalysisStatus::InvalidBatchSize);
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QVERIFY(!analyzer->finished());

        const auto invalidMappingBudget = analyzer->analyzeBatch(1, 8, 0);
        QCOMPARE(invalidMappingBudget.status, H264AnnexBAnalysisStatus::InvalidBatchSize);
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QVERIFY(!analyzer->finished());

        const auto first = analyzer->analyzeBatch(1, 8);
        QCOMPARE(first.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(analyzer->scanCursor(), quint64(8));
        QVERIFY(!first.progressiveIndexUpdate.has_value());
        const auto second = analyzer->analyzeBatch(1, 8);
        QCOMPARE(second.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(analyzer->scanCursor(), quint64(16));
        QVERIFY(!second.progressiveIndexUpdate.has_value());
        const auto third = analyzer->analyzeBatch(1, 8);
        QCOMPARE(third.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(analyzer->scanCursor(), quint64(20));
        QVERIFY(third.progressiveIndexUpdate.has_value());
        QCOMPARE(third.progressiveIndexUpdate->firstRecordIndex, quint64{0});
        QCOMPARE(third.progressiveIndexUpdate->indexedThroughByteOffset, quint64{20});
        QVERIFY(third.progressiveIndexUpdate->records.empty());
        QVERIFY(third.progressiveIndexUpdate->endOfSource);
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

    void retainsMappedPrefixWhenPayloadReadingFails() {
        constexpr quint64 payloadLength = 65'537;
        std::vector<std::byte> data(static_cast<std::size_t>(4U + payloadLength),
                                    std::byte{0x12});
        data[0] = std::byte{0x00};
        data[1] = std::byte{0x00};
        data[2] = std::byte{0x01};
        data[3] = std::byte{0x65};
        FailAtOffsetSource source(std::move(data), 4U + 65'536U);
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        H264AnnexBAnalysisBatch terminalBatch;
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto batch = analyzer->analyzeBatch();
            if (batch.status == H264AnnexBAnalysisStatus::SourceError) {
                terminalBatch = std::move(batch);
                break;
            }
            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::InProgress);
            QVERIFY(batch.nalUnitNodes.empty());
        }

        QCOMPARE(terminalBatch.status, H264AnnexBAnalysisStatus::SourceError);
        QCOMPARE(terminalBatch.errorMessage, QStringLiteral("injected payload read failure"));
        QCOMPARE(terminalBatch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(terminalBatch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->children().size(), std::size_t(3));
        const auto header = analyzer->tree().node(nal->children().at(1));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(header.has_value());
        QVERIFY(rbsp.has_value());
        QCOMPARE(header->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::SourceError);
        QVERIFY(rbsp->location().has_value());
        QCOMPARE(rbsp->location()->sourceSpans().size(), std::size_t(1));
        QCOMPARE(rbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().front().bitLength(), quint64(65'536U * 8U));
    }

    void reportsMapperResourceLimitsWithTheCommittedPrefix() {
        MemorySource source(
            bytes({0x00, 0x00, 0x01, 0x65, 0x00, 0x00, 0x03, 0x01}));
        H264EbspRbspMapLimits limits;
        limits.maximumMappingSegments = 1;
        limits.maximumExcludedSpans = 4;
        limits.maximumIssues = 4;
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, std::nullopt, limits);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::ResourceLimit);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::ResourceLimit);
        QCOMPARE(nal->children().size(), std::size_t(4));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        const auto excluded = analyzer->tree().node(nal->children().at(3));
        QVERIFY(rbsp.has_value());
        QVERIFY(excluded.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().front().bitLength(), quint64(16));
        QCOMPARE(excluded->name(), QStringLiteral("emulation_prevention_three_byte[0]"));
        QCOMPARE(excluded->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(48));
        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
        QCOMPARE(root->diagnostics().front().code,
                 streamview::core::DiagnosticCode::ResourceLimit);
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
        QCOMPARE(nal->state(), MaterializationState::Cancelled);
        QCOMPARE(nal->children().size(), std::size_t(3));
        const auto header = analyzer->tree().node(nal->children().at(1));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(header.has_value());
        QVERIFY(rbsp.has_value());
        QCOMPARE(header->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->state(), MaterializationState::Cancelled);
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::Cancelled);
        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Cancelled);
        QCOMPARE(root->diagnostics().front().code,
                 streamview::core::DiagnosticCode::Cancelled);
    }

    void resumesScannerCancellationWithoutReplayingNodes() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0x12}));
        CancellationSource cancellation;
        QVERIFY(cancellation.requestCancellation());
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto cancelled = analyzer->analyzeBatch();
        QCOMPARE(cancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        QVERIFY(cancelled.nalUnitNodes.empty());
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QCOMPARE(analyzer->tree().nodeCount(), std::size_t(1));
        const auto cancelledRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(cancelledRoot.has_value());
        QCOMPARE(cancelledRoot->state(), MaterializationState::Cancelled);
        QCOMPARE(cancelledRoot->diagnostics().size(), std::size_t(1));

        QString resumeError = QStringLiteral("stale error");
        QVERIFY(analyzer->resumeAfterCancellation(std::nullopt, &resumeError));
        QVERIFY(resumeError.isEmpty());
        QVERIFY(!analyzer->finished());
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QCOMPARE(analyzer->tree().nodeCount(), std::size_t(1));
        const auto resumedRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(resumedRoot.has_value());
        QCOMPARE(resumedRoot->state(), MaterializationState::Indexing);
        QVERIFY(resumedRoot->diagnostics().empty());

        const auto resumed = analyzer->analyzeBatch();
        QCOMPARE(resumed.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(resumed.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(resumed.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->name(), QStringLiteral("nal_unit[0]"));
        const auto completedRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(completedRoot.has_value());
        QCOMPARE(completedRoot->state(), MaterializationState::Materialized);
        QVERIFY(completedRoot->diagnostics().empty());

        const auto replay = analyzer->analyzeBatch();
        QCOMPARE(replay.status, H264AnnexBAnalysisStatus::Complete);
        QVERIFY(replay.nalUnitNodes.empty());
    }

    void batchesCancelsAndResumesAHundredGigabyteSparseSource() {
        HundredGigabyteSparseSource source;
        CancellationSource cancellation;
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(
            source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto first = analyzer->analyzeBatch(1, 64, 64);
        QCOMPARE(first.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(first.nalUnitNodes.size(), std::size_t{1});
        const auto firstId = first.nalUnitNodes.front();
        QVERIFY(cancellation.requestCancellation());

        const auto cancelled = analyzer->analyzeBatch(1, 2048, 64);
        QCOMPARE(cancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        QVERIFY(cancelled.nalUnitNodes.empty());
        QVERIFY(analyzer->finished());
        const auto cancelledRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(cancelledRoot.has_value());
        QCOMPARE(cancelledRoot->children().size(), std::size_t{1});
        QCOMPARE(cancelledRoot->children().front(), firstId);

        QVERIFY(analyzer->resumeAfterCancellation());
        const auto resumed = analyzer->analyzeBatch(1, 64, 64);
        QCOMPARE(resumed.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(resumed.nalUnitNodes.size(), std::size_t{1});
        QVERIFY(resumed.nalUnitNodes.front() != firstId);
        const auto resumedRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(resumedRoot.has_value());
        QCOMPARE(resumedRoot->children().size(), std::size_t{2});
        QCOMPARE(resumedRoot->children().at(0), firstId);
        QCOMPARE(resumedRoot->children().at(1), resumed.nalUnitNodes.front());

        QVERIFY(source.maximumRequestSize() <= SourcePager::pageSizeBytes());
        QVERIFY(source.highestReadEnd() <= SourcePager::pageSizeBytes());
        QVERIFY(analyzer->scanCursor() < SourcePager::pageSizeBytes());
    }

    void resumesRepeatedMapperCancellationsWithStableAppendOnlyNodes() {
        std::vector<std::byte> data;
        const auto appendNal = [&data](quint8 header,
                                       std::size_t payloadLength,
                                       std::byte payloadByte) {
            const auto prefix = bytes({0x00, 0x00, 0x01});
            data.insert(data.end(), prefix.begin(), prefix.end());
            data.push_back(static_cast<std::byte>(header));
            data.insert(data.end(), payloadLength, payloadByte);
        };
        appendNal(0x65, 1025, std::byte{0x12});
        const quint64 secondStart = static_cast<quint64>(data.size());
        appendNal(0x41, 1025, std::byte{0x34});
        appendNal(0x41, 1, std::byte{0x56});

        ArmedCancellingSource source(std::move(data));
        CancellationSource initialCancellation;
        QVERIFY(initialCancellation.requestCancellation());
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(
            source, &errorMessage, initialCancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto initiallyCancelled = analyzer->analyzeBatch();
        QCOMPARE(initiallyCancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        QVERIFY(initiallyCancelled.nalUnitNodes.empty());

        CancellationSource firstMapperCancellation;
        source.arm(4, firstMapperCancellation);
        QVERIFY(analyzer->resumeAfterCancellation(firstMapperCancellation.token()));
        const auto firstCancelledNal = analyzer->analyzeBatch();
        QCOMPARE(firstCancelledNal.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(firstCancelledNal.nalUnitNodes.size(), std::size_t(1));
        const auto firstId = firstCancelledNal.nalUnitNodes.front();
        const auto firstNal = analyzer->tree().node(firstId);
        QVERIFY(firstNal.has_value());
        QCOMPARE(firstNal->name(), QStringLiteral("nal_unit[0]"));
        QCOMPARE(firstNal->state(), MaterializationState::Cancelled);

        const auto nodesBeforeSecondResume = analyzer->tree().nodeCount();
        CancellationSource secondMapperCancellation;
        source.arm(secondStart + 4U, secondMapperCancellation);
        QVERIFY(analyzer->resumeAfterCancellation(secondMapperCancellation.token()));
        QCOMPARE(analyzer->tree().nodeCount(), nodesBeforeSecondResume);
        const auto secondCancelledNal = analyzer->analyzeBatch();
        QCOMPARE(secondCancelledNal.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(secondCancelledNal.nalUnitNodes.size(), std::size_t(1));
        const auto secondId = secondCancelledNal.nalUnitNodes.front();
        QVERIFY(secondId != firstId);
        const auto secondNal = analyzer->tree().node(secondId);
        QVERIFY(secondNal.has_value());
        QCOMPARE(secondNal->name(), QStringLiteral("nal_unit[1]"));
        QCOMPARE(secondNal->state(), MaterializationState::Cancelled);

        const auto nodesBeforeFinalResume = analyzer->tree().nodeCount();
        QVERIFY(analyzer->resumeAfterCancellation());
        QCOMPARE(analyzer->tree().nodeCount(), nodesBeforeFinalResume);
        const auto completed = analyzer->analyzeBatch();
        QCOMPARE(completed.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(completed.nalUnitNodes.size(), std::size_t(1));
        const auto thirdId = completed.nalUnitNodes.front();
        QVERIFY(thirdId != firstId);
        QVERIFY(thirdId != secondId);
        const auto thirdNal = analyzer->tree().node(thirdId);
        QVERIFY(thirdNal.has_value());
        QCOMPARE(thirdNal->name(), QStringLiteral("nal_unit[2]"));
        QCOMPARE(thirdNal->state(), MaterializationState::Materialized);

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
        QVERIFY(root->diagnostics().empty());
        QCOMPARE(root->children().size(), std::size_t(3));
        QCOMPARE(root->children().at(0), firstId);
        QCOMPARE(root->children().at(1), secondId);
        QCOMPARE(root->children().at(2), thirdId);
        QVERIFY(analyzer->tree().hasPartialResults());
        QVERIFY(!analyzer->tree().isFullyMaterialized());
    }

    void rejectsRequestedReplacementTokenWithoutMutation() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65}));
        CancellationSource cancellation;
        QVERIFY(cancellation.requestCancellation());
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto cancelled = analyzer->analyzeBatch();
        QCOMPARE(cancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        const auto rootBefore = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootBefore.has_value());
        const auto nodeCount = analyzer->tree().nodeCount();
        const auto cursor = analyzer->scanCursor();

        CancellationSource replacement;
        QVERIFY(replacement.requestCancellation());
        QString resumeError;
        QVERIFY(!analyzer->resumeAfterCancellation(replacement.token(), &resumeError));
        QVERIFY(resumeError.contains(QStringLiteral("already requested")));
        QVERIFY(analyzer->finished());
        QCOMPARE(analyzer->tree().nodeCount(), nodeCount);
        QCOMPARE(analyzer->scanCursor(), cursor);
        const auto rootAfter = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootAfter.has_value());
        QCOMPARE(rootAfter->state(), rootBefore->state());
        QCOMPARE(rootAfter->diagnostics().size(), rootBefore->diagnostics().size());
        QCOMPARE(rootAfter->diagnostics().front().message,
                 rootBefore->diagnostics().front().message);

        const auto replay = analyzer->analyzeBatch();
        QCOMPARE(replay.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(replay.errorMessage, cancelled.errorMessage);
        QVERIFY(replay.nalUnitNodes.empty());
    }

    void rejectsNonCancelledAnalyzersWithoutMutation() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        QString resumeError;
        QVERIFY(!analyzer->resumeAfterCancellation(std::nullopt, &resumeError));
        QVERIFY(resumeError.contains(QStringLiteral("cancelled")));
        QVERIFY(!analyzer->finished());
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QCOMPARE(analyzer->tree().nodeCount(), std::size_t(1));

        const auto completed = analyzer->analyzeBatch();
        QCOMPARE(completed.status, H264AnnexBAnalysisStatus::Complete);
        const auto rootBefore = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootBefore.has_value());
        const auto nodeCount = analyzer->tree().nodeCount();
        const auto cursor = analyzer->scanCursor();
        QVERIFY(!analyzer->resumeAfterCancellation());
        QCOMPARE(analyzer->tree().nodeCount(), nodeCount);
        QCOMPARE(analyzer->scanCursor(), cursor);
        const auto rootAfter = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootAfter.has_value());
        QCOMPARE(rootAfter->state(), rootBefore->state());
        QCOMPARE(rootAfter->diagnostics().size(), rootBefore->diagnostics().size());
        const auto replay = analyzer->analyzeBatch();
        QCOMPARE(replay.status, H264AnnexBAnalysisStatus::Complete);
        QVERIFY(replay.nalUnitNodes.empty());
    }

    void rejectsIrrecoverableFailureStatesWithoutMutation() {
        FailAfterFirstReadSource failingSource;
        QString errorMessage;
        auto sourceErrorAnalyzer = H264AnnexBAnalyzer::create(failingSource, &errorMessage);
        QVERIFY2(sourceErrorAnalyzer.has_value(), qPrintable(errorMessage));
        const auto sourceError = sourceErrorAnalyzer->analyzeBatch();
        QCOMPARE(sourceError.status, H264AnnexBAnalysisStatus::SourceError);
        const auto sourceErrorCount = sourceErrorAnalyzer->tree().nodeCount();
        const auto sourceErrorRoot =
            sourceErrorAnalyzer->tree().node(sourceErrorAnalyzer->tree().rootId());
        QVERIFY(sourceErrorRoot.has_value());
        QVERIFY(!sourceErrorAnalyzer->resumeAfterCancellation());
        QCOMPARE(sourceErrorAnalyzer->tree().nodeCount(), sourceErrorCount);
        QCOMPARE(sourceErrorAnalyzer->tree()
                     .node(sourceErrorAnalyzer->tree().rootId())
                     ->state(),
                 sourceErrorRoot->state());
        QCOMPARE(sourceErrorAnalyzer->analyzeBatch().status,
                 H264AnnexBAnalysisStatus::SourceError);

        MemorySource limitedSource(
            bytes({0x00, 0x00, 0x01, 0x65, 0x00, 0x00, 0x03, 0x01}));
        H264EbspRbspMapLimits limits;
        limits.maximumMappingSegments = 1;
        limits.maximumExcludedSpans = 4;
        limits.maximumIssues = 4;
        auto limitedAnalyzer =
            H264AnnexBAnalyzer::create(limitedSource, &errorMessage, std::nullopt, limits);
        QVERIFY2(limitedAnalyzer.has_value(), qPrintable(errorMessage));
        const auto limited = limitedAnalyzer->analyzeBatch();
        QCOMPARE(limited.status, H264AnnexBAnalysisStatus::ResourceLimit);
        const auto limitedCount = limitedAnalyzer->tree().nodeCount();
        const auto limitedRoot =
            limitedAnalyzer->tree().node(limitedAnalyzer->tree().rootId());
        QVERIFY(limitedRoot.has_value());
        QVERIFY(!limitedAnalyzer->resumeAfterCancellation());
        QCOMPARE(limitedAnalyzer->tree().nodeCount(), limitedCount);
        QCOMPARE(limitedAnalyzer->tree().node(limitedAnalyzer->tree().rootId())->state(),
                 limitedRoot->state());
        QCOMPARE(limitedAnalyzer->analyzeBatch().status,
                 H264AnnexBAnalysisStatus::ResourceLimit);
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
