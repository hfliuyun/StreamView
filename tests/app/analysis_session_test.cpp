#include "analysis_session.h"

#include <streamview/core/paged_cache.h>
#include <streamview/core/source.h>
#include <streamview/core/source_pager.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/analysis_cache.h>
#include <streamview/rules/analysis_cache_owner.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/h264_annex_b_detector.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_package.h>

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

using streamview::app::AnalysisBatchResult;
using streamview::app::AnalysisBatchStatus;
using streamview::app::AnalysisSession;
using streamview::app::AnalysisSessionCacheOptions;
using streamview::app::AnalysisSessionCacheStatus;
using streamview::app::AnalysisSessionNavigationResult;
using streamview::app::AnalysisSessionNavigationStatus;
using streamview::app::AnalysisSessionRestoreStatus;
using streamview::app::AnalysisSessionReturnResult;
using streamview::app::AnalysisSessionReturnStatus;
using streamview::app::AnalysisSessionSamplePageRequest;
using streamview::app::AnalysisSessionSampleStatus;
using streamview::app::NavigationFrame;
using streamview::app::RawDisplayMode;
using streamview::app::SessionAnnotation;
using streamview::app::SessionBookmark;
using streamview::app::SessionDocument;
using streamview::app::SessionUserState;
using streamview::core::AnalysisNodeId;
using streamview::core::MaterializationState;
using streamview::core::PagedCachePageKind;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::rules::RulePackageCatalog;

/// `trackIndices_` holds table readers that borrow `analyzer_`
/// (`mp4_sample_table_extractor.h:52-56`), so relocating a session would leave
/// every reader decoding through a moved-from analyzer. Deleting the move and
/// copy operations is what makes that unrepresentable, and these assertions fail
/// at compile time if any of them is ever reintroduced.
static_assert(!std::is_move_constructible_v<AnalysisSession>);
static_assert(!std::is_move_assignable_v<AnalysisSession>);
static_assert(!std::is_copy_constructible_v<AnalysisSession>);
static_assert(!std::is_copy_assignable_v<AnalysisSession>);

namespace {

class DirectSqliteConnection final {
public:
    explicit DirectSqliteConnection(const QString& path)
        : name_(QStringLiteral("streamview-analysis-session-test-%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))),
          database_(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_)) {
        database_.setDatabaseName(path);
        database_.open();
    }

    ~DirectSqliteConnection() {
        database_.close();
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }

    DirectSqliteConnection(const DirectSqliteConnection&) = delete;
    DirectSqliteConnection& operator=(const DirectSqliteConnection&) = delete;

    [[nodiscard]] bool isOpen() const noexcept { return database_.isOpen(); }
    [[nodiscard]] QString errorMessage() const { return database_.lastError().text(); }

    [[nodiscard]] bool execute(const QString& statement, QString* errorMessage = nullptr) {
        QSqlQuery query(database_);
        if (query.exec(statement)) {
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = query.lastError().text();
        }
        return false;
    }

private:
    QString name_;
    QSqlDatabase database_;
};

class MemorySource final : public RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> bytes,
                          QString identity = QStringLiteral("memory-source"))
        : bytes_(std::move(bytes)), identity_(std::move(identity)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return bytes_.size(); }
    [[nodiscard]] QString identity() const override { return identity_; }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (byteOffset >= bytes_.size()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto available = bytes_.size() - static_cast<std::size_t>(byteOffset);
        const auto count = std::min(available, destination.size());
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(byteOffset), count,
                    destination.begin());
        const auto status = count == destination.size() ? SourceReadStatus::Complete
                                                        : SourceReadStatus::EndOfSource;
        return {status, count, {}};
    }

private:
    std::vector<std::byte> bytes_;
    QString identity_;
};

class ToggleErrorMemorySource final : public RandomAccessSource {
public:
    explicit ToggleErrorMemorySource(std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return bytes_.size(); }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("toggle-error.mp4");
    }
    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (failReads_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected navigation read error")};
        }
        if (byteOffset >= bytes_.size()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto available = bytes_.size() - static_cast<std::size_t>(byteOffset);
        const auto count = std::min(available, destination.size());
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(byteOffset), count,
                    destination.begin());
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

    void setFailReads(bool failReads) noexcept { failReads_ = failReads; }

private:
    std::vector<std::byte> bytes_;
    bool failReads_ = false;
};

class OversizedSource final : public RandomAccessSource {
public:
    explicit OversizedSource(bool* destroyed) : destroyed_(destroyed) {}
    ~OversizedSource() override { *destroyed_ = true; }

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return (std::numeric_limits<quint64>::max() / 8U) + 1U;
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("oversized"); }
    [[nodiscard]] SourceReadResult
    readAt(quint64, std::span<std::byte> destination) const override {
        std::fill(destination.begin(), destination.end(), std::byte{0});
        return {SourceReadStatus::Complete, destination.size(), {}};
    }

private:
    bool* destroyed_ = nullptr;
};

class InitialReadFailureSource final : public RandomAccessSource {
public:
    explicit InitialReadFailureSource(bool* destroyed) : destroyed_(destroyed) {}
    ~InitialReadFailureSource() override { *destroyed_ = true; }

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return 4; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("unreadable"); }
    [[nodiscard]] SourceReadResult
    readAt(quint64, std::span<std::byte>) const override {
        return {SourceReadStatus::Error, 0, QStringLiteral("initial page unavailable")};
    }

private:
    bool* destroyed_ = nullptr;
};

std::vector<std::byte> validAnnexB() {
    return {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x65}};
}

QByteArray validAnnexBBytes() { return QByteArray::fromHex("00000165"); }

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

class SingleInitialReadSource final : public RandomAccessSource {
public:
    [[nodiscard]] quint64 sizeBytes() const noexcept override { return 4; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("single-read"); }
    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        ++readCount_;
        if (readCount_ != 1U) {
            return {SourceReadStatus::Error, 0, QStringLiteral("unexpected repeated read")};
        }
        const auto fixture = validAnnexB();
        if (byteOffset != 0U || destination.size() != fixture.size()) {
            return {SourceReadStatus::Error, 0, QStringLiteral("unexpected page request")};
        }
        std::copy(fixture.begin(), fixture.end(), destination.begin());
        return {SourceReadStatus::Complete, fixture.size(), {}};
    }

    [[nodiscard]] std::size_t readCount() const noexcept { return readCount_; }

private:
    mutable std::size_t readCount_ = 0;
};

class HundredGigabyteInitialSource final : public RandomAccessSource {
public:
    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return 100ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("hundred-gigabyte-virtual-source");
    }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        ++readCount_;
        lastOffset_ = byteOffset;
        lastRequestSize_ = destination.size();
        if (byteOffset != 0U || destination.size() !=
                                    streamview::core::SourcePager::pageSizeBytes()) {
            return {SourceReadStatus::Error, 0, QStringLiteral("unexpected initial read")};
        }

        std::fill(destination.begin(), destination.end(), std::byte{0xFF});
        const auto fixture = validAnnexB();
        std::copy(fixture.begin(), fixture.end(), destination.begin());
        return {SourceReadStatus::Complete, destination.size(), {}};
    }

    [[nodiscard]] std::size_t readCount() const noexcept { return readCount_; }
    [[nodiscard]] quint64 lastOffset() const noexcept { return lastOffset_; }
    [[nodiscard]] std::size_t lastRequestSize() const noexcept { return lastRequestSize_; }

private:
    mutable std::size_t readCount_ = 0;
    mutable quint64 lastOffset_ = 0;
    mutable std::size_t lastRequestSize_ = 0;
};

[[nodiscard]] streamview::rules::RulePackageCatalog makeCatalogWithOfficialPackages() {
    streamview::rules::RulePackageCatalog catalog;
    auto aac = streamview::rules::loadAacAdtsRulePackage();
    if (aac.succeeded() && aac.package.has_value()) {
        (void)catalog.registerPackage(std::move(*aac.package));
    }
    auto h264 = streamview::rules::loadH264AnnexBRulePackage();
    if (h264.succeeded() && h264.package.has_value()) {
        (void)catalog.registerPackage(std::move(*h264.package));
    }
    auto mp4 = streamview::rules::loadMp4IsobmffRulePackage();
    if (mp4.succeeded() && mp4.package.has_value()) {
        (void)catalog.registerPackage(std::move(*mp4.package));
    }
    return catalog;
}

/// Opens an MP4 fixture with the container rule pinned instead of detected.
///
/// Format detection cannot be relied on for AVC-in-MP4: a `VisualSampleEntry`
/// ends `... 00 00 | 00 01 | 0a` (reserved, frame_count, compressorname length),
/// and box sizes of the form `00 00 01 xx` add more, so a real file reaches two
/// valid-looking NAL headers and therefore H.264 `Strong`. `createPrepared`
/// resolves that tie in H.264's favour, leaving the session without a container
/// analyzer. Pinning is what a caller that already knows the container does, and
/// it keeps these tests measuring sample navigation rather than detection.
[[nodiscard]] std::unique_ptr<AnalysisSession> openPinnedMp4Fixture(
    const QString& fixturePath, const streamview::rules::RulePackageCatalog& catalog) {
    auto loaded = streamview::rules::loadMp4IsobmffRulePackage();
    if (!loaded.succeeded() || !loaded.package.has_value()) {
        return nullptr;
    }
    auto pinned = streamview::rules::RuleEntryPointIdentity::create(
        loaded.package->identity(), QStringLiteral("main"));
    if (!pinned.has_value()) {
        return nullptr;
    }

    QString errorMessage;
    auto source = streamview::core::FileSource::open(fixturePath, &errorMessage);
    if (source == nullptr) {
        return nullptr;
    }
    auto fingerprint = source->fingerprint();
    if (!fingerprint.succeeded()) {
        return nullptr;
    }
    source.reset();

    auto document = SessionDocument::create(fixturePath, fixturePath,
                                            std::move(*fingerprint.fingerprint),
                                            std::move(*pinned));
    if (!document.has_value()) {
        return nullptr;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return nullptr;
    }
    const QString sessionPath = directory.filePath(QStringLiteral("pinned.svsession"));
    if (!document->save(sessionPath, &errorMessage)) {
        return nullptr;
    }

    auto restored = AnalysisSession::restoreSession(sessionPath, catalog);
    if (restored.status != AnalysisSessionRestoreStatus::Restored) {
        return nullptr;
    }
    return std::move(restored.session);
}

