#include <streamview/rules/mp4_sample_table_extractor.h>

#include <streamview/core/analysis_model.h>
#include <streamview/rules/window_decoder.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

namespace streamview::rules {

namespace {

// ISOBMFF box types. These constants are precisely why this component lives in
// the rules layer: a sample table is identifiable only by box identity, so the
// bridge cannot sit in `src/core/` or `src/app/` without hardcoding format
// knowledge there (ADR-0105 section 2, slice P5j-3b).
constexpr quint64 kBoxTypeMoov = 0x6D6F6F76;
constexpr quint64 kBoxTypeTrak = 0x7472616B;
constexpr quint64 kBoxTypeTkhd = 0x746B6864;
constexpr quint64 kBoxTypeMdia = 0x6D646961;
constexpr quint64 kBoxTypeMdhd = 0x6D646864;
constexpr quint64 kBoxTypeMinf = 0x6D696E66;
constexpr quint64 kBoxTypeStbl = 0x7374626C;
constexpr quint64 kBoxTypeStsd = 0x73747364;
constexpr quint64 kBoxTypeStts = 0x73747473;
constexpr quint64 kBoxTypeStsc = 0x73747363;
constexpr quint64 kBoxTypeStsz = 0x7374737A;
constexpr quint64 kBoxTypeStz2 = 0x73747A32;
constexpr quint64 kBoxTypeStco = 0x7374636F;
constexpr quint64 kBoxTypeCo64 = 0x636F3634;
constexpr quint64 kBoxTypeStss = 0x73747373;
constexpr quint64 kBoxTypeCtts = 0x63747473;

/// FourCC of the box a `Box` structure node decoded. The rule emits `size` then
/// `type`, which the name check below asserts rather than assumes.
[[nodiscard]] std::optional<quint64> boxTypeOf(const core::AnalysisTree& tree,
                                               core::AnalysisNodeId boxStructId) {
    const auto boxStruct = tree.node(boxStructId);
    if (!boxStruct || boxStruct->children().size() < 2) {
        return std::nullopt;
    }
    const auto typeNode = tree.node(boxStruct->children()[1]);
    if (!typeNode || typeNode->name() != QStringLiteral("type")) {
        return std::nullopt;
    }
    return typeNode->value().toULongLong();
}

/// Lazy container child of a box structure, covering the `largesize` and
/// extends-to-EOF framings as well as the ordinary one.
[[nodiscard]] std::optional<core::AnalysisNodeId> containerPayloadOf(
    const core::AnalysisTree& tree,
    core::AnalysisNodeId boxStructId) {
    const auto boxStruct = tree.node(boxStructId);
    if (!boxStruct) {
        return std::nullopt;
    }
    std::optional<core::AnalysisNodeId> found;
    for (const auto childId : boxStruct->children()) {
        const auto child = tree.node(childId);
        if (child && child->metadata().containerChildStructIndex.has_value()) {
            found = childId;
        }
    }
    return found;
}

/// Decoded payload structure of a typed-container box such as `stts`. The
/// analyzer appends exactly one structure under such a container node.
[[nodiscard]] std::optional<core::AnalysisNodeId> payloadStructOf(
    const core::AnalysisTree& tree,
    core::AnalysisNodeId boxStructId) {
    const auto payloadId = containerPayloadOf(tree, boxStructId);
    if (!payloadId.has_value()) {
        return std::nullopt;
    }
    const auto payload = tree.node(*payloadId);
    if (!payload || payload->children().empty()) {
        return std::nullopt;
    }
    return payload->children().front();
}

[[nodiscard]] std::optional<core::AnalysisNodeId> fieldByName(const core::AnalysisTree& tree,
                                                              core::AnalysisNodeId structId,
                                                              const QString& name) {
    const auto structNode = tree.node(structId);
    if (!structNode) {
        return std::nullopt;
    }
    for (const auto childId : structNode->children()) {
        const auto child = tree.node(childId);
        if (child && child->name() == name) {
            return childId;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<quint64> scalarFieldValue(const core::AnalysisTree& tree,
                                                      core::AnalysisNodeId structId,
                                                      const QString& name) {
    const auto fieldId = fieldByName(tree, structId, name);
    if (!fieldId.has_value()) {
        return std::nullopt;
    }
    const auto field = tree.node(*fieldId);
    if (!field) {
        return std::nullopt;
    }
    bool ok = false;
    const auto value = field->value().toULongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return value;
}

/// The paged entry table of a sample table payload structure, if it has one. A
/// uniform `stsz` and an unsupported `stz2` field width both yield none.
[[nodiscard]] std::optional<core::AnalysisNodeId> windowChildOf(const core::AnalysisTree& tree,
                                                                core::AnalysisNodeId structId) {
    const auto structNode = tree.node(structId);
    if (!structNode) {
        return std::nullopt;
    }
    for (const auto childId : structNode->children()) {
        const auto child = tree.node(childId);
        if (child && child->metadata().window.has_value()) {
            return childId;
        }
    }
    return std::nullopt;
}

/// Entries the window region can actually supply. Mirrors the clamp
/// `WindowDecoder::decodeWindow` applies, so a declared count that overruns the
/// box is never mistaken for available data.
[[nodiscard]] std::optional<quint64> availableWindowEntryCount(const core::AnalysisTree& tree,
                                                               core::AnalysisNodeId windowNodeId) {
    const auto node = tree.node(windowNodeId);
    if (!node || !node->metadata().window.has_value() || !node->location().has_value()) {
        return std::nullopt;
    }
    const auto& window = *node->metadata().window;
    if (window.entrySizeBits == 0 || (window.entrySizeBits % 8U) != 0) {
        return std::nullopt;
    }
    const quint64 regionBytes = node->location()->logicalRange().bitLength() / 8U;
    const quint64 entryBytes = window.entrySizeBits / 8U;
    return std::min(window.entryCount, regionBytes / entryBytes);
}

/// Decodes exactly one window entry. Entries already decoded come back from the
/// analyzer's memoized per-node window state, so repeated single-entry reads cost
/// a map lookup rather than a source read.
[[nodiscard]] std::optional<core::AnalysisNodeId> decodeWindowEntry(
    const Mp4IsobmffAnalyzer& analyzer,
    core::AnalysisNodeId windowNodeId,
    quint64 entryIndex) {
    auto decoder = analyzer.windowDecoder(windowNodeId);
    if (!decoder.has_value()) {
        return std::nullopt;
    }
    const auto result = decoder->decodeWindow({entryIndex, 1});
    // A final page that stops short of a declared count reports TruncatedSource
    // while still returning the entries it did decode.
    if (result.status != DslExecutionStatus::Materialized &&
        result.status != DslExecutionStatus::TruncatedSource) {
        return std::nullopt;
    }
    if (result.entryNodes.size() != 1) {
        return std::nullopt;
    }
    return result.entryNodes.front();
}

[[nodiscard]] std::optional<quint64> entryFieldValue(const core::AnalysisTree& tree,
                                                     core::AnalysisNodeId entryNodeId,
                                                     std::size_t fieldOrdinal) {
    const auto entry = tree.node(entryNodeId);
    if (!entry || entry->children().size() <= fieldOrdinal) {
        return std::nullopt;
    }
    const auto field = tree.node(entry->children()[fieldOrdinal]);
    if (!field) {
        return std::nullopt;
    }
    bool ok = false;
    const auto value = field->value().toULongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return value;
}

enum class RowCollectStatus : quint8 {
    Collected,
    SourceError,
    ResourceLimit,
    Cancelled,
};

/// Reads a run-length table page by page, handing each entry node to `sink`.
///
/// Run-length rows are bounded by run count rather than sample count, so these
/// are the only tables the extraction materializes. Per-sample and per-chunk
/// tables stay behind readers.
template <typename RowSink>
[[nodiscard]] RowCollectStatus collectWindowRows(const Mp4IsobmffAnalyzer& analyzer,
                                                 core::AnalysisNodeId windowNodeId,
                                                 quint64 declaredCount,
                                                 const Mp4SampleTableExtractionRequest& request,
                                                 RowSink&& sink) {
    if (declaredCount > request.maximumRunRowsPerTable) {
        return RowCollectStatus::ResourceLimit;
    }
    const quint64 pageSize = request.runRowPageSize == 0 ? 1U : request.runRowPageSize;

    quint64 collected = 0;
    quint64 pageIndex = 0;
    while (collected < declaredCount) {
        if (request.cancellation && request.cancellation->isCancellationRequested()) {
            return RowCollectStatus::Cancelled;
        }
        auto decoder = analyzer.windowDecoder(windowNodeId);
        if (!decoder.has_value()) {
            return RowCollectStatus::SourceError;
        }
        const auto page = decoder->decodeWindow({pageIndex, pageSize});
        if (page.status == DslExecutionStatus::Cancelled) {
            return RowCollectStatus::Cancelled;
        }
        if (page.status == DslExecutionStatus::ResourceLimit) {
            return RowCollectStatus::ResourceLimit;
        }
        if (page.status != DslExecutionStatus::Materialized &&
            page.status != DslExecutionStatus::TruncatedSource) {
            return RowCollectStatus::SourceError;
        }
        // A table that declares more rows than its box carries runs out of pages
        // before `declaredCount` is reached; that is a source defect, not an
        // empty table.
        if (page.entryNodes.empty()) {
            return RowCollectStatus::SourceError;
        }
        for (const auto entryNodeId : page.entryNodes) {
            if (collected >= declaredCount) {
                break;
            }
            if (!sink(entryNodeId)) {
                return RowCollectStatus::SourceError;
            }
            ++collected;
        }
        ++pageIndex;
    }
    return RowCollectStatus::Collected;
}

[[nodiscard]] Mp4SampleTableExtractionStatus toExtractionStatus(RowCollectStatus status) noexcept {
    switch (status) {
        case RowCollectStatus::Collected:
            return Mp4SampleTableExtractionStatus::Extracted;
        case RowCollectStatus::ResourceLimit:
            return Mp4SampleTableExtractionStatus::ResourceLimit;
        case RowCollectStatus::Cancelled:
            return Mp4SampleTableExtractionStatus::Cancelled;
        case RowCollectStatus::SourceError:
            break;
    }
    return Mp4SampleTableExtractionStatus::SourceError;
}

/// Reads the first field of the entry at `index`. Serves `stsz` entry sizes,
/// `stco`/`co64` chunk offsets, and the 8- and 16-bit `stz2` widths, all of which
/// carry exactly one value per entry.
[[nodiscard]] std::function<std::optional<quint64>(quint64)> makeEntryValueReader(
    const Mp4IsobmffAnalyzer& analyzer,
    core::AnalysisNodeId windowNodeId) {
    const auto* analyzerPtr = &analyzer;
    return [analyzerPtr, windowNodeId](quint64 index) -> std::optional<quint64> {
        const auto entryNodeId = decodeWindowEntry(*analyzerPtr, windowNodeId, index);
        if (!entryNodeId.has_value()) {
            return std::nullopt;
        }
        return entryFieldValue(analyzerPtr->tree(), *entryNodeId, 0);
    };
}

/// Reads a 4-bit `stz2` size. ISO/IEC 14496-12 section 8.7.3.3 packs two sizes
/// per byte, so the window entry is a pair and the sample index selects a nibble.
/// An odd sample count leaves the final low nibble as padding, which no sample
/// index reaches.
[[nodiscard]] std::function<std::optional<quint64>(quint64)> makePackedPairSizeReader(
    const Mp4IsobmffAnalyzer& analyzer,
    core::AnalysisNodeId windowNodeId) {
    const auto* analyzerPtr = &analyzer;
    return [analyzerPtr, windowNodeId](quint64 sampleIndex) -> std::optional<quint64> {
        const auto entryNodeId =
            decodeWindowEntry(*analyzerPtr, windowNodeId, sampleIndex / 2U);
        if (!entryNodeId.has_value()) {
            return std::nullopt;
        }
        return entryFieldValue(analyzerPtr->tree(),
                               *entryNodeId,
                               static_cast<std::size_t>(sampleIndex % 2U));
    };
}

/// Answers whether `stss` lists a 1-based sample number.
///
/// ISO/IEC 14496-12 section 8.6.2 requires the sample numbers to be strictly
/// increasing, so membership is a binary search: one query decodes O(log n)
/// entries and therefore materializes O(log n) nodes, never the whole table. A
/// file that violates that ordering can produce a false negative here; detecting
/// it would require reading every entry, which is exactly the unbounded expansion
/// this seam exists to avoid.
[[nodiscard]] std::function<std::optional<bool>(quint64)> makeSyncSampleReader(
    const Mp4IsobmffAnalyzer& analyzer,
    core::AnalysisNodeId windowNodeId,
    quint64 entryCount) {
    const auto* analyzerPtr = &analyzer;
    return [analyzerPtr, windowNodeId, entryCount](quint64 sampleNumber) -> std::optional<bool> {
        if (entryCount == 0) {
            return false;
        }
        quint64 low = 0;
        quint64 high = entryCount;  // exclusive
        while (low < high) {
            const quint64 mid = low + (high - low) / 2U;
            const auto entryNodeId = decodeWindowEntry(*analyzerPtr, windowNodeId, mid);
            if (!entryNodeId.has_value()) {
                return std::nullopt;
            }
            const auto value = entryFieldValue(analyzerPtr->tree(), *entryNodeId, 0);
            if (!value.has_value()) {
                return std::nullopt;
            }
            if (*value == sampleNumber) {
                return true;
            }
            if (*value < sampleNumber) {
                low = mid + 1U;
            } else {
                high = mid;
            }
        }
        return false;
    };
}

/// First target format declared anywhere under a sample description entry.
///
/// The declaration sits on a configuration payload nested inside the entry
/// (`avcC` SPS/PPS bytes, an `esds` AudioSpecificConfig), so the depth is a rule
/// detail rather than a fixed shape. The search is a bounded pre-order walk and
/// returns an empty string when the entry declares none.
[[nodiscard]] QString findTargetFormat(const core::AnalysisTree& tree,
                                       core::AnalysisNodeId rootId,
                                       quint64 maximumNodesVisited) {
    std::vector<core::AnalysisNodeId> pending{rootId};
    quint64 visited = 0;
    while (!pending.empty()) {
        if (visited >= maximumNodesVisited) {
            return QString();
        }
        ++visited;

        const auto nodeId = pending.back();
        pending.pop_back();
        const auto node = tree.node(nodeId);
        if (!node) {
            continue;
        }
        const auto& targetFormat = node->metadata().targetFormat;
        if (targetFormat.has_value() && !targetFormat->isEmpty()) {
            return *targetFormat;
        }
        const auto& children = node->children();
        for (auto it = children.crbegin(); it != children.crend(); ++it) {
            pending.push_back(*it);
        }
    }
    return QString();
}

/// Child box structures of a container box, or an empty list when the box is not
/// a container or was never drilled.
[[nodiscard]] std::vector<core::AnalysisNodeId> containerChildStructs(
    const core::AnalysisTree& tree,
    core::AnalysisNodeId boxStructId) {
    const auto payloadId = containerPayloadOf(tree, boxStructId);
    if (!payloadId.has_value()) {
        return {};
    }
    const auto payload = tree.node(*payloadId);
    if (!payload) {
        return {};
    }
    return payload->children();
}

enum class TrackOutcome : quint8 {
    Extracted,
    /// Not a track this index describes; the movie may still extract.
    Skipped,
    Failed,
};

/// Collects the `stsd` bindings of a track.
[[nodiscard]] bool extractSampleDescriptions(const core::AnalysisTree& tree,
                                             core::AnalysisNodeId stsdStructId,
                                             const Mp4SampleTableExtractionRequest& request,
                                             std::vector<Mp4SampleDescriptionBinding>* out) {
    const auto version = scalarFieldValue(tree, stsdStructId, QStringLiteral("stsd_version"));
    if (!version.has_value() || *version != 0) {
        return false;
    }
    const auto entriesId = containerPayloadOf(tree, stsdStructId);
    if (!entriesId.has_value()) {
        return false;
    }
    const auto entries = tree.node(*entriesId);
    if (!entries) {
        return false;
    }

    quint32 index = 0;
    for (const auto entryStructId : entries->children()) {
        if (index == std::numeric_limits<quint32>::max()) {
            return false;
        }
        ++index;
        Mp4SampleDescriptionBinding binding;
        binding.sampleDescriptionIndex = index;
        binding.entryNode = entryStructId;
        binding.targetFormat =
            findTargetFormat(tree, entryStructId, request.maximumSampleEntryNodesVisited);
        out->push_back(std::move(binding));
    }
    return true;
}

[[nodiscard]] TrackOutcome extractTrack(const Mp4IsobmffAnalyzer& analyzer,
                                        core::AnalysisNodeId trakStructId,
                                        const Mp4SampleTableExtractionRequest& request,
                                        Mp4ExtractedTrackTables* out,
                                        Mp4SampleTableExtractionStatus* failure,
                                        QString* errorMessage) {
    const auto& tree = analyzer.tree();

    const auto fail = [&](Mp4SampleTableExtractionStatus status, const QString& message) {
        *failure = status;
        *errorMessage = message;
        return TrackOutcome::Failed;
    };

    // trak -> tkhd (track id) and mdia
    std::optional<quint64> trackId;
    std::optional<core::AnalysisNodeId> mdiaStructId;
    for (const auto childId : containerChildStructs(tree, trakStructId)) {
        const auto type = boxTypeOf(tree, childId);
        if (!type.has_value()) {
            continue;
        }
        if (*type == kBoxTypeTkhd && !trackId.has_value()) {
            trackId = scalarFieldValue(tree, childId, QStringLiteral("tkhd_v0_track_id"));
            if (!trackId.has_value()) {
                trackId = scalarFieldValue(tree, childId, QStringLiteral("tkhd_v1_track_id"));
            }
        } else if (*type == kBoxTypeMdia && !mdiaStructId.has_value()) {
            mdiaStructId = childId;
        }
    }
    if (!mdiaStructId.has_value()) {
        return TrackOutcome::Skipped;
    }

    // mdia -> mdhd (timescale) and minf
    std::optional<quint64> timescale;
    std::optional<core::AnalysisNodeId> minfStructId;
    for (const auto childId : containerChildStructs(tree, *mdiaStructId)) {
        const auto type = boxTypeOf(tree, childId);
        if (!type.has_value()) {
            continue;
        }
        if (*type == kBoxTypeMdhd && !timescale.has_value()) {
            timescale = scalarFieldValue(tree, childId, QStringLiteral("mdhd_v0_timescale"));
            if (!timescale.has_value()) {
                timescale = scalarFieldValue(tree, childId, QStringLiteral("mdhd_v1_timescale"));
            }
        } else if (*type == kBoxTypeMinf && !minfStructId.has_value()) {
            minfStructId = childId;
        }
    }
    if (!minfStructId.has_value()) {
        return TrackOutcome::Skipped;
    }

    // minf -> stbl
    std::optional<core::AnalysisNodeId> stblStructId;
    for (const auto childId : containerChildStructs(tree, *minfStructId)) {
        const auto type = boxTypeOf(tree, childId);
        if (type.has_value() && *type == kBoxTypeStbl) {
            stblStructId = childId;
            break;
        }
    }
    if (!stblStructId.has_value()) {
        return TrackOutcome::Skipped;
    }

    Mp4TrackSampleTables tables;
    Mp4SampleTableReaders readers;
    tables.sourceSizeBytes = request.sourceSizeBytes;

    bool haveSizeTable = false;
    bool haveOffsetTable = false;

    for (const auto boxStructId : containerChildStructs(tree, *stblStructId)) {
        if (request.cancellation && request.cancellation->isCancellationRequested()) {
            return fail(Mp4SampleTableExtractionStatus::Cancelled, QString());
        }

        const auto type = boxTypeOf(tree, boxStructId);
        if (!type.has_value()) {
            continue;
        }

        if (*type == kBoxTypeStsd) {
            if (!tables.sampleDescriptions.empty()) {
                continue;
            }
            if (!extractSampleDescriptions(tree, boxStructId, request,
                                           &tables.sampleDescriptions)) {
                return fail(Mp4SampleTableExtractionStatus::UnsupportedTables,
                            QStringLiteral("Sample description box could not be bound"));
            }
            continue;
        }

        if (*type != kBoxTypeStts && *type != kBoxTypeStsc && *type != kBoxTypeStsz &&
            *type != kBoxTypeStz2 && *type != kBoxTypeStco && *type != kBoxTypeCo64 &&
            *type != kBoxTypeStss && *type != kBoxTypeCtts) {
            continue;
        }

        const auto payloadStructId = payloadStructOf(tree, boxStructId);
        if (!payloadStructId.has_value()) {
            return fail(Mp4SampleTableExtractionStatus::SourceError,
                        QStringLiteral("Sample table box carries no decoded payload"));
        }
        const auto version = scalarFieldValue(tree, *payloadStructId, QStringLiteral("version"));
        if (!version.has_value()) {
            return fail(Mp4SampleTableExtractionStatus::SourceError,
                        QStringLiteral("Sample table payload carries no version field"));
        }
        // `ctts` alone defines a second version; every other table here is v0.
        const quint64 maximumVersion = (*type == kBoxTypeCtts) ? 1U : 0U;
        if (*version > maximumVersion) {
            return fail(Mp4SampleTableExtractionStatus::UnsupportedTables,
                        QStringLiteral("Unsupported sample table version"));
        }

        const auto windowNodeId = windowChildOf(tree, *payloadStructId);

        if (*type == kBoxTypeStts) {
            if (!tables.timeToSample.empty()) {
                continue;
            }
            const auto entryCount =
                scalarFieldValue(tree, *payloadStructId, QStringLiteral("entry_count"));
            if (!entryCount.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("stts declares no entry count"));
            }
            if (*entryCount != 0 && !windowNodeId.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("stts declares entries but carries no entry table"));
            }
            if (*entryCount > request.maximumRunRowsPerTable) {
                return fail(Mp4SampleTableExtractionStatus::ResourceLimit,
                            QStringLiteral("stts exceeds the extraction row budget"));
            }
            tables.timeToSample.reserve(static_cast<std::size_t>(*entryCount));
            if (*entryCount != 0) {
                const auto status = collectWindowRows(
                    analyzer, *windowNodeId, *entryCount, request,
                    [&](core::AnalysisNodeId entryNodeId) {
                        const auto sampleCount = entryFieldValue(tree, entryNodeId, 0);
                        const auto sampleDelta = entryFieldValue(tree, entryNodeId, 1);
                        if (!sampleCount.has_value() || !sampleDelta.has_value()) {
                            return false;
                        }
                        tables.timeToSample.push_back({*sampleCount, *sampleDelta});
                        return true;
                    });
                if (status != RowCollectStatus::Collected) {
                    return fail(toExtractionStatus(status),
                                QStringLiteral("stts entries could not be read"));
                }
            }
            continue;
        }

        if (*type == kBoxTypeStsc) {
            if (!tables.sampleToChunk.empty()) {
                continue;
            }
            const auto entryCount =
                scalarFieldValue(tree, *payloadStructId, QStringLiteral("entry_count"));
            if (!entryCount.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("stsc declares no entry count"));
            }
            if (*entryCount != 0 && !windowNodeId.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("stsc declares entries but carries no entry table"));
            }
            if (*entryCount > request.maximumRunRowsPerTable) {
                return fail(Mp4SampleTableExtractionStatus::ResourceLimit,
                            QStringLiteral("stsc exceeds the extraction row budget"));
            }
            tables.sampleToChunk.reserve(static_cast<std::size_t>(*entryCount));
            if (*entryCount != 0) {
                const auto status = collectWindowRows(
                    analyzer, *windowNodeId, *entryCount, request,
                    [&](core::AnalysisNodeId entryNodeId) {
                        const auto firstChunk = entryFieldValue(tree, entryNodeId, 0);
                        const auto samplesPerChunk = entryFieldValue(tree, entryNodeId, 1);
                        const auto descriptionIndex = entryFieldValue(tree, entryNodeId, 2);
                        if (!firstChunk.has_value() || !samplesPerChunk.has_value() ||
                            !descriptionIndex.has_value()) {
                            return false;
                        }
                        if (*descriptionIndex > std::numeric_limits<quint32>::max()) {
                            return false;
                        }
                        tables.sampleToChunk.push_back(
                            {*firstChunk, *samplesPerChunk,
                             static_cast<quint32>(*descriptionIndex)});
                        return true;
                    });
                if (status != RowCollectStatus::Collected) {
                    return fail(toExtractionStatus(status),
                                QStringLiteral("stsc entries could not be read"));
                }
            }
            continue;
        }

        if (*type == kBoxTypeCtts) {
            if (!tables.compositionOffsets.empty()) {
                continue;
            }
            const auto entryCount =
                scalarFieldValue(tree, *payloadStructId, QStringLiteral("entry_count"));
            if (!entryCount.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("ctts declares no entry count"));
            }
            if (*entryCount != 0 && !windowNodeId.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("ctts declares entries but carries no entry table"));
            }
            if (*entryCount > request.maximumRunRowsPerTable) {
                return fail(Mp4SampleTableExtractionStatus::ResourceLimit,
                            QStringLiteral("ctts exceeds the extraction row budget"));
            }
            // The offset stays raw here; sign reinterpretation is keyed on the
            // version by the index (ADR-0105 section 3.2).
            tables.compositionOffsetVersion = static_cast<quint8>(*version);
            tables.compositionOffsets.reserve(static_cast<std::size_t>(*entryCount));
            if (*entryCount != 0) {
                const auto status = collectWindowRows(
                    analyzer, *windowNodeId, *entryCount, request,
                    [&](core::AnalysisNodeId entryNodeId) {
                        const auto sampleCount = entryFieldValue(tree, entryNodeId, 0);
                        const auto rawOffset = entryFieldValue(tree, entryNodeId, 1);
                        if (!sampleCount.has_value() || !rawOffset.has_value()) {
                            return false;
                        }
                        tables.compositionOffsets.push_back({*sampleCount, *rawOffset});
                        return true;
                    });
                if (status != RowCollectStatus::Collected) {
                    return fail(toExtractionStatus(status),
                                QStringLiteral("ctts entries could not be read"));
                }
            }
            continue;
        }

