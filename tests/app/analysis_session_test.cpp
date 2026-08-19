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
};

QTEST_GUILESS_MAIN(AnalysisSessionTest)

#include "analysis_session_test.moc"
