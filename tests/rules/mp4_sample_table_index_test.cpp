#include <streamview/rules/mp4_sample_table_index.h>

#include <streamview/core/cancellation.h>
#include <streamview/core/paged_cache.h>
#include <streamview/core/sample_descriptor.h>

#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using streamview::core::CancellationSource;
using streamview::core::PagedCachePageKind;
using streamview::core::SampleDescriptor;
using streamview::rules::Mp4CompositionOffsetRow;
using streamview::rules::Mp4SampleDescriptionBinding;
using streamview::rules::Mp4SamplePageRequest;
using streamview::rules::Mp4SamplePageResult;
using streamview::rules::Mp4SampleTableIndex;
using streamview::rules::Mp4SampleTableIndexStatus;
using streamview::rules::Mp4SampleTableReaders;
using streamview::rules::Mp4SampleToChunkRow;
using streamview::rules::Mp4TimeToSampleRow;
using streamview::rules::Mp4TrackSampleTables;

namespace {

/// Stands in for the window decoders that will feed the index in production: the
/// per-sample and per-chunk tables live outside the index and are read entry by
/// entry, so the test can also count how much of each table a page touches.
struct TableFixture final {
    std::vector<quint64> sampleSizes;
    std::vector<quint64> chunkOffsets;
    std::vector<quint64> syncSampleNumbers;  // 1-based, as stss stores them
    quint64 sampleSizeCalls = 0;
    quint64 chunkOffsetCalls = 0;
    quint64 syncCalls = 0;
};

[[nodiscard]] Mp4SampleTableReaders readersFor(const std::shared_ptr<TableFixture>& fixture) {
    Mp4SampleTableReaders readers;
    readers.sampleSize = [fixture](quint64 sampleIndex) -> std::optional<quint64> {
        ++fixture->sampleSizeCalls;
        if (sampleIndex >= fixture->sampleSizes.size()) {
            return std::nullopt;
        }
        return fixture->sampleSizes[static_cast<std::size_t>(sampleIndex)];
    };
    readers.chunkOffset = [fixture](quint64 chunkIndex) -> std::optional<quint64> {
        ++fixture->chunkOffsetCalls;
        if (chunkIndex >= fixture->chunkOffsets.size()) {
            return std::nullopt;
        }
        return fixture->chunkOffsets[static_cast<std::size_t>(chunkIndex)];
    };
    readers.isSyncSample = [fixture](quint64 sampleNumber) -> std::optional<bool> {
        ++fixture->syncCalls;
        return std::find(fixture->syncSampleNumbers.begin(), fixture->syncSampleNumbers.end(),
                         sampleNumber)
               != fixture->syncSampleNumbers.end();
    };
    return readers;
}

/// A uniform track: `sampleCount` samples of `sampleSize` bytes, `samplesPerChunk`
/// per chunk, one `stts` run, no `stss`, no `ctts`.
[[nodiscard]] Mp4TrackSampleTables uniformTables(quint64 sampleCount,
                                                 quint64 sampleSize,
                                                 quint64 samplesPerChunk,
                                                 quint64 sampleDelta = 3000) {
    Mp4TrackSampleTables tables;
    tables.trackId = 1;
    tables.timescale = 90000;
    tables.defaultSampleSize = sampleSize;
    tables.declaredSampleCount = sampleCount;
    tables.sampleToChunk.push_back(Mp4SampleToChunkRow{1, samplesPerChunk, 1});
    tables.chunkCount = sampleCount / samplesPerChunk;
    tables.timeToSample.push_back(Mp4TimeToSampleRow{sampleCount, sampleDelta});
    tables.sampleDescriptions.push_back(
        Mp4SampleDescriptionBinding{.sampleDescriptionIndex = 1,
                                    .entryNode = streamview::core::AnalysisNodeId{7},
                                    .targetFormat = QStringLiteral("video.h264.nal"),
                                    .configurationNode = streamview::core::AnalysisNodeId{8},
                                    .prefixLengthBytes = 4});
    return tables;
}

[[nodiscard]] std::vector<quint64> ascendingChunkOffsets(quint64 chunkCount,
                                                         quint64 first,
                                                         quint64 stride) {
    std::vector<quint64> offsets;
    offsets.reserve(static_cast<std::size_t>(chunkCount));
    for (quint64 index = 0; index < chunkCount; ++index) {
        offsets.push_back(first + index * stride);
    }
    return offsets;
}

[[nodiscard]] quint64 spanByteOffset(const SampleDescriptor& descriptor) {
    return descriptor.sourceSpans.at(0).start().byteOffset();
}

[[nodiscard]] quint64 spanByteLength(const SampleDescriptor& descriptor) {
    return descriptor.sourceSpans.at(0).bitLength() / 8U;
}

/// Two `stsc` runs with different `samples_per_chunk` and different
/// `sample_description_index`, plus a variable `stsz`. Sample layout:
///   chunk 0 @1000: s0(10) s1(20)          run 0, sdi 1
///   chunk 1 @2000: s2(30) s3(40)          run 0, sdi 1
///   chunk 2 @3000: s4(50) s5(60) s6(70)   run 1, sdi 2
///   chunk 3 @4000: s7(80) s8(90) s9(100)  run 1, sdi 2
struct MixedRunTrack final {
    Mp4TrackSampleTables tables;
    std::shared_ptr<TableFixture> fixture;
};

[[nodiscard]] MixedRunTrack mixedRunTrack() {
    MixedRunTrack track;
    track.fixture = std::make_shared<TableFixture>();
    track.fixture->sampleSizes = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    track.fixture->chunkOffsets = {1000, 2000, 3000, 4000};

    auto& tables = track.tables;
    tables.trackId = 3;
    tables.timescale = 1000;
    tables.sourceSizeBytes = 100000;
    tables.declaredSampleCount = 10;
    tables.defaultSampleSize = 0;  // per-sample sizes come from the reader
    tables.chunkCount = 4;
    tables.sampleToChunk.push_back(Mp4SampleToChunkRow{1, 2, 1});
    tables.sampleToChunk.push_back(Mp4SampleToChunkRow{3, 3, 2});
    tables.timeToSample.push_back(Mp4TimeToSampleRow{10, 1000});
    // The video entry is length-prefixed and the audio entry is an opaque access
    // unit, which is how real extraction reports them: presence of a prefix size
    // is what distinguishes the two framings.
    tables.sampleDescriptions.push_back(
        Mp4SampleDescriptionBinding{.sampleDescriptionIndex = 1,
                                    .entryNode = streamview::core::AnalysisNodeId{11},
                                    .targetFormat = QStringLiteral("video.h264.nal"),
                                    .configurationNode = streamview::core::AnalysisNodeId{13},
                                    .prefixLengthBytes = 4});
    tables.sampleDescriptions.push_back(
        Mp4SampleDescriptionBinding{.sampleDescriptionIndex = 2,
                                    .entryNode = streamview::core::AnalysisNodeId{12},
                                    .targetFormat = QStringLiteral("audio.aac.asc"),
                                    .configurationNode = streamview::core::AnalysisNodeId{14},
                                    .prefixLengthBytes = std::nullopt});
    return track;
}

} // namespace

