#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/mp4_sample_table_extractor.h>
#include <streamview/rules/mp4_sample_table_index.h>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtTest>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

using streamview::core::CancellationSource;
using streamview::core::SampleDescriptor;
using streamview::rules::Mp4IsobmffAnalyzer;
using streamview::rules::Mp4SampleTableExtractionRequest;
using streamview::rules::Mp4SampleTableExtractionStatus;
using streamview::rules::Mp4SampleTableExtractor;
using streamview::rules::Mp4SampleTableIndex;

namespace {

[[nodiscard]] std::vector<std::byte> readFixtureBytes(const QString& relativePath) {
    const QString fullPath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/") + relativePath;
    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Failed to open test fixture: %s", qUtf8Printable(fullPath));
    }
    const QByteArray bytes = file.readAll();
    std::vector<std::byte> result(static_cast<std::size_t>(bytes.size()));
    std::transform(bytes.cbegin(), bytes.cend(), result.begin(), [](char ch) {
        return static_cast<std::byte>(ch);
    });
    return result;
}

/// In-memory source that can be told to fail reads at or beyond a byte offset,
/// so a table whose bytes become unreadable can be simulated without editing a
/// fixture. The failure switch is mutable because the analyzer holds the source
/// by const reference.
class MemorySource final : public streamview::core::RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> data) : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }

    [[nodiscard]] QString identity() const override { return QStringLiteral("memory"); }

    [[nodiscard]] streamview::core::SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (failFromOffset_.has_value() && byteOffset >= *failFromOffset_) {
            return {streamview::core::SourceReadStatus::Error, 0, QStringLiteral("injected")};
        }
        if (destination.empty()) {
            return {streamview::core::SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= data_.size()) {
            return {streamview::core::SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const auto count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.data() + offset, count, destination.data());
        return {count == destination.size()
                    ? streamview::core::SourceReadStatus::Complete
                    : streamview::core::SourceReadStatus::EndOfSource,
                count, {}};
    }

    void failReadsFrom(quint64 byteOffset) const { failFromOffset_ = byteOffset; }
    void clearFailures() const { failFromOffset_.reset(); }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return data_; }

private:
    std::vector<std::byte> data_;
    mutable std::optional<quint64> failFromOffset_;
};

/// The bytes a descriptor's single source span covers.
[[nodiscard]] std::vector<std::byte> spanBytes(const MemorySource& source,
                                               const SampleDescriptor& descriptor) {
    if (descriptor.sourceSpans.size() != 1) {
        return {};
    }
    const auto& span = descriptor.sourceSpans.front();
    const quint64 begin = span.start().byteOffset();
    const quint64 length = span.bitLength() / 8U;
    if (begin > source.bytes().size() || length > source.bytes().size() - begin) {
        return {};
    }
    return std::vector<std::byte>(source.bytes().begin() + static_cast<std::ptrdiff_t>(begin),
                                  source.bytes().begin() +
                                      static_cast<std::ptrdiff_t>(begin + length));
}

} // namespace

class Mp4SampleTableExtractorTest : public QObject {
    Q_OBJECT

private:
    // Kept alive for the lifetime of one test function: the bound readers borrow
    // the analyzer, so it must outlive every index built from it.
    std::vector<std::byte> bytes_;
    std::optional<MemorySource> source_;
    std::optional<Mp4IsobmffAnalyzer> analyzer_;
    streamview::rules::Mp4IsobmffAnalysisBatch batch_;

    void analyzeFixture(const QString& relativePath) {
        analyzer_.reset();
        source_.reset();
        bytes_ = readFixtureBytes(relativePath);
        source_.emplace(bytes_);
        QString error;
        analyzer_ = Mp4IsobmffAnalyzer::create(*source_, &error);
        QVERIFY2(analyzer_.has_value(), qUtf8Printable(error));
        batch_ = analyzer_->analyzeBatch();
        QVERIFY2(batch_.complete(), qUtf8Printable(batch_.errorMessage));
    }

    [[nodiscard]] Mp4SampleTableExtractionRequest defaultRequest() const {
        Mp4SampleTableExtractionRequest request;
        request.sourceSizeBytes = source_->sizeBytes();
        return request;
    }

private slots:
    void cleanup() {
        analyzer_.reset();
        source_.reset();
        bytes_.clear();
        batch_ = {};
    }

