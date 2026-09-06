#pragma once

#include "session_document.h"

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/core/source_pager.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/aac_adts_detector.h>
#include <streamview/rules/analysis_cache_owner.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/format_selection.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/h264_annex_b_detector.h>
#include <streamview/rules/mp4_box_detector.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/payload_transform.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_execution_session.h>
#include <streamview/rules/rule_package.h>
#include <streamview/rules/structural_entry_runner.h>
#include <streamview/rules/configuration_summary.h>
#include <streamview/rules/mp4_sample_table_extractor.h>
#include <streamview/rules/mp4_sample_table_index.h>
#include <streamview/rules/sample_payload_runner.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace streamview::app {

struct AnalysisSessionRestoreResult;

struct AnalysisSessionCacheOptions final {
    QString databasePath;
    rules::AnalysisCacheOwnerOptions ownerOptions;

    [[nodiscard]] bool enabled() const noexcept { return !databasePath.isEmpty(); }
};

enum class AnalysisSessionCacheStatus : quint8 {
    Disabled,
    Active,
    Failed,
};

enum class AnalysisBatchStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    ResourceLimit,
    InvalidRule,
    InvalidBatchSize,
};

struct AnalysisBatchResult final {
    AnalysisBatchStatus status = AnalysisBatchStatus::InProgress;
    std::vector<core::AnalysisNodeId> topLevelNodes;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == AnalysisBatchStatus::Complete;
    }
};

/// One entered sample, pushed onto the same navigation stack as a child format.
///
/// A sample frame records the track and sample it came from so returning lands
/// on the exact row the UI navigated away from, which is what lets a return
/// restore the sample page and its selection without persisting anything.
struct SampleNavigationFrame final {
    quint32 trackId = 0;
    quint64 sampleIndex = 0;
    core::SampleDescriptor sample;
    QString targetFormat;
};

struct NavigationFrame final {
    core::AnalysisNodeId parentTargetNodeId;
    QString targetFormat;
    std::shared_ptr<const rules::RulePackage> package;
    rules::RulePackageEntryPoint entryPoint;
    core::SourceMapping sourceMapping;
    std::shared_ptr<core::AnalysisTree> tree;
    core::AnalysisNodeId childRootStructureNodeId;
    /// Set when this frame was entered through `enterSample`, absent when it
    /// was entered through `enterChildFormat`. Both kinds share one stack so a
    /// single `returnToParent` unwinds either.
    std::optional<SampleNavigationFrame> sample;
};

enum class AnalysisSessionNavigationStatus : quint8 {
    Entered,
    NodeNotFound,
    MissingTargetFormat,
    InvalidTargetLocation,
    MissingContent,
    VersionConflict,
    IncompatibleLanguage,
    IncompatibleEngine,
    InvalidRulePackage,
    InvalidDefinition,
    Unsupported,
    TruncatedSource,
    InvalidSyntax,
    DependencyUnavailable,
    SourceError,
    Cancelled,
    ResourceLimit,
};

enum class AnalysisSessionSampleStatus : quint8 {
    /// Descriptors were produced, or a sample sub-tree was entered.
    Available,
    /// The active analyzer is not a container that carries sample tables.
    UnsupportedSource,
    /// Analysis has not produced a container tree to index yet.
    NotAnalyzed,
    /// No track carries the sample tables this index needs.
    NoTracks,
    /// The requested track id is not one of the indexed tracks.
    TrackNotFound,
    /// The requested sample is outside the track's sample count.
    SampleNotFound,
    /// The tables disagree with each other, or with their declared counts.
    InconsistentTables,
    /// A table variant or framing this slice does not decode.
    UnsupportedTables,
    /// The sample entry declares no target format, so no rule claims it.
    MissingTargetFormat,
    /// The sample entry declares a target format no installed package claims.
    UnsupportedFormat,
    /// A configuration the sample decodes against is missing or unreadable.
    DependencyUnavailable,
    /// A table read or a sample read failed.
    SourceError,
    /// The sample resolves outside the media source, or a coordinate overflowed.
    InvalidSampleRange,
    Cancelled,
    ResourceLimit,
    /// The rule package resolved for the sample's target format is unusable.
    InvalidRulePackage,
    /// The resolved rule compiled, but not into an executable sample entry.
    InvalidDefinition,
};