        if (*type == kBoxTypeStsz) {
            if (haveSizeTable) {
                continue;
            }
            const auto sampleSize =
                scalarFieldValue(tree, *payloadStructId, QStringLiteral("sample_size"));
            const auto sampleCount =
                scalarFieldValue(tree, *payloadStructId, QStringLiteral("sample_count"));
            if (!sampleSize.has_value() || !sampleCount.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("stsz declares no sample size or count"));
            }
            tables.declaredSampleCount = *sampleCount;
            if (*sampleSize != 0) {
                // A uniform track has no entry table at all, so no reader is
                // bound and the index answers from `defaultSampleSize`.
                tables.defaultSampleSize = *sampleSize;
            } else {
                if (*sampleCount != 0 && !windowNodeId.has_value()) {
                    return fail(
                        Mp4SampleTableExtractionStatus::SourceError,
                        QStringLiteral("stsz declares samples but carries no entry table"));
                }
                if (windowNodeId.has_value()) {
                    readers.sampleSize = makeEntryValueReader(analyzer, *windowNodeId);
                }
            }
            haveSizeTable = true;
            continue;
        }

        if (*type == kBoxTypeStz2) {
            if (haveSizeTable) {
                continue;
            }
            const auto fieldSize =
                scalarFieldValue(tree, *payloadStructId, QStringLiteral("field_size"));
            const auto sampleCount =
                scalarFieldValue(tree, *payloadStructId, QStringLiteral("sample_count"));
            if (!fieldSize.has_value() || !sampleCount.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("stz2 declares no field size or sample count"));
            }
            if (*fieldSize != 4 && *fieldSize != 8 && *fieldSize != 16) {
                return fail(Mp4SampleTableExtractionStatus::UnsupportedTables,
                            QStringLiteral("Unsupported stz2 field size"));
            }
            tables.declaredSampleCount = *sampleCount;
            if (*sampleCount != 0 && !windowNodeId.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("stz2 declares samples but carries no entry table"));
            }
            if (windowNodeId.has_value()) {
                readers.sampleSize = (*fieldSize == 4)
                                         ? makePackedPairSizeReader(analyzer, *windowNodeId)
                                         : makeEntryValueReader(analyzer, *windowNodeId);
            }
            haveSizeTable = true;
            continue;
        }

        if (*type == kBoxTypeStco || *type == kBoxTypeCo64) {
            if (haveOffsetTable) {
                continue;
            }
            const auto entryCount =
                scalarFieldValue(tree, *payloadStructId, QStringLiteral("entry_count"));
            if (!entryCount.has_value()) {
                return fail(Mp4SampleTableExtractionStatus::SourceError,
                            QStringLiteral("Chunk offset box declares no entry count"));
            }
            if (*entryCount != 0 && !windowNodeId.has_value()) {
                return fail(
                    Mp4SampleTableExtractionStatus::SourceError,
                    QStringLiteral("Chunk offset box declares entries but carries no table"));
            }
            tables.chunkCount = *entryCount;
            if (windowNodeId.has_value()) {
                readers.chunkOffset = makeEntryValueReader(analyzer, *windowNodeId);
            }
            haveOffsetTable = true;
            continue;
        }

        // stss
        if (tables.hasSyncSampleTable) {
            continue;
        }
        const auto entryCount =
            scalarFieldValue(tree, *payloadStructId, QStringLiteral("entry_count"));
        if (!entryCount.has_value()) {
            return fail(Mp4SampleTableExtractionStatus::SourceError,
                        QStringLiteral("stss declares no entry count"));
        }
        // A present-but-empty stss means no sample is a sync sample, which is
        // distinct from an absent stss (ISO/IEC 14496-12 section 8.6.2.1).
        tables.hasSyncSampleTable = true;
        if (*entryCount == 0) {
            readers.isSyncSample = [](quint64) -> std::optional<bool> { return false; };
            continue;
        }
        if (!windowNodeId.has_value()) {
            return fail(Mp4SampleTableExtractionStatus::SourceError,
                        QStringLiteral("stss declares entries but carries no entry table"));
        }
        // Membership is answered by binary search, where a missing tail entry is
        // indistinguishable from a sample that is simply not listed. A truncated
        // table is therefore rejected here rather than silently reported as
        // "not a sync sample". The per-sample and per-chunk readers need no such
        // check: an out-of-range read surfaces as a page-level SourceError.
        const auto available = availableWindowEntryCount(tree, *windowNodeId);
        if (!available.has_value() || *available != *entryCount) {
            return fail(Mp4SampleTableExtractionStatus::SourceError,
                        QStringLiteral("stss declares more entries than its box carries"));
        }
        readers.isSyncSample = makeSyncSampleReader(analyzer, *windowNodeId, *entryCount);
    }

    // A track without a sample size table declares no sample count, so it is not
    // one this index describes.
    if (!haveSizeTable) {
        return TrackOutcome::Skipped;
    }

    if (!trackId.has_value() || *trackId == 0 ||
        *trackId > std::numeric_limits<quint32>::max()) {
        return fail(Mp4SampleTableExtractionStatus::IncompleteTrack,
                    QStringLiteral("Track carries sample tables but no usable track identifier"));
    }
    if (!timescale.has_value() || *timescale == 0 ||
        *timescale > std::numeric_limits<quint32>::max()) {
        return fail(Mp4SampleTableExtractionStatus::IncompleteTrack,
                    QStringLiteral("Track carries sample tables but no usable media timescale"));
    }
    if (!haveOffsetTable) {
        return fail(Mp4SampleTableExtractionStatus::IncompleteTrack,
                    QStringLiteral("Track carries sample tables but no chunk offset table"));
    }

    tables.trackId = static_cast<quint32>(*trackId);
    tables.timescale = static_cast<quint32>(*timescale);

    out->tables = std::move(tables);
    out->readers = std::move(readers);
    return TrackOutcome::Extracted;
}

} // namespace