[[nodiscard]] streamview::rules::RulePackageLoadResult
makeTwoCompoundEntrypointPackage() {
    const QByteArray manifest = QByteArrayLiteral(
        "manifest-version = 2\n\n"
        "[package]\n"
        "id = \"org.example.compound\"\n"
        "version = \"0.1.0\"\n"
        "authors = [\"StreamView Tests\"]\n"
        "license = \"Apache-2.0\"\n"
        "dependencies = []\n\n"
        "[compatibility]\n"
        "language = \"0.1\"\n"
        "engine = \">=0.1.0 <0.2.0\"\n\n"
        "[[entrypoints]]\n"
        "id = \"audio\"\n"
        "format = \"audio.aac.asc\"\n"
        "source = \"src/audio.svfmt\"\n"
        "target = \"AudioHeader\"\n"
        "profiles = [\"test\"]\n"
        "depth = \"structural\"\n\n"
        "[[entrypoints]]\n"
        "id = \"video\"\n"
        "format = \"video.h264.nal\"\n"
        "source = \"src/video.svfmt\"\n"
        "target = \"VideoHeader\"\n"
        "profiles = [\"test\"]\n"
        "depth = \"structural\"\n");
    const QByteArray audio = QByteArrayLiteral(
        "struct AudioHeader { bits<8> kind; }\n"
        "struct AudioBody { @lazy(available_bytes()) bytes audio_data; }\n"
        "payload<none> AudioHeader switch (kind) { case 18: AudioBody; }\n"
        "entry AudioHeader;\n");
    const QByteArray video = QByteArrayLiteral(
        "struct VideoHeader { bits<8> kind; }\n"
        "struct VideoBody { @lazy(available_bytes()) bytes video_data; }\n"
        "payload<none> VideoHeader switch (kind) { case 103: VideoBody; }\n"
        "entry VideoHeader;\n");
    return streamview::rules::RulePackage::fromFiles({
        {QStringLiteral("rule.toml"), manifest},
        {QStringLiteral("src/audio.svfmt"), audio},
        {QStringLiteral("src/video.svfmt"), video},
    });
}

[[nodiscard]] streamview::rules::RulePackageLoadResult
makeAudioNavigationPackage(const QByteArray& source) {
    const QByteArray manifest = QByteArrayLiteral(
        "manifest-version = 1\n\n"
        "[package]\n"
        "id = \"org.example.navigation\"\n"
        "version = \"0.1.0\"\n"
        "authors = [\"StreamView Tests\"]\n"
        "license = \"Apache-2.0\"\n"
        "dependencies = []\n\n"
        "[compatibility]\n"
        "language = \"0.1\"\n"
        "engine = \">=0.1.0 <0.2.0\"\n\n"
        "[[entrypoints]]\n"
        "id = \"audio\"\n"
        "format = \"audio.aac.asc\"\n"
        "source = \"src/audio.svfmt\"\n"
        "profiles = [\"test\"]\n"
        "depth = \"structural\"\n");
    return streamview::rules::RulePackage::fromFiles({
        {QStringLiteral("rule.toml"), manifest},
        {QStringLiteral("src/audio.svfmt"), source},
    });
}

[[nodiscard]] std::optional<AnalysisNodeId> findNodeByTargetFormat(
    const streamview::core::AnalysisTree& tree,
    AnalysisNodeId currentId,
    const QString& targetFormat) {
    const auto node = tree.node(currentId);
    if (!node) {
        return std::nullopt;
    }
    if (node->metadata().targetFormat.has_value() && *node->metadata().targetFormat == targetFormat) {
        return currentId;
    }
    for (const auto childId : node->children()) {
        if (auto found = findNodeByTargetFormat(tree, childId, targetFormat)) {
            return found;
        }
    }
    return std::nullopt;
}

void collectNodesByTargetFormat(
    const streamview::core::AnalysisTree& tree,
    AnalysisNodeId currentId,
    const QString& targetFormat,
    std::vector<AnalysisNodeId>& results) {
    const auto node = tree.node(currentId);
    if (!node) {
        return;
    }
    if (node->metadata().targetFormat.has_value() && *node->metadata().targetFormat == targetFormat) {
        results.push_back(currentId);
    }
    for (const auto childId : node->children()) {
        collectNodesByTargetFormat(tree, childId, targetFormat, results);
    }
}

[[nodiscard]] std::vector<AnalysisNodeId> findAllNodesByTargetFormat(
    const streamview::core::AnalysisTree& tree,
    AnalysisNodeId currentId,
    const QString& targetFormat) {
    std::vector<AnalysisNodeId> results;
    collectNodesByTargetFormat(tree, currentId, targetFormat, results);
    return results;
}

[[nodiscard]] std::optional<streamview::core::AnalysisNode> findChildNamed(
    const streamview::core::AnalysisTree& tree,
    AnalysisNodeId parentId,
    const QString& name) {
    const auto parent = tree.node(parentId);
    if (!parent) {
        return std::nullopt;
    }
    for (const auto childId : parent->children()) {
        const auto child = tree.node(childId);
        if (child && child->name() == name) {
            return child;
        }
    }
    return std::nullopt;
}

} // namespace

class AnalysisSessionTest final : public QObject {
    Q_OBJECT

private slots:
    void ownsTheSourceAndRunsTheSharedAnalyzer() {
        QString errorMessage;
        auto session = AnalysisSession::create(
            std::make_unique<MemorySource>(validAnnexB(), QStringLiteral("fixture.264")),
            &errorMessage);

        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QCOMPARE(session->cacheStatus(), AnalysisSessionCacheStatus::Disabled);
        QCOMPARE(session->identity(), QStringLiteral("fixture.264"));
        QCOMPARE(session->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.h264"));
        QCOMPARE(session->ruleIdentity().entryPointId(), QStringLiteral("annex-b"));
        QCOMPARE(session->sizeBytes(), quint64{4});
        QVERIFY(!session->finished());
        QVERIFY(session->formatDetection().candidate.has_value());
        QCOMPARE(session->formatDetection().candidate->confidence,
                 streamview::rules::H264AnnexBDetectionConfidence::Probable);

        while (!session->finished()) {
            const auto batch = session->analyzeBatch(1);
            QVERIFY(batch.status != AnalysisBatchStatus::InvalidBatchSize);
        }
        QCOMPARE(session->scanCursor(), session->sizeBytes());

        const auto root = session->tree().node(session->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
        QVERIFY(session->tree().nodeCount() > std::size_t{1});
    }

    void rejectsAnEmptySourceOwner() {
        QString errorMessage;

        const auto session = AnalysisSession::create(nullptr, &errorMessage);

        QVERIFY(session == nullptr);
        QVERIFY(errorMessage.contains(QStringLiteral("source"), Qt::CaseInsensitive));
    }

    void keepsTheSourceAliveThroughADeferredCoordinateError() {
        bool destroyed = false;
        QString errorMessage;

        {
            auto session = AnalysisSession::create(
                std::make_unique<OversizedSource>(&destroyed), &errorMessage);

            QVERIFY2(session != nullptr, qPrintable(errorMessage));
            const auto batch = session->analyzeBatch();
            QCOMPARE(batch.status, AnalysisBatchStatus::SourceError);
            QVERIFY(batch.errorMessage.contains(QStringLiteral("bit coordinate")));
            QVERIFY(!destroyed);
        }
        QVERIFY(destroyed);
    }

    void rejectsASourceWhenItsInitialRawPageCannotBeRead() {
        bool destroyed = false;
        QString errorMessage;

        const auto session = AnalysisSession::create(
            std::make_unique<InitialReadFailureSource>(&destroyed), &errorMessage);

        QVERIFY(session == nullptr);
        QVERIFY(destroyed);
        QCOMPARE(errorMessage, QStringLiteral("initial page unavailable"));
    }

    void reusesThePreparedRawPageForFormatDetection() {
        QString errorMessage;
        auto source = std::make_unique<SingleInitialReadSource>();
        const auto* sourceObserver = source.get();

        const auto session = AnalysisSession::create(std::move(source), &errorMessage);

        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QCOMPARE(sourceObserver->readCount(), std::size_t{1});
        QVERIFY(session->formatDetection().candidate.has_value());
    }

    void opensAHundredGigabyteVirtualSourceFromOneBoundedInitialPage() {
        QString errorMessage;
        auto source = std::make_unique<HundredGigabyteInitialSource>();
        const auto* sourceObserver = source.get();

        const auto session = AnalysisSession::create(std::move(source), &errorMessage);

        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QCOMPARE(session->sizeBytes(), quint64{100ULL * 1024ULL * 1024ULL * 1024ULL});
        QCOMPARE(session->identity(), QStringLiteral("hundred-gigabyte-virtual-source"));
        QCOMPARE(session->initialPage().bytes.size(),
                 static_cast<std::size_t>(streamview::core::SourcePager::pageSizeBytes()));
        QCOMPARE(sourceObserver->readCount(), std::size_t{1});
        QCOMPARE(sourceObserver->lastOffset(), quint64{0});
        QCOMPARE(sourceObserver->lastRequestSize(), session->initialPage().bytes.size());
        QVERIFY(session->formatDetection().candidate.has_value());
    }

    void keepsUnknownSourceBytesAvailableWithoutACandidate() {
        QString errorMessage;
        auto session = AnalysisSession::create(
            std::make_unique<MemorySource>(
                std::vector<std::byte>{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}}),
            &errorMessage);

        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QVERIFY(!session->formatDetection().candidate.has_value());
        QCOMPARE(session->initialPage().bytes.size(), std::size_t{3});

        while (!session->finished()) {
            (void)session->analyzeBatch();
        }
        const auto root = session->tree().node(session->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
    }

    void savesAndRestoresAnExactlyPinnedFileSession() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("fixture.264"));
        const QString sessionPath = directory.filePath(QStringLiteral("fixture.svsession"));
        QVERIFY(writeFile(mediaPath, validAnnexBBytes()));

        QString errorMessage;
        auto original = AnalysisSession::openFile(mediaPath, &errorMessage);
        QVERIFY2(original != nullptr, qPrintable(errorMessage));
        const auto expectedRule = original->ruleIdentity();
        SessionUserState state;
        state.bookmarks = {SessionBookmark{QStringLiteral("header"), 24}};
        state.annotations = {SessionAnnotation{QStringLiteral("IDR"), 24, 8}};
        state.expandedPaths = {QStringLiteral("root/nal_unit[0]")};
        state.view.rawDisplayMode = RawDisplayMode::Combined;
        state.view.selectedSourceBitOffset = 25;
        state.view.selectedAnalysisPath = QStringLiteral("root/nal_unit[0]/NalUnitHeader");
        QVERIFY2(original->saveSession(sessionPath, state, &errorMessage),
                 qPrintable(errorMessage));
        original.reset();

        auto package = streamview::rules::loadH264AnnexBRulePackage();
        QVERIFY2(package.succeeded(), qPrintable(package.errorMessage));
        streamview::rules::RulePackageCatalog catalog;
        QVERIFY(catalog.registerPackage(std::move(*package.package)).succeeded());

        AnalysisSessionCacheOptions cacheOptions;
        cacheOptions.databasePath =
            directory.filePath(QStringLiteral("restored-analysis-cache.sqlite"));
        auto restored =
            AnalysisSession::restoreSession(sessionPath, catalog, std::move(cacheOptions));

        QVERIFY2(restored.succeeded(), qPrintable(restored.errorMessage));
        QCOMPARE(restored.session->cacheStatus(), AnalysisSessionCacheStatus::Active);
        QCOMPARE(restored.session->ruleIdentity(), expectedRule);
        QVERIFY(restored.session->userState() == state);
        QCOMPARE(restored.ruleStatus,
                 std::optional(streamview::rules::RuleCatalogLookupStatus::Found));
    }

