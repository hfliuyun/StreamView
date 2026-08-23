#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/paged_cache.h>
#include <streamview/core/sample_descriptor.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace streamview::rules {

/// Composite sample-table index for ISOBMFF tracks.
///
/// This component is deliberately MP4-specific and lives in `src/rules/`: it
/// consumes ISOBMFF box semantics (`stts`, `stsc`, `stsz`/`stz2`, `stco`/`co64`,
/// `stss`, `ctts`). Only its output type, `core::SampleDescriptor`, is
/// format-neutral. See ADR-0105 section 2 and slice P5j-2.
///
/// Memory is bounded independently of sample count. Run-length tables (`stts`,
/// `stsc`, `ctts`) are held as rows, which is bounded by run count, not sample
/// count. The tables that carry one entry per sample or per chunk (`stsz`/`stz2`,
/// `stco`/`co64`, `stss`) are never expanded: they are read through
/// `Mp4SampleTableReaders` for only the entries a requested page actually
/// touches. No sample payload is ever copied; each descriptor carries
/// `core::SourceSpan` coordinates into the root media source.

enum class Mp4SampleTableIndexStatus : quint8 {
    Built,
    /// Tables disagree with each other or with their declared counts.
    InconsistentTables,
    /// A table version or variant this slice does not decode.
    UnsupportedTables,
    /// Checked arithmetic rejected a wrap in an offset, extent, or timeline.
    ArithmeticOverflow,
    /// A sample resolves outside the media source, or two samples overlap.
    OutOfSourceRange,
    /// Malformed page request, or a coordinate the core types cannot represent.
    InvalidRequest,
    /// The page exceeded its descriptor or table-read budget.
    ResourceLimit,
    /// A table reader could not supply an entry it was asked for.
    SourceError,
    Cancelled,
};

/// One `stts` row: `sampleCount` consecutive samples sharing `sampleDelta`.
struct Mp4TimeToSampleRow final {
    quint64 sampleCount = 0;
    quint64 sampleDelta = 0;
};

/// One `stsc` row. `firstChunk` is 1-based per ISO/IEC 14496-12.
struct Mp4SampleToChunkRow final {
    quint64 firstChunk = 0;
    quint64 samplesPerChunk = 0;
    quint32 sampleDescriptionIndex = 1;
};

/// One `ctts` row. The offset stays raw and unsigned exactly as the rule decoded
/// it; sign reinterpretation is keyed on `compositionOffsetVersion` and happens
/// only when a presentation timestamp is derived (ADR-0105 section 3.2).
struct Mp4CompositionOffsetRow final {
    quint64 sampleCount = 0;
    quint64 rawSampleOffset = 0;
};

/// Binds a 1-based `sample_description_index` to the rule's `stsd` entry and the
/// target format that entry declares.
///
/// This binding lives in the rules layer because it carries ISOBMFF and codec
/// identity. `core::SampleDescriptor` deliberately holds only the numeric index,
/// so no codec string reaches `src/core/` (ADR-0105 section 2).
struct Mp4SampleDescriptionBinding final {
    quint32 sampleDescriptionIndex = 0;
    /// The `stsd` entry structure in the analyzed tree.
    core::AnalysisNodeId entryNode{0};
    /// Target format the rule declared for this entry, e.g. `video.h264.nal`.
    /// Empty when the entry declares none.
    QString targetFormat;
};

/// On-demand accessors for the tables that carry one entry per sample or chunk.
///
/// Each reader returns `std::nullopt` when the underlying table cannot supply the
/// entry, which the index reports as `SourceError` for the requested page only.
struct Mp4SampleTableReaders final {
    /// 0-based sample index to size in bytes, from `stsz` entries or `stz2`.
    /// Not consulted when `Mp4TrackSampleTables::defaultSampleSize` is non-zero.
    std::function<std::optional<quint64>(quint64)> sampleSize;
    /// 0-based chunk index to absolute byte offset, from `stco` or `co64`.
    std::function<std::optional<quint64>(quint64)> chunkOffset;
    /// 1-based sample number to whether `stss` lists it. Consulted only when
    /// `Mp4TrackSampleTables::hasSyncSampleTable` is true.
    std::function<std::optional<bool>(quint64)> isSyncSample;
};

