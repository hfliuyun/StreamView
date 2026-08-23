#include <streamview/rules/mp4_sample_table_index.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace streamview::rules {

namespace {

constexpr quint64 kMaxByteCoordinate = std::numeric_limits<quint64>::max() / 8U;

[[nodiscard]] bool addWouldOverflow(quint64 left, quint64 right) noexcept {
    return right > std::numeric_limits<quint64>::max() - left;
}

[[nodiscard]] bool multiplyWouldOverflow(quint64 left, quint64 right) noexcept {
    return left != 0 && right > std::numeric_limits<quint64>::max() / left;
}

/// Signed timeline accumulation. `qint64` overflow is UB, so every step is pre-checked.
[[nodiscard]] bool signedAddWouldOverflow(qint64 left, qint64 right) noexcept {
    if (right > 0) {
        return left > std::numeric_limits<qint64>::max() - right;
    }
    if (right < 0) {
        return left < std::numeric_limits<qint64>::min() - right;
    }
    return false;
}

[[nodiscard]] bool unsignedFitsSigned(quint64 value) noexcept {
    return value <= static_cast<quint64>(std::numeric_limits<qint64>::max());
}

/// ISO/IEC 14496-12 ctts version 1: sample_offset is two's-complement signed 32-bit.
/// The DSL has no signed fixed-width field type (ADR-0105 section 3.2), so the rule decodes
/// the raw unsigned value and the sign is reinterpreted here, keyed on the decoded version.
[[nodiscard]] qint64 reinterpretCompositionOffset(quint64 rawOffset, quint8 version) noexcept {
    const quint64 truncated = rawOffset & 0xFFFFFFFFULL;
    if (version == 0) {
        return static_cast<qint64>(truncated);
    }
    if (truncated >= 0x80000000ULL) {
        return static_cast<qint64>(truncated) - static_cast<qint64>(0x100000000ULL);
    }
    return static_cast<qint64>(truncated);
}

/// Half-open byte ranges intersect. Used to reject overlapping samples without
/// assuming chunk offsets increase, which ISO/IEC 14496-12 does not require.
[[nodiscard]] bool rangesIntersect(quint64 firstStart,
                                   quint64 firstSize,
                                   quint64 secondStart,
                                   quint64 secondSize) noexcept {
    if (firstSize == 0 || secondSize == 0) {
        return false;
    }
    return firstStart < secondStart + secondSize && secondStart < firstStart + firstSize;
}

} // namespace

