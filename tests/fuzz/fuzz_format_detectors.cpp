#include "standalone_fuzz_driver.h"

#include <streamview/core/source.h>
#include <streamview/rules/aac_adts_detector.h>
#include <streamview/rules/aac_adts_scanner.h>
#include <streamview/rules/h264_annex_b_detector.h>
#include <streamview/rules/h264_start_code_scanner.h>
#include <streamview/rules/mp4_box_detector.h>
#include <streamview/rules/mp4_box_scanner.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

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

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
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
