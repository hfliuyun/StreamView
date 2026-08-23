#pragma once

#include <streamview/core/coordinates.h>

#include <QtGlobal>

#include <vector>

namespace streamview::core {

/// Format-neutral description of one logical access unit inside a media source.
///
/// The struct deliberately carries no codec name and no container box name: the
/// binding between a sample and its codec-specific description is resolved
/// outside `src/core/` by a rules-layer component (see ADR-0105 section 2).
struct SampleDescriptor final {
    quint32 trackId = 0;
    quint64 sampleIndex = 0;             // 0-based sample ordinal within track
    quint32 sampleDescriptionIndex = 1;  // 1-based index into the track's sample descriptions
    std::vector<SourceSpan> sourceSpans; // Absolute span(s) in the root media source
    qint64 dts = 0;                      // Decoding Time Stamp in timescale units
    qint64 pts = 0;                      // Presentation Time Stamp in timescale units
    quint64 duration = 0;                // Sample duration in timescale units
    quint32 timescale = 1;               // Track timescale
    bool isSyncSample = true;            // Keyframe / random access point
};

} // namespace streamview::core