Mp4SampleTableIndexBuildResult Mp4SampleTableIndex::build(Mp4TrackSampleTables tables,
                                                          Mp4SampleTableReaders readers) {
    Mp4SampleTableIndexBuildResult result;

    if (tables.timescale == 0) {
        result.status = Mp4SampleTableIndexStatus::InconsistentTables;
        result.errorMessage = QStringLiteral("Track timescale must be greater than zero");
        return result;
    }

    if (tables.compositionOffsetVersion > 1) {
        result.status = Mp4SampleTableIndexStatus::UnsupportedTables;
        result.errorMessage = QStringLiteral("Unsupported ctts version");
        return result;
    }

    // Run-length tables are the only per-track allocation, so their length is capped.
    if (tables.timeToSample.size() > maximumRunRows()
        || tables.sampleToChunk.size() > maximumRunRows()
        || tables.compositionOffsets.size() > maximumRunRows()) {
        result.status = Mp4SampleTableIndexStatus::ResourceLimit;
        result.errorMessage = QStringLiteral("Sample table run count exceeds the index budget");
        return result;
    }

    if (!readers.chunkOffset) {
        result.status = Mp4SampleTableIndexStatus::InvalidRequest;
        result.errorMessage = QStringLiteral("A chunk offset reader is required");
        return result;
    }
    if (tables.defaultSampleSize == 0 && !readers.sampleSize) {
        result.status = Mp4SampleTableIndexStatus::InvalidRequest;
        result.errorMessage =
            QStringLiteral("A sample size reader is required when stsz declares no default size");
        return result;
    }
    if (tables.hasSyncSampleTable && !readers.isSyncSample) {
        result.status = Mp4SampleTableIndexStatus::InvalidRequest;
        result.errorMessage = QStringLiteral("A sync sample reader is required when stss exists");
        return result;
    }

    // stsz/stz2 sample_count is authoritative; every other table must agree with it.
    const quint64 sampleCount = tables.declaredSampleCount;

    if (sampleCount == 0) {
        // An empty track is legal: no chunk or timing table is required to describe it.
        result.status = Mp4SampleTableIndexStatus::Built;
        result.index =
            Mp4SampleTableIndex(std::move(tables), std::move(readers), 0);
        return result;
    }

    if (tables.sampleToChunk.empty()) {
        result.status = Mp4SampleTableIndexStatus::InconsistentTables;
        result.errorMessage = QStringLiteral("stsc is required to locate samples");
        return result;
    }

    if (tables.chunkCount == 0) {
        result.status = Mp4SampleTableIndexStatus::InconsistentTables;
        result.errorMessage = QStringLiteral("stco/co64 is required to locate samples");
        return result;
    }

    // stsc entries are ordered by strictly increasing 1-based first_chunk with positive
    // samples_per_chunk; anything else makes chunk assignment ambiguous.
    quint64 previousFirstChunk = 0;
    for (const auto& entry : tables.sampleToChunk) {
        if (entry.firstChunk == 0 || entry.firstChunk <= previousFirstChunk) {
            result.status = Mp4SampleTableIndexStatus::InconsistentTables;
            result.errorMessage =
                QStringLiteral("stsc first_chunk values must strictly increase from 1");
            return result;
        }
        if (entry.samplesPerChunk == 0) {
            result.status = Mp4SampleTableIndexStatus::InconsistentTables;
            result.errorMessage = QStringLiteral("stsc samples_per_chunk must be positive");
            return result;
        }
        if (entry.sampleDescriptionIndex == 0) {
            result.status = Mp4SampleTableIndexStatus::InconsistentTables;
            result.errorMessage = QStringLiteral("stsc sample_description_index must be 1-based");
            return result;
        }
        if (!tables.sampleDescriptions.empty()
            && entry.sampleDescriptionIndex > tables.sampleDescriptions.size()) {
            result.status = Mp4SampleTableIndexStatus::InconsistentTables;
            result.errorMessage =
                QStringLiteral("stsc sample_description_index has no matching stsd entry");
            return result;
        }
        if (entry.firstChunk > tables.chunkCount) {
            result.status = Mp4SampleTableIndexStatus::InconsistentTables;
            result.errorMessage =
                QStringLiteral("stsc references a chunk beyond the stco/co64 table");
            return result;
        }
        previousFirstChunk = entry.firstChunk;
    }

    if (tables.sampleToChunk.front().firstChunk != 1) {
        result.status = Mp4SampleTableIndexStatus::InconsistentTables;
        result.errorMessage = QStringLiteral("stsc must describe chunk 1");
        return result;
    }

    // The chunks described by stsc must cover exactly the samples stsz declares.
    quint64 coveredSamples = 0;
    for (std::size_t runIndex = 0; runIndex < tables.sampleToChunk.size(); ++runIndex) {
        const auto& entry = tables.sampleToChunk[runIndex];
        const quint64 nextFirstChunk = runIndex + 1 < tables.sampleToChunk.size()
                                           ? tables.sampleToChunk[runIndex + 1].firstChunk
                                           : tables.chunkCount + 1;
        const quint64 runChunks = nextFirstChunk - entry.firstChunk;
        if (multiplyWouldOverflow(runChunks, entry.samplesPerChunk)) {
            result.status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result.errorMessage = QStringLiteral("stsc run sample count overflows");
            return result;
        }
        const quint64 runSamples = runChunks * entry.samplesPerChunk;
        if (addWouldOverflow(coveredSamples, runSamples)) {
            result.status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result.errorMessage = QStringLiteral("stsc total sample count overflows");
            return result;
        }
        coveredSamples += runSamples;
    }

    if (coveredSamples != sampleCount) {
        result.status = Mp4SampleTableIndexStatus::InconsistentTables;
        result.errorMessage = QStringLiteral(
            "stsc chunk layout does not account for exactly the declared sample count");
        return result;
    }

    // stts must cover every sample; a shorter table leaves samples without a DTS.
    quint64 sttsCoveredSamples = 0;
    for (const auto& entry : tables.timeToSample) {
        if (addWouldOverflow(sttsCoveredSamples, entry.sampleCount)) {
            result.status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result.errorMessage = QStringLiteral("stts total sample count overflows");
            return result;
        }
        sttsCoveredSamples += entry.sampleCount;
    }
    if (sttsCoveredSamples != sampleCount) {
        result.status = Mp4SampleTableIndexStatus::InconsistentTables;
        result.errorMessage =
            QStringLiteral("stts does not describe exactly the declared sample count");
        return result;
    }

    // ctts, when present, must also cover every sample.
    if (!tables.compositionOffsets.empty()) {
        quint64 cttsCoveredSamples = 0;
        for (const auto& entry : tables.compositionOffsets) {
            if (addWouldOverflow(cttsCoveredSamples, entry.sampleCount)) {
                result.status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
                result.errorMessage = QStringLiteral("ctts total sample count overflows");
                return result;
            }
            cttsCoveredSamples += entry.sampleCount;
        }
        if (cttsCoveredSamples != sampleCount) {
            result.status = Mp4SampleTableIndexStatus::InconsistentTables;
            result.errorMessage =
                QStringLiteral("ctts does not describe exactly the declared sample count");
            return result;
        }
    }

    result.status = Mp4SampleTableIndexStatus::Built;
    result.index = Mp4SampleTableIndex(std::move(tables), std::move(readers), sampleCount);
    return result;
}