/// Track-level scalars and run-length tables of a single track.
struct Mp4TrackSampleTables final {
    quint32 trackId = 0;
    /// Media timescale from `mdhd`. Must be non-zero.
    quint32 timescale = 1;
    /// Size of the root media source in bytes, used to reject out-of-source
    /// samples. Zero disables the check for callers without a bounded source.
    quint64 sourceSizeBytes = 0;

    std::vector<Mp4TimeToSampleRow> timeToSample;    // stts
    std::vector<Mp4SampleToChunkRow> sampleToChunk;  // stsc

    /// `stco`/`co64` `entry_count`. The offsets themselves are read on demand.
    quint64 chunkCount = 0;

    /// Non-zero `stsz.sample_size`: every sample shares this size and the
    /// `sampleSize` reader is never consulted. Zero means the reader is
    /// authoritative for per-sample sizes.
    quint64 defaultSampleSize = 0;
    /// `stsz`/`stz2` `sample_count`. Authoritative sample count for the track.
    quint64 declaredSampleCount = 0;

    /// False when the track has no `stss`, in which case every sample is a sync
    /// sample (ISO/IEC 14496-12 section 8.6.2.1). Distinct from a present but
    /// empty table.
    bool hasSyncSampleTable = false;

    std::vector<Mp4CompositionOffsetRow> compositionOffsets;  // ctts
    /// `ctts` version as decoded by the rule; governs sign reinterpretation.
    quint8 compositionOffsetVersion = 0;

    /// `stsd` entries in declaration order, 1-based indices.
    std::vector<Mp4SampleDescriptionBinding> sampleDescriptions;
};

struct Mp4SamplePageRequest final {
    quint64 pageIndex = 0;
    quint64 pageSize = 256;
    /// Upper bound on descriptors materialized for one page request.
    quint64 maximumDescriptorsPerPage = 4096;
    /// Upper bound on table-reader calls for one page request. Bounds the
    /// mid-chunk catch-up a page starting inside a large chunk has to perform.
    quint64 maximumTableReadsPerPage = 1U << 20U;
    std::optional<core::CancellationToken> cancellation;
};

struct Mp4SamplePageResult final {
    Mp4SampleTableIndexStatus status = Mp4SampleTableIndexStatus::InconsistentTables;
    std::vector<core::SampleDescriptor> descriptors;
    quint64 firstSampleIndex = 0;
    /// Table-reader calls this page performed. Lets callers assert that work is
    /// bounded by page size rather than by table length.
    quint64 tableReadCount = 0;
    /// Cache page identity for this descriptor page. Distinct tracks never
    /// collide (ADR-0105 section 6 item 1).
    core::PagedCachePageKey cacheKey;
    QString errorMessage;

    [[nodiscard]] bool built() const noexcept {
        return status == Mp4SampleTableIndexStatus::Built;
    }
};

struct Mp4SampleTableIndexBuildResult;

class Mp4SampleTableIndex final {
public:
    /// Discriminates this index kind within a track's cache namespace.
    static constexpr quint8 kSampleIndexKind = 1;

    /// Largest number of run-length rows accepted for one table, so a malformed
    /// declaration cannot make the index itself unbounded.
    static constexpr std::size_t maximumRunRows() noexcept { return 1U << 22U; }

    /// Validates the run-length tables against each other and against the
    /// declared sample count. Every cross-table disagreement is rejected here
    /// rather than surfacing as a malformed descriptor later.
    [[nodiscard]] static Mp4SampleTableIndexBuildResult
    build(Mp4TrackSampleTables tables, Mp4SampleTableReaders readers);

    /// Sample count the tables agree on.
    [[nodiscard]] quint64 sampleCount() const noexcept { return sampleCount_; }

    [[nodiscard]] const Mp4TrackSampleTables& tables() const noexcept { return tables_; }

    [[nodiscard]] core::PagedCachePageKey cachePageKey(quint64 pageIndex) const noexcept;

    /// Resolves a 1-based `sample_description_index` to its `stsd` binding.
    [[nodiscard]] const Mp4SampleDescriptionBinding*
    sampleDescription(quint32 sampleDescriptionIndex) const noexcept;