/// One indexed track, as the session exposes it.
///
/// `targetFormat` is copied from the rules-layer `stsd` binding rather than
/// derived here, so the session never inspects a sample entry itself.
struct AnalysisSessionTrack final {
    quint32 trackId = 0;
    quint32 timescale = 1;
    quint64 sampleCount = 0;
    QString targetFormat;
};

struct AnalysisSessionTracksResult final {
    AnalysisSessionSampleStatus status = AnalysisSessionSampleStatus::UnsupportedSource;
    std::vector<AnalysisSessionTrack> tracks;
    QString errorMessage;

    [[nodiscard]] bool available() const noexcept {
        return status == AnalysisSessionSampleStatus::Available;
    }
};

struct AnalysisSessionSamplePageRequest final {
    quint32 trackId = 0;
    quint64 pageIndex = 0;
    quint64 pageSize = 256;
    std::optional<core::CancellationToken> cancellation;
};

struct AnalysisSessionSamplePageResult final {
    AnalysisSessionSampleStatus status = AnalysisSessionSampleStatus::UnsupportedSource;
    std::vector<core::SampleDescriptor> descriptors;
    quint64 firstSampleIndex = 0;
    quint64 sampleCount = 0;
    QString errorMessage;

    [[nodiscard]] bool available() const noexcept {
        return status == AnalysisSessionSampleStatus::Available;
    }
};

struct AnalysisSessionEnterSampleResult final {
    AnalysisSessionSampleStatus status = AnalysisSessionSampleStatus::UnsupportedSource;
    /// Root of the entered sample's sub-tree.
    std::optional<core::AnalysisNodeId> sampleNodeId;
    std::shared_ptr<core::AnalysisTree> tree;
    /// Descriptor the sample was entered from, so a caller need not re-page to
    /// learn the sample's coordinates and timeline.
    core::SampleDescriptor sample;
    /// Nodes of the executed units, in framing order. Empty for a sample that
    /// carries a single opaque access unit.
    std::vector<core::AnalysisNodeId> unitNodeIds;
    QString errorMessage;

    [[nodiscard]] bool entered() const noexcept {
        return status == AnalysisSessionSampleStatus::Available && tree != nullptr;
    }
};

struct AnalysisSessionNavigationResult final {
    AnalysisSessionNavigationStatus status = AnalysisSessionNavigationStatus::InvalidDefinition;
    std::optional<core::AnalysisNodeId> childRootStructureNodeId;
    std::shared_ptr<core::AnalysisTree> tree;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisSessionNavigationStatus::Entered && tree != nullptr;
    }
};

enum class AnalysisSessionReturnStatus : quint8 {
    Returned,
    AtRoot,
};

struct AnalysisSessionReturnResult final {
    AnalysisSessionReturnStatus status = AnalysisSessionReturnStatus::AtRoot;
    std::optional<core::AnalysisNodeId> restoredParentTargetNodeId;
    const core::AnalysisTree* activeTree = nullptr;
    /// The sample the unwound frame had entered, when it was a sample frame.
    /// Carries the track, sample ordinal, and descriptor the caller needs to
    /// put the sample row and its highlight back exactly as they were, which is
    /// what keeps the restored state out of the persisted session document.
    std::optional<SampleNavigationFrame> restoredSample;

    [[nodiscard]] bool returned() const noexcept {
        return status == AnalysisSessionReturnStatus::Returned;
    }
};