class Mp4SampleTableIndexTest : public QObject {
    Q_OBJECT

private slots:
    void buildsUniformTrackAcrossChunks() {
        auto tables = uniformTables(6, 100, 2);
        tables.sourceSizeBytes = 1000000;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000, 3000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());
        const auto& index = *build.index;
        QCOMPARE(index.sampleCount(), 6ULL);

        Mp4SamplePageRequest request;
        request.pageIndex = 0;
        request.pageSize = 6;
        const auto page = index.descriptorPage(request);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(page.descriptors.size(), static_cast<std::size_t>(6));
        QCOMPARE(page.firstSampleIndex, 0ULL);

        const std::vector<quint64> expectedOffsets = {1000, 1100, 2000, 2100, 3000, 3100};
        for (std::size_t position = 0; position < expectedOffsets.size(); ++position) {
            const auto& descriptor = page.descriptors[position];
            QCOMPARE(spanByteOffset(descriptor), expectedOffsets[position]);
            QCOMPARE(spanByteLength(descriptor), 100ULL);
            QCOMPARE(descriptor.sampleIndex, static_cast<quint64>(position));
            QCOMPARE(descriptor.trackId, 1U);
            QCOMPARE(descriptor.timescale, 90000U);
            QCOMPARE(descriptor.dts, static_cast<qint64>(position) * 3000LL);
            QCOMPARE(descriptor.pts, descriptor.dts);
            QCOMPARE(descriptor.duration, 3000ULL);
        }