Mp4SampleTableIndex::Mp4SampleTableIndex(Mp4TrackSampleTables tables,
                                         Mp4SampleTableReaders readers,
                                         quint64 sampleCount)
    : tables_(std::move(tables)), readers_(std::move(readers)), sampleCount_(sampleCount) {}

core::PagedCachePageKey Mp4SampleTableIndex::cachePageKey(quint64 pageIndex) const noexcept {
    core::PagedCachePageKey key;
    key.kind = core::PagedCachePageKind::ProgressiveIndex;
    // Track id and index kind must both participate so independent tracks never collide.
    key.streamId =
        (static_cast<quint64>(tables_.trackId) << 8U) | static_cast<quint64>(kSampleIndexKind);
    key.pageIndex = pageIndex;
    return key;
}

const Mp4SampleDescriptionBinding*
Mp4SampleTableIndex::sampleDescription(quint32 sampleDescriptionIndex) const noexcept {
    if (sampleDescriptionIndex == 0
        || sampleDescriptionIndex > tables_.sampleDescriptions.size()) {
        return nullptr;
    }
    return &tables_.sampleDescriptions[static_cast<std::size_t>(sampleDescriptionIndex - 1U)];
}

quint64 Mp4SampleTableIndex::samplesInRun(std::size_t runIndex) const noexcept {
    const auto& entry = tables_.sampleToChunk[runIndex];
    const quint64 nextFirstChunk = runIndex + 1 < tables_.sampleToChunk.size()
                                       ? tables_.sampleToChunk[runIndex + 1].firstChunk
                                       : tables_.chunkCount + 1;
    // build() proved this product does not overflow.
    return (nextFirstChunk - entry.firstChunk) * entry.samplesPerChunk;
}

std::optional<quint64>
Mp4SampleTableIndex::readSampleSize(quint64 sampleIndex,
                                    Mp4SamplePageResult* result,
                                    const Mp4SamplePageRequest& request) const {
    if (tables_.defaultSampleSize != 0) {
        return tables_.defaultSampleSize;
    }
    if (result->tableReadCount >= request.maximumTableReadsPerPage) {
        result->status = Mp4SampleTableIndexStatus::ResourceLimit;
        result->errorMessage = QStringLiteral("Page exceeded its sample table read budget");
        return std::nullopt;
    }
    ++result->tableReadCount;
    const auto size = readers_.sampleSize(sampleIndex);
    if (!size.has_value()) {
        result->status = Mp4SampleTableIndexStatus::SourceError;
        result->errorMessage = QStringLiteral("Sample size table could not supply an entry");
        return std::nullopt;
    }
    return size;
}

