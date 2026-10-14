#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/rules/riff_chunk_scanner.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_execution_session.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <vector>

namespace streamview::rules {

enum class WavRiffAnalysisStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    ResourceLimit,
    InvalidRule,
    InvalidBatchSize,
};

struct WavRiffAnalysisBatch final {
    WavRiffAnalysisStatus status = WavRiffAnalysisStatus::InProgress;
    std::vector<core::AnalysisNodeId> recordNodes;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == WavRiffAnalysisStatus::Complete;
    }
};

[[nodiscard]] RulePackageLoadResult loadWavRulePackage();

class WavRiffAnalyzer final {
public:
    [[nodiscard]] static std::optional<WavRiffAnalyzer>
    create(const core::RandomAccessSource& source,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt);

    [[nodiscard]] static std::optional<WavRiffAnalyzer>
    create(const core::RandomAccessSource& source,
           const RuleCatalogLookupResult& resolvedRule,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt);

    WavRiffAnalyzer(const WavRiffAnalyzer&) = delete;
    WavRiffAnalyzer(WavRiffAnalyzer&&) noexcept = default;
    WavRiffAnalyzer& operator=(const WavRiffAnalyzer&) = delete;
    WavRiffAnalyzer& operator=(WavRiffAnalyzer&&) noexcept = default;

    /// Performs incremental batch analysis over the RIFF/WAVE chunk chain.
    /// Work budget accounting follows the length-chain fast path: one
    /// inspected position per chunk header.
    [[nodiscard]] WavRiffAnalysisBatch analyzeBatch(
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
    explicit WavRiffAnalyzer(const core::RandomAccessSource& source,
                             RuleEntryPointIdentity ruleIdentity,
                             DslTypedProgram program,
                             quint32 recordStructIndex,
                             core::AnalysisTree tree,
                             std::optional<core::CancellationToken> cancellation);

    [[nodiscard]] std::optional<core::FieldLocation>
    makeLocation(std::vector<core::SourceSpan> sourceSpans);

    [[nodiscard]] bool publishRecord(const RiffChunkRecord& record,
                                     WavRiffAnalysisBatch& batch,
                                     bool allowExecutionCancellation,
                                     WavRiffAnalysisStatus* failureStatus,
                                     QString* errorMessage);

    void markRootPartial(core::DiagnosticCode code,
                         core::MaterializationState state,
                         const QString& message);

    const core::RandomAccessSource* source_ = nullptr;
    RuleEntryPointIdentity ruleIdentity_;
    RuleExecutionSession executionSession_;
    quint32 recordStructIndex_ = 0;
    RiffChunkScanner scanner_;
    core::AnalysisTree tree_;
    std::optional<core::CancellationToken> cancellation_;
    quint64 nextRecordIndex_ = 0;
    quint64 nextViewId_ = 1;
    bool terminal_ = false;
    WavRiffAnalysisStatus terminalStatus_ = WavRiffAnalysisStatus::InProgress;
    QString terminalErrorMessage_;
};

} // namespace streamview::rules