        // A uniform stsz never consults the per-sample reader, and only the three
        // chunk offsets the page actually spans are read.
        QCOMPARE(fixture->sampleSizeCalls, 0ULL);
        QCOMPARE(fixture->chunkOffsetCalls, 3ULL);
        QCOMPARE(page.tableReadCount, 3ULL);
    }

    void buildsVariableSampleSizesAcrossRuns() {
        auto track = mixedRunTrack();
        auto build = Mp4SampleTableIndex::build(std::move(track.tables), readersFor(track.fixture));
        QVERIFY(build.succeeded());
        const auto& index = *build.index;
        QCOMPARE(index.sampleCount(), 10ULL);

        Mp4SamplePageRequest request;
        request.pageSize = 10;
        const auto page = index.descriptorPage(request);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(page.descriptors.size(), static_cast<std::size_t>(10));

        const std::vector<quint64> expectedOffsets = {1000, 1010, 2000, 2030, 3000,
                                                     3050, 3110, 4000, 4080, 4170};
        const std::vector<quint64> expectedSizes = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
        for (std::size_t position = 0; position < expectedOffsets.size(); ++position) {
            const auto& descriptor = page.descriptors[position];
            QCOMPARE(spanByteOffset(descriptor), expectedOffsets[position]);
            QCOMPARE(spanByteLength(descriptor), expectedSizes[position]);
        }

        // sample_description_index follows the stsc run, so it switches at sample 4.
        for (std::size_t position = 0; position < 4; ++position) {
            QCOMPARE(page.descriptors[position].sampleDescriptionIndex, 1U);
        }
        for (std::size_t position = 4; position < 10; ++position) {
            QCOMPARE(page.descriptors[position].sampleDescriptionIndex, 2U);
        }
    }

    void bindsSampleDescriptionIndexToStsdEntryAndTargetFormat() {
        auto track = mixedRunTrack();
        auto build = Mp4SampleTableIndex::build(std::move(track.tables), readersFor(track.fixture));
        QVERIFY(build.succeeded());
        const auto& index = *build.index;

        const auto* video = index.sampleDescription(1);
        QVERIFY(video != nullptr);
        QCOMPARE(video->targetFormat, QStringLiteral("video.h264.nal"));
        QCOMPARE(video->entryNode.value(), 11ULL);

        const auto* audio = index.sampleDescription(2);
        QVERIFY(audio != nullptr);
        QCOMPARE(audio->targetFormat, QStringLiteral("audio.aac.asc"));
        QCOMPARE(audio->entryNode.value(), 12ULL);

        // The index is 1-based; 0 and anything past the table have no binding.
        QCOMPARE(index.sampleDescription(0), nullptr);
        QCOMPARE(index.sampleDescription(3), nullptr);
    }

    void readsSixtyFourBitChunkOffsets() {
        auto tables = uniformTables(4, 4096, 2);
        tables.sourceSizeBytes = 0x200000000ULL;  // 8 GiB
        auto fixture = std::make_shared<TableFixture>();
        // co64-scale offsets: both chunks live past the 32-bit boundary.
        fixture->chunkOffsets = {0x100000000ULL, 0x180000000ULL};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 4;
        const auto page = build.index->descriptorPage(request);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(spanByteOffset(page.descriptors[0]), 0x100000000ULL);
        QCOMPARE(spanByteOffset(page.descriptors[1]), 0x100001000ULL);
        QCOMPARE(spanByteOffset(page.descriptors[2]), 0x180000000ULL);
        QCOMPARE(spanByteOffset(page.descriptors[3]), 0x180001000ULL);
    }

    void treatsEverySampleAsSyncWhenSyncSampleTableIsAbsent() {
        // ISO/IEC 14496-12 section 8.6.2.1: with no stss, every sample in the track is a
        // sync sample. This is a track-wide default (ADR-0105 section 3.1).
        auto tables = uniformTables(4, 100, 2);
        QCOMPARE(tables.hasSyncSampleTable, false);
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 4;
        const auto page = build.index->descriptorPage(request);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        for (const auto& descriptor : page.descriptors) {
            QVERIFY(descriptor.isSyncSample);
        }
        // The absent table is answered from the default, not by consulting a reader.
        QCOMPARE(fixture->syncCalls, 0ULL);
    }

    void honoursSyncSampleTableWhenPresent() {
        auto tables = uniformTables(4, 100, 2);
        tables.hasSyncSampleTable = true;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};
        fixture->syncSampleNumbers = {1, 4};  // stss numbers are 1-based

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 4;
        const auto page = build.index->descriptorPage(request);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(page.descriptors[0].isSyncSample, true);
        QCOMPARE(page.descriptors[1].isSyncSample, false);
        QCOMPARE(page.descriptors[2].isSyncSample, false);
        QCOMPARE(page.descriptors[3].isSyncSample, true);
    }

    void requiresSyncSampleReaderWhenTableExists() {
        auto tables = uniformTables(4, 100, 2);
        tables.hasSyncSampleTable = true;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};
        auto readers = readersFor(fixture);
        readers.isSyncSample = nullptr;

        const auto build = Mp4SampleTableIndex::build(std::move(tables), std::move(readers));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InvalidRequest);
    }

    void derivesPresentationTimestampsFromCompositionOffsets() {
        auto tables = uniformTables(4, 100, 2, 1000);
        tables.compositionOffsetVersion = 0;
        tables.compositionOffsets.push_back(Mp4CompositionOffsetRow{2, 2000});
        tables.compositionOffsets.push_back(Mp4CompositionOffsetRow{2, 0});
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 4;
        const auto page = build.index->descriptorPage(request);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(page.descriptors[0].dts, 0LL);
        QCOMPARE(page.descriptors[0].pts, 2000LL);
        QCOMPARE(page.descriptors[1].dts, 1000LL);
        QCOMPARE(page.descriptors[1].pts, 3000LL);
        // The ctts cursor crosses into the second run mid-page.
        QCOMPARE(page.descriptors[2].dts, 2000LL);
        QCOMPARE(page.descriptors[2].pts, 2000LL);
        QCOMPARE(page.descriptors[3].pts, 3000LL);
    }

    void allowsNegativePresentationTimestampsForVersionOneOffsets() {
        // ADR-0105 section 3.2: the DSL has no signed fixed-width field type, so a ctts
        // version 1 sample_offset arrives raw and unsigned and is reinterpreted here.
        // 0xFFFFFC18 is -1000 in two's complement, so sample 0 presents before it
        // decodes. A negative pts is legal and must never be clamped.
        auto tables = uniformTables(4, 100, 2, 1000);
        tables.compositionOffsetVersion = 1;
        tables.compositionOffsets.push_back(Mp4CompositionOffsetRow{1, 0xFFFFFC18ULL});
        tables.compositionOffsets.push_back(Mp4CompositionOffsetRow{3, 0});
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 4;
        const auto page = build.index->descriptorPage(request);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(page.descriptors[0].dts, 0LL);
        QCOMPARE(page.descriptors[0].pts, -1000LL);
        QVERIFY(page.descriptors[0].pts < page.descriptors[0].dts);
        QCOMPARE(page.descriptors[1].pts, 1000LL);
    }

    void readsVersionZeroCompositionOffsetsAsUnsigned() {
        // The same raw bits under version 0 are an unsigned offset, not a negative one.
        auto tables = uniformTables(4, 100, 2, 1000);
        tables.compositionOffsetVersion = 0;
        tables.compositionOffsets.push_back(Mp4CompositionOffsetRow{4, 0xFFFFFC18ULL});
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 4;
        const auto page = build.index->descriptorPage(request);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(page.descriptors[0].pts, 4294966296LL);
        QVERIFY(page.descriptors[0].pts > page.descriptors[0].dts);
    }

    void rejectsUnsupportedCompositionOffsetVersion() {
        auto tables = uniformTables(4, 100, 2);
        tables.compositionOffsetVersion = 2;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::UnsupportedTables);
    }

    void buildsEmptyTrackWithoutSampleTables() {
        Mp4TrackSampleTables tables;
        tables.trackId = 9;
        tables.timescale = 600;
        tables.declaredSampleCount = 0;
        tables.defaultSampleSize = 0;
        auto fixture = std::make_shared<TableFixture>();

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY2(build.succeeded(), qPrintable(build.errorMessage));
        QCOMPARE(build.index->sampleCount(), 0ULL);

        // There is no page 0 in an empty track.
        Mp4SamplePageRequest request;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::InvalidRequest);
        QVERIFY(page.descriptors.empty());
    }

    void rejectsMissingSampleToChunkTable() {
        auto tables = uniformTables(4, 100, 2);
        tables.sampleToChunk.clear();
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsMissingChunkOffsetTable() {
        auto tables = uniformTables(4, 100, 2);
        tables.chunkCount = 0;
        auto fixture = std::make_shared<TableFixture>();

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsZeroTimescale() {
        auto tables = uniformTables(4, 100, 2);
        tables.timescale = 0;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsSampleToChunkCoverageMismatch() {
        // stsc describes 2 chunks of 2 samples, but stsz declares 5 samples.
        auto tables = uniformTables(4, 100, 2);
        tables.declaredSampleCount = 5;
        tables.timeToSample.clear();
        tables.timeToSample.push_back(Mp4TimeToSampleRow{5, 3000});
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsTimeToSampleCoverageMismatch() {
        auto tables = uniformTables(4, 100, 2);
        tables.timeToSample.clear();
        tables.timeToSample.push_back(Mp4TimeToSampleRow{3, 3000});  // one sample short
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsCompositionOffsetCoverageMismatch() {
        auto tables = uniformTables(4, 100, 2);
        tables.compositionOffsets.push_back(Mp4CompositionOffsetRow{3, 0});  // covers 3 of 4
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsNonIncreasingSampleToChunkFirstChunk() {
        auto tables = uniformTables(6, 100, 2);
        tables.sampleToChunk.clear();
        tables.sampleToChunk.push_back(Mp4SampleToChunkRow{1, 2, 1});
        tables.sampleToChunk.push_back(Mp4SampleToChunkRow{1, 2, 1});  // repeats chunk 1
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000, 3000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsSampleToChunkBeyondChunkOffsetTable() {
        auto tables = uniformTables(4, 100, 2);
        tables.sampleToChunk.clear();
        tables.sampleToChunk.push_back(Mp4SampleToChunkRow{1, 2, 1});
        tables.sampleToChunk.push_back(Mp4SampleToChunkRow{5, 2, 1});  // only 2 chunks exist
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsSampleDescriptionIndexWithoutStsdBinding() {
        auto tables = uniformTables(4, 100, 2);
        tables.sampleToChunk.clear();
        tables.sampleToChunk.push_back(Mp4SampleToChunkRow{1, 2, 2});  // only one stsd entry
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsZeroSampleDescriptionIndex() {
        auto tables = uniformTables(4, 100, 2);
        tables.sampleToChunk.clear();
        tables.sampleToChunk.push_back(Mp4SampleToChunkRow{1, 2, 0});
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::InconsistentTables);
    }

    void rejectsRunTableLongerThanTheIndexBudget() {
        auto tables = uniformTables(4, 100, 2);
        tables.timeToSample.assign(Mp4SampleTableIndex::maximumRunRows() + 1,
                                   Mp4TimeToSampleRow{1, 1});
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        const auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QCOMPARE(build.status, Mp4SampleTableIndexStatus::ResourceLimit);
    }

    void rejectsSampleExtentOverflow() {
        auto tables = uniformTables(2, 1000, 2);
        tables.sourceSizeBytes = 0;  // unbounded source, so only checked arithmetic can reject
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {std::numeric_limits<quint64>::max() - 10ULL};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 2;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::ArithmeticOverflow);
        QVERIFY(page.descriptors.empty());
    }

    void rejectsBitCoordinateOverflow() {
        // SourceSpan is bit-addressed, so a byte offset above 2^64/8 has no
        // representable bit coordinate (ADR-0105 section 2 item 3).
        auto tables = uniformTables(2, 8, 2);
        tables.sourceSizeBytes = 0;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {0x2000000000000000ULL};  // 2^61 bytes

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 2;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::ArithmeticOverflow);
    }

    void rejectsDecodeTimelineOverflowWhileSeeking() {
        // Seeking to page 1 has to traverse the first stts run, whose duration exceeds
        // the signed timeline range.
        auto tables = uniformTables(4, 100, 2);
        tables.sourceSizeBytes = 0;
        tables.timeToSample.clear();
        tables.timeToSample.push_back(Mp4TimeToSampleRow{2, 1ULL << 62U});
        tables.timeToSample.push_back(Mp4TimeToSampleRow{2, 1000});
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageIndex = 1;
        request.pageSize = 2;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::ArithmeticOverflow);
        QVERIFY(page.descriptors.empty());
    }

    void rejectsDecodeTimelineOverflowWhileAdvancing() {
        auto tables = uniformTables(4, 100, 2);
        tables.sourceSizeBytes = 0;
        tables.timeToSample.clear();
        tables.timeToSample.push_back(Mp4TimeToSampleRow{4, 1ULL << 62U});
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 4;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::ArithmeticOverflow);
        QVERIFY(page.descriptors.empty());
    }

    void rejectsSamplesBeyondTheMediaSource() {
        auto tables = uniformTables(4, 100, 2);
        tables.sourceSizeBytes = 2050;  // chunk 1 ends at 2200
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 4;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::OutOfSourceRange);
        QVERIFY(page.descriptors.empty());
    }

    void rejectsOverlappingSampleRanges() {
        // Chunk 1 starts inside chunk 0's extent, so sample 2 would claim bytes that
        // sample 1 already owns.
        auto tables = uniformTables(4, 100, 2);
        tables.sourceSizeBytes = 100000;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 1050};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 4;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::OutOfSourceRange);
        QVERIFY(page.descriptors.empty());
    }

    void rejectsZeroPageSize() {
        auto tables = uniformTables(4, 100, 2);
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 0;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::InvalidRequest);
    }

    void rejectsPageBeyondTheSampleCount() {
        auto tables = uniformTables(4, 100, 2);
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageIndex = 4;
        request.pageSize = 2;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::InvalidRequest);
    }

    void rejectsPagesOverTheDescriptorBudget() {
        auto tables = uniformTables(10, 100, 2);
        tables.sourceSizeBytes = 100000;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = ascendingChunkOffsets(5, 1000, 1000);

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        Mp4SamplePageRequest request;
        request.pageSize = 10;
        request.maximumDescriptorsPerPage = 4;
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::ResourceLimit);
        QVERIFY(page.descriptors.empty());
    }

    void rejectsPagesOverTheTableReadBudget() {
        // A page starting deep inside one chunk of a variable-stsz track has to walk the
        // preceding sample sizes, which is exactly what the read budget bounds.
        constexpr quint64 sampleCount = 1000;
        Mp4TrackSampleTables tables;
        tables.trackId = 2;
        tables.timescale = 1000;
        tables.sourceSizeBytes = 10000000;
        tables.declaredSampleCount = sampleCount;
        tables.chunkCount = 1;
        tables.sampleToChunk.push_back(Mp4SampleToChunkRow{1, sampleCount, 1});
        tables.timeToSample.push_back(Mp4TimeToSampleRow{sampleCount, 10});

        auto fixture = std::make_shared<TableFixture>();
        fixture->sampleSizes.assign(static_cast<std::size_t>(sampleCount), 100);
        fixture->chunkOffsets = {1000};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());
        const auto& index = *build.index;

        Mp4SamplePageRequest budgeted;
        budgeted.pageIndex = 50;  // starts at sample 500
        budgeted.pageSize = 10;
        budgeted.maximumTableReadsPerPage = 100;
        const auto rejected = index.descriptorPage(budgeted);
        QCOMPARE(rejected.status, Mp4SampleTableIndexStatus::ResourceLimit);
        QVERIFY(rejected.descriptors.empty());

        // With the default budget the same page succeeds. The reads are one chunk offset,
        // 499 catch-up sizes to reach the overlap-guard predecessor (sample 499), that
        // predecessor's own size, then ten sizes for the page itself: 511 in total.
        Mp4SamplePageRequest allowed;
        allowed.pageIndex = 50;
        allowed.pageSize = 10;
        const auto page = index.descriptorPage(allowed);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(page.tableReadCount, 511ULL);
        QCOMPARE(spanByteOffset(page.descriptors[0]), 1000ULL + 500ULL * 100ULL);
    }

    void reportsSourceErrorScopedToTheFailingPage() {
        auto tables = uniformTables(4, 100, 2);
        tables.sourceSizeBytes = 100000;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000};  // chunk 1 cannot be supplied

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());
        const auto& index = *build.index;

        Mp4SamplePageRequest second;
        second.pageIndex = 1;
        second.pageSize = 2;
        const auto failed = index.descriptorPage(second);
        QCOMPARE(failed.status, Mp4SampleTableIndexStatus::SourceError);
        QVERIFY(failed.descriptors.empty());

        // The failure belongs to that page alone; page 0 still resolves.
        Mp4SamplePageRequest first;
        first.pageIndex = 0;
        first.pageSize = 2;
        const auto page = index.descriptorPage(first);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(page.descriptors.size(), static_cast<std::size_t>(2));
    }

    void cancelsPageWithoutEmittingPartialDescriptors() {
        auto tables = uniformTables(10, 100, 2);
        tables.sourceSizeBytes = 100000;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = ascendingChunkOffsets(5, 1000, 1000);

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());

        CancellationSource source;
        QVERIFY(source.requestCancellation());

        Mp4SamplePageRequest request;
        request.pageSize = 10;
        request.cancellation = source.token();
        const auto page = build.index->descriptorPage(request);
        QCOMPARE(page.status, Mp4SampleTableIndexStatus::Cancelled);
        QVERIFY(page.descriptors.empty());
    }

    void cancellationIsScopedToTheRequestedPage() {
        auto tables = uniformTables(10, 100, 2);
        tables.sourceSizeBytes = 100000;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = ascendingChunkOffsets(5, 1000, 1000);

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());
        const auto& index = *build.index;

        CancellationSource cancelled;
        QVERIFY(cancelled.requestCancellation());

        Mp4SamplePageRequest first;
        first.pageIndex = 0;
        first.pageSize = 2;
        first.cancellation = cancelled.token();
        QCOMPARE(index.descriptorPage(first).status, Mp4SampleTableIndexStatus::Cancelled);

        // A different page with its own scope is unaffected, and so is an uncancelled
        // request for the very page that was cancelled.
        CancellationSource live;
        Mp4SamplePageRequest second;
        second.pageIndex = 1;
        second.pageSize = 2;
        second.cancellation = live.token();
        const auto secondPage = index.descriptorPage(second);
        QVERIFY2(secondPage.built(), qPrintable(secondPage.errorMessage));

        Mp4SamplePageRequest retry;
        retry.pageIndex = 0;
        retry.pageSize = 2;
        const auto retried = index.descriptorPage(retry);
        QVERIFY2(retried.built(), qPrintable(retried.errorMessage));
        QCOMPARE(retried.descriptors.size(), static_cast<std::size_t>(2));
    }

    void repeatedPageRequestsAreIdempotent() {
        auto track = mixedRunTrack();
        auto fixture = track.fixture;
        auto build = Mp4SampleTableIndex::build(std::move(track.tables), readersFor(fixture));
        QVERIFY(build.succeeded());
        const auto& index = *build.index;

        Mp4SamplePageRequest request;
        request.pageIndex = 1;
        request.pageSize = 3;

        const auto first = index.descriptorPage(request);
        QVERIFY2(first.built(), qPrintable(first.errorMessage));
        const quint64 readsForFirst = fixture->sampleSizeCalls + fixture->chunkOffsetCalls;

        const auto second = index.descriptorPage(request);
        QVERIFY2(second.built(), qPrintable(second.errorMessage));
        const quint64 readsForSecond =
            fixture->sampleSizeCalls + fixture->chunkOffsetCalls - readsForFirst;

        // Repeating a page costs the same work and yields the same descriptors: no
        // duplicate allocation accumulates on the index.
        QCOMPARE(readsForSecond, readsForFirst);
        QCOMPARE(first.tableReadCount, second.tableReadCount);
        QCOMPARE(second.descriptors.size(), first.descriptors.size());
        QCOMPARE(second.firstSampleIndex, first.firstSampleIndex);
        QCOMPARE(second.cacheKey, first.cacheKey);
        for (std::size_t position = 0; position < first.descriptors.size(); ++position) {
            QCOMPARE(spanByteOffset(second.descriptors[position]),
                     spanByteOffset(first.descriptors[position]));
            QCOMPARE(spanByteLength(second.descriptors[position]),
                     spanByteLength(first.descriptors[position]));
            QCOMPARE(second.descriptors[position].dts, first.descriptors[position].dts);
            QCOMPARE(second.descriptors[position].pts, first.descriptors[position].pts);
            QCOMPARE(second.descriptors[position].sampleDescriptionIndex,
                     first.descriptors[position].sampleDescriptionIndex);
        }
    }

    void pagesTileTheSampleRangeWithoutGaps() {
        auto track = mixedRunTrack();
        auto build = Mp4SampleTableIndex::build(std::move(track.tables), readersFor(track.fixture));
        QVERIFY(build.succeeded());
        const auto& index = *build.index;

        // Page boundaries land mid-chunk and mid-stsc-run, so this also proves the
        // cursors re-seek correctly rather than only walking from sample 0.
        const std::vector<quint64> expectedOffsets = {1000, 1010, 2000, 2030, 3000,
                                                      3050, 3110, 4000, 4080, 4170};
        std::vector<quint64> collected;
        for (quint64 pageIndex = 0; pageIndex < 4; ++pageIndex) {
            Mp4SamplePageRequest request;
            request.pageIndex = pageIndex;
            request.pageSize = 3;
            const auto page = index.descriptorPage(request);
            QVERIFY2(page.built(), qPrintable(page.errorMessage));
            QCOMPARE(page.firstSampleIndex, pageIndex * 3ULL);
            for (const auto& descriptor : page.descriptors) {
                QCOMPARE(descriptor.sampleIndex, static_cast<quint64>(collected.size()));
                collected.push_back(spanByteOffset(descriptor));
            }
        }
        QCOMPARE(collected, expectedOffsets);

        // The final page is short rather than padded.
        Mp4SamplePageRequest last;
        last.pageIndex = 3;
        last.pageSize = 3;
        QCOMPARE(index.descriptorPage(last).descriptors.size(), static_cast<std::size_t>(1));
    }

    void cachePageKeysDistinguishTracksAndPages() {
        auto firstTables = uniformTables(4, 100, 2);
        firstTables.trackId = 1;
        auto secondTables = uniformTables(4, 100, 2);
        secondTables.trackId = 2;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1000, 2000};

        auto firstBuild = Mp4SampleTableIndex::build(std::move(firstTables), readersFor(fixture));
        auto secondBuild = Mp4SampleTableIndex::build(std::move(secondTables), readersFor(fixture));
        QVERIFY(firstBuild.succeeded());
        QVERIFY(secondBuild.succeeded());

        const auto firstKey = firstBuild.index->cachePageKey(0);
        const auto secondKey = secondBuild.index->cachePageKey(0);
        QCOMPARE(firstKey.kind, PagedCachePageKind::ProgressiveIndex);
        // Independent tracks must never share a cache page (ADR-0105 section 6 item 1).
        QVERIFY(firstKey.streamId != secondKey.streamId);
        QVERIFY(firstKey != secondKey);
        QVERIFY(firstBuild.index->cachePageKey(1) != firstKey);
        QCOMPARE(firstBuild.index->cachePageKey(3).pageIndex, 3ULL);
    }

    void servesPagesFromALargeVirtualSourceWithoutCopyingPayload() {
        // A 100 GB source with 4 KiB samples in 4 MiB chunks: 26 214 400 samples. The
        // index holds one stts row, one stsc row and a chunk count, so nothing here
        // scales with sample count, and no payload byte is ever read.
        constexpr quint64 sourceSize = 100ULL * 1024ULL * 1024ULL * 1024ULL;
        constexpr quint64 sampleSize = 4096;
        constexpr quint64 samplesPerChunk = 1024;
        constexpr quint64 chunkSize = sampleSize * samplesPerChunk;
        constexpr quint64 headerBytes = 4096;
        constexpr quint64 chunkCount = (sourceSize - headerBytes) / chunkSize;
        constexpr quint64 sampleCount = chunkCount * samplesPerChunk;

        Mp4TrackSampleTables tables;
        tables.trackId = 4;
        tables.timescale = 90000;
        tables.sourceSizeBytes = sourceSize;
        tables.defaultSampleSize = sampleSize;
        tables.declaredSampleCount = sampleCount;
        tables.chunkCount = chunkCount;
        tables.sampleToChunk.push_back(Mp4SampleToChunkRow{1, samplesPerChunk, 1});
        tables.timeToSample.push_back(Mp4TimeToSampleRow{sampleCount, 1500});

        // The chunk offsets are computed rather than materialized, exactly as a window
        // decoder over a 100 GB file would supply them.
        quint64 chunkOffsetCalls = 0;
        Mp4SampleTableReaders readers;
        readers.chunkOffset = [&chunkOffsetCalls](quint64 chunkIndex) -> std::optional<quint64> {
            ++chunkOffsetCalls;
            return headerBytes + chunkIndex * chunkSize;
        };

        auto build = Mp4SampleTableIndex::build(std::move(tables), std::move(readers));
        QVERIFY2(build.succeeded(), qPrintable(build.errorMessage));
        const auto& index = *build.index;
        QCOMPARE(index.sampleCount(), sampleCount);
        QVERIFY(index.sampleCount() > 26000000ULL);

        // Ask for a page in the middle of the file. Seeking there must not touch the
        // samples before it.
        const quint64 middlePage = sampleCount / 2ULL / 256ULL;
        Mp4SamplePageRequest request;
        request.pageIndex = middlePage;
        request.pageSize = 256;
        const auto page = index.descriptorPage(request);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(page.descriptors.size(), static_cast<std::size_t>(256));

        // Work is bounded by the page, not by the table: a uniform stsz needs no
        // per-sample reads at all, and this page sits inside a single chunk.
        QCOMPARE(chunkOffsetCalls, 1ULL);
        QCOMPARE(page.tableReadCount, 1ULL);

        // Every descriptor is a physical span in the original file, contiguous and
        // never a copy.
        const quint64 firstSample = middlePage * 256ULL;
        for (std::size_t position = 0; position < page.descriptors.size(); ++position) {
            const auto& descriptor = page.descriptors[position];
            const quint64 sampleIndex = firstSample + static_cast<quint64>(position);
            const quint64 expectedOffset = headerBytes + (sampleIndex / samplesPerChunk) * chunkSize
                                           + (sampleIndex % samplesPerChunk) * sampleSize;
            QCOMPARE(spanByteOffset(descriptor), expectedOffset);
            QCOMPARE(spanByteLength(descriptor), sampleSize);
            QVERIFY(spanByteOffset(descriptor) + spanByteLength(descriptor) <= sourceSize);
            QCOMPARE(descriptor.sourceSpans.size(), static_cast<std::size_t>(1));
        }
        // Offsets this deep exceed 32-bit range, which is the co64 case the index has to
        // address without ever materializing the offsets it skipped over.
        QVERIFY(spanByteOffset(page.descriptors.front()) > 0xFFFFFFFFULL);

        // Paging further into the file stays just as cheap.
        chunkOffsetCalls = 0;
        Mp4SamplePageRequest tail;
        tail.pageIndex = (sampleCount / 256ULL) - 1ULL;
        tail.pageSize = 256;
        const auto tailPage = index.descriptorPage(tail);
        QVERIFY2(tailPage.built(), qPrintable(tailPage.errorMessage));
        QCOMPARE(chunkOffsetCalls, 1ULL);
        QCOMPARE(tailPage.descriptors.size(), static_cast<std::size_t>(256));
    }

    void detectsOverlapAcrossAPageBoundary() {
        // The overlapping pair straddles a page boundary: sample 1 ends at 1200 and
        // sample 2 starts at 1150. Whether a corrupt table is rejected must not depend
        // on where the caller happens to cut its pages, so the page starting at
        // sample 2 has to compare against sample 1 rather than trusting its own first
        // sample blindly.
        auto tables = uniformTables(4, 100, 2);
        tables.sourceSizeBytes = 100000;
        auto fixture = std::make_shared<TableFixture>();
        fixture->chunkOffsets = {1100, 1150};

        auto build = Mp4SampleTableIndex::build(std::move(tables), readersFor(fixture));
        QVERIFY(build.succeeded());
        const auto& index = *build.index;

        Mp4SamplePageRequest secondPage;
        secondPage.pageIndex = 1;
        secondPage.pageSize = 2;
        const auto rejected = index.descriptorPage(secondPage);
        QCOMPARE(rejected.status, Mp4SampleTableIndexStatus::OutOfSourceRange);
        QVERIFY(rejected.descriptors.empty());

        // Page 0 covers only the non-overlapping pair, so it still resolves: the
        // rejection is scoped to the page that actually contains the overlap.
        Mp4SamplePageRequest firstPage;
        firstPage.pageIndex = 0;
        firstPage.pageSize = 2;
        const auto page = index.descriptorPage(firstPage);
        QVERIFY2(page.built(), qPrintable(page.errorMessage));
        QCOMPARE(spanByteOffset(page.descriptors[0]), 1100ULL);
        QCOMPARE(spanByteOffset(page.descriptors[1]), 1200ULL);
    }
};

QTEST_GUILESS_MAIN(Mp4SampleTableIndexTest)

#include "mp4_sample_table_index_test.moc"