std::optional<quint64>
Mp4SampleTableIndex::readChunkOffset(quint64 chunkIndex,
                                     Mp4SamplePageResult* result,
                                     const Mp4SamplePageRequest& request) const {
    if (chunkIndex >= tables_.chunkCount) {
        result->status = Mp4SampleTableIndexStatus::InconsistentTables;
        result->errorMessage = QStringLiteral("Sample maps to a chunk beyond the stco/co64 table");
        return std::nullopt;
    }
    if (result->tableReadCount >= request.maximumTableReadsPerPage) {
        result->status = Mp4SampleTableIndexStatus::ResourceLimit;
        result->errorMessage = QStringLiteral("Page exceeded its sample table read budget");
        return std::nullopt;
    }
    ++result->tableReadCount;
    const auto offset = readers_.chunkOffset(chunkIndex);
    if (!offset.has_value()) {
        result->status = Mp4SampleTableIndexStatus::SourceError;
        result->errorMessage = QStringLiteral("Chunk offset table could not supply an entry");
        return std::nullopt;
    }
    return offset;
}

bool Mp4SampleTableIndex::seekCursor(quint64 sampleIndex,
                                     ChunkCursor* cursor,
                                     Mp4SamplePageResult* result,
                                     const Mp4SamplePageRequest& request) const {
    quint64 consumedSamples = 0;
    bool located = false;
    for (std::size_t runIndex = 0; runIndex < tables_.sampleToChunk.size(); ++runIndex) {
        const quint64 runSamples = samplesInRun(runIndex);
        if (sampleIndex < consumedSamples + runSamples) {
            const auto& entry = tables_.sampleToChunk[runIndex];
            const quint64 offsetInRun = sampleIndex - consumedSamples;
            cursor->runIndex = runIndex;
            cursor->runFirstSampleIndex = consumedSamples;
            cursor->chunkIndex = entry.firstChunk - 1U + offsetInRun / entry.samplesPerChunk;
            cursor->ordinalInChunk = offsetInRun % entry.samplesPerChunk;
            cursor->sampleDescriptionIndex = entry.sampleDescriptionIndex;
            located = true;
            break;
        }
        consumedSamples += runSamples;
    }

    if (!located) {
        result->status = Mp4SampleTableIndexStatus::InconsistentTables;
        result->errorMessage = QStringLiteral("stsc does not cover the requested sample");
        return false;
    }

    const auto chunkStart = readChunkOffset(cursor->chunkIndex, result, request);
    if (!chunkStart.has_value()) {
        return false;
    }
    cursor->chunkStartOffset = *chunkStart;

    // Catch up on the samples preceding this one inside its chunk. With a uniform
    // stsz this is one multiply; otherwise it costs one read per preceding sample,
    // which is why the page carries an explicit table read budget.
    cursor->offsetInChunk = 0;
    if (tables_.defaultSampleSize != 0) {
        if (multiplyWouldOverflow(cursor->ordinalInChunk, tables_.defaultSampleSize)) {
            result->status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result->errorMessage = QStringLiteral("Chunk-relative sample offset overflows");
            return false;
        }
        cursor->offsetInChunk = cursor->ordinalInChunk * tables_.defaultSampleSize;
        return true;
    }

    const quint64 chunkFirstSampleIndex = sampleIndex - cursor->ordinalInChunk;
    for (quint64 preceding = 0; preceding < cursor->ordinalInChunk; ++preceding) {
        const auto size = readSampleSize(chunkFirstSampleIndex + preceding, result, request);
        if (!size.has_value()) {
            return false;
        }
        if (addWouldOverflow(cursor->offsetInChunk, *size)) {
            result->status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result->errorMessage = QStringLiteral("Chunk-relative sample offset overflows");
            return false;
        }
        cursor->offsetInChunk += *size;
    }
    return true;
}

