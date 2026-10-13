#include "standalone_fuzz_driver.h"

#include <streamview/core/source.h>
#include <streamview/rules/aac_adts_detector.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/aac_adts_scanner.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/h264_annex_b_detector.h>
#include <streamview/rules/h264_ebsp_rbsp_mapper.h>
#include <streamview/rules/h264_start_code_scanner.h>
#include <streamview/rules/mp4_box_detector.h>
#include <streamview/rules/mp4_box_scanner.h>
#include <streamview/rules/structural_entry_runner.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>

namespace {

class FuzzMemorySource final : public streamview::core::RandomAccessSource {
public:
    explicit FuzzMemorySource(const uint8_t* data, size_t size)
        : data_(data), size_(size) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(size_);
    }

    [[nodiscard]] QString identity() const override {
        return QStringLiteral("fuzz_format_source");
    }

    [[nodiscard]] streamview::core::SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (byteOffset >= size_) {
            return {streamview::core::SourceReadStatus::EndOfSource, 0, QStringLiteral("EOF")};
        }
        const size_t available = size_ - static_cast<size_t>(byteOffset);
        const size_t toCopy = std::min(available, destination.size());
        if (toCopy > 0) {
            std::memcpy(destination.data(), data_ + byteOffset, toCopy);
        }
        if (toCopy < destination.size()) {
            return {streamview::core::SourceReadStatus::EndOfSource, static_cast<quint64>(toCopy), {}};
        }
        return {streamview::core::SourceReadStatus::Complete, static_cast<std::size_t>(toCopy), {}};
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

const streamview::rules::DslTypedProgram& ascProgram() {
    static const auto program = [] {
        const auto package = streamview::rules::loadAacAdtsRulePackage();
        if (!package.succeeded()) {
            std::abort();
        }
        const auto* text = package.package->fileContents(QStringLiteral("src/aac_asc.svfmt"));
        if (text == nullptr) {
            std::abort();
        }
        const auto parsed = streamview::rules::DslParser::parse(QString::fromUtf8(*text));
        auto compiled = streamview::rules::DslCompiler::compile(parsed.program);
        if (!parsed.succeeded() || !compiled.succeeded()) {
            std::abort();
        }
        return std::move(*compiled.program);
    }();
    return program;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > streamview::fuzz::kMaxFuzzInputSize) {
        return 0;
    }

    const std::span<const std::byte> sourcePrefix(
        reinterpret_cast<const std::byte*>(data), size);
    const quint64 sourceSizeBytes = static_cast<quint64>(size);
    const FuzzMemorySource source(data, size);

    // 1. H.264 Annex B detection and start-code scanning
    {
        const auto detectResult = streamview::rules::detectH264AnnexBCandidate(
            sourcePrefix, sourceSizeBytes);
        (void)detectResult.candidate.has_value();

        streamview::rules::H264StartCodeScanner h264Scanner(source);
        for (int batch = 0; batch < 4; ++batch) {
            const auto scanBatch = h264Scanner.scanBatch(32, 4096);
            if (scanBatch.complete()) {
                break;
            }
        }
    }

    // 2. AAC ADTS detection and frame scanning
    {
        const auto detectResult = streamview::rules::detectAacAdtsCandidate(
            sourcePrefix, sourceSizeBytes);
        (void)detectResult.candidate.has_value();

        streamview::rules::AacAdtsScanner aacScanner(source);
        for (int batch = 0; batch < 4; ++batch) {
            const auto scanBatch = aacScanner.scanBatch(32, 4096);
            if (scanBatch.complete()) {
                break;
            }
        }
    }

    // 3. MP4 Box detection and hierarchy scanning
    {
        const auto detectResult = streamview::rules::detectMp4Candidate(
            sourcePrefix, sourceSizeBytes);
        (void)detectResult.candidate.has_value();

        streamview::rules::Mp4BoxScanner mp4Scanner(source);
        for (int batch = 0; batch < 4; ++batch) {
            const auto scanBatch = mp4Scanner.scanBatch(32, 4096);
            if (scanBatch.complete()) {
                break;
            }
        }
    }

    auto h264 = streamview::rules::H264AnnexBAnalyzer::create(source);
    auto aac = streamview::rules::AacAdtsAnalyzer::create(source);
    if (!h264 || !aac) {
        std::abort();
    }
    for (int batch = 0; batch < 4; ++batch) {
        if (h264->analyzeBatch(8, 4096, 4096).status !=
            streamview::rules::H264AnnexBAnalysisStatus::InProgress) {
            break;
        }
    }
    for (int batch = 0; batch < 4; ++batch) {
        if (aac->analyzeBatch(8, 4096).status !=
            streamview::rules::AacAdtsAnalysisStatus::InProgress) {
            break;
        }
    }

    const quint64 sourceBits = sourceSizeBytes * 8U;
    const auto span = streamview::core::SourceSpan::create(
        streamview::core::SourceBitAddress(0), sourceBits);
    if (!span) {
        std::abort();
    }
    const auto mapping = streamview::core::SourceMapping::create(
        streamview::core::LogicalViewId(1), {*span});
    if (!mapping) {
        std::abort();
    }
    streamview::rules::StructuralExecutionOptions options;
    options.limits.maximumInstructions = 10'000;
    options.limits.maximumMaterializedNodes = 1'000;
    (void)streamview::rules::StructuralEntryRunner::execute(
        source, *mapping, ascProgram(), options);

    streamview::rules::H264EbspRbspMapper mapper(
        source, streamview::core::LogicalViewId(2), *span, {512, 512, 512});
    for (int batch = 0; batch < 4; ++batch) {
        const auto result = mapper.mapBatch(4096);
        if (result.complete()) {
            quint64 accountedBits = mapper.mapping().logicalBitLength();
            for (const auto& forwarded : mapper.mapping().sourceSpans()) {
                if (forwarded.endExclusive().absoluteBitOffset() > sourceBits) {
                    std::abort();
                }
            }
            for (const auto& excluded : mapper.excludedSpans()) {
                if (excluded.sourceSpan.endExclusive().absoluteBitOffset() > sourceBits) {
                    std::abort();
                }
                accountedBits += excluded.sourceSpan.bitLength();
            }
            if (accountedBits != sourceBits) {
                std::abort();
            }
            break;
        }
        if (result.status != streamview::rules::H264EbspRbspMapStatus::InProgress) {
            break;
        }
    }

    return 0;
}

#if !defined(STREAMVIEW_ENABLE_LIBFUZZER)
int main(int argc, char** argv) {
#if defined(STREAMVIEW_FUZZ_CORPUS_DIR)
    return streamview::fuzz::runStandalone(argc, argv, STREAMVIEW_FUZZ_CORPUS_DIR);
#else
    return streamview::fuzz::runStandalone(argc, argv, nullptr);
#endif
}
#endif
