#pragma once

#include "session_document.h"

#include <streamview/core/analysis_model.h>
#include <streamview/core/source.h>
#include <streamview/core/source_pager.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/aac_adts_detector.h>
#include <streamview/rules/analysis_cache_owner.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/h264_annex_b_detector.h>
#include <streamview/rules/mp4_box_detector.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/rule_catalog.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <future>
#include <memory>
#include <optional>
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
    void enableCache(AnalysisSessionCacheOptions cacheOptions);
    [[nodiscard]] bool saveSession(const QString& sessionPath,
                                   const SessionUserState& userState,
                                   QString* errorMessage = nullptr) const;

private:
    AnalysisSession(std::unique_ptr<core::RandomAccessSource> source,
                    QString sourcePath,
                    core::SourcePage initialPage,
                    rules::H264AnnexBDetectionResult formatDetection,
                    rules::AacAdtsDetectionResult aacFormatDetection,
                    rules::Mp4DetectionResult mp4FormatDetection,
                    std::variant<rules::H264AnnexBAnalyzer, rules::AacAdtsAnalyzer, rules::Mp4IsobmffAnalyzer> analyzer,
                    SessionUserState userState,
                    std::unique_ptr<rules::AnalysisCacheOwner> cacheOwner,
                    AnalysisSessionCacheStatus cacheStatus,
                    QString cacheErrorMessage);
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

    std::unique_ptr<core::RandomAccessSource> source_;
    QString sourcePath_;
    core::SourcePage initialPage_;
    rules::H264AnnexBDetectionResult formatDetection_;
    rules::AacAdtsDetectionResult aacFormatDetection_;
    rules::Mp4DetectionResult mp4FormatDetection_;
    std::variant<rules::H264AnnexBAnalyzer, rules::AacAdtsAnalyzer, rules::Mp4IsobmffAnalyzer> analyzer_;
    SessionUserState userState_;
    std::unique_ptr<rules::AnalysisCacheOwner> cacheOwner_;
    std::vector<std::future<rules::AnalysisCacheOwnerWriteResult>> pendingCacheWrites_;
    AnalysisSessionCacheStatus cacheStatus_ = AnalysisSessionCacheStatus::Disabled;
    QString cacheErrorMessage_;
    quint64 nextProgressiveCachePageIndex_ = 0;
    bool materializedCacheSubmitted_ = false;
    bool analysisStarted_ = false;
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