bool Mp4SampleTableIndex::advanceCursor(ChunkCursor* cursor,
                                        quint64 consumedSampleSize,
                                        Mp4SamplePageResult* result,
                                        const Mp4SamplePageRequest& request) const {
    const auto& entry = tables_.sampleToChunk[cursor->runIndex];

    // Still inside the current chunk: the next sample simply follows this one.
    if (cursor->ordinalInChunk + 1U < entry.samplesPerChunk) {
        ++cursor->ordinalInChunk;
        if (addWouldOverflow(cursor->offsetInChunk, consumedSampleSize)) {
            result->status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result->errorMessage = QStringLiteral("Chunk-relative sample offset overflows");
            return false;
        }
        cursor->offsetInChunk += consumedSampleSize;
        return true;
    }

    // Crossing a chunk boundary. Chunk-relative state restarts.
    ++cursor->chunkIndex;
    cursor->ordinalInChunk = 0;
    cursor->offsetInChunk = 0;

    // stsc runs partition the chunk range contiguously, so leaving this run's last
    // chunk means entering the next run's first chunk.
    const quint64 runChunkLimit = cursor->runIndex + 1 < tables_.sampleToChunk.size()
                                      ? tables_.sampleToChunk[cursor->runIndex + 1].firstChunk - 1U
                                      : tables_.chunkCount;
    if (cursor->chunkIndex >= runChunkLimit) {
        if (cursor->runIndex + 1 >= tables_.sampleToChunk.size()) {
            // build() proved stsc covers exactly sampleCount_ samples, and the caller
            // never advances past the last sample, so this is unreachable for
            // validated tables.
            result->status = Mp4SampleTableIndexStatus::InconsistentTables;
            result->errorMessage = QStringLiteral("stsc runs out of chunks before samples");
            return false;
        }
        cursor->runFirstSampleIndex += samplesInRun(cursor->runIndex);
        ++cursor->runIndex;
        const auto& nextEntry = tables_.sampleToChunk[cursor->runIndex];
        cursor->chunkIndex = nextEntry.firstChunk - 1U;
        cursor->sampleDescriptionIndex = nextEntry.sampleDescriptionIndex;
    }

    const auto chunkStart = readChunkOffset(cursor->chunkIndex, result, request);
    if (!chunkStart.has_value()) {
        return false;
    }
    cursor->chunkStartOffset = *chunkStart;
    return true;
}

bool Mp4SampleTableIndex::seekTimeline(quint64 sampleIndex,
                                      TimelineCursor* cursor,
                                      Mp4SampleTableIndexStatus* failure,
                                      QString* errorMessage) const noexcept {
    // One pass over the stts runs to reach the page's first sample. Later samples in
    // the page move through advanceTimeline() in O(1), so a page never costs
    // run count times page size.
    quint64 consumedSamples = 0;
    qint64 accumulatedDts = 0;

    for (std::size_t runIndex = 0; runIndex < tables_.timeToSample.size(); ++runIndex) {
        const auto& entry = tables_.timeToSample[runIndex];
        if (sampleIndex < consumedSamples + entry.sampleCount) {
            const quint64 offsetInRun = sampleIndex - consumedSamples;
            if (multiplyWouldOverflow(offsetInRun, entry.sampleDelta)) {
                *failure = Mp4SampleTableIndexStatus::ArithmeticOverflow;
                *errorMessage = QStringLiteral("stts decode time offset overflows");
                return false;
            }
            const quint64 withinRun = offsetInRun * entry.sampleDelta;
            if (!unsignedFitsSigned(withinRun)
                || signedAddWouldOverflow(accumulatedDts, static_cast<qint64>(withinRun))) {
                *failure = Mp4SampleTableIndexStatus::ArithmeticOverflow;
                *errorMessage = QStringLiteral("stts decode timeline overflows");
                return false;
            }
            cursor->runIndex = runIndex;
            cursor->ordinalInRun = offsetInRun;
            cursor->dts = accumulatedDts + static_cast<qint64>(withinRun);
            return true;
        }

        if (multiplyWouldOverflow(entry.sampleCount, entry.sampleDelta)) {
            *failure = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            *errorMessage = QStringLiteral("stts run duration overflows");
            return false;
        }
        const quint64 runDuration = entry.sampleCount * entry.sampleDelta;
        if (!unsignedFitsSigned(runDuration)
            || signedAddWouldOverflow(accumulatedDts, static_cast<qint64>(runDuration))) {
            *failure = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            *errorMessage = QStringLiteral("stts decode timeline overflows");
            return false;
        }
        accumulatedDts += static_cast<qint64>(runDuration);
        consumedSamples += entry.sampleCount;
    }

    *failure = Mp4SampleTableIndexStatus::InconsistentTables;
    *errorMessage = QStringLiteral("stts does not cover the requested sample");
    return false;
}

