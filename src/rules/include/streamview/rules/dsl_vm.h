#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/bit_reader.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/rules/dsl_ir.h>

#include <QString>
#include <QtGlobal>

#include <optional>

namespace streamview::rules {

enum class DslExecutionStatus : quint8 {
    Materialized,
    TruncatedSource,
    InvalidSyntax,
    SourceError,
    Cancelled,
    ResourceLimit,
    InvalidDefinition,
};

struct DslExecutionLimits final {
    [[nodiscard]] static constexpr quint64 defaultMaximumInstructions() noexcept {
        return 1'000'000;
    }
    [[nodiscard]] static constexpr quint32 defaultMaximumCallDepth() noexcept { return 64; }
    [[nodiscard]] static constexpr quint32 defaultMaximumViewDepth() noexcept { return 64; }
    [[nodiscard]] static constexpr quint32 defaultMaximumNodeDepth() noexcept { return 256; }
    [[nodiscard]] static constexpr quint64 defaultMaximumMaterializedNodes() noexcept {
        return 100'000;
    }
    [[nodiscard]] static constexpr quint64 defaultCancellationCheckInterval() noexcept {
        return 1'024;
    }

    quint64 maximumInstructions = defaultMaximumInstructions();
    quint32 maximumCallDepth = defaultMaximumCallDepth();
    quint32 maximumViewDepth = defaultMaximumViewDepth();
    quint32 maximumNodeDepth = defaultMaximumNodeDepth();
    quint64 maximumMaterializedNodes = defaultMaximumMaterializedNodes();
    quint64 cancellationCheckInterval = defaultCancellationCheckInterval();
};

struct DslExecutionOptions final {
    DslExecutionLimits limits;
    std::optional<core::CancellationToken> cancellation;
};

struct DslExecutionResult final {
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
    std::optional<core::AnalysisNodeId> structureNode;
    quint64 bitsConsumed = 0;
    quint64 instructionsExecuted = 0;
    quint64 nodesCreated = 0;
    QString errorMessage;

    [[nodiscard]] bool materialized() const noexcept {
        return status == DslExecutionStatus::Materialized;
    }
};

class DslVirtualMachine final {
public:
    [[nodiscard]] static DslExecutionResult execute(
        const DslTypedProgram& program,
        quint32 structureIndex,
        core::BitReader& reader,
        const core::SourceMapping& mapping,
        quint64 logicalStart,
        core::AnalysisTree& tree,
        core::AnalysisNodeId parentId,
        const DslExecutionOptions& options = {});
};

} // namespace streamview::rules