    /// Builds descriptors for one page. Descriptors are computed fresh per call
    /// and never retained on the index, so repeating a page request is
    /// idempotent and allocates no more than one page.
    [[nodiscard]] Mp4SamplePageResult descriptorPage(const Mp4SamplePageRequest& request) const;

private:
    /// Cursor over the sample-to-chunk layout, advanced sample by sample so a
    /// page costs reader calls proportional to the page, not to the table.
    struct ChunkCursor final {
        std::size_t runIndex = 0;
        quint64 runFirstSampleIndex = 0;
        quint64 chunkIndex = 0;          // 0-based
        quint64 ordinalInChunk = 0;
        quint64 offsetInChunk = 0;       // accumulated sizes of preceding samples
        quint64 chunkStartOffset = 0;    // absolute byte offset of the chunk
        quint32 sampleDescriptionIndex = 1;
    };

    /// Cursor over `stts`. Seeking costs one pass over the runs per page; every
    /// later sample in the page is O(1), so per-page work stays proportional to
    /// the page rather than to run count times page size.
    struct TimelineCursor final {
        std::size_t runIndex = 0;
        quint64 ordinalInRun = 0;
        qint64 dts = 0;  // decode time of the sample the cursor points at
    };

    /// Cursor over `ctts`, with the same seek-once-then-advance shape.
    struct CompositionCursor final {
        std::size_t runIndex = 0;
        quint64 ordinalInRun = 0;
    };

    Mp4SampleTableIndex(Mp4TrackSampleTables tables,
                        Mp4SampleTableReaders readers,
                        quint64 sampleCount);

    [[nodiscard]] quint64 samplesInRun(std::size_t runIndex) const noexcept;

    /// Positions a cursor on `sampleIndex`, performing the mid-chunk catch-up
    /// needed when a page does not start on a chunk boundary.
    [[nodiscard]] bool seekCursor(quint64 sampleIndex,
                                  ChunkCursor* cursor,
                                  Mp4SamplePageResult* result,
                                  const Mp4SamplePageRequest& request) const;

    [[nodiscard]] bool advanceCursor(ChunkCursor* cursor,
                                     quint64 consumedSampleSize,
                                     Mp4SamplePageResult* result,
                                     const Mp4SamplePageRequest& request) const;

    [[nodiscard]] std::optional<quint64> readSampleSize(quint64 sampleIndex,
                                                        Mp4SamplePageResult* result,
                                                        const Mp4SamplePageRequest& request) const;

    [[nodiscard]] std::optional<quint64> readChunkOffset(quint64 chunkIndex,
                                                         Mp4SamplePageResult* result,
                                                         const Mp4SamplePageRequest& request) const;

    /// Positions a timeline cursor on `sampleIndex`, accumulating its decode time.
    [[nodiscard]] bool seekTimeline(quint64 sampleIndex,
                                    TimelineCursor* cursor,
                                    Mp4SampleTableIndexStatus* failure,
                                    QString* errorMessage) const noexcept;

    /// Moves a timeline cursor to the next sample in O(1).
    [[nodiscard]] bool advanceTimeline(TimelineCursor* cursor,
                                       Mp4SampleTableIndexStatus* failure,
                                       QString* errorMessage) const noexcept;

    [[nodiscard]] quint64 durationAt(const TimelineCursor& cursor) const noexcept;

    [[nodiscard]] bool seekComposition(quint64 sampleIndex,
                                       CompositionCursor* cursor) const noexcept;

    [[nodiscard]] bool advanceComposition(CompositionCursor* cursor) const noexcept;

    [[nodiscard]] qint64 compositionOffsetAt(const CompositionCursor& cursor) const noexcept;

    Mp4TrackSampleTables tables_;
    Mp4SampleTableReaders readers_;
    quint64 sampleCount_ = 0;
};

struct Mp4SampleTableIndexBuildResult final {
    Mp4SampleTableIndexStatus status = Mp4SampleTableIndexStatus::InconsistentTables;
    std::optional<Mp4SampleTableIndex> index;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == Mp4SampleTableIndexStatus::Built && index.has_value();
    }
};

} // namespace streamview::rules