bool Mp4SampleTableIndex::advanceTimeline(TimelineCursor* cursor,
                                          Mp4SampleTableIndexStatus* failure,
                                          QString* errorMessage) const noexcept {
    const quint64 delta = tables_.timeToSample[cursor->runIndex].sampleDelta;
    if (!unsignedFitsSigned(delta)
        || signedAddWouldOverflow(cursor->dts, static_cast<qint64>(delta))) {
        *failure = Mp4SampleTableIndexStatus::ArithmeticOverflow;
        *errorMessage = QStringLiteral("stts decode timeline overflows");
        return false;
    }
    cursor->dts += static_cast<qint64>(delta);

    ++cursor->ordinalInRun;
    if (cursor->ordinalInRun < tables_.timeToSample[cursor->runIndex].sampleCount) {
        return true;
    }

    // Skip any zero-length runs so the cursor always rests on a real sample.
    cursor->ordinalInRun = 0;
    ++cursor->runIndex;
    while (cursor->runIndex < tables_.timeToSample.size()
           && tables_.timeToSample[cursor->runIndex].sampleCount == 0) {
        ++cursor->runIndex;
    }
    if (cursor->runIndex >= tables_.timeToSample.size()) {
        // build() proved stts covers exactly sampleCount_ samples and the caller never
        // advances past the last sample, so this is unreachable for validated tables.
        *failure = Mp4SampleTableIndexStatus::InconsistentTables;
        *errorMessage = QStringLiteral("stts runs out of entries before samples");
        return false;
    }
    return true;
}

quint64 Mp4SampleTableIndex::durationAt(const TimelineCursor& cursor) const noexcept {
    return tables_.timeToSample[cursor.runIndex].sampleDelta;
}

bool Mp4SampleTableIndex::seekComposition(quint64 sampleIndex,
                                          CompositionCursor* cursor) const noexcept {
    // An absent ctts means pts == dts (ADR-0105 section 3.2).
    if (tables_.compositionOffsets.empty()) {
        return true;
    }

    quint64 consumedSamples = 0;
    for (std::size_t runIndex = 0; runIndex < tables_.compositionOffsets.size(); ++runIndex) {
        const quint64 runSamples = tables_.compositionOffsets[runIndex].sampleCount;
        if (sampleIndex < consumedSamples + runSamples) {
            cursor->runIndex = runIndex;
            cursor->ordinalInRun = sampleIndex - consumedSamples;
            return true;
        }
        consumedSamples += runSamples;
    }
    return false;
}

bool Mp4SampleTableIndex::advanceComposition(CompositionCursor* cursor) const noexcept {
    if (tables_.compositionOffsets.empty()) {
        return true;
    }

    ++cursor->ordinalInRun;
    if (cursor->ordinalInRun < tables_.compositionOffsets[cursor->runIndex].sampleCount) {
        return true;
    }

    cursor->ordinalInRun = 0;
    ++cursor->runIndex;
    while (cursor->runIndex < tables_.compositionOffsets.size()
           && tables_.compositionOffsets[cursor->runIndex].sampleCount == 0) {
        ++cursor->runIndex;
    }
    // build() proved ctts covers exactly sampleCount_ samples, so running out here
    // is unreachable for validated tables.
    return cursor->runIndex < tables_.compositionOffsets.size();
}

qint64 Mp4SampleTableIndex::compositionOffsetAt(const CompositionCursor& cursor) const noexcept {
    if (tables_.compositionOffsets.empty()) {
        return 0;
    }
    return reinterpretCompositionOffset(tables_.compositionOffsets[cursor.runIndex].rawSampleOffset,
                                        tables_.compositionOffsetVersion);
}

