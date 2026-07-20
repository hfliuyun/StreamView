#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/rules/dsl.h>

#include <QString>
#include <QtGlobal>

#include <optional>

namespace streamview::rules {

enum class DslExecutionStatus : quint8 {
    Materialized,
    TruncatedSource,
    InvalidSyntax,
    SourceError,
    InvalidDefinition,
};

struct DslExecutionResult final {
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
    std::optional<core::AnalysisNodeId> structureNode;
    quint64 bitsConsumed = 0;
    QString errorMessage;

    [[nodiscard]] bool materialized() const noexcept {
        return status == DslExecutionStatus::Materialized;
    }
};

class DslExecutor final {
public:
    [[nodiscard]] static DslExecutionResult
    decodeStruct(const DslProgram& program,
                 const QString& structureName,
                 core::BitReader& reader,
                 const core::SourceMapping& mapping,
                 quint64 logicalStart,
                 core::AnalysisTree& tree,
                 core::AnalysisNodeId parentId);
};

} // namespace streamview::rules