    void rejectsAChangedSourceBeforeApplyingSavedLocations() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("fixture.264"));
        const QString sessionPath = directory.filePath(QStringLiteral("fixture.svsession"));
        QVERIFY(writeFile(mediaPath, validAnnexBBytes()));
        QString errorMessage;
        auto original = AnalysisSession::openFile(mediaPath, &errorMessage);
        QVERIFY2(original != nullptr, qPrintable(errorMessage));
        QVERIFY2(original->saveSession(sessionPath, {}, &errorMessage), qPrintable(errorMessage));
        original.reset();
        QVERIFY(writeFile(mediaPath, QByteArray::fromHex("0000014c")));

        streamview::rules::RulePackageCatalog catalog;
        const auto restored = AnalysisSession::restoreSession(sessionPath, catalog);

        QCOMPARE(restored.status, AnalysisSessionRestoreStatus::SourceFingerprintMismatch);
        QVERIFY(restored.session == nullptr);
        QVERIFY(restored.errorMessage.contains(QStringLiteral("fingerprint"),
                                               Qt::CaseInsensitive));
    }

    void diagnosesMissingAndConflictingExactRuleContent() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("fixture.264"));
        const QString sessionPath = directory.filePath(QStringLiteral("fixture.svsession"));
        QVERIFY(writeFile(mediaPath, validAnnexBBytes()));
        QString errorMessage;
        auto original = AnalysisSession::openFile(mediaPath, &errorMessage);
        QVERIFY2(original != nullptr, qPrintable(errorMessage));
        QVERIFY2(original->saveSession(sessionPath, {}, &errorMessage), qPrintable(errorMessage));
        original.reset();

        streamview::rules::RulePackageCatalog missingCatalog;
        const auto missing = AnalysisSession::restoreSession(sessionPath, missingCatalog);
        QCOMPARE(missing.status, AnalysisSessionRestoreStatus::RuleLookupError);
        QCOMPARE(missing.ruleStatus,
                 std::optional(streamview::rules::RuleCatalogLookupStatus::MissingContent));

        auto bundled = streamview::rules::loadH264AnnexBRulePackage();
        QVERIFY2(bundled.succeeded(), qPrintable(bundled.errorMessage));
        std::vector<streamview::rules::RulePackageFile> changedFiles = bundled.package->files();
        auto source = std::find_if(changedFiles.begin(), changedFiles.end(), [](const auto& file) {
            return file.path == QStringLiteral("src/h264_annex_b.svfmt");
        });
        QVERIFY(source != changedFiles.end());
        source->contents.append('\n');
        auto changed = streamview::rules::RulePackage::fromFiles(std::move(changedFiles));
        QVERIFY2(changed.succeeded(), qPrintable(changed.errorMessage));
        streamview::rules::RulePackageCatalog conflictingCatalog;
        QVERIFY(conflictingCatalog.registerPackage(std::move(*changed.package)).succeeded());

        const auto conflict = AnalysisSession::restoreSession(sessionPath, conflictingCatalog);
        QCOMPARE(conflict.status, AnalysisSessionRestoreStatus::RuleLookupError);
        QCOMPARE(conflict.ruleStatus,
                 std::optional(streamview::rules::RuleCatalogLookupStatus::VersionConflict));
    }

    void diagnosesAnExactlyPinnedRuleThatTheCurrentEngineCannotRun() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("fixture.264"));
        const QString sessionPath = directory.filePath(QStringLiteral("fixture.svsession"));
        QVERIFY(writeFile(mediaPath, validAnnexBBytes()));

        auto loaded = streamview::rules::loadH264AnnexBRulePackage();
        QVERIFY2(loaded.succeeded(), qPrintable(loaded.errorMessage));
        std::vector<streamview::rules::RulePackageFile> files = loaded.package->files();
        auto manifest = std::find_if(files.begin(), files.end(), [](const auto& file) {
            return file.path == QStringLiteral("rule.toml");
        });
        QVERIFY(manifest != files.end());
        QVERIFY(manifest->contents.contains(QByteArrayLiteral(">=0.1.0 <0.2.0")));
        manifest->contents.replace(QByteArrayLiteral(">=0.1.0 <0.2.0"),
                                   QByteArrayLiteral(">=0.2.0 <0.3.0"));
        auto incompatible = streamview::rules::RulePackage::fromFiles(std::move(files));
        QVERIFY2(incompatible.succeeded(), qPrintable(incompatible.errorMessage));
        auto pin = streamview::rules::RuleEntryPointIdentity::create(
            incompatible.package->identity(), QStringLiteral("annex-b"));
        QVERIFY(pin.has_value());

        QString errorMessage;
        auto source = streamview::core::FileSource::open(mediaPath, &errorMessage);
        QVERIFY2(source != nullptr, qPrintable(errorMessage));
        auto fingerprint = source->fingerprint();
        QVERIFY2(fingerprint.succeeded(), qPrintable(fingerprint.errorMessage));
        auto document = streamview::app::SessionDocument::create(
            mediaPath, mediaPath, std::move(*fingerprint.fingerprint), std::move(*pin));
        QVERIFY(document.has_value());
        QVERIFY2(document->save(sessionPath, &errorMessage), qPrintable(errorMessage));
        source.reset();

        streamview::rules::RulePackageCatalog catalog;
        QVERIFY(catalog.registerPackage(std::move(*incompatible.package)).succeeded());
        const auto restored = AnalysisSession::restoreSession(sessionPath, catalog);

        QCOMPARE(restored.status, AnalysisSessionRestoreStatus::RuleLookupError);
        QCOMPARE(restored.ruleStatus,
                 std::optional(streamview::rules::RuleCatalogLookupStatus::IncompatibleEngine));
    }

    void refusesToPersistAPathLikeVirtualSourceIdentity() {
        QString errorMessage;
        const auto session = AnalysisSession::create(
            std::make_unique<MemorySource>(validAnnexB(), QStringLiteral("looks-like-a-path.264")),
            &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));

        QVERIFY(!session->saveSession(QStringLiteral("unused.svsession"), {}, &errorMessage));
        QVERIFY(errorMessage.contains(QStringLiteral("local file"), Qt::CaseInsensitive));
    }

    void writesStableProgressiveAndMaterializedPagesForLocalFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("fixture.264"));
        const QString cachePath = directory.filePath(QStringLiteral("analysis-cache.sqlite"));
        QVERIFY(writeFile(mediaPath, validAnnexBBytes()));

        AnalysisSessionCacheOptions cacheOptions;
        cacheOptions.databasePath = cachePath;
        QString errorMessage;
        auto session = AnalysisSession::openFile(mediaPath, cacheOptions, &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QCOMPARE(session->cacheStatus(), AnalysisSessionCacheStatus::Active);
        const auto ruleIdentity = session->ruleIdentity();
        const std::size_t expectedNodeCountBeforeDestruction = [&] {
            while (!session->finished()) {
                (void)session->analyzeBatch();
            }
            return session->tree().nodeCount();
        }();
        session.reset();

        auto source = streamview::core::FileSource::open(mediaPath, &errorMessage);
        QVERIFY2(source != nullptr, qPrintable(errorMessage));
        auto fingerprint = source->fingerprint();
        QVERIFY2(fingerprint.succeeded(), qPrintable(fingerprint.errorMessage));
        auto cacheNamespace = streamview::rules::AnalysisCacheNamespace::create(
            *fingerprint.fingerprint, ruleIdentity, {}, &errorMessage);
        QVERIFY2(cacheNamespace.has_value(), qPrintable(errorMessage));
        auto owner = streamview::rules::AnalysisCacheOwner::start(
            cachePath, std::move(*cacheNamespace));
        QVERIFY2(owner.succeeded(), qPrintable(owner.errorMessage));

        auto progressive = owner.owner->readProgressiveIndex(
            {PagedCachePageKind::ProgressiveIndex, 0, 0});
        QVERIFY(progressive.accepted());
        QVERIFY(progressive.completion.wait_for(std::chrono::seconds(10)) ==
                std::future_status::ready);
        const auto progressiveResult = progressive.completion.get();
        QVERIFY2(progressiveResult.found(), qPrintable(progressiveResult.errorMessage));
        QCOMPARE(progressiveResult.page->firstRecordIndex, quint64{0});
        QCOMPARE(progressiveResult.page->indexedThroughByteOffset, quint64{4});
        QCOMPARE(progressiveResult.page->records.size(), std::size_t{1});
        QVERIFY(progressiveResult.page->endOfSource);

        auto materialized = owner.owner->readMaterializedResult(
            {PagedCachePageKind::MaterializedResult, 0, 0});
        QVERIFY(materialized.accepted());
        QVERIFY(materialized.completion.wait_for(std::chrono::seconds(10)) ==
                std::future_status::ready);
        const auto materializedResult = materialized.completion.get();
        QVERIFY2(materializedResult.found(), qPrintable(materializedResult.errorMessage));
        QCOMPARE(materializedResult.page->nodes.size(), expectedNodeCountBeforeDestruction);
        QCOMPARE(materializedResult.page->nodes.front().id,
                 streamview::core::AnalysisNodeId(1));
    }

    void cacheSetupAndQueueFailuresDoNotInvalidateTheLiveSession() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("fixture.264"));
        QVERIFY(writeFile(mediaPath, validAnnexBBytes()));

        AnalysisSessionCacheOptions openFailureOptions;
        openFailureOptions.databasePath = directory.path();
        QString errorMessage;
        auto openFailure =
            AnalysisSession::openFile(mediaPath, openFailureOptions, &errorMessage);
        QVERIFY2(openFailure != nullptr, qPrintable(errorMessage));
        QCOMPARE(openFailure->cacheStatus(), AnalysisSessionCacheStatus::Failed);
        QVERIFY(!openFailure->cacheErrorMessage().isEmpty());
        const auto openFailureBatch = openFailure->analyzeBatch();
        QCOMPARE(openFailureBatch.status, AnalysisBatchStatus::Complete);

        AnalysisSessionCacheOptions queueFailureOptions;
        queueFailureOptions.databasePath =
            directory.filePath(QStringLiteral("small-queue-cache.sqlite"));
        queueFailureOptions.ownerOptions.maximumRetainedWriteBytes = 1;
        auto queueFailure =
            AnalysisSession::openFile(mediaPath, queueFailureOptions, &errorMessage);
        QVERIFY2(queueFailure != nullptr, qPrintable(errorMessage));
        QCOMPARE(queueFailure->cacheStatus(), AnalysisSessionCacheStatus::Active);
        const auto queueFailureBatch = queueFailure->analyzeBatch();
        QCOMPARE(queueFailureBatch.status, AnalysisBatchStatus::Complete);
        QCOMPARE(queueFailure->cacheStatus(), AnalysisSessionCacheStatus::Failed);
        QVERIFY(queueFailure->tree().nodeCount() > 1U);
    }

    void enablesTheCandidateCacheAfterThePreviousPathOwnerIsReleased() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstPath = directory.filePath(QStringLiteral("first.264"));
        const QString secondPath = directory.filePath(QStringLiteral("second.264"));
        const QString cachePath = directory.filePath(QStringLiteral("analysis-cache.sqlite"));
        QVERIFY(writeFile(firstPath, validAnnexBBytes()));
        QVERIFY(writeFile(secondPath, QByteArray::fromHex("0000014c")));

        AnalysisSessionCacheOptions cacheOptions;
        cacheOptions.databasePath = cachePath;
        QString errorMessage;
        auto first = AnalysisSession::openFile(firstPath, cacheOptions, &errorMessage);
        QVERIFY2(first != nullptr, qPrintable(errorMessage));
        QCOMPARE(first->cacheStatus(), AnalysisSessionCacheStatus::Active);

        auto candidate = AnalysisSession::openFile(secondPath, &errorMessage);
        QVERIFY2(candidate != nullptr, qPrintable(errorMessage));
        candidate->enableCache(cacheOptions);
        QCOMPARE(candidate->cacheStatus(), AnalysisSessionCacheStatus::Failed);

        first.reset();
        candidate->enableCache(cacheOptions);
        QCOMPARE(candidate->cacheStatus(), AnalysisSessionCacheStatus::Active);
    }

    void pollsAcceptedStorageFailuresWithoutInvalidatingAnalysis() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("fixture.264"));
        const QString cachePath = directory.filePath(QStringLiteral("analysis-cache.sqlite"));
        QVERIFY(writeFile(mediaPath, validAnnexBBytes()));

        AnalysisSessionCacheOptions cacheOptions;
        cacheOptions.databasePath = cachePath;
        QString errorMessage;
        auto session = AnalysisSession::openFile(mediaPath, cacheOptions, &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QCOMPARE(session->cacheStatus(), AnalysisSessionCacheStatus::Active);

        DirectSqliteConnection injector(cachePath);
        QVERIFY2(injector.isOpen(), qPrintable(injector.errorMessage()));
        QVERIFY2(injector.execute(
                     QStringLiteral(
                         "CREATE TRIGGER fail_session_cache BEFORE INSERT ON cache_pages "
                         "BEGIN SELECT RAISE(ABORT, 'forced session cache failure'); END"),
                     &errorMessage),
                 qPrintable(errorMessage));

        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);
        QVERIFY(session->tree().nodeCount() > 1U);
        QTRY_VERIFY_WITH_TIMEOUT(([&session] {
                                     session->pollCacheWrites();
                                     return !session->cacheWritesPending();
                                 }()),
                                 10000);
        QCOMPARE(session->cacheStatus(), AnalysisSessionCacheStatus::Failed);
        QVERIFY(!session->cacheErrorMessage().isEmpty());

        QVERIFY2(injector.execute(QStringLiteral("DROP TRIGGER fail_session_cache"),
                                  &errorMessage),
                 qPrintable(errorMessage));
        session->enableCache(cacheOptions);
        QCOMPARE(session->cacheStatus(), AnalysisSessionCacheStatus::Failed);
        QVERIFY(session->cacheErrorMessage().contains(QStringLiteral("after analysis")));
        QVERIFY(!session->cacheWritesPending());
    }

    void unknownBinarySourceWithAccidentalFfF1DefaultsToH264WithoutCandidate() {
        std::vector<std::byte> stream(2048, std::byte{0x22});
        stream[500] = std::byte{0xFF};
        stream[501] = std::byte{0xF1};
        stream[502] = std::byte{0x50};
        stream[503] = std::byte{0x80};
        stream[504] = std::byte{0x10};
        stream[505] = std::byte{0x1F};
        stream[506] = std::byte{0xFC};

        QString errorMessage;
        auto session = AnalysisSession::create(
            std::make_unique<MemorySource>(std::move(stream)), &errorMessage);

        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QVERIFY(!session->formatDetection().candidate.has_value());
        QVERIFY(session->aacFormatDetection().candidate.has_value());
        QCOMPARE(session->aacFormatDetection().candidate->confidence,
                 streamview::rules::AacAdtsDetectionConfidence::Weak);
        QCOMPARE(session->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.h264"));

        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);
        const auto root = session->tree().node(session->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
    }

    void malformedH264WithAccidentalSyncwordInPayloadDefaultsToH264() {
        std::vector<std::byte> stream(512, std::byte{0x33});
        stream[0] = std::byte{0x00};
        stream[1] = std::byte{0x00};
        stream[2] = std::byte{0x00};
        stream[3] = std::byte{0x01};
        stream[4] = std::byte{0x80}; // invalid forbidden_zero_bit=1
        stream[200] = std::byte{0xFF};
        stream[201] = std::byte{0xF1};
        stream[202] = std::byte{0x50};
        stream[203] = std::byte{0x80};
        stream[204] = std::byte{0x10};
        stream[205] = std::byte{0x1F};
        stream[206] = std::byte{0xFC};

        QString errorMessage;
        auto session = AnalysisSession::create(
            std::make_unique<MemorySource>(std::move(stream)), &errorMessage);

        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QVERIFY(session->formatDetection().candidate.has_value());
        QCOMPARE(session->formatDetection().candidate->confidence,
                 streamview::rules::H264AnnexBDetectionConfidence::Weak);
        QVERIFY(session->aacFormatDetection().candidate.has_value());
        QCOMPARE(session->aacFormatDetection().candidate->confidence,
                 streamview::rules::AacAdtsDetectionConfidence::Weak);
        QCOMPARE(session->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.h264"));

        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);
        QCOMPARE(batch.topLevelNodes.size(), std::size_t(1));
    }

    void restoresAndAnalyzesAacAdtsSessionWithResolvedRule() {
        auto makeFrame = [](quint16 frameLength) {
            std::vector<std::byte> frame(frameLength, std::byte{0x55});
            frame[0] = std::byte{0xFF};
            frame[1] = std::byte{0xF1};
            frame[2] = std::byte{0x50};
            frame[3] = std::byte{static_cast<quint8>(0x80U | ((frameLength >> 11U) & 0x03U))};
            frame[4] = std::byte{static_cast<quint8>((frameLength >> 3U) & 0xFFU)};
            frame[5] = std::byte{static_cast<quint8>(((frameLength & 0x07U) << 5U) | 0x1FU)};
            frame[6] = std::byte{0xFC};
            return frame;
        };

        auto makeAacPkg = []() {
            const QByteArray toml = QByteArrayLiteral(
                "manifest-version = 1\n\n"
                "[package]\n"
                "id = \"org.streamview.aac\"\n"
                "version = \"0.1.0\"\n"
                "authors = [\"StreamView Contributors\"]\n"
                "license = \"Apache-2.0\"\n"
                "dependencies = []\n\n"
                "[compatibility]\n"
                "language = \"0.1\"\n"
                "engine = \">=0.1.0 <0.2.0\"\n\n"
                "[[entrypoints]]\n"
                "id = \"adts\"\n"
                "format = \"audio.aac.adts\"\n"
                "source = \"src/adts.svfmt\"\n"
                "profiles = [\"aac-adts\"]\n"
                "depth = \"structural\"\n");

            const QByteArray svfmt = QByteArrayLiteral(
                "struct AdtsHeader {\n"
                "    bits<12> syncword @equals(4095);\n"
                "    bits<1> id;\n"
                "    bits<2> layer;\n"
                "    bits<1> protection_absent;\n"
                "}\n\n"
                "@index(progressive) sequence<AdtsHeader> frames = scan(adts_frame);\n"
                "entry frames;\n");

            std::vector<streamview::rules::RulePackageFile> files{
                {QStringLiteral("rule.toml"), toml},
                {QStringLiteral("src/adts.svfmt"), svfmt}};
            return streamview::rules::RulePackage::fromFiles(std::move(files));
        };

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("fixture.aac"));
        const QString sessionPath = directory.filePath(QStringLiteral("fixture.svsession"));

        std::vector<std::byte> aacBytes;
        const auto f1 = makeFrame(150);
        const auto f2 = makeFrame(200);
        const auto f3 = makeFrame(180);
        aacBytes.insert(aacBytes.end(), f1.begin(), f1.end());
        aacBytes.insert(aacBytes.end(), f2.begin(), f2.end());
        aacBytes.insert(aacBytes.end(), f3.begin(), f3.end());
        QVERIFY(writeFile(
            mediaPath,
            QByteArray(reinterpret_cast<const char*>(aacBytes.data()),
                       static_cast<qsizetype>(aacBytes.size()))));

        auto loadedPkg = makeAacPkg();
        QVERIFY2(loadedPkg.succeeded(), qPrintable(loadedPkg.errorMessage));
        auto aacPin = streamview::rules::RuleEntryPointIdentity::create(
            loadedPkg.package->identity(), QStringLiteral("adts"));
        QVERIFY(aacPin.has_value());

        QString errorMessage;
        auto source = streamview::core::FileSource::open(mediaPath, &errorMessage);
        QVERIFY2(source != nullptr, qPrintable(errorMessage));
        auto fingerprint = source->fingerprint();
        QVERIFY2(fingerprint.succeeded(), qPrintable(fingerprint.errorMessage));
        auto document = SessionDocument::create(
            mediaPath, mediaPath, std::move(*fingerprint.fingerprint), std::move(*aacPin));
        QVERIFY(document.has_value());
        QVERIFY2(document->save(sessionPath, &errorMessage), qPrintable(errorMessage));
        source.reset();

        streamview::rules::RulePackageCatalog catalog;
        QVERIFY(catalog.registerPackage(std::move(*loadedPkg.package)).succeeded());

        const auto restored = AnalysisSession::restoreSession(sessionPath, catalog);
        QCOMPARE(restored.status, AnalysisSessionRestoreStatus::Restored);
        QVERIFY(restored.session != nullptr);
        QCOMPARE(restored.session->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.aac"));
        QCOMPARE(restored.session->ruleIdentity().entryPointId(), QStringLiteral("adts"));

        const auto batch = restored.session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);
        QCOMPARE(batch.topLevelNodes.size(), std::size_t(3));
        QVERIFY(restored.session->finished());
    }

    void opensRealAdtsWithBundledPackage() {
        auto makeFrame = [](quint16 frameLength) {
            std::vector<std::byte> frame(frameLength, std::byte{0x55});
            frame[0] = std::byte{0xFF};
            frame[1] = std::byte{0xF1};
            frame[2] = std::byte{0x50};
            frame[3] = std::byte{static_cast<quint8>(0x80U | ((frameLength >> 11U) & 0x03U))};
            frame[4] = std::byte{static_cast<quint8>((frameLength >> 3U) & 0xFFU)};
            frame[5] = std::byte{static_cast<quint8>(((frameLength & 0x07U) << 5U) | 0x1FU)};
            frame[6] = std::byte{0xFC};
            return frame;
        };

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("real_3frame.aac"));

        std::vector<std::byte> aacBytes;
        const auto f1 = makeFrame(150);
        const auto f2 = makeFrame(200);
        const auto f3 = makeFrame(180);
        aacBytes.insert(aacBytes.end(), f1.begin(), f1.end());
        aacBytes.insert(aacBytes.end(), f2.begin(), f2.end());
        aacBytes.insert(aacBytes.end(), f3.begin(), f3.end());
        QVERIFY(writeFile(
            mediaPath,
            QByteArray(reinterpret_cast<const char*>(aacBytes.data()),
                       static_cast<qsizetype>(aacBytes.size()))));

        QString errorMessage;
        auto session = AnalysisSession::openFile(mediaPath, &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));

        // Format detection identifies AAC Strong candidate
        QVERIFY(session->aacFormatDetection().candidate.has_value());
        QCOMPARE(session->aacFormatDetection().candidate->confidence,
                 streamview::rules::AacAdtsDetectionConfidence::Strong);
        // H.264 candidate is empty
        QVERIFY(!session->formatDetection().candidate.has_value());

        // With bundled AAC rules activated, session binds to org.streamview.aac / adts
        QCOMPARE(session->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.aac"));
        QCOMPARE(session->ruleIdentity().entryPointId(), QStringLiteral("adts"));

        // Analysis execution runs on AAC analyzer to completion
        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);
        QCOMPARE(batch.topLevelNodes.size(), std::size_t(3));
        QVERIFY(session->finished());
    }

    void opensH264WhenBothAacAndH264CandidatesAreStrong() {
        // 1. Generate 3 valid ADTS frames (length-chain step)
        auto makeFrame = [](quint16 frameLength) {
            std::vector<std::byte> frame(frameLength, std::byte{0x55});
            frame[0] = std::byte{0xFF};
            frame[1] = std::byte{0xF1};
            frame[2] = std::byte{0x50};
            frame[3] = std::byte{static_cast<quint8>(0x80U | ((frameLength >> 11U) & 0x03U))};
            frame[4] = std::byte{static_cast<quint8>((frameLength >> 3U) & 0xFFU)};
            frame[5] = std::byte{static_cast<quint8>(((frameLength & 0x07U) << 5U) | 0x1FU)};
            frame[6] = std::byte{0xFC};
            return frame;
        };

        const auto f1 = makeFrame(150);
        const auto f2 = makeFrame(200);
        const auto f3 = makeFrame(180);

        // 2. Generate 3 valid H.264 NAL units (AUD + SPS + PPS)
        const QByteArray h264Bytes = QByteArray::fromHex("00000109100000016742001eda01402000000168ce3c80");

        // Combine: place H.264 NAL units and ADTS frames within the 64 KiB initial page
        QByteArray mixedBytes = h264Bytes;
        mixedBytes.append(reinterpret_cast<const char*>(f1.data()), static_cast<qsizetype>(f1.size()));
        mixedBytes.append(reinterpret_cast<const char*>(f2.data()), static_cast<qsizetype>(f2.size()));
        mixedBytes.append(reinterpret_cast<const char*>(f3.data()), static_cast<qsizetype>(f3.size()));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("contention_h264_aac.bin"));
        QVERIFY(writeFile(mediaPath, mixedBytes));

        QString errorMessage;
        auto session = AnalysisSession::openFile(mediaPath, &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));

        // Both candidates are Strong
        QVERIFY(session->formatDetection().candidate.has_value());
        QCOMPARE(session->formatDetection().candidate->confidence,
                 streamview::rules::H264AnnexBDetectionConfidence::Strong);
        QVERIFY(session->aacFormatDetection().candidate.has_value());
        QCOMPARE(session->aacFormatDetection().candidate->confidence,
                 streamview::rules::AacAdtsDetectionConfidence::Strong);

        // Because chooseAac requires (aacConf == Strong && h264Conf != Strong),
        // contention defaults cleanly to H.264
        QCOMPARE(session->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.h264"));
        QCOMPARE(session->ruleIdentity().entryPointId(), QStringLiteral("annex-b"));
    }

    void opensAdtsWithProbableConfidenceByFallingBackToH264() {
        auto makeFrame = [](quint16 frameLength) {
            std::vector<std::byte> frame(frameLength, std::byte{0x55});
            frame[0] = std::byte{0xFF};
            frame[1] = std::byte{0xF1};
            frame[2] = std::byte{0x50};
            frame[3] = std::byte{static_cast<quint8>(0x80U | ((frameLength >> 11U) & 0x03U))};
            frame[4] = std::byte{static_cast<quint8>((frameLength >> 3U) & 0xFFU)};
            frame[5] = std::byte{static_cast<quint8>(((frameLength & 0x07U) << 5U) | 0x1FU)};
            frame[6] = std::byte{0xFC};
            return frame;
        };

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("probable_2frame.aac"));

        std::vector<std::byte> aacBytes;
        const auto f1 = makeFrame(150);
        const auto f2 = makeFrame(200);
        aacBytes.insert(aacBytes.end(), f1.begin(), f1.end());
        aacBytes.insert(aacBytes.end(), f2.begin(), f2.end());
        QVERIFY(writeFile(
            mediaPath,
            QByteArray(reinterpret_cast<const char*>(aacBytes.data()),
                       static_cast<qsizetype>(aacBytes.size()))));

        QString errorMessage;
        auto session = AnalysisSession::openFile(mediaPath, &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));

        // Format detection identifies AAC Probable candidate
        QVERIFY(session->aacFormatDetection().candidate.has_value());
        QCOMPARE(session->aacFormatDetection().candidate->confidence,
                 streamview::rules::AacAdtsDetectionConfidence::Probable);

        // Session uses H.264 identity
        QCOMPARE(session->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.h264"));
        QCOMPARE(session->ruleIdentity().entryPointId(), QStringLiteral("annex-b"));

        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);
    }

    void activatesMp4AnalysisSessionWithBundledRulePackage() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mediaPath = directory.filePath(QStringLiteral("sample.mp4"));

        // 3 boxes: ftyp (16 bytes), free (8 bytes), mdat (24 bytes) -> Strong candidate
        std::vector<std::byte> mp4Bytes(48);
        // ftyp
        mp4Bytes[3] = std::byte{16};
        mp4Bytes[4] = std::byte{0x66}; mp4Bytes[5] = std::byte{0x74}; mp4Bytes[6] = std::byte{0x79}; mp4Bytes[7] = std::byte{0x70};
        // free
        mp4Bytes[19] = std::byte{8};
        mp4Bytes[20] = std::byte{0x66}; mp4Bytes[21] = std::byte{0x72}; mp4Bytes[22] = std::byte{0x65}; mp4Bytes[23] = std::byte{0x65};
        // mdat
        mp4Bytes[27] = std::byte{24};
        mp4Bytes[28] = std::byte{0x6D}; mp4Bytes[29] = std::byte{0x64}; mp4Bytes[30] = std::byte{0x61}; mp4Bytes[31] = std::byte{0x74};

        QVERIFY(writeFile(
            mediaPath,
            QByteArray(reinterpret_cast<const char*>(mp4Bytes.data()),
                       static_cast<qsizetype>(mp4Bytes.size()))));

        QString errorMessage;
        auto session = AnalysisSession::openFile(mediaPath, &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));

        QVERIFY(session->mp4FormatDetection().candidate.has_value());
        QCOMPARE(session->mp4FormatDetection().candidate->confidence,
                 streamview::rules::Mp4DetectionConfidence::Strong);

        QCOMPARE(session->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.mp4"));
        QCOMPARE(session->ruleIdentity().entryPointId(), QStringLiteral("main"));

        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);
    }

    void navigatesIntoAacAscSubFormatAndReturnsToParent() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        QString errorMessage;
        auto session = AnalysisSession::openFile(fixturePath, &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));

        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);

        const auto catalog = makeCatalogWithOfficialPackages();
        const auto ascNodeOpt = findNodeByTargetFormat(session->tree(), session->tree().rootId(), QStringLiteral("audio.aac.asc"));
        QVERIFY(ascNodeOpt.has_value());
        const auto ascNodeId = *ascNodeOpt;

        const auto navResult = session->enterChildFormat(ascNodeId, catalog);
        QVERIFY2(navResult.succeeded(), qPrintable(navResult.errorMessage));
        QCOMPARE(navResult.status, AnalysisSessionNavigationStatus::Entered);
        QCOMPARE(session->navigationDepth(), std::size_t{1});
        QVERIFY(session->canReturnToParent());

        const auto* frame = session->currentNavigationFrame();
        QVERIFY(frame != nullptr);
        QCOMPARE(frame->parentTargetNodeId, ascNodeId);
        QCOMPARE(frame->targetFormat, QStringLiteral("audio.aac.asc"));

        const auto& childTree = session->activeTree();
        const auto childRootNode = childTree.node(childTree.rootId());
        QVERIFY(childRootNode.has_value());
        QVERIFY(!childRootNode->children().empty());

        const auto structNode = childTree.node(childRootNode->children().front());
        QVERIFY(structNode.has_value());
        QCOMPARE(structNode->name(), QStringLiteral("AudioSpecificConfig"));
        QCOMPARE(structNode->children().size(), std::size_t{6});

        // Verify fields and coordinates: root physical offset 146 (bit offset 1168)
        const auto aotNode = childTree.node(structNode->children()[0]);
        QVERIFY(aotNode.has_value());
        QCOMPARE(aotNode->name(), QStringLiteral("audio_object_type"));
        QCOMPARE(aotNode->value(), quint64{2});
        QVERIFY(aotNode->location().has_value());
        QVERIFY(!aotNode->location()->sourceSpans().empty());
        QCOMPARE(aotNode->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1168});
        QCOMPARE(aotNode->location()->sourceSpans().front().bitLength(), quint64{5});

        const auto sfiNode = childTree.node(structNode->children()[1]);
        QVERIFY(sfiNode.has_value());
        QCOMPARE(sfiNode->name(), QStringLiteral("sampling_frequency_index"));
        QCOMPARE(sfiNode->value(), quint64{4});
        QVERIFY(sfiNode->location().has_value());
        QCOMPARE(sfiNode->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1173});
        QCOMPARE(sfiNode->location()->sourceSpans().front().bitLength(), quint64{4});

        const auto retResult = session->returnToParent();
        QCOMPARE(retResult.status, AnalysisSessionReturnStatus::Returned);
        QVERIFY(retResult.restoredParentTargetNodeId.has_value());
        QCOMPARE(*retResult.restoredParentTargetNodeId, ascNodeId);
        QCOMPARE(session->navigationDepth(), std::size_t{0});
        QVERIFY(!session->canReturnToParent());
        QCOMPARE(&session->activeTree(), &session->tree());
    }

    void navigatesIntoH264SpsAndPpsWithContextSharing() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_avc1_avcC.mp4");
        QString errorMessage;
        auto session = AnalysisSession::openFile(fixturePath, &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));

        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);

        const auto catalog = makeCatalogWithOfficialPackages();
        const auto nalNodes = findAllNodesByTargetFormat(session->tree(), session->tree().rootId(), QStringLiteral("video.h264.nal"));
        QCOMPARE(nalNodes.size(), std::size_t{3});
        const auto spsNodeId = nalNodes[0];
        const auto ppsNodeId = nalNodes[1];

        // 1. Enter SPS NAL
        const auto spsNav = session->enterChildFormat(spsNodeId, catalog);
        QVERIFY2(spsNav.succeeded(), qPrintable(spsNav.errorMessage));
        QCOMPARE(spsNav.status, AnalysisSessionNavigationStatus::Entered);
        QCOMPARE(session->navigationDepth(), std::size_t{1});
        QVERIFY(session->canReturnToParent());

        const auto& spsTree = session->activeTree();
        QVERIFY(spsNav.childRootStructureNodeId.has_value());
        const auto spsHeaderNode = spsTree.node(*spsNav.childRootStructureNodeId);
        QVERIFY(spsHeaderNode.has_value());
        QCOMPARE(spsHeaderNode->name(), QStringLiteral("NalUnitHeader"));

        // Has SPS RBSP child in the same tree
        QVERIFY(!spsHeaderNode->children().empty());
        const auto spsRbspNode = spsTree.node(spsHeaderNode->children().back());
        QVERIFY(spsRbspNode.has_value());
        QCOMPARE(spsRbspNode->name(), QStringLiteral("SequenceParameterSetRbsp"));

        // 2. Return to MP4 root
        const auto spsRet = session->returnToParent();
        QCOMPARE(spsRet.status, AnalysisSessionReturnStatus::Returned);
        QVERIFY(spsRet.restoredParentTargetNodeId.has_value());
        QCOMPARE(*spsRet.restoredParentTargetNodeId, spsNodeId);
        QCOMPARE(session->navigationDepth(), std::size_t{0});
        QCOMPARE(&session->activeTree(), &session->tree());

        // 3. Enter PPS NAL (shares session context from SPS)
        const auto ppsNav = session->enterChildFormat(ppsNodeId, catalog);
        QVERIFY2(ppsNav.succeeded(), qPrintable(ppsNav.errorMessage));
        QCOMPARE(ppsNav.status, AnalysisSessionNavigationStatus::Entered);
        QCOMPARE(session->navigationDepth(), std::size_t{1});

        const auto& ppsTree = session->activeTree();
        QVERIFY(ppsNav.childRootStructureNodeId.has_value());
        const auto ppsHeaderNode = ppsTree.node(*ppsNav.childRootStructureNodeId);
        QVERIFY(ppsHeaderNode.has_value());
        QCOMPARE(ppsHeaderNode->name(), QStringLiteral("NalUnitHeader"));
        QVERIFY(!ppsHeaderNode->children().empty());
        const auto ppsRbspNode = ppsTree.node(ppsHeaderNode->children().back());
        QVERIFY(ppsRbspNode.has_value());
        QCOMPARE(ppsRbspNode->name(), QStringLiteral("PictureParameterSetRbsp"));

        // 4. Return to parent
        const auto ppsRet = session->returnToParent();
        QCOMPARE(ppsRet.status, AnalysisSessionReturnStatus::Returned);
        QCOMPARE(session->navigationDepth(), std::size_t{0});
        QCOMPARE(&session->activeTree(), &session->tree());
    }

    void standalonePpsWithoutPriorSpsFailsWithDependencyUnavailableAndPreservesState() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_avc1_avcC.mp4");
        QString errorMessage;
        auto session = AnalysisSession::openFile(fixturePath, &errorMessage);
        QVERIFY2(session != nullptr, qPrintable(errorMessage));

        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);

        const auto catalog = makeCatalogWithOfficialPackages();
        const auto nalNodes = findAllNodesByTargetFormat(session->tree(), session->tree().rootId(), QStringLiteral("video.h264.nal"));
        QCOMPARE(nalNodes.size(), std::size_t{3});
        const auto ppsNodeId = nalNodes[1];

        // Directly enter PPS without SPS execution
        const auto ppsNav = session->enterChildFormat(ppsNodeId, catalog);
        QCOMPARE(ppsNav.status, AnalysisSessionNavigationStatus::DependencyUnavailable);
        QVERIFY(!ppsNav.succeeded());
        QCOMPARE(session->navigationDepth(), std::size_t{0});
        QVERIFY(!session->canReturnToParent());
        QCOMPARE(&session->activeTree(), &session->tree());

        const auto spsNav = session->enterChildFormat(nalNodes[0], catalog);
        QVERIFY2(spsNav.succeeded(), qPrintable(spsNav.errorMessage));
        const auto subRoot = session->activeTree().node(session->activeTree().rootId());
        QVERIFY(subRoot.has_value());
        QCOMPARE(subRoot->children().size(), std::size_t(1));
        QCOMPARE(session->returnToParent().status, AnalysisSessionReturnStatus::Returned);

        const auto recoveredPps = session->enterChildFormat(ppsNodeId, catalog);
        QVERIFY2(recoveredPps.succeeded(), qPrintable(recoveredPps.errorMessage));
        const auto ppsHeader = session->activeTree().node(*recoveredPps.childRootStructureNodeId);
        QVERIFY(ppsHeader.has_value());
        QCOMPARE(ppsHeader->name(), QStringLiteral("NalUnitHeader"));
    }

    void enterChildFormatFailsClosedOnNodeNotFoundAndMissingTargetFormat() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        auto session = AnalysisSession::openFile(fixturePath);
        QVERIFY(session != nullptr);
        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);

        const auto catalog = makeCatalogWithOfficialPackages();

        // 1. Node not found
        const auto res1 = session->enterChildFormat(AnalysisNodeId(99999), catalog);
        QCOMPARE(res1.status, AnalysisSessionNavigationStatus::NodeNotFound);
        QCOMPARE(session->navigationDepth(), std::size_t{0});
        QCOMPARE(&session->activeTree(), &session->tree());

        // 2. Root node has no target format
        const auto res2 = session->enterChildFormat(session->tree().rootId(), catalog);
        QCOMPARE(res2.status, AnalysisSessionNavigationStatus::MissingTargetFormat);
        QCOMPARE(session->navigationDepth(), std::size_t{0});
        QCOMPARE(&session->activeTree(), &session->tree());
    }

    void enterChildFormatFailsClosedOnCatalogResolutionErrors() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        auto session = AnalysisSession::openFile(fixturePath);
        QVERIFY(session != nullptr);
        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);

        const auto ascNodeOpt = findNodeByTargetFormat(session->tree(), session->tree().rootId(), QStringLiteral("audio.aac.asc"));
        QVERIFY(ascNodeOpt.has_value());

        // Empty catalog -> MissingContent
        RulePackageCatalog emptyCatalog;
        const auto res = session->enterChildFormat(*ascNodeOpt, emptyCatalog);
        QCOMPARE(res.status, AnalysisSessionNavigationStatus::MissingContent);
        QCOMPARE(session->navigationDepth(), std::size_t{0});
        QCOMPARE(&session->activeTree(), &session->tree());
    }

    void returnToParentAtRootIsNoOp() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        auto session = AnalysisSession::openFile(fixturePath);
        QVERIFY(session != nullptr);

        const auto ret = session->returnToParent();
        QCOMPARE(ret.status, AnalysisSessionReturnStatus::AtRoot);
        QVERIFY(!ret.restoredParentTargetNodeId.has_value());
        QCOMPARE(ret.activeTree, &session->tree());
        QCOMPARE(session->navigationDepth(), std::size_t{0});
    }

    void supportsRepeatedEnterAndReturnCyclesWithoutStaleState() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        auto session = AnalysisSession::openFile(fixturePath);
        QVERIFY(session != nullptr);
        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);

        const auto catalog = makeCatalogWithOfficialPackages();
        const auto ascNodeOpt = findNodeByTargetFormat(session->tree(), session->tree().rootId(), QStringLiteral("audio.aac.asc"));
        QVERIFY(ascNodeOpt.has_value());
        const auto ascNodeId = *ascNodeOpt;

        for (int cycle = 0; cycle < 3; ++cycle) {
            const auto navResult = session->enterChildFormat(ascNodeId, catalog);
            QVERIFY2(navResult.succeeded(), qPrintable(navResult.errorMessage));
            QCOMPARE(session->navigationDepth(), std::size_t{1});
            QVERIFY(session->canReturnToParent());

            const auto retResult = session->returnToParent();
            QCOMPARE(retResult.status, AnalysisSessionReturnStatus::Returned);
            QCOMPARE(session->navigationDepth(), std::size_t{0});
            QCOMPARE(&session->activeTree(), &session->tree());
        }
    }

    void sessionDocumentPersistenceIgnoresNavigationStack() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString docPath = directory.filePath(QStringLiteral("saved.svdoc"));

        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        auto session = AnalysisSession::openFile(fixturePath);
        QVERIFY(session != nullptr);
        const auto batch = session->analyzeBatch();
        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);

        const auto catalog = makeCatalogWithOfficialPackages();
        const auto ascNodeOpt = findNodeByTargetFormat(session->tree(), session->tree().rootId(), QStringLiteral("audio.aac.asc"));
        QVERIFY(ascNodeOpt.has_value());
        const auto ascNodeId = *ascNodeOpt;

        const auto navResult = session->enterChildFormat(ascNodeId, catalog);
        QVERIFY(navResult.succeeded());
        QCOMPARE(session->navigationDepth(), std::size_t{1});

        SessionUserState state;
        state.view.selectedSourceBitOffset = 1168;
        QString saveError;
        QVERIFY2(session->saveSession(docPath, state, &saveError), qPrintable(saveError));

        const auto restore = AnalysisSession::restoreSession(docPath, catalog);
        QCOMPARE(restore.status, AnalysisSessionRestoreStatus::Restored);
        QVERIFY(restore.session != nullptr);
        QCOMPARE(restore.session->navigationDepth(), std::size_t{0});
        QVERIFY(!restore.session->canReturnToParent());
        QCOMPARE(&restore.session->activeTree(), &restore.session->tree());
    }

    void compoundNavigationDoesNotReuseAnotherEntrypointsProgram() {
        QFile audioFile(QStringLiteral(
            STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4"));
        QFile videoFile(QStringLiteral(
            STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_avc1_avcC.mp4"));
        QVERIFY(audioFile.open(QIODevice::ReadOnly));
        QVERIFY(videoFile.open(QIODevice::ReadOnly));
        const QByteArray media = audioFile.readAll() + videoFile.readAll();
        std::vector<std::byte> bytes(static_cast<std::size_t>(media.size()));
        std::memcpy(bytes.data(), media.constData(), static_cast<std::size_t>(media.size()));

        auto session = AnalysisSession::create(
            std::make_unique<MemorySource>(std::move(bytes), QStringLiteral("combined.mp4")));
        QVERIFY(session != nullptr);
        while (!session->finished()) {
            const auto batch = session->analyzeBatch();
            QVERIFY(batch.status == AnalysisBatchStatus::InProgress ||
                    batch.status == AnalysisBatchStatus::Complete);
        }

        const auto audioNode = findNodeByTargetFormat(
            session->tree(), session->tree().rootId(), QStringLiteral("audio.aac.asc"));
        const auto videoNode = findNodeByTargetFormat(
            session->tree(), session->tree().rootId(), QStringLiteral("video.h264.nal"));
        QVERIFY(audioNode.has_value());
        QVERIFY(videoNode.has_value());

        auto package = makeTwoCompoundEntrypointPackage();
        QVERIFY2(package.succeeded(), qPrintable(package.errorMessage));
        RulePackageCatalog catalog;
        QVERIFY(catalog.registerPackage(std::move(*package.package)).succeeded());

        const auto audioNavigation = session->enterChildFormat(*audioNode, catalog);
        QVERIFY2(audioNavigation.succeeded(), qPrintable(audioNavigation.errorMessage));
        QCOMPARE(session->activeTree()
                     .node(*audioNavigation.childRootStructureNodeId)
                     ->name(),
                 QStringLiteral("AudioHeader"));
        QCOMPARE(session->returnToParent().status, AnalysisSessionReturnStatus::Returned);

        const auto videoNavigation = session->enterChildFormat(*videoNode, catalog);
        QVERIFY2(videoNavigation.succeeded(), qPrintable(videoNavigation.errorMessage));
        QCOMPARE(session->activeTree()
                     .node(*videoNavigation.childRootStructureNodeId)
                     ->name(),
                 QStringLiteral("VideoHeader"));
    }

    void childExecutionFailuresPreserveActiveNavigationState() {
        const QString fixturePath = QStringLiteral(
            STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        const auto assertUnchanged = [](const AnalysisSession& session,
                                        const streamview::core::AnalysisTree* rootTree,
                                        std::size_t rootNodeCount) {
            QCOMPARE(session.navigationDepth(), std::size_t(0));
            QVERIFY(!session.canReturnToParent());
            QCOMPARE(&session.activeTree(), rootTree);
            QCOMPARE(session.tree().nodeCount(), rootNodeCount);
        };
        const auto makeCatalog = [](const QByteArray& source) {
            auto package = makeAudioNavigationPackage(source);
            if (!package.succeeded()) {
                return std::optional<RulePackageCatalog>{};
            }
            RulePackageCatalog catalog;
            const auto registered = catalog.registerPackage(std::move(*package.package));
            if (!registered.succeeded()) {
                return std::optional<RulePackageCatalog>{};
            }
            return std::optional<RulePackageCatalog>(std::move(catalog));
        };

        {
            auto session = AnalysisSession::openFile(fixturePath);
            QVERIFY(session != nullptr);
            QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);
            const auto nodeId = findNodeByTargetFormat(
                session->tree(), session->tree().rootId(), QStringLiteral("audio.aac.asc"));
            QVERIFY(nodeId.has_value());
            const auto* rootTree = &session->tree();
            const auto rootNodeCount = rootTree->nodeCount();
            auto catalog = makeCatalog(
                QByteArrayLiteral("struct Audio { bits<16> value; } entry Audio;\n"));
            QVERIFY(catalog.has_value());

            streamview::core::CancellationSource cancellation;
            QVERIFY(cancellation.requestCancellation());
            streamview::rules::StructuralExecutionOptions options;
            options.cancellation = cancellation.token();
            const auto cancelled = session->enterChildFormat(*nodeId, *catalog, options);
            QCOMPARE(cancelled.status, AnalysisSessionNavigationStatus::Cancelled);
            assertUnchanged(*session, rootTree, rootNodeCount);

            options = {};
            options.limits.maximumInstructions = 1;
            const auto limited = session->enterChildFormat(*nodeId, *catalog, options);
            QCOMPARE(limited.status, AnalysisSessionNavigationStatus::ResourceLimit);
            assertUnchanged(*session, rootTree, rootNodeCount);
        }

        {
            auto session = AnalysisSession::openFile(fixturePath);
            QVERIFY(session != nullptr);
            QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);
            const auto nodeId = findNodeByTargetFormat(
                session->tree(), session->tree().rootId(), QStringLiteral("audio.aac.asc"));
            QVERIFY(nodeId.has_value());
            const auto* rootTree = &session->tree();
            const auto rootNodeCount = rootTree->nodeCount();
            auto catalog = makeCatalog(
                QByteArrayLiteral("struct Audio { bits<24> value; } entry Audio;\n"));
            QVERIFY(catalog.has_value());

            const auto truncated = session->enterChildFormat(*nodeId, *catalog);
            QCOMPARE(truncated.status, AnalysisSessionNavigationStatus::TruncatedSource);
            assertUnchanged(*session, rootTree, rootNodeCount);
        }

        {
            QFile fixture(fixturePath);
            QVERIFY(fixture.open(QIODevice::ReadOnly));
            const QByteArray media = fixture.readAll();
            std::vector<std::byte> bytes(static_cast<std::size_t>(media.size()));
            std::memcpy(bytes.data(), media.constData(), static_cast<std::size_t>(media.size()));
            auto source = std::make_unique<ToggleErrorMemorySource>(std::move(bytes));
            auto* sourceControl = source.get();
            auto session = AnalysisSession::create(std::move(source));
            QVERIFY(session != nullptr);
            QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);
            const auto nodeId = findNodeByTargetFormat(
                session->tree(), session->tree().rootId(), QStringLiteral("audio.aac.asc"));
            QVERIFY(nodeId.has_value());
            const auto* rootTree = &session->tree();
            const auto rootNodeCount = rootTree->nodeCount();
            auto catalog = makeCatalog(
                QByteArrayLiteral("struct Audio { bits<16> value; } entry Audio;\n"));
            QVERIFY(catalog.has_value());

            sourceControl->setFailReads(true);
            const auto sourceError = session->enterChildFormat(*nodeId, *catalog);
            QCOMPARE(sourceError.status, AnalysisSessionNavigationStatus::SourceError);
            assertUnchanged(*session, rootTree, rootNodeCount);
        }
    }

    void reportsIndexedTracksWithTheirDeclaredTargetFormats() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_two_tracks.mp4");
        const auto catalog = makeCatalogWithOfficialPackages();
        auto session = openPinnedMp4Fixture(fixturePath, catalog);
        QVERIFY(session != nullptr);
        QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);

        const auto tracks = session->tracks();
        QVERIFY2(tracks.available(), qPrintable(tracks.errorMessage));
        QCOMPARE(tracks.tracks.size(), std::size_t{2});

        QCOMPARE(tracks.tracks[0].trackId, quint32{1});
        QCOMPARE(tracks.tracks[0].timescale, quint32{30000});
        QCOMPARE(tracks.tracks[0].sampleCount, quint64{2});
        QCOMPARE(tracks.tracks[0].targetFormat, QStringLiteral("video.h264.nal"));

        QCOMPARE(tracks.tracks[1].trackId, quint32{2});
        QCOMPARE(tracks.tracks[1].timescale, quint32{44100});
        QCOMPARE(tracks.tracks[1].sampleCount, quint64{2});
        QCOMPARE(tracks.tracks[1].targetFormat, QStringLiteral("audio.aac.asc"));
    }

    void pagesSamplesForOneTrackAndRejectsUnknownTracks() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_avc_multi_nal.mp4");
        const auto catalog = makeCatalogWithOfficialPackages();
        auto session = openPinnedMp4Fixture(fixturePath, catalog);
        QVERIFY(session != nullptr);
        QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);

        AnalysisSessionSamplePageRequest request;
        request.trackId = 1;
        request.pageIndex = 0;
        request.pageSize = 2;
        const auto firstPage = session->samplesForTrack(request);
        QVERIFY2(firstPage.available(), qPrintable(firstPage.errorMessage));
        QCOMPARE(firstPage.descriptors.size(), std::size_t{2});
        QCOMPARE(firstPage.firstSampleIndex, quint64{0});
        QCOMPARE(firstPage.sampleCount, quint64{3});
        QCOMPARE(firstPage.descriptors[0].sampleIndex, quint64{0});
        QCOMPARE(firstPage.descriptors[0].timescale, quint32{30000});
        QVERIFY(firstPage.descriptors[0].isSyncSample);
        QVERIFY(!firstPage.descriptors[1].isSyncSample);

        // The second page starts inside the second chunk, so the page boundary
        // does not coincide with a chunk boundary.
        request.pageIndex = 1;
        const auto secondPage = session->samplesForTrack(request);
        QVERIFY2(secondPage.available(), qPrintable(secondPage.errorMessage));
        QCOMPARE(secondPage.descriptors.size(), std::size_t{1});
        QCOMPARE(secondPage.firstSampleIndex, quint64{2});
        QCOMPARE(secondPage.descriptors[0].sampleIndex, quint64{2});

        request.trackId = 99;
        request.pageIndex = 0;
        const auto missing = session->samplesForTrack(request);
        QCOMPARE(missing.status, AnalysisSessionSampleStatus::TrackNotFound);
        QVERIFY(missing.descriptors.empty());
    }

    void entersAvcSampleAsMultipleNalUnitsAndReturnsToTheSampleRow() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_avc_multi_nal.mp4");
        const auto catalog = makeCatalogWithOfficialPackages();
        auto session = openPinnedMp4Fixture(fixturePath, catalog);
        QVERIFY(session != nullptr);
        QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);

        const auto entered = session->enterSample(1, 0, catalog);
        QVERIFY2(entered.entered(), qPrintable(entered.errorMessage));
        QCOMPARE(entered.status, AnalysisSessionSampleStatus::Available);
        QCOMPARE(session->navigationDepth(), std::size_t{1});
        QVERIFY(session->canReturnToParent());

        QCOMPARE(entered.unitNodeIds.size(), std::size_t{3});
        QCOMPARE(entered.sample.sampleIndex, quint64{0});
        QCOMPARE(entered.sample.trackId, quint32{1});

        QVERIFY(entered.sampleNodeId.has_value());
        const auto& sampleTree = session->activeTree();
        const auto sampleNode = sampleTree.node(*entered.sampleNodeId);
        QVERIFY(sampleNode.has_value());
        QCOMPARE(sampleNode->state(), MaterializationState::Materialized);

        const auto firstUnit = sampleTree.node(entered.unitNodeIds.front());
        QVERIFY(firstUnit.has_value());
        const auto header = findChildNamed(sampleTree, firstUnit->id(),
                                          QStringLiteral("NalUnitHeader"));
        QVERIFY(header.has_value());
        const auto sps = findChildNamed(sampleTree, header->id(),
                                        QStringLiteral("SequenceParameterSetRbsp"));
        QVERIFY(sps.has_value());

        const auto secondUnit = sampleTree.node(entered.unitNodeIds[1]);
        QVERIFY(secondUnit.has_value());
        const auto secondHeader = findChildNamed(sampleTree, secondUnit->id(),
                                                 QStringLiteral("NalUnitHeader"));
        QVERIFY(secondHeader.has_value());
        QVERIFY(findChildNamed(sampleTree, secondHeader->id(),
                               QStringLiteral("PictureParameterSetRbsp"))
                    .has_value());

        const auto currentSample = session->currentSampleFrame();
        QVERIFY(currentSample != nullptr);
        QCOMPARE(currentSample->trackId, quint32{1});
        QCOMPARE(currentSample->sampleIndex, quint64{0});

        const auto returned = session->returnToParent();
        QCOMPARE(returned.status, AnalysisSessionReturnStatus::Returned);
        QCOMPARE(session->navigationDepth(), std::size_t{0});
        QCOMPARE(&session->activeTree(), &session->tree());
        QVERIFY(returned.restoredSample.has_value());
        QCOMPARE(returned.restoredSample->trackId, quint32{1});
        QCOMPARE(returned.restoredSample->sampleIndex, quint64{0});
        QCOMPARE(returned.restoredSample->targetFormat, QStringLiteral("video.h264.nal"));
        QCOMPARE(returned.restoredSample->sample.sourceSpans.size(), std::size_t{1});
        QCOMPARE(session->currentSampleFrame(), nullptr);
    }

    void entersAacSampleAsOpaqueAccessUnitReferencingItsConfiguration() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_aac_opaque.mp4");
        const auto catalog = makeCatalogWithOfficialPackages();
        auto session = openPinnedMp4Fixture(fixturePath, catalog);
        QVERIFY(session != nullptr);
        QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);

        const auto entered = session->enterSample(1, 1, catalog);
        QVERIFY2(entered.entered(), qPrintable(entered.errorMessage));
        QCOMPARE(entered.unitNodeIds.size(), std::size_t{1});
        QCOMPARE(entered.sample.sampleIndex, quint64{1});
        QVERIFY(entered.sample.isSyncSample);

        const auto& sampleTree = session->activeTree();
        QVERIFY(entered.sampleNodeId.has_value());
        const auto accessUnit = sampleTree.node(entered.unitNodeIds.front());
        QVERIFY(accessUnit.has_value());
        QCOMPARE(accessUnit->kind(), streamview::core::AnalysisNodeKind::CompressedPayload);

        QVERIFY(findChildNamed(sampleTree, *entered.sampleNodeId,
                               QStringLiteral("configuration_node"))
                    .has_value());
        const auto summary = findChildNamed(sampleTree, *entered.sampleNodeId,
                                            QStringLiteral("configuration_summary"));
        QVERIFY(summary.has_value());
        QVERIFY(summary->value().toString().contains(QStringLiteral("44100")));

        const auto returned = session->returnToParent();
        QCOMPARE(returned.status, AnalysisSessionReturnStatus::Returned);
        QVERIFY(returned.restoredSample.has_value());
        QCOMPARE(returned.restoredSample->targetFormat, QStringLiteral("audio.aac.asc"));
    }

    void enterSampleFailsClosedOnUnknownTrackAndSampleAndUnsupportedSource() {
        const auto catalog = makeCatalogWithOfficialPackages();
        {
            const QString fixturePath =
                QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_avc_multi_nal.mp4");
            auto session = openPinnedMp4Fixture(fixturePath, catalog);
            QVERIFY(session != nullptr);
            QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);

            const auto unknownTrack = session->enterSample(42, 0, catalog);
            QCOMPARE(unknownTrack.status, AnalysisSessionSampleStatus::TrackNotFound);
            QCOMPARE(session->navigationDepth(), std::size_t{0});

            const auto unknownSample = session->enterSample(1, 999, catalog);
            QCOMPARE(unknownSample.status, AnalysisSessionSampleStatus::SampleNotFound);
            QCOMPARE(session->navigationDepth(), std::size_t{0});
        }

        {
            auto session = AnalysisSession::create(
                std::make_unique<MemorySource>(validAnnexB(), QStringLiteral("fixture.264")));
            QVERIFY(session != nullptr);
            QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);

            const auto tracks = session->tracks();
            QCOMPARE(tracks.status, AnalysisSessionSampleStatus::UnsupportedSource);
            QVERIFY(tracks.tracks.empty());
        }

        {
            const QString fixturePath =
                QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_avc_multi_nal.mp4");
            auto session = openPinnedMp4Fixture(fixturePath, catalog);
            QVERIFY(session != nullptr);
            const auto notAnalyzed = session->tracks();
            QCOMPARE(notAnalyzed.status, AnalysisSessionSampleStatus::NotAnalyzed);
        }
    }

    void truncatedSampleUnitFailsWithoutDisturbingTheParentTreeOrPaging() {
        const QString fixturePath = QStringLiteral(
            STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_avc_truncated_unit.mp4");
        const auto catalog = makeCatalogWithOfficialPackages();
        auto session = openPinnedMp4Fixture(fixturePath, catalog);
        QVERIFY(session != nullptr);
        QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);

        const auto* rootTree = &session->tree();

        AnalysisSessionSamplePageRequest request;
        request.trackId = 1;
        request.pageSize = 2;
        const auto pageBefore = session->samplesForTrack(request);
        QVERIFY(pageBefore.available());

        // Baselined after paging: building the sample index materializes the
        // lazy sample-table regions, so an earlier baseline would measure that
        // growth instead of what the failed enterSample leaves behind.
        const auto rootNodeCount = rootTree->nodeCount();

        // Sample 0 is intact, so entering it first creates the sample session
        // whose tree the runner writes into. That tree, not the container tree,
        // is what a failed sample has to leave untouched: samples of one track
        // share this tree by entrypoint identity, so a half-written failure
        // would corrupt every later sample.
        const auto firstEntry = session->enterSample(1, 0, catalog);
        QVERIFY2(firstEntry.entered(), qPrintable(firstEntry.errorMessage));
        const auto* sampleTree = &session->activeTree();
        QVERIFY(sampleTree != rootTree);
        const auto sampleTreeNodeCount = sampleTree->nodeCount();
        QCOMPARE(session->returnToParent().status, AnalysisSessionReturnStatus::Returned);

        const auto failed = session->enterSample(1, 1, catalog);
        QVERIFY(!failed.entered());
        QCOMPARE(failed.status, AnalysisSessionSampleStatus::InvalidSampleRange);
        QCOMPARE(session->navigationDepth(), std::size_t{0});
        QVERIFY(!session->canReturnToParent());
        QCOMPARE(&session->activeTree(), rootTree);
        QCOMPARE(session->tree().nodeCount(), rootNodeCount);
        QCOMPARE(sampleTree->nodeCount(), sampleTreeNodeCount);
        QCOMPARE(session->currentSampleFrame(), nullptr);

        const auto pageAfter = session->samplesForTrack(request);
        QVERIFY(pageAfter.available());
        QCOMPARE(pageAfter.descriptors.size(), pageBefore.descriptors.size());
        QCOMPARE(pageAfter.sampleCount, pageBefore.sampleCount);

        const auto stillWorks = session->enterSample(1, 0, catalog);
        QVERIFY2(stillWorks.entered(), qPrintable(stillWorks.errorMessage));
        QCOMPARE(session->navigationDepth(), std::size_t{1});
        QCOMPARE(&session->activeTree(), sampleTree);
    }

    void trackIndexIsNotCachedFromAPartiallyAnalyzedContainer() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_two_tracks.mp4");
        const auto catalog = makeCatalogWithOfficialPackages();
        auto session = openPinnedMp4Fixture(fixturePath, catalog);
        QVERIFY(session != nullptr);

        // One box per batch, so the movie is still incomplete when tracks() is
        // first asked. The extractor skips a `trak` that has no `stbl` yet
        // instead of failing, and the built index latches with no invalidation
        // path, so answering here would cache a short track list permanently.
        std::size_t batches = 0;
        AnalysisBatchResult batch;
        do {
            batch = session->analyzeBatch(1);
            ++batches;
            if (batch.status == AnalysisBatchStatus::InProgress) {
                const auto early = session->tracks();
                QCOMPARE(early.status, AnalysisSessionSampleStatus::NotAnalyzed);
                QVERIFY(early.tracks.empty());
            }
            QVERIFY(batches < 512);
        } while (batch.status == AnalysisBatchStatus::InProgress);

        QCOMPARE(batch.status, AnalysisBatchStatus::Complete);
        QVERIFY2(batches > 1, "the fixture must need several batches for this to prove anything");

        const auto tracks = session->tracks();
        QVERIFY2(tracks.available(), qPrintable(tracks.errorMessage));
        QCOMPARE(tracks.tracks.size(), std::size_t{2});
        QCOMPARE(tracks.tracks[0].trackId, quint32{1});
        QCOMPARE(tracks.tracks[1].trackId, quint32{2});
    }

    void tracksRefusesToAnswerFromATerminallyFailedContainer() {
        const QString fixturePath = QStringLiteral(
            STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_terminal_resource_limit.mp4");
        const auto catalog = makeCatalogWithOfficialPackages();
        auto session = openPinnedMp4Fixture(fixturePath, catalog);
        QVERIFY(session != nullptr);

        std::size_t batches = 0;
        AnalysisBatchResult batch;
        do {
            batch = session->analyzeBatch();
            ++batches;
            QVERIFY(batches < 512);
        } while (batch.status == AnalysisBatchStatus::InProgress);

        QCOMPARE(batch.status, AnalysisBatchStatus::ResourceLimit);
        QVERIFY2(session->finished(),
                 "a terminal failure still reports the analysis as finished, which is exactly "
                 "why finished() cannot gate sample indexing");

        const auto& containerTree = session->tree();
        const auto root = containerTree.node(containerTree.rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), streamview::core::MaterializationState::Invalid);
        QVERIFY2(root->children().size() >= 2,
                 "the abandoned tree must still hold the movie box, so indexing it would "
                 "produce a plausible-looking track list");

        const auto tracks = session->tracks();
        QCOMPARE(tracks.status, AnalysisSessionSampleStatus::ResourceLimit);
        QVERIFY(tracks.tracks.empty());
        QVERIFY(!tracks.errorMessage.isEmpty());

        const auto repeated = session->tracks();
        QCOMPARE(repeated.status, AnalysisSessionSampleStatus::ResourceLimit);
        QVERIFY(repeated.tracks.empty());

        AnalysisSessionSamplePageRequest request;
        request.trackId = 1;
        request.pageSize = 2;
        const auto page = session->samplesForTrack(request);
        QCOMPARE(page.status, AnalysisSessionSampleStatus::ResourceLimit);

        const auto entered = session->enterSample(1, 0, catalog);
        QCOMPARE(entered.status, AnalysisSessionSampleStatus::ResourceLimit);
        QCOMPARE(session->navigationDepth(), std::size_t{0});
    }

    void sampleNavigationStackIsNotPersistedIntoTheSessionDocument() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_avc_multi_nal.mp4");
        const auto catalog = makeCatalogWithOfficialPackages();
        auto session = openPinnedMp4Fixture(fixturePath, catalog);
        QVERIFY(session != nullptr);
        QCOMPARE(session->analyzeBatch().status, AnalysisBatchStatus::Complete);

        QVERIFY(session->enterSample(1, 0, catalog).entered());
        QCOMPARE(session->navigationDepth(), std::size_t{1});

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sessionPath = directory.filePath(QStringLiteral("sample.svsession"));
        SessionUserState state;
        QString errorMessage;
        QVERIFY2(session->saveSession(sessionPath, state, &errorMessage),
                 qPrintable(errorMessage));

        const auto restored = AnalysisSession::restoreSession(sessionPath, catalog);
        QCOMPARE(restored.status, AnalysisSessionRestoreStatus::Restored);
        QVERIFY(restored.session != nullptr);
        QCOMPARE(restored.session->navigationDepth(), std::size_t{0});
        QCOMPARE(restored.session->currentSampleFrame(), nullptr);
    }
};

QTEST_GUILESS_MAIN(AnalysisSessionTest)

#include "analysis_session_test.moc"
