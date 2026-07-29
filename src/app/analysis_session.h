#pragma once

#include "session_document.h"

#include <streamview/core/analysis_model.h>
#include <streamview/core/source.h>
#include <streamview/core/source_pager.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/h264_annex_b_detector.h>
#include <streamview/rules/rule_catalog.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <memory>

namespace streamview::app {

struct AnalysisSessionRestoreResult;

class AnalysisSession final {
public:
    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    openFile(const QString& path, QString* errorMessage = nullptr);

    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    create(std::unique_ptr<core::RandomAccessSource> source,
           QString* errorMessage = nullptr);
    [[nodiscard]] static AnalysisSessionRestoreResult
    restoreSession(const QString& sessionPath, const rules::RulePackageCatalog& catalog);

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

    [[nodiscard]] rules::H264AnnexBAnalysisBatch analyzeBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = rules::H264StartCodeScanner::defaultWorkBudget());
    [[nodiscard]] const core::AnalysisTree& tree() const noexcept { return analyzer_.tree(); }
    [[nodiscard]] bool finished() const noexcept { return analyzer_.finished(); }
    [[nodiscard]] quint64 scanCursor() const noexcept { return analyzer_.scanCursor(); }
    [[nodiscard]] const rules::RuleEntryPointIdentity& ruleIdentity() const noexcept {
        return analyzer_.ruleIdentity();
    }
    [[nodiscard]] const SessionUserState& userState() const noexcept { return userState_; }
    [[nodiscard]] bool saveSession(const QString& sessionPath,
                                   const SessionUserState& userState,
                                   QString* errorMessage = nullptr) const;

private:
    AnalysisSession(std::unique_ptr<core::RandomAccessSource> source,
                    QString sourcePath,
                    core::SourcePage initialPage,
                    rules::H264AnnexBDetectionResult formatDetection,
                    rules::H264AnnexBAnalyzer analyzer,
                    SessionUserState userState);
    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    createPrepared(std::unique_ptr<core::RandomAccessSource> source,
                   QString sourcePath,
                   const rules::RuleCatalogLookupResult* resolvedRule,
                   SessionUserState userState,
                   QString* errorMessage);

    std::unique_ptr<core::RandomAccessSource> source_;
    QString sourcePath_;
    core::SourcePage initialPage_;
    rules::H264AnnexBDetectionResult formatDetection_;
    rules::H264AnnexBAnalyzer analyzer_;
    SessionUserState userState_;
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