    // The end-to-end slice: a real analyzed tree yields tables and readers that
    // Mp4SampleTableIndex::build accepts, and the resulting descriptors point at
    // the actual sample bytes. No table is hand-built anywhere in this test.
    //
    // This proves the P5j-2 reader seam binds to the real window decoder. It is
    // not the ffprobe cross-check that P5j-6 requires.
    void extractsCompleteTrackAndProducesDescriptors() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_complete_track.mp4"));

        const auto extraction =
            Mp4SampleTableExtractor::extract(*analyzer_, batch_, defaultRequest());
        QVERIFY2(extraction.extracted(), qUtf8Printable(extraction.errorMessage));
        QCOMPARE(extraction.tracks.size(), std::size_t{1});

        const auto& extracted = extraction.tracks.front();
        QCOMPARE(extracted.tables.trackId, quint32{1});
        QCOMPARE(extracted.tables.timescale, quint32{30000});
        QCOMPARE(extracted.tables.declaredSampleCount, quint64{6});
        QCOMPARE(extracted.tables.defaultSampleSize, quint64{0});
        QCOMPARE(extracted.tables.chunkCount, quint64{2});
        QVERIFY(extracted.tables.hasSyncSampleTable);
        QCOMPARE(extracted.tables.timeToSample.size(), std::size_t{1});
        QCOMPARE(extracted.tables.timeToSample.front().sampleCount, quint64{6});
        QCOMPARE(extracted.tables.timeToSample.front().sampleDelta, quint64{1000});
        QCOMPARE(extracted.tables.sampleToChunk.size(), std::size_t{1});
        QCOMPARE(extracted.tables.sampleToChunk.front().firstChunk, quint64{1});
        QCOMPARE(extracted.tables.sampleToChunk.front().samplesPerChunk, quint64{3});
        QCOMPARE(extracted.tables.compositionOffsets.size(), std::size_t{2});
        QCOMPARE(extracted.tables.compositionOffsetVersion, quint8{0});
        QCOMPARE(extracted.tables.compositionOffsets.front().sampleCount, quint64{3});
        QCOMPARE(extracted.tables.compositionOffsets.front().rawSampleOffset, quint64{500});
        QVERIFY(static_cast<bool>(extracted.readers.sampleSize));
        QVERIFY(static_cast<bool>(extracted.readers.chunkOffset));
        QVERIFY(static_cast<bool>(extracted.readers.isSyncSample));

        // The video sample entry declares the H.264 target format on its avcC
        // configuration payload.
        QCOMPARE(extracted.tables.sampleDescriptions.size(), std::size_t{1});
        QCOMPARE(extracted.tables.sampleDescriptions.front().sampleDescriptionIndex, quint32{1});
        QCOMPARE(extracted.tables.sampleDescriptions.front().targetFormat,
                 QStringLiteral("video.h264.nal"));

        auto build = Mp4SampleTableIndex::build(extracted.tables, extracted.readers);
        QVERIFY2(build.succeeded(), qUtf8Printable(build.errorMessage));
        QCOMPARE(build.index->sampleCount(), quint64{6});

        streamview::rules::Mp4SamplePageRequest pageRequest;
        pageRequest.pageSize = 6;
        const auto page = build.index->descriptorPage(pageRequest);
        QVERIFY2(page.built(), qUtf8Printable(page.errorMessage));
        QCOMPARE(page.descriptors.size(), std::size_t{6});

        const std::vector<quint64> expectedSizes{1000, 200, 150, 300, 120, 90};
        for (std::size_t index = 0; index < page.descriptors.size(); ++index) {
            const auto& descriptor = page.descriptors[index];
            QCOMPARE(descriptor.trackId, quint32{1});
            QCOMPARE(descriptor.sampleIndex, static_cast<quint64>(index));
            QCOMPARE(descriptor.timescale, quint32{30000});
            QCOMPARE(descriptor.duration, quint64{1000});
            QCOMPARE(descriptor.dts, static_cast<qint64>(index) * 1000);
            QCOMPARE(descriptor.sourceSpans.size(), std::size_t{1});
            QCOMPARE(descriptor.sourceSpans.front().bitLength(), expectedSizes[index] * 8U);

            // Each sample was filled with a distinct byte, so this proves the
            // span points at that sample rather than at a plausible offset.
            const auto actual = spanBytes(*source_, descriptor);
            QCOMPARE(actual.size(), static_cast<std::size_t>(expectedSizes[index]));
            const auto expectedByte = static_cast<std::byte>(0xA1 + index);
            QVERIFY(std::all_of(actual.cbegin(), actual.cend(), [expectedByte](std::byte value) {
                return value == expectedByte;
            }));
        }