Mp4SamplePageResult
Mp4SampleTableIndex::descriptorPage(const Mp4SamplePageRequest& request) const {
    Mp4SamplePageResult result;
    result.cacheKey = cachePageKey(request.pageIndex);

    if (request.pageSize == 0) {
        result.status = Mp4SampleTableIndexStatus::InvalidRequest;
        result.errorMessage = QStringLiteral("Page size must be greater than zero");
        return result;
    }

    if (request.cancellation && request.cancellation->isCancellationRequested()) {
        result.status = Mp4SampleTableIndexStatus::Cancelled;
        return result;
    }

    if (multiplyWouldOverflow(request.pageIndex, request.pageSize)) {
        result.status = Mp4SampleTableIndexStatus::InvalidRequest;
        result.errorMessage = QStringLiteral("Page start index overflows");
        return result;
    }
    const quint64 startIndex = request.pageIndex * request.pageSize;
    if (startIndex >= sampleCount_) {
        result.status = Mp4SampleTableIndexStatus::InvalidRequest;
        result.errorMessage = QStringLiteral("Page start index is beyond the sample count");
        return result;
    }

    const quint64 pageCount = std::min(request.pageSize, sampleCount_ - startIndex);
    if (pageCount > request.maximumDescriptorsPerPage) {
        result.status = Mp4SampleTableIndexStatus::ResourceLimit;
        result.errorMessage = QStringLiteral("Requested page exceeds the descriptor budget");
        return result;
    }

    result.firstSampleIndex = startIndex;
    // Descriptors are built fresh per request and never accumulated on the index, so a
    // repeated page allocates exactly one page worth of memory rather than growing.
    result.descriptors.reserve(static_cast<std::size_t>(pageCount));

    // The overlap guard is seeded from the sample immediately before the page, so a chunk
    // boundary that coincides with a page boundary is still checked. Seeding it any other
    // way would make rejection of a corrupt table depend on the caller's page size.
    // Positioning on the predecessor and stepping forward costs one extra size read over
    // seeking straight to the page, and stays inside the page's read budget.
    ChunkCursor cursor;
    quint64 previousStart = 0;
    quint64 previousSize = 0;
    bool hasPrevious = false;

    if (startIndex == 0) {
        if (!seekCursor(0, &cursor, &result, request)) {
            result.descriptors.clear();
            return result;
        }
    } else {
        if (!seekCursor(startIndex - 1U, &cursor, &result, request)) {
            result.descriptors.clear();
            return result;
        }
        const auto precedingSize = readSampleSize(startIndex - 1U, &result, request);
        if (!precedingSize.has_value()) {
            result.descriptors.clear();
            return result;
        }
        if (addWouldOverflow(cursor.chunkStartOffset, cursor.offsetInChunk)) {
            result.status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result.errorMessage = QStringLiteral("Sample byte offset overflows");
            result.descriptors.clear();
            return result;
        }
        previousStart = cursor.chunkStartOffset + cursor.offsetInChunk;
        previousSize = *precedingSize;
        hasPrevious = true;

        if (!advanceCursor(&cursor, *precedingSize, &result, request)) {
            result.descriptors.clear();
            return result;
        }
    }

    // Both timing tables are seeked once for the page and then advanced in O(1) per
    // sample, so a page never costs run count times page size.
    TimelineCursor timeline;
    {
        auto failure = Mp4SampleTableIndexStatus::InconsistentTables;
        QString errorMessage;
        if (!seekTimeline(startIndex, &timeline, &failure, &errorMessage)) {
            result.status = failure;
            result.errorMessage = std::move(errorMessage);
            result.descriptors.clear();
            return result;
        }
    }

    CompositionCursor composition;
    if (!seekComposition(startIndex, &composition)) {
        result.status = Mp4SampleTableIndexStatus::InconsistentTables;
        result.errorMessage = QStringLiteral("ctts does not cover the requested sample");
        result.descriptors.clear();
        return result;
    }

    for (quint64 offset = 0; offset < pageCount; ++offset) {
        if (request.cancellation && request.cancellation->isCancellationRequested()) {
            result.status = Mp4SampleTableIndexStatus::Cancelled;
            result.descriptors.clear();
            return result;
        }

        const quint64 sampleIndex = startIndex + offset;

        const auto sampleSize = readSampleSize(sampleIndex, &result, request);
        if (!sampleSize.has_value()) {
            result.descriptors.clear();
            return result;
        }

        if (addWouldOverflow(cursor.chunkStartOffset, cursor.offsetInChunk)) {
            result.status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result.errorMessage = QStringLiteral("Sample byte offset overflows");
            result.descriptors.clear();
            return result;
        }
        const quint64 sampleOffset = cursor.chunkStartOffset + cursor.offsetInChunk;

        if (addWouldOverflow(sampleOffset, *sampleSize)) {
            result.status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result.errorMessage = QStringLiteral("Sample byte extent overflows");
            result.descriptors.clear();
            return result;
        }

        if (tables_.sourceSizeBytes != 0
            && sampleOffset + *sampleSize > tables_.sourceSizeBytes) {
            result.status = Mp4SampleTableIndexStatus::OutOfSourceRange;
            result.errorMessage = QStringLiteral("Sample extends beyond the media source");
            result.descriptors.clear();
            return result;
        }

        // Adjacent samples must not claim intersecting bytes. Within a chunk the layout
        // is sequential by construction, so this catches a chunk offset that lands
        // inside the previous chunk's extent without assuming chunk offsets ascend.
        if (hasPrevious && rangesIntersect(previousStart, previousSize, sampleOffset, *sampleSize)) {
            result.status = Mp4SampleTableIndexStatus::OutOfSourceRange;
            result.errorMessage = QStringLiteral("Sample byte ranges overlap");
            result.descriptors.clear();
            return result;
        }

        // SourceSpan is bit-addressed (ADR-0105 section 2 item 3), so byte coordinates
        // are converted here.
        if (sampleOffset > kMaxByteCoordinate || *sampleSize > kMaxByteCoordinate) {
            result.status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result.errorMessage = QStringLiteral("Sample bit coordinate overflows");
            result.descriptors.clear();
            return result;
        }

        const qint64 dts = timeline.dts;
        const quint64 duration = durationAt(timeline);
        const qint64 compositionOffset = compositionOffsetAt(composition);

        if (signedAddWouldOverflow(dts, compositionOffset)) {
            result.status = Mp4SampleTableIndexStatus::ArithmeticOverflow;
            result.errorMessage = QStringLiteral("Presentation timeline overflows");
            result.descriptors.clear();
            return result;
        }
        // A negative pts is legal and must not be clamped (ADR-0105 section 3.2).
        const qint64 pts = dts + compositionOffset;

        bool isSync = true;
        if (tables_.hasSyncSampleTable) {
            if (result.tableReadCount >= request.maximumTableReadsPerPage) {
                result.status = Mp4SampleTableIndexStatus::ResourceLimit;
                result.errorMessage = QStringLiteral("Page exceeded its sample table read budget");
                result.descriptors.clear();
                return result;
            }
            ++result.tableReadCount;
            const auto sync = readers_.isSyncSample(sampleIndex + 1U);
            if (!sync.has_value()) {
                result.status = Mp4SampleTableIndexStatus::SourceError;
                result.errorMessage = QStringLiteral("Sync sample table could not supply an entry");
                result.descriptors.clear();
                return result;
            }
            isSync = *sync;
        }

        const auto span = core::SourceSpan::create(core::SourceBitAddress(sampleOffset * 8U),
                                                   *sampleSize * 8U);
        if (!span.has_value()) {
            result.status = Mp4SampleTableIndexStatus::InvalidRequest;
            result.errorMessage = QStringLiteral("Sample source span is not representable");
            result.descriptors.clear();
            return result;
        }

        core::SampleDescriptor descriptor;
        descriptor.trackId = tables_.trackId;
        descriptor.sampleIndex = sampleIndex;
        descriptor.sampleDescriptionIndex = cursor.sampleDescriptionIndex;
        descriptor.sourceSpans.push_back(*span);
        descriptor.dts = dts;
        descriptor.pts = pts;
        descriptor.duration = duration;
        descriptor.timescale = tables_.timescale;
        descriptor.isSyncSample = isSync;
        result.descriptors.push_back(std::move(descriptor));

        previousStart = sampleOffset;
        previousSize = *sampleSize;
        hasPrevious = true;

        if (offset + 1U >= pageCount) {
            break;
        }

        if (!advanceCursor(&cursor, *sampleSize, &result, request)) {
            result.descriptors.clear();
            return result;
        }

        {
            auto failure = Mp4SampleTableIndexStatus::InconsistentTables;
            QString errorMessage;
            if (!advanceTimeline(&timeline, &failure, &errorMessage)) {
                result.status = failure;
                result.errorMessage = std::move(errorMessage);
                result.descriptors.clear();
                return result;
            }
        }

        if (!advanceComposition(&composition)) {
            result.status = Mp4SampleTableIndexStatus::InconsistentTables;
            result.errorMessage = QStringLiteral("ctts does not cover the requested sample");
            result.descriptors.clear();
            return result;
        }
    }

    result.status = Mp4SampleTableIndexStatus::Built;
    return result;
}

} // namespace streamview::rules
