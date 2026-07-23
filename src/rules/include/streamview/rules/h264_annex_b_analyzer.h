#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/h264_start_code_scanner.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <vector>

namespace streamview::rules {

enum class H264AnnexBAnalysisStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    InvalidBatchSize,
    InvalidRule,
};

struct H264AnnexBAnalysisBatch final {
    H264AnnexBAnalysisStatus status = H264AnnexBAnalysisStatus::InProgress;
    std::vector<core::AnalysisNodeId> nalUnitNodes;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == H264AnnexBAnalysisStatus::Complete;
    }
};

[[nodiscard]] QString h264AnnexBRuleSource(QString* errorMessage = nullptr);

class H264AnnexBAnalyzer final {
public:
    [[nodiscard]] static std::optional<H264AnnexBAnalyzer>
    create(const core::RandomAccessSource& source,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt);

    H264AnnexBAnalyzer(const H264AnnexBAnalyzer&) = delete;
    H264AnnexBAnalyzer(H264AnnexBAnalyzer&&) noexcept = default;
    H264AnnexBAnalyzer& operator=(const H264AnnexBAnalyzer&) = delete;
    H264AnnexBAnalyzer& operator=(H264AnnexBAnalyzer&&) noexcept = default;

    [[nodiscard]] H264AnnexBAnalysisBatch analyzeBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = H264StartCodeScanner::defaultWorkBudget());

    [[nodiscard]] const core::AnalysisTree& tree() const noexcept { return tree_; }
    [[nodiscard]] bool finished() const noexcept { return terminal_; }
    [[nodiscard]] quint64 scanCursor() const noexcept { return scanner_.cursor(); }

private:
    H264AnnexBAnalyzer(const core::RandomAccessSource& source,
                      std::optional<core::CancellationToken> cancellation,
                      DslProgram program,
                      QString elementType,
                      core::AnalysisTree tree);

    [[nodiscard]] std::optional<core::FieldLocation>
    makeLocation(std::vector<core::SourceSpan> sourceSpans);
    [[nodiscard]] bool publishRecord(const H264StartCodeRecord& record,
                                     H264AnnexBAnalysisBatch& batch,
                                     H264AnnexBAnalysisStatus* failureStatus,
                                     QString* errorMessage);
    void markRootPartial(core::DiagnosticCode code,
                         core::MaterializationState state,
                         const QString& message);

    const core::RandomAccessSource* source_ = nullptr;
    H264StartCodeScanner scanner_;
    DslProgram program_;
    QString elementType_;
    core::AnalysisTree tree_;
    quint64 nextViewId_ = 1;
    quint64 nextNalUnitIndex_ = 0;
    bool terminal_ = false;
    H264AnnexBAnalysisStatus terminalStatus_ = H264AnnexBAnalysisStatus::InProgress;
    QString terminalErrorMessage_;
};

} // namespace streamview::rules
