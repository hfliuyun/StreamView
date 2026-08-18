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

struct CompoundTransactionHooks final {
    // Hooks run before tree nodes enter their terminal success states. Unexpected
    // exceptions fail the compound operation and trigger best-effort rollback.
    std::function<void()> onCommit;
    std::function<void()> onRollback;
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
    CompoundTransactionHooks transactionHooks;
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
    std::vector<std::optional<quint64>> headerFieldValues;
    QString errorMessage;

    [[nodiscard]] bool materialized() const noexcept {
        return status == DslExecutionStatus::Materialized;
    }
};

class CompoundStructuralRunner final {
public:
    [[nodiscard]] static CompoundStructuralExecutionResult execute(
        const DslTypedProgram& program,
        const CompoundStructuralExecutionRequest& request);
};

} // namespace streamview::rules
