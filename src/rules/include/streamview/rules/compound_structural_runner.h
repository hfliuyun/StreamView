#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/dsl_vm.h>
#include <streamview/rules/payload_transform.h>

#include <QString>
#include <QtGlobal>

#include <functional>
#include <optional>
#include <vector>

namespace streamview::rules {

struct CompoundStructuralExecutionResult;

struct CompoundTransactionFailure final {
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
    QString errorMessage;
};

struct CompoundTransactionHooks final {
    // Hooks run before tree nodes enter their terminal success states. Unexpected
    // exceptions fail the compound operation and trigger best-effort rollback.
    std::function<void(const CompoundStructuralExecutionResult&)> onCommitWithResult;
    std::function<void()> onCommit;
    std::function<void()> onRollback;
    std::function<std::optional<CompoundTransactionFailure>(
        const CompoundStructuralExecutionResult&)>
        onPrepareCommit;
};

struct CompoundStructuralExecutionRequest final {
    const core::RandomAccessSource* source = nullptr;
    const core::SourceMapping* headerMapping = nullptr;
    quint32 headerStructureIndex = 0;

    std::optional<quint32> payloadStructureIndex;
    const core::SourceMapping* payloadMapping = nullptr;
    quint64 payloadLogicalStart = 0;

    QString transformProviderId = QStringLiteral("none");
    const PayloadTransformRegistry* transformRegistry = nullptr;

    core::AnalysisTree* tree = nullptr;
    core::AnalysisNodeId parentId;

    DslExecutionOptions options;
    bool requireExactConsumption = true;
    // Kept as the source-compatible fallback when both phases share one resolver.
    DslContextValueResolver contextValueResolver;
    CompoundTransactionHooks transactionHooks;
    DslContextValueResolver headerContextValueResolver;
    DslContextValueResolver payloadContextValueResolver;
    bool autoDispatchPayload = false;
    std::function<DslContextValueResolver(quint32)> payloadContextResolverFactory;
    bool inspectionBudgetExhausted = false;
};

struct CompoundStructuralExecutionResult final {
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
    std::optional<core::AnalysisNodeId> headerNodeId;
    std::optional<core::AnalysisNodeId> payloadNodeId;
    quint64 headerBitsConsumed = 0;
    quint64 payloadBitsConsumed = 0;
    quint64 instructionsExecuted = 0;
    quint64 nodesCreated = 0;
    quint64 inspectedByteCount = 0;
    std::vector<PayloadExcludedSpan> excludedSpans;
    std::optional<core::SourceMapping> forwardedPayloadMapping;
    std::vector<core::ParseDiagnostic> transformDiagnostics;
    std::vector<std::optional<quint64>> headerFieldValues;
    std::vector<std::optional<quint64>> payloadFieldValues;
    std::optional<DslExecutionContextValues> headerContextValues;
    std::optional<DslExecutionContextValues> payloadContextValues;
    std::vector<DslExecutionContextImport> headerContextImports;
    std::vector<DslExecutionContextImport> payloadContextImports;
    QString errorMessage;
    std::optional<quint32> selectedPayloadStructureIndex;
    std::optional<quint64> selectedPayloadCaseValue;

    [[nodiscard]] bool materialized() const noexcept {
        return status == DslExecutionStatus::Materialized;
    }
};

class CompoundStructuralRunner final {
  public:
    [[nodiscard]] static CompoundStructuralExecutionResult
    execute(const DslTypedProgram& program, const CompoundStructuralExecutionRequest& request);
};

} // namespace streamview::rules