class AnalysisSession final {
public:
    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    openFile(const QString& path, QString* errorMessage = nullptr);
    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    openFile(const QString& path,
             AnalysisSessionCacheOptions cacheOptions,
             QString* errorMessage = nullptr);

    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    create(std::unique_ptr<core::RandomAccessSource> source,
           QString* errorMessage = nullptr);
    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    create(std::unique_ptr<core::RandomAccessSource> source,
           QString sourcePath,
           QString* errorMessage = nullptr);
    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    openFileWithExplicitRule(const QString& path,
                             const rules::RulePackageCatalog& catalog,
                             const rules::RuleEntryPointIdentity& targetRule,
                             AnalysisSessionCacheOptions cacheOptions = {},
                             QString* errorMessage = nullptr);
    [[nodiscard]] static AnalysisSessionRestoreResult
    restoreSession(const QString& sessionPath, const rules::RulePackageCatalog& catalog);
    [[nodiscard]] static AnalysisSessionRestoreResult
    restoreSession(const QString& sessionPath,
                   const rules::RulePackageCatalog& catalog,
                   AnalysisSessionCacheOptions cacheOptions);

    AnalysisSession(const AnalysisSession&) = delete;
    AnalysisSession& operator=(const AnalysisSession&) = delete;
    AnalysisSession(AnalysisSession&&) = delete;
    AnalysisSession& operator=(AnalysisSession&&) = delete;
    ~AnalysisSession() = default;

    [[nodiscard]] const core::RandomAccessSource& source() const noexcept { return *source_; }
    [[nodiscard]] QString identity() const { return source_->identity(); }
    [[nodiscard]] quint64 sizeBytes() const noexcept { return source_->sizeBytes(); }
    [[nodiscard]] const core::SourcePage& initialPage() const noexcept { return initialPage_; }
    [[nodiscard]] const rules::H264AnnexBDetectionResult& formatDetection() const noexcept {
        return formatDetection_;
    }
    [[nodiscard]] const rules::AacAdtsDetectionResult& aacFormatDetection() const noexcept {
        return aacFormatDetection_;
    }
    [[nodiscard]] const rules::Mp4DetectionResult& mp4FormatDetection() const noexcept {
        return mp4FormatDetection_;
    }
    [[nodiscard]] const rules::FormatSelection& formatSelection() const noexcept {
        return formatSelection_;
    }

