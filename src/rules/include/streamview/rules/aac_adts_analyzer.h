#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/rules/aac_adts_scanner.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_execution_session.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <vector>

namespace streamview::rules {

enum class AacAdtsAnalysisStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    ResourceLimit,
    InvalidRule,
    InvalidBatchSize,
};

struct AacAdtsAnalysisBatch final {
    AacAdtsAnalysisStatus status = AacAdtsAnalysisStatus::InProgress;
    std::vector<core::AnalysisNodeId> frameNodes;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == AacAdtsAnalysisStatus::Complete;
    }
};

class AacAdtsAnalyzer final {
public:
    [[nodiscard]] static std::optional<AacAdtsAnalyzer>
    create(const core::RandomAccessSource& source,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt);

    [[nodiscard]] static std::optional<AacAdtsAnalyzer>
    create(const core::RandomAccessSource& source,
           const RuleCatalogLookupResult& resolvedRule,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt);

    AacAdtsAnalyzer(const AacAdtsAnalyzer&) = delete;
    AacAdtsAnalyzer(AacAdtsAnalyzer&&) noexcept = default;
    AacAdtsAnalyzer& operator=(const AacAdtsAnalyzer&) = delete;
    AacAdtsAnalyzer& operator=(AacAdtsAnalyzer&&) noexcept = default;

    /**
     * @brief Performs incremental batch analysis on the ADTS stream.
     *
     * Work budget accounting semantics:
     * - Fast path (in-sync): advances by frame header bytes per inspected frame.
     * - Resynchronization path: advances byte-by-byte for each byte searched during sync recovery.
     */
    [[nodiscard]] AacAdtsAnalysisBatch analyzeBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = 256U * 1024U);

    [[nodiscard]] bool resumeAfterCancellation(
        std::optional<core::CancellationToken> cancellation = std::nullopt,
        QString* errorMessage = nullptr);

    [[nodiscard]] const core::AnalysisTree& tree() const noexcept { return tree_; }
    [[nodiscard]] bool finished() const noexcept { return terminal_; }
    [[nodiscard]] quint64 scanCursor() const noexcept { return scanner_.cursor(); }
    [[nodiscard]] const RuleEntryPointIdentity& ruleIdentity() const noexcept {
        return ruleIdentity_;
    }

private:
    explicit AacAdtsAnalyzer(const core::RandomAccessSource& source,
                             RuleEntryPointIdentity ruleIdentity,
                             DslTypedProgram program,
                             quint32 headerStructIndex,
                             core::AnalysisTree tree,
                             std::optional<core::CancellationToken> cancellation);

    [[nodiscard]] std::optional<core::FieldLocation>
    makeLocation(std::vector<core::SourceSpan> sourceSpans);

    [[nodiscard]] bool publishRecord(const AacAdtsRecord& record,
                                     AacAdtsAnalysisBatch& batch,
                                     bool allowExecutionCancellation,
                                     AacAdtsAnalysisStatus* failureStatus,
                                     QString* errorMessage);

    void markRootPartial(core::DiagnosticCode code,
                         core::MaterializationState state,
                         const QString& message);

    const core::RandomAccessSource* source_ = nullptr;
    RuleEntryPointIdentity ruleIdentity_;
    RuleExecutionSession executionSession_;
    quint32 headerStructIndex_ = 0;
    AacAdtsScanner scanner_;
    core::AnalysisTree tree_;
    std::optional<core::CancellationToken> cancellation_;
    quint64 nextFrameIndex_ = 0;
    quint64 nextViewId_ = 1;
    bool terminal_ = false;
    AacAdtsAnalysisStatus terminalStatus_ = AacAdtsAnalysisStatus::InProgress;
    QString terminalErrorMessage_;
};

} // namespace streamview::rules