Mp4SampleTableExtractionResult Mp4SampleTableExtractor::extract(
    const Mp4IsobmffAnalyzer& analyzer,
    const Mp4IsobmffAnalysisBatch& batch,
    const Mp4SampleTableExtractionRequest& request) {
    Mp4SampleTableExtractionResult result;
    const auto& tree = analyzer.tree();

    std::vector<Mp4ExtractedTrackTables> tracks;

    for (const auto boxNodeId : batch.boxNodes) {
        if (request.cancellation && request.cancellation->isCancellationRequested()) {
            result.status = Mp4SampleTableExtractionStatus::Cancelled;
            return result;
        }

        const auto boxNode = tree.node(boxNodeId);
        if (!boxNode || boxNode->children().empty()) {
            continue;
        }
        const auto boxStructId = boxNode->children().front();
        const auto boxType = boxTypeOf(tree, boxStructId);
        if (!boxType.has_value() || *boxType != kBoxTypeMoov) {
            continue;
        }

        for (const auto trakStructId : containerChildStructs(tree, boxStructId)) {
            const auto trakType = boxTypeOf(tree, trakStructId);
            if (!trakType.has_value() || *trakType != kBoxTypeTrak) {
                continue;
            }

            Mp4ExtractedTrackTables track;
            auto failure = Mp4SampleTableExtractionStatus::SourceError;
            QString errorMessage;
            const auto outcome =
                extractTrack(analyzer, trakStructId, request, &track, &failure, &errorMessage);
            if (outcome == TrackOutcome::Failed) {
                // A partial track list is never returned: a caller must not be
                // able to mistake a truncated movie for the whole one.
                result.status = failure;
                result.errorMessage = std::move(errorMessage);
                return result;
            }
            if (outcome == TrackOutcome::Extracted) {
                tracks.push_back(std::move(track));
            }
        }
    }

    if (tracks.empty()) {
        result.status = Mp4SampleTableExtractionStatus::NoTracks;
        result.errorMessage = QStringLiteral("No indexable track was found in the analyzed tree");
        return result;
    }

    result.status = Mp4SampleTableExtractionStatus::Extracted;
    result.tracks = std::move(tracks);
    return result;
}

} // namespace streamview::rules
