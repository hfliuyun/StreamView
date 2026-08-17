#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/rules/mp4_box_scanner.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_execution_session.h>
#include <streamview/rules/window_decoder.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace streamview::rules {

enum class Mp4IsobmffAnalysisStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    ResourceLimit,
    InvalidRule,
    InvalidBatchSize,
};

struct Mp4IsobmffAnalysisBatch final {
    Mp4IsobmffAnalysisStatus status = Mp4IsobmffAnalysisStatus::InProgress;
    std::vector<core::AnalysisNodeId> boxNodes;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == Mp4IsobmffAnalysisStatus::Complete;
    }
};

class Mp4IsobmffAnalyzer final {
public:
    [[nodiscard]] static std::optional<Mp4IsobmffAnalyzer>
    create(const core::RandomAccessSource& source,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt);

    [[nodiscard]] static std::optional<Mp4IsobmffAnalyzer>
    create(const core::RandomAccessSource& source,
           const RuleCatalogLookupResult& resolvedRule,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt);

    Mp4IsobmffAnalyzer(const Mp4IsobmffAnalyzer&) = delete;
    Mp4IsobmffAnalyzer(Mp4IsobmffAnalyzer&&) noexcept = default;
    Mp4IsobmffAnalyzer& operator=(const Mp4IsobmffAnalyzer&) = delete;
    Mp4IsobmffAnalyzer& operator=(Mp4IsobmffAnalyzer&&) noexcept = default;

    [[nodiscard]] Mp4IsobmffAnalysisBatch analyzeBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = 256U * 1024U);

    [[nodiscard]] bool resumeAfterCancellation(
        std::optional<core::CancellationToken> cancellation = std::nullopt,
        QString* errorMessage = nullptr);

    [[nodiscard]] const core::AnalysisTree& tree() const noexcept { return *tree_; }
    [[nodiscard]] bool finished() const noexcept { return terminal_; }
    [[nodiscard]] quint64 scanCursor() const noexcept { return scanner_.cursor(); }
    [[nodiscard]] const RuleEntryPointIdentity& ruleIdentity() const noexcept {
        return ruleIdentity_;
    }
    [[nodiscard]] std::shared_ptr<RunnerExecutionBudget> budget() const noexcept {
        return budget_;
    }
    [[nodiscard]] std::optional<WindowDecoder> windowDecoder(core::AnalysisNodeId windowNodeId) const;

private:
    explicit Mp4IsobmffAnalyzer(const core::RandomAccessSource& source,
                                RuleEntryPointIdentity ruleIdentity,
                                DslTypedProgram program,
                                quint32 headerStructIndex,
                                std::shared_ptr<core::AnalysisTree> tree,
                                std::shared_ptr<RunnerExecutionBudget> budget,
                                std::optional<core::CancellationToken> cancellation);

    [[nodiscard]] std::optional<core::FieldLocation>
    makeLocation(std::vector<core::SourceSpan> sourceSpans);

    [[nodiscard]] bool publishRecord(const Mp4BoxRecord& record,
                                     Mp4IsobmffAnalysisBatch& batch,
                                     Mp4IsobmffAnalysisStatus* failureStatus,
                                     QString* errorMessage);

    [[nodiscard]] bool recursivelyDrillContainer(core::AnalysisNodeId containerNodeId,
                                                Mp4IsobmffAnalysisStatus* failureStatus,
                                                QString* errorMessage);

    void markRootPartial(core::DiagnosticCode code,
                         core::MaterializationState state,
                         const QString& message);

    const core::RandomAccessSource* source_ = nullptr;
    RuleEntryPointIdentity ruleIdentity_;
    DslTypedProgram program_;
    quint32 headerStructIndex_ = 0;
    Mp4BoxScanner scanner_;
    std::shared_ptr<core::AnalysisTree> tree_;
    std::shared_ptr<RunnerExecutionBudget> budget_;
    std::optional<core::CancellationToken> cancellation_;
    struct QueuedRecord final {
        Mp4BoxRecord record;
    };
    std::deque<QueuedRecord> queuedRecords_;
    std::optional<Mp4BoxScanStatus> deferredScanStatus_;
    QString deferredScanErrorMessage_;
    mutable std::unordered_map<quint64, std::shared_ptr<WindowDecoder::State>>
        windowDecoderStates_;
    quint64 nextBoxIndex_ = 0;
    quint64 nextViewId_ = 1;
    bool terminal_ = false;
    Mp4IsobmffAnalysisStatus terminalStatus_ = Mp4IsobmffAnalysisStatus::InProgress;
    QString terminalErrorMessage_;
};

} // namespace streamview::rules