    [[nodiscard]] AnalysisBatchResult analyzeBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = 256U * 1024U,
        quint64 maximumMappedBytes = 64U * 1024U * 1024U);
    [[nodiscard]] const core::AnalysisTree& tree() const noexcept {
        return std::visit([](const auto& a) -> const core::AnalysisTree& { return a.tree(); },
                          analyzer_);
    }
    [[nodiscard]] bool finished() const noexcept {
        return std::visit([](const auto& a) -> bool { return a.finished(); }, analyzer_);
    }
    [[nodiscard]] quint64 scanCursor() const noexcept {
        return std::visit([](const auto& a) -> quint64 { return a.scanCursor(); }, analyzer_);
    }
    [[nodiscard]] const rules::RuleEntryPointIdentity& ruleIdentity() const noexcept {
        return std::visit(
            [](const auto& a) -> const rules::RuleEntryPointIdentity& { return a.ruleIdentity(); },
            analyzer_);
    }
    [[nodiscard]] const SessionUserState& userState() const noexcept { return userState_; }
    [[nodiscard]] SessionUserState& userState() noexcept { return userState_; }
    [[nodiscard]] AnalysisSessionCacheStatus cacheStatus() const noexcept {
        return cacheStatus_;
    }
    [[nodiscard]] const QString& cacheErrorMessage() const noexcept {
        return cacheErrorMessage_;
    }
    void pollCacheWrites();
    [[nodiscard]] bool cacheWritesPending() const noexcept {
        return !pendingCacheWrites_.empty();
    }
    [[nodiscard]] const std::optional<core::SourceFingerprint>& initialFingerprint() const noexcept {
        return initialFingerprint_;
    }
    void enableCache(AnalysisSessionCacheOptions cacheOptions);
    void requestCancellation() noexcept;
    [[nodiscard]] bool isCancellationRequested() const noexcept;
    [[nodiscard]] SessionSaveResult saveSession(
        const QString& sessionPath,
        const SessionUserState& userState) const;
    [[nodiscard]] bool overrideFormat(
        const rules::RulePackageCatalog& catalog,
        const rules::RuleEntryPointIdentity& targetRule,
        QString* errorMessage = nullptr);

    [[nodiscard]] AnalysisSessionNavigationResult enterChildFormat(
        core::AnalysisNodeId nodeId,
        const rules::RulePackageCatalog& catalog,
        const rules::StructuralExecutionOptions& options = {});
    [[nodiscard]] AnalysisSessionReturnResult returnToParent();

    /// Tracks the container declares sample tables for.
    ///
    /// Extraction happens once per session and is cached: the bound table
    /// readers borrow `analyzer_`, so they are stored in this session, which
    /// neither moves nor reassigns its analyzer.
    [[nodiscard]] AnalysisSessionTracksResult tracks();

    /// Descriptors for one page of a track's samples.
    [[nodiscard]] AnalysisSessionSamplePageResult
    samplesForTrack(const AnalysisSessionSamplePageRequest& request);

    /// Executes one sample against the rule package its `stsd` entry declares
    /// and pushes a sample frame, so `returnToParent` lands back on the sample's
    /// own row.
    ///
    /// A failure leaves the parent tree, the navigation stack, and the cached
    /// index exactly as they were.
    [[nodiscard]] AnalysisSessionEnterSampleResult
    enterSample(quint32 trackId,
                quint64 sampleIndex,
                const rules::RulePackageCatalog& catalog,
                const rules::StructuralExecutionOptions& options = {});

    [[nodiscard]] const core::AnalysisTree& activeTree() const noexcept {
        return navigationStack_.empty() ? tree() : *navigationStack_.back().tree;
    }
    [[nodiscard]] std::size_t navigationDepth() const noexcept { return navigationStack_.size(); }
    [[nodiscard]] bool canReturnToParent() const noexcept { return !navigationStack_.empty(); }
    [[nodiscard]] const NavigationFrame* currentNavigationFrame() const noexcept {
        return navigationStack_.empty() ? nullptr : &navigationStack_.back();
    }
    /// The entered sample of the current frame, or nullptr when the current
    /// frame is a child format rather than a sample.
    [[nodiscard]] const SampleNavigationFrame* currentSampleFrame() const noexcept {
        if (navigationStack_.empty()) {
            return nullptr;
        }
        const auto& sample = navigationStack_.back().sample;
        return sample.has_value() ? &*sample : nullptr;
    }