        // ctts v0 rows (3, 500) then (3, 1000).
        QCOMPARE(page.descriptors[0].pts, qint64{500});
        QCOMPARE(page.descriptors[2].pts, qint64{2500});
        QCOMPARE(page.descriptors[3].pts, qint64{4000});

        // stss lists samples 1 and 4, i.e. 0-based 0 and 3.
        QVERIFY(page.descriptors[0].isSyncSample);
        QVERIFY(!page.descriptors[1].isSyncSample);
        QVERIFY(!page.descriptors[2].isSyncSample);
        QVERIFY(page.descriptors[3].isSyncSample);
        QVERIFY(!page.descriptors[4].isSyncSample);
        QVERIFY(!page.descriptors[5].isSyncSample);
    }

    // An absent stss makes every sample a sync sample, and an absent ctts makes
    // pts equal dts (ISO/IEC 14496-12 section 8.6.2.1).
    void treatsAbsentSyncTableAsAllSyncAndAbsentCompositionAsIdentity() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_stss_absent_no_ctts.mp4"));

        const auto extraction =
            Mp4SampleTableExtractor::extract(*analyzer_, batch_, defaultRequest());
        QVERIFY2(extraction.extracted(), qUtf8Printable(extraction.errorMessage));
        QCOMPARE(extraction.tracks.size(), std::size_t{1});

        const auto& extracted = extraction.tracks.front();
        QVERIFY(!extracted.tables.hasSyncSampleTable);
        QVERIFY(!static_cast<bool>(extracted.readers.isSyncSample));
        QVERIFY(extracted.tables.compositionOffsets.empty());
        QCOMPARE(extracted.tables.timescale, quint32{44100});
        QCOMPARE(extracted.tables.sampleDescriptions.size(), std::size_t{1});
        QCOMPARE(extracted.tables.sampleDescriptions.front().targetFormat,
                 QStringLiteral("audio.aac.asc"));

        auto build = Mp4SampleTableIndex::build(extracted.tables, extracted.readers);
        QVERIFY2(build.succeeded(), qUtf8Printable(build.errorMessage));

        streamview::rules::Mp4SamplePageRequest pageRequest;
        pageRequest.pageSize = 4;
        const auto page = build.index->descriptorPage(pageRequest);
        QVERIFY2(page.built(), qUtf8Printable(page.errorMessage));
        QCOMPARE(page.descriptors.size(), std::size_t{4});
        for (const auto& descriptor : page.descriptors) {
            QVERIFY(descriptor.isSyncSample);
            QCOMPARE(descriptor.pts, descriptor.dts);
        }
    }

    // ctts version 1 offsets are signed, so a sample can present before it
    // decodes. The negative offset must not be clamped away.
    void preservesNegativeCompositionOffsetFromVersionOneTable() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_ctts_v1_negative.mp4"));

        const auto extraction =
            Mp4SampleTableExtractor::extract(*analyzer_, batch_, defaultRequest());
        QVERIFY2(extraction.extracted(), qUtf8Printable(extraction.errorMessage));

        const auto& extracted = extraction.tracks.front();
        QCOMPARE(extracted.tables.compositionOffsetVersion, quint8{1});
        QCOMPARE(extracted.tables.compositionOffsets.size(), std::size_t{3});

        auto build = Mp4SampleTableIndex::build(extracted.tables, extracted.readers);
        QVERIFY2(build.succeeded(), qUtf8Printable(build.errorMessage));

        streamview::rules::Mp4SamplePageRequest pageRequest;
        pageRequest.pageSize = 4;
        const auto page = build.index->descriptorPage(pageRequest);
        QVERIFY2(page.built(), qUtf8Printable(page.errorMessage));
        QCOMPARE(page.descriptors.size(), std::size_t{4});

        // Rows (1, +3000), (1, -3000), (2, 0) against dts 0/3000/6000/9000.
        QCOMPARE(page.descriptors[0].dts, qint64{0});
        QCOMPARE(page.descriptors[0].pts, qint64{3000});
        QCOMPARE(page.descriptors[1].dts, qint64{3000});
        QCOMPARE(page.descriptors[1].pts, qint64{0});
        QCOMPARE(page.descriptors[2].pts, qint64{6000});
        QCOMPARE(page.descriptors[3].pts, qint64{9000});
        QVERIFY(page.descriptors[1].pts < page.descriptors[1].dts);
    }

    // A non-zero stsz.sample_size means the rule emits no entry table at all, so
    // no sampleSize reader may be bound and the index must answer from
    // defaultSampleSize without ever consulting one.
    void bindsNoSizeReaderForUniformSampleSizeTrack() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_uniform_sample_size.mp4"));

        const auto extraction =
            Mp4SampleTableExtractor::extract(*analyzer_, batch_, defaultRequest());
        QVERIFY2(extraction.extracted(), qUtf8Printable(extraction.errorMessage));

        const auto& extracted = extraction.tracks.front();
        QCOMPARE(extracted.tables.defaultSampleSize, quint64{512});
        QCOMPARE(extracted.tables.declaredSampleCount, quint64{4});
        QVERIFY(!static_cast<bool>(extracted.readers.sampleSize));

        auto build = Mp4SampleTableIndex::build(extracted.tables, extracted.readers);
        QVERIFY2(build.succeeded(), qUtf8Printable(build.errorMessage));

        streamview::rules::Mp4SamplePageRequest pageRequest;
        pageRequest.pageSize = 4;
        const auto page = build.index->descriptorPage(pageRequest);
        QVERIFY2(page.built(), qUtf8Printable(page.errorMessage));
        QCOMPARE(page.descriptors.size(), std::size_t{4});
        for (const auto& descriptor : page.descriptors) {
            QCOMPARE(descriptor.sourceSpans.size(), std::size_t{1});
            QCOMPARE(descriptor.sourceSpans.front().bitLength(), quint64{512} * 8U);
        }

        // Two chunks of two samples: the second chunk restarts at its own offset
        // rather than continuing the first.
        const quint64 firstChunkStart = page.descriptors[0].sourceSpans.front().start().byteOffset();
        QCOMPARE(page.descriptors[1].sourceSpans.front().start().byteOffset(),
                 firstChunkStart + 512U);
        QCOMPARE(page.descriptors[2].sourceSpans.front().start().byteOffset(),
                 firstChunkStart + 1024U);
    }

    // stz2 packs sizes at 4, 8, or 16 bits per entry. The 4-bit width shares one
    // window entry between two samples, and an odd sample count leaves a padding
    // nibble no sample index reaches. Three tracks in one file also prove
    // extraction is per-track rather than first-track-only.
    void readsCompactSampleSizesAtEveryFieldWidth() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_stz2_tracks.mp4"));

        const auto extraction =
            Mp4SampleTableExtractor::extract(*analyzer_, batch_, defaultRequest());
        QVERIFY2(extraction.extracted(), qUtf8Printable(extraction.errorMessage));
        QCOMPARE(extraction.tracks.size(), std::size_t{3});

        const std::vector<std::vector<quint64>> expectedSizes{
            {3, 5, 7, 9, 11},
            {200, 150, 100, 50},
            {1000, 2000, 3000},
        };
        const std::vector<int> expectedFillBase{0xE1, 0xF1, 0x11};

        for (std::size_t trackOrdinal = 0; trackOrdinal < extraction.tracks.size();
             ++trackOrdinal) {
            const auto& extracted = extraction.tracks[trackOrdinal];
            QCOMPARE(extracted.tables.trackId, static_cast<quint32>(trackOrdinal + 1));
            QVERIFY(static_cast<bool>(extracted.readers.sampleSize));
            QCOMPARE(extracted.tables.defaultSampleSize, quint64{0});

            const auto& sizes = expectedSizes[trackOrdinal];
            QCOMPARE(extracted.tables.declaredSampleCount,
                     static_cast<quint64>(sizes.size()));

            auto build = Mp4SampleTableIndex::build(extracted.tables, extracted.readers);
            QVERIFY2(build.succeeded(), qUtf8Printable(build.errorMessage));

            streamview::rules::Mp4SamplePageRequest pageRequest;
            pageRequest.pageSize = sizes.size();
            const auto page = build.index->descriptorPage(pageRequest);
            QVERIFY2(page.built(), qUtf8Printable(page.errorMessage));
            QCOMPARE(page.descriptors.size(), sizes.size());

            for (std::size_t index = 0; index < sizes.size(); ++index) {
                const auto& descriptor = page.descriptors[index];
                QCOMPARE(descriptor.sourceSpans.size(), std::size_t{1});
                QCOMPARE(descriptor.sourceSpans.front().bitLength(), sizes[index] * 8U);
                const auto actual = spanBytes(*source_, descriptor);
                const auto expectedByte =
                    static_cast<std::byte>((expectedFillBase[trackOrdinal] + static_cast<int>(index)) &
                                           0xFF);
                QCOMPARE(actual.size(), static_cast<std::size_t>(sizes[index]));
                QVERIFY(std::all_of(actual.cbegin(),
                                    actual.cend(),
                                    [expectedByte](std::byte value) {
                                        return value == expectedByte;
                                    }));
            }
        }
    }

    // co64 carries 64-bit offsets, and a page that does not start on a chunk
    // boundary must perform the mid-chunk size catch-up.
    void readsSixtyFourBitChunkOffsetsAcrossPagesThatStartMidChunk() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_co64_multichunk.mp4"));

        const auto extraction =
            Mp4SampleTableExtractor::extract(*analyzer_, batch_, defaultRequest());
        QVERIFY2(extraction.extracted(), qUtf8Printable(extraction.errorMessage));

        const auto& extracted = extraction.tracks.front();
        QCOMPARE(extracted.tables.chunkCount, quint64{3});
        QVERIFY(static_cast<bool>(extracted.readers.chunkOffset));

        auto build = Mp4SampleTableIndex::build(extracted.tables, extracted.readers);
        QVERIFY2(build.succeeded(), qUtf8Printable(build.errorMessage));
        QCOMPARE(build.index->sampleCount(), quint64{6});

        streamview::rules::Mp4SamplePageRequest wholeRequest;
        wholeRequest.pageSize = 6;
        const auto whole = build.index->descriptorPage(wholeRequest);
        QVERIFY2(whole.built(), qUtf8Printable(whole.errorMessage));
        QCOMPARE(whole.descriptors.size(), std::size_t{6});

        // Pages of 3 start mid-chunk because chunks hold 2 samples. The spans
        // must agree with the single-page read.
        streamview::rules::Mp4SamplePageRequest midChunkRequest;
        midChunkRequest.pageSize = 3;
        midChunkRequest.pageIndex = 1;
        const auto midChunk = build.index->descriptorPage(midChunkRequest);
        QVERIFY2(midChunk.built(), qUtf8Printable(midChunk.errorMessage));
        QCOMPARE(midChunk.descriptors.size(), std::size_t{3});
        QCOMPARE(midChunk.firstSampleIndex, quint64{3});
        for (std::size_t index = 0; index < midChunk.descriptors.size(); ++index) {
            const auto& fromPage = midChunk.descriptors[index];
            const auto& fromWhole = whole.descriptors[index + 3];
            QCOMPARE(fromPage.sampleIndex, fromWhole.sampleIndex);
            QCOMPARE(fromPage.sourceSpans.size(), std::size_t{1});
            QCOMPARE(fromPage.sourceSpans.front().start().byteOffset(),
                     fromWhole.sourceSpans.front().start().byteOffset());
            QCOMPARE(fromPage.sourceSpans.front().bitLength(),
                     fromWhole.sourceSpans.front().bitLength());
            QCOMPARE(fromPage.dts, fromWhole.dts);
        }

        // stss lists samples 1 and 5, i.e. 0-based 0 and 4.
        QVERIFY(whole.descriptors[0].isSyncSample);
        QVERIFY(whole.descriptors[4].isSyncSample);
        QVERIFY(!whole.descriptors[3].isSyncSample);
    }

    // Reader work is bounded by page size, not by table length: this is the
    // property that keeps a large track from being materialized to answer one
    // page.
    void boundsReaderCallsByPageSizeRatherThanTableLength() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_complete_track.mp4"));

        const auto extraction =
            Mp4SampleTableExtractor::extract(*analyzer_, batch_, defaultRequest());
        QVERIFY2(extraction.extracted(), qUtf8Printable(extraction.errorMessage));

        auto build = Mp4SampleTableIndex::build(extraction.tracks.front().tables,
                                               extraction.tracks.front().readers);
        QVERIFY2(build.succeeded(), qUtf8Printable(build.errorMessage));

        streamview::rules::Mp4SamplePageRequest smallRequest;
        smallRequest.pageSize = 1;
        const auto small = build.index->descriptorPage(smallRequest);
        QVERIFY2(small.built(), qUtf8Printable(small.errorMessage));
        QCOMPARE(small.descriptors.size(), std::size_t{1});

        streamview::rules::Mp4SamplePageRequest largeRequest;
        largeRequest.pageSize = 6;
        const auto large = build.index->descriptorPage(largeRequest);
        QVERIFY2(large.built(), qUtf8Printable(large.errorMessage));
        QCOMPARE(large.descriptors.size(), std::size_t{6});

        QVERIFY(small.tableReadCount > 0);
        QVERIFY(large.tableReadCount > small.tableReadCount);

        // Repeating a page is idempotent and costs the same, because entries the
        // window decoder already produced are memoized rather than re-decoded.
        const auto repeated = build.index->descriptorPage(smallRequest);
        QVERIFY2(repeated.built(), qUtf8Printable(repeated.errorMessage));
        QCOMPARE(repeated.tableReadCount, small.tableReadCount);
        QCOMPARE(repeated.descriptors.size(), small.descriptors.size());
        QCOMPARE(repeated.descriptors.front().sourceSpans.front().start().byteOffset(),
                 small.descriptors.front().sourceSpans.front().start().byteOffset());
    }

    // A reader that cannot supply an entry fails only the page that needed it.
    // Other pages, other tracks, and the analyzed tree stay usable.
    void containsReaderFailureToTheAffectedPage() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_co64_multichunk.mp4"));

        const auto extraction =
            Mp4SampleTableExtractor::extract(*analyzer_, batch_, defaultRequest());
        QVERIFY2(extraction.extracted(), qUtf8Printable(extraction.errorMessage));

        auto build = Mp4SampleTableIndex::build(extraction.tracks.front().tables,
                                               extraction.tracks.front().readers);
        QVERIFY2(build.succeeded(), qUtf8Printable(build.errorMessage));

        // Read the first page while the source is healthy, so its window entries
        // are memoized and stay answerable afterwards.
        streamview::rules::Mp4SamplePageRequest firstPage;
        firstPage.pageSize = 2;
        firstPage.pageIndex = 0;
        const auto healthy = build.index->descriptorPage(firstPage);
        QVERIFY2(healthy.built(), qUtf8Printable(healthy.errorMessage));
        QCOMPARE(healthy.descriptors.size(), std::size_t{2});

        const auto nodeCountBefore = analyzer_->tree().nodeCount();

        // Fail every read past the moov so the not-yet-decoded stsz entries
        // become unavailable.
        source_->failReadsFrom(1);

        streamview::rules::Mp4SamplePageRequest laterPage;
        laterPage.pageSize = 2;
        laterPage.pageIndex = 2;
        const auto failed = build.index->descriptorPage(laterPage);
        QVERIFY(!failed.built());
        QCOMPARE(failed.status, streamview::rules::Mp4SampleTableIndexStatus::SourceError);
        QVERIFY(failed.descriptors.empty());

        source_->clearFailures();

        // The previously decoded page still answers, and the failure created no
        // stray nodes in the parent tree.
        const auto healthyAgain = build.index->descriptorPage(firstPage);
        QVERIFY2(healthyAgain.built(), qUtf8Printable(healthyAgain.errorMessage));
        QCOMPARE(healthyAgain.descriptors.size(), std::size_t{2});
        QCOMPARE(healthyAgain.descriptors.front().sourceSpans.front().start().byteOffset(),
                 healthy.descriptors.front().sourceSpans.front().start().byteOffset());
        QCOMPARE(analyzer_->tree().nodeCount(), nodeCountBefore);
    }

    // A tree with no moov yields NoTracks rather than an empty success, so a
    // caller cannot mistake "nothing found" for "no samples".
    void reportsNoTracksWhenTreeCarriesNoMovieBox() {
        // A bare ftyp followed by mdat: structurally valid, no moov.
        std::vector<std::byte> data;
        const auto append = [&data](std::initializer_list<quint8> values) {
            for (const auto value : values) {
                data.push_back(static_cast<std::byte>(value));
            }
        };
        append({0, 0, 0, 0x10, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 0, 0});
        append({0, 0, 0, 0x0C, 'm', 'd', 'a', 't', 0xAA, 0xBB, 0xCC, 0xDD});

        MemorySource source(std::move(data));
        QString error;
        auto analyzer = Mp4IsobmffAnalyzer::create(source, &error);
        QVERIFY2(analyzer.has_value(), qUtf8Printable(error));
        const auto batch = analyzer->analyzeBatch();
        QVERIFY2(batch.complete(), qUtf8Printable(batch.errorMessage));

        Mp4SampleTableExtractionRequest request;
        request.sourceSizeBytes = source.sizeBytes();
        const auto extraction = Mp4SampleTableExtractor::extract(*analyzer, batch, request);
        QVERIFY(!extraction.extracted());
        QCOMPARE(extraction.status, Mp4SampleTableExtractionStatus::NoTracks);
        QVERIFY(extraction.tracks.empty());
        QVERIFY(!extraction.errorMessage.isEmpty());
    }

    // A row budget below the table's declared row count is refused as a resource
    // limit instead of silently truncating the tables.
    void refusesRunTablesBeyondTheRowBudget() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_ctts_v1_negative.mp4"));

        auto request = defaultRequest();
        request.maximumRunRowsPerTable = 2;  // ctts declares 3 rows
        const auto extraction = Mp4SampleTableExtractor::extract(*analyzer_, batch_, request);
        QVERIFY(!extraction.extracted());
        QCOMPARE(extraction.status, Mp4SampleTableExtractionStatus::ResourceLimit);
        QVERIFY(extraction.tracks.empty());
    }

    // A cancelled token stops extraction and returns no partial track list.
    void reportsCancellation() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_stz2_tracks.mp4"));

        CancellationSource source;
        QVERIFY(source.requestCancellation());

        auto request = defaultRequest();
        request.cancellation = source.token();
        const auto extraction = Mp4SampleTableExtractor::extract(*analyzer_, batch_, request);
        QVERIFY(!extraction.extracted());
        QCOMPARE(extraction.status, Mp4SampleTableExtractionStatus::Cancelled);
        QVERIFY(extraction.tracks.empty());
    }

    // A single-entry page read is the seam the readers rely on: it must yield
    // the same value whether reached directly or through the index.
    void readsIndividualWindowEntriesIdempotently() {
        analyzeFixture(QStringLiteral("mp4_p5j3b_complete_track.mp4"));

        const auto extraction =
            Mp4SampleTableExtractor::extract(*analyzer_, batch_, defaultRequest());
        QVERIFY2(extraction.extracted(), qUtf8Printable(extraction.errorMessage));

        const auto& readers = extraction.tracks.front().readers;
        const std::vector<quint64> expectedSizes{1000, 200, 150, 300, 120, 90};
        for (std::size_t index = 0; index < expectedSizes.size(); ++index) {
            const auto first = readers.sampleSize(index);
            QVERIFY(first.has_value());
            QCOMPARE(*first, expectedSizes[index]);
            const auto second = readers.sampleSize(index);
            QVERIFY(second.has_value());
            QCOMPARE(*second, *first);
        }

        // Reading past the declared sample count reports absence rather than a
        // fabricated value.
        QVERIFY(!readers.sampleSize(expectedSizes.size()).has_value());
        QVERIFY(!readers.chunkOffset(2).has_value());

        // isSyncSample takes 1-based sample numbers.
        QCOMPARE(readers.isSyncSample(1), std::optional<bool>{true});
        QCOMPARE(readers.isSyncSample(2), std::optional<bool>{false});
        QCOMPARE(readers.isSyncSample(4), std::optional<bool>{true});
        QCOMPARE(readers.isSyncSample(7), std::optional<bool>{false});
    }
};

QTEST_MAIN(Mp4SampleTableExtractorTest)
#include "mp4_sample_table_extractor_test.moc"
