#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/mp4_sample_table_index.h>

#include <QString>
#include <QtGlobal>

#include <optional>
#include <vector>

namespace streamview::rules {

/// Bridges an analyzed ISOBMFF tree to `Mp4SampleTableIndex`.
///
/// `Mp4SampleTableIndex` (slice P5j-2) takes its per-sample and per-chunk tables
/// through the caller-bound `Mp4SampleTableReaders` seam, which keeps those tables
/// out of memory but leaves open *where* the values come from. This component
/// closes that gap: it walks a tree produced by `Mp4IsobmffAnalyzer`, collects the
/// track scalars and run-length rows into `Mp4TrackSampleTables`, and binds each
/// reader to the analyzer's window decoder so an entry is decoded only when a page
/// actually asks for it.
///
/// It lives in `src/rules/` because locating these tables requires box types and
/// MP4 rule struct shapes. No box name, box type constant, or rule struct name may
/// leak into `src/core/` or `src/app/` (ADR-0105 sections 2 and P5j-2).
///
/// Division of labour: this component reports only what the *tree* fails to
/// provide. Cross-table consistency — stsc/stts coverage against the declared
/// sample count, run-row bounds, version support — stays with
/// `Mp4SampleTableIndex::build()`, which is where those rules already live.

enum class Mp4SampleTableExtractionStatus : quint8 {
    Extracted,
    /// The batch carries no `moov`/`trak` structure to extract from.
    NoTracks,
    /// A track has sample tables but is missing a track id, timescale, sample
    /// size table, or chunk offset table.
    IncompleteTrack,
    /// A table variant this slice does not decode, such as an unsupported
    /// compact sample size field width.
    UnsupportedTables,
    /// A window entry the extraction needed could not be decoded.
    SourceError,
    /// A declared run-length table exceeds the extraction row budget.
    ResourceLimit,
    Cancelled,
};

/// One track's tables together with the readers bound to its lazy tables.
///
/// `readers` borrow the analyzer that produced them. The analyzer must outlive
/// them and must not be moved while they are held, since each reader decodes
/// through it on demand.
struct Mp4ExtractedTrackTables final {
    Mp4TrackSampleTables tables;
    Mp4SampleTableReaders readers;
};

struct Mp4SampleTableExtractionRequest final {
    /// Size of the root media source in bytes, propagated to
    /// `Mp4TrackSampleTables::sourceSizeBytes`. Zero disables the range check.
    quint64 sourceSizeBytes = 0;
    /// Upper bound on rows collected from one run-length table (`stts`, `stsc`,
    /// `ctts`), so a malformed declaration cannot make extraction unbounded.
    quint64 maximumRunRowsPerTable = Mp4SampleTableIndex::maximumRunRows();
    /// Entries decoded per window page while collecting run-length rows.
    quint64 runRowPageSize = 256;
    /// Upper bound on nodes visited while looking for the target format an
    /// `stsd` entry declares.
    quint64 maximumSampleEntryNodesVisited = 4096;
    std::optional<core::CancellationToken> cancellation;
};

struct Mp4SampleTableExtractionResult final {
    Mp4SampleTableExtractionStatus status = Mp4SampleTableExtractionStatus::NoTracks;
    /// Tracks in declaration order. Empty unless `status` is `Extracted`: a
    /// partial extraction is never returned, so a caller cannot mistake a
    /// truncated track list for the whole movie.
    std::vector<Mp4ExtractedTrackTables> tracks;
    QString errorMessage;

    [[nodiscard]] bool extracted() const noexcept {
        return status == Mp4SampleTableExtractionStatus::Extracted;
    }
};

class Mp4SampleTableExtractor final {
public:
    /// Extracts every indexable track from `batch`.
    ///
    /// A `trak` carrying no sample table box is skipped rather than failing the
    /// extraction, since not every track in a movie is one this index describes.
    /// A `trak` that does carry sample tables but lacks a scalar the index needs
    /// yields `IncompleteTrack`.
    [[nodiscard]] static Mp4SampleTableExtractionResult
    extract(const Mp4IsobmffAnalyzer& analyzer,
            const Mp4IsobmffAnalysisBatch& batch,
            const Mp4SampleTableExtractionRequest& request = {});
};

} // namespace streamview::rules
