#include "standalone_fuzz_driver.h"

#include <streamview/core/source.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/mp4_sample_table_extractor.h>
#include <streamview/rules/mp4_sample_table_index.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using streamview::rules::Mp4CompositionOffsetRow;
using streamview::rules::Mp4IsobmffAnalyzer;
using streamview::rules::Mp4SamplePageRequest;
using streamview::rules::Mp4SampleTableExtractionRequest;
using streamview::rules::Mp4SampleTableExtractor;
using streamview::rules::Mp4SampleTableIndex;
using streamview::rules::Mp4SampleTableReaders;
using streamview::rules::Mp4SampleToChunkRow;
using streamview::rules::Mp4TimeToSampleRow;
using streamview::rules::Mp4TrackSampleTables;

namespace {

class FuzzMemorySource final : public streamview::core::RandomAccessSource {
public:
    explicit FuzzMemorySource(const uint8_t* data, size_t size)
        : data_(data), size_(size) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(size_);
    }

    [[nodiscard]] QString identity() const override {
        return QStringLiteral("fuzz_mp4_source");
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
            return {streamview::core::SourceReadStatus::EndOfSource, static_cast<std::size_t>(toCopy), {}};
        }
        return {streamview::core::SourceReadStatus::Complete, static_cast<std::size_t>(toCopy), {}};
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

template <typename T>
T readScalar(const uint8_t*& cursor, const uint8_t* end) {
    if (cursor + sizeof(T) > end) {
        return 0;
    }
    T val;
    std::memcpy(&val, cursor, sizeof(T));
    cursor += sizeof(T);
    return val;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }

    // Path 1: End-to-end extraction from parsed ISOBMFF tree
    {
        const FuzzMemorySource source(data, size);
        auto analyzerOpt = Mp4IsobmffAnalyzer::create(source);
        if (analyzerOpt.has_value()) {
            Mp4IsobmffAnalyzer& analyzer = *analyzerOpt;
            const auto batch = analyzer.analyzeBatch(32, 64 * 1024);
            Mp4SampleTableExtractionRequest request;
            request.sourceSizeBytes = static_cast<quint64>(size);
            request.maximumRunRowsPerTable = 1024;
            request.runRowPageSize = 64;

            const auto extraction = Mp4SampleTableExtractor::extract(analyzer, batch, request);
            if (extraction.extracted()) {
                for (const auto& track : extraction.tracks) {
                    auto indexResult = Mp4SampleTableIndex::build(track.tables, track.readers);
                    if (indexResult.succeeded() && indexResult.index.has_value()) {
                        (void)indexResult.index->descriptorPage(
                            Mp4SamplePageRequest{ .pageIndex = 0, .pageSize = 16 });
                    }
                }
            }
        }
    }

    // Path 2: Direct synthetic table fuzzing for arithmetic and bounds verification
    if (size >= 32) {
        const uint8_t* cursor = data;
        const uint8_t* end = data + size;

        Mp4TrackSampleTables tables;
        tables.trackId = readScalar<quint32>(cursor, end);
        tables.timescale = readScalar<quint32>(cursor, end);
        if (tables.timescale == 0) {
            tables.timescale = 1000;
        }
        tables.sourceSizeBytes = readScalar<quint64>(cursor, end);
        tables.declaredSampleCount = readScalar<quint32>(cursor, end) % 65536U;
        tables.chunkCount = readScalar<quint32>(cursor, end) % 65536U;

        // Populate run-length tables with bounds-capped rows
        const size_t rowBudget = 32;
        while (cursor + 16 <= end && tables.timeToSample.size() < rowBudget) {
            const quint64 count = readScalar<quint64>(cursor, end);
            const quint64 delta = readScalar<quint64>(cursor, end);
            tables.timeToSample.push_back(Mp4TimeToSampleRow{count, delta});
        }

        while (cursor + 24 <= end && tables.sampleToChunk.size() < rowBudget) {
            const quint64 firstChunk = readScalar<quint64>(cursor, end);
            const quint64 samplesPerChunk = readScalar<quint64>(cursor, end);
            const quint32 descIndex = readScalar<quint32>(cursor, end);
            tables.sampleToChunk.push_back(
                Mp4SampleToChunkRow{firstChunk, samplesPerChunk, descIndex});
        }

        Mp4SampleTableReaders readers;
        readers.sampleSize = [](quint64 idx) -> std::optional<quint64> {
            return (idx < 65536) ? std::optional<quint64>(idx * 16 + 8) : std::nullopt;
        };
        readers.chunkOffset = [](quint64 idx) -> std::optional<quint64> {
            return (idx < 65536) ? std::optional<quint64>(idx * 64 + 1024) : std::nullopt;
        };
        readers.isSyncSample = [](quint64 idx) -> std::optional<bool> {
            return (idx < 1024) ? std::optional<bool>((idx % 10) == 1) : std::nullopt;
        };

        auto indexResult = Mp4SampleTableIndex::build(tables, readers);
        if (indexResult.succeeded() && indexResult.index.has_value()) {
            (void)indexResult.index->descriptorPage(
                Mp4SamplePageRequest{ .pageIndex = 0, .pageSize = 32 });
            (void)indexResult.index->descriptorPage(
                Mp4SamplePageRequest{ .pageIndex = 1, .pageSize = 16 });
            (void)indexResult.index->descriptorPage(
                Mp4SamplePageRequest{ .pageIndex = 1000000, .pageSize = 32 });
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