private:
    AnalysisSession(std::unique_ptr<core::RandomAccessSource> source,
                    QString sourcePath,
                    core::SourcePage initialPage,
                    rules::H264AnnexBDetectionResult formatDetection,
                    rules::AacAdtsDetectionResult aacFormatDetection,
                    rules::Mp4DetectionResult mp4FormatDetection,
                    rules::FormatSelection formatSelection,
                    std::variant<rules::H264AnnexBAnalyzer, rules::AacAdtsAnalyzer, rules::Mp4IsobmffAnalyzer> analyzer,
                    SessionUserState userState,
                    std::unique_ptr<rules::AnalysisCacheOwner> cacheOwner,
                    AnalysisSessionCacheStatus cacheStatus,
                    QString cacheErrorMessage,
                    std::optional<core::SourceFingerprint> initialFingerprint = std::nullopt,
                    std::shared_ptr<core::CancellationSource> cancellationSource = nullptr);
    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    createPrepared(std::unique_ptr<core::RandomAccessSource> source,
                   QString sourcePath,
                   const rules::RuleCatalogLookupResult* resolvedRule,
                   SessionUserState userState,
                   AnalysisSessionCacheOptions cacheOptions,
                   std::optional<core::SourceFingerprint> verifiedFingerprint,
                   QString* errorMessage);
    void disableCache(QString errorMessage);
    void acceptCacheWrite(rules::AnalysisCacheOwnerWriteSubmission submission);
    void publishCachePages(const rules::H264AnnexBAnalysisBatch& batch);

    struct SubFormatSession final {
        std::shared_ptr<core::AnalysisTree> tree;
        std::unique_ptr<rules::RuleExecutionSession> ruleSession;
    };

    /// Builds the per-track indices once, on first use.
    ///
    /// Returns the status to report when indexing could not be completed;
    /// `Available` means `trackIndices_` is populated and usable.
    [[nodiscard]] AnalysisSessionSampleStatus ensureSampleIndices(QString* errorMessage);

    [[nodiscard]] rules::Mp4SampleTableIndex* findTrackIndex(quint32 trackId) noexcept;

    std::unique_ptr<core::RandomAccessSource> source_;
    QString sourcePath_;
    core::SourcePage initialPage_;
    rules::H264AnnexBDetectionResult formatDetection_;
    rules::AacAdtsDetectionResult aacFormatDetection_;
    rules::Mp4DetectionResult mp4FormatDetection_;
    rules::FormatSelection formatSelection_;
    std::variant<rules::H264AnnexBAnalyzer, rules::AacAdtsAnalyzer, rules::Mp4IsobmffAnalyzer> analyzer_;
    SessionUserState userState_;
    std::unique_ptr<rules::AnalysisCacheOwner> cacheOwner_;
    std::vector<std::future<rules::AnalysisCacheOwnerWriteResult>> pendingCacheWrites_;
    AnalysisSessionCacheStatus cacheStatus_ = AnalysisSessionCacheStatus::Disabled;
    QString cacheErrorMessage_;
    quint64 nextProgressiveCachePageIndex_ = 0;
    bool materializedCacheSubmitted_ = false;
    bool analysisStarted_ = false;
    /// Status of the most recent batch. The analyzer reports a terminal failure
    /// once and then replays it, but its own terminal status is private, so the
    /// session keeps the mapped value to tell an abandoned analysis apart from a
    /// completed one.
    AnalysisBatchStatus lastBatchStatus_ = AnalysisBatchStatus::InProgress;
    std::vector<NavigationFrame> navigationStack_;
    std::unordered_map<QString, SubFormatSession> subFormatSessions_;
    // Declared after `analyzer_` on purpose: each index holds table readers that
    // borrow the analyzer (`mp4_sample_table_extractor.h:52-56`), and members are
    // destroyed in reverse declaration order, so the readers are torn down while
    // the analyzer they read through is still alive.
    std::vector<rules::Mp4SampleTableIndex> trackIndices_;
    bool sampleIndicesBuilt_ = false;
    /// Rule sessions for sample execution, keyed by the resolved entrypoint
    /// identity so parameter sets published by one sample stay visible to the
    /// next sample of the same track.
    std::unordered_map<QString, SubFormatSession> sampleSessions_;
    std::optional<core::SourceFingerprint> initialFingerprint_;
    std::shared_ptr<core::CancellationSource> cancellationSource_;
};

enum class AnalysisSessionRestoreStatus : quint8 {
    Restored,
    SessionDocumentError,
    SourceOpenError,
    SourceFingerprintError,
    SourceFingerprintMismatch,
    RuleLookupError,
    AnalyzerError,
};

struct AnalysisSessionRestoreResult final {
    AnalysisSessionRestoreStatus status = AnalysisSessionRestoreStatus::SessionDocumentError;
    std::unique_ptr<AnalysisSession> session;
    std::optional<SessionDocumentLoadStatus> documentStatus;
    std::optional<rules::RuleCatalogLookupStatus> ruleStatus;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisSessionRestoreStatus::Restored && session != nullptr;
    }
};

} // namespace streamview::app
