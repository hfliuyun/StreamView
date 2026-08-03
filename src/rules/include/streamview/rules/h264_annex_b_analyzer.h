#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/h264_ebsp_rbsp_mapper.h>
#include <streamview/rules/h264_start_code_scanner.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_execution_session.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace streamview::rules {

enum class H264AnnexBAnalysisStatus : quint8 {
    InProgress,
    Complete,
    Cancelled,
    SourceError,
    InvalidBatchSize,
    ResourceLimit,
    InvalidRule,
};

struct H264ProgressiveIndexUpdate final {
    quint64 firstRecordIndex = 0;
    quint64 indexedThroughByteOffset = 0;
    bool endOfSource = false;
    std::vector<H264StartCodeRecord> records;
};

struct H264AnnexBAnalysisBatch final {
    H264AnnexBAnalysisStatus status = H264AnnexBAnalysisStatus::InProgress;
    std::vector<core::AnalysisNodeId> nalUnitNodes;
    std::optional<H264ProgressiveIndexUpdate> progressiveIndexUpdate;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == H264AnnexBAnalysisStatus::Complete;
    }
};

[[nodiscard]] QString h264AnnexBRuleSource(QString* errorMessage = nullptr);
[[nodiscard]] RulePackageLoadResult loadH264AnnexBRulePackage();

class H264AnnexBAnalyzer final {
public:
    [[nodiscard]] static std::optional<H264AnnexBAnalyzer>
    create(const core::RandomAccessSource& source,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt,
           H264EbspRbspMapLimits mapperLimits = {});
    [[nodiscard]] static std::optional<H264AnnexBAnalyzer>
    create(const core::RandomAccessSource& source,
           const RuleCatalogLookupResult& resolvedRule,
           QString* errorMessage = nullptr,
           std::optional<core::CancellationToken> cancellation = std::nullopt,
           H264EbspRbspMapLimits mapperLimits = {});

    H264AnnexBAnalyzer(const H264AnnexBAnalyzer&) = delete;
    H264AnnexBAnalyzer(H264AnnexBAnalyzer&&) noexcept = default;
    H264AnnexBAnalyzer& operator=(const H264AnnexBAnalyzer&) = delete;
    H264AnnexBAnalyzer& operator=(H264AnnexBAnalyzer&&) noexcept = default;

    [[nodiscard]] H264AnnexBAnalysisBatch analyzeBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = H264StartCodeScanner::defaultWorkBudget(),
        quint64 maximumMappedBytes = H264EbspRbspMapper::defaultWorkBudget());
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
    struct QueuedRecord final {
        H264StartCodeRecord record;
        bool allowExecutionCancellation = false;
    };

    struct PendingNalUnit final {
        H264StartCodeRecord record;
        core::AnalysisNodeId node;
        quint64 index = 0;
        H264EbspRbspMapper mapper;
        std::optional<DslTypedPayloadCase> payloadCase;
        bool allowExecutionCancellation = false;
    };

    H264AnnexBAnalyzer(const core::RandomAccessSource& source,
                      std::optional<core::CancellationToken> cancellation,
                      H264EbspRbspMapLimits mapperLimits,
                      RuleEntryPointIdentity ruleIdentity,
                      DslTypedProgram program,
                      quint32 elementStructIndex,
                      core::AnalysisTree tree);

    [[nodiscard]] std::optional<core::FieldLocation>
    makeLocation(std::vector<core::SourceSpan> sourceSpans);
    [[nodiscard]] bool publishRecord(const H264StartCodeRecord& record,
                                     H264AnnexBAnalysisBatch& batch,
                                     bool allowExecutionCancellation,
                                     H264AnnexBAnalysisStatus* failureStatus,
                                     QString* errorMessage);
    [[nodiscard]] bool appendTrailingZeroRegion(const H264StartCodeRecord& record,
                                                core::AnalysisNodeId nalNode,
                                                QString* errorMessage);
    [[nodiscard]] std::optional<DslTypedPayloadCase> payloadCaseFor(quint64 nalUnitType) const;
    [[nodiscard]] bool decodePayloadStructure(PendingNalUnit& pending,
                                              core::AnalysisNodeId rbspNode,
                                              const QString& rbspPath,
                                              bool* payloadDecoded,
                                              H264AnnexBAnalysisStatus* failureStatus,
                                              QString* errorMessage);
    [[nodiscard]] bool finishPendingNalUnit(const H264EbspRbspMapBatch& mapBatch,
                                            H264AnnexBAnalysisBatch& batch,
                                            H264AnnexBAnalysisStatus* failureStatus,
                                            QString* errorMessage);
    void markRootPartial(core::DiagnosticCode code,
                         core::MaterializationState state,
                         const QString& message);

    const core::RandomAccessSource* source_ = nullptr;
    H264StartCodeScanner scanner_;
    std::optional<core::CancellationToken> cancellation_;
    H264EbspRbspMapLimits mapperLimits_;
    RuleEntryPointIdentity ruleIdentity_;
    quint32 elementStructIndex_ = 0;
    core::AnalysisTree tree_;
    RuleExecutionSession executionSession_;
    std::deque<QueuedRecord> queuedRecords_;
    std::optional<PendingNalUnit> pendingNalUnit_;
    std::optional<StartCodeScanStatus> deferredScanStatus_;
    QString deferredScanErrorMessage_;
    quint64 nextViewId_ = 1;
    quint64 nextNalUnitIndex_ = 0;
    quint64 nextStableRecordIndex_ = 0;
    quint64 stableIndexedThroughByteOffset_ = 0;
    bool terminal_ = false;
    H264AnnexBAnalysisStatus terminalStatus_ = H264AnnexBAnalysisStatus::InProgress;
    QString terminalErrorMessage_;
};

} // namespace streamview::rules
