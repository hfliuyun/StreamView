#pragma once

#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/dsl_vm.h>

#include <QString>
#include <QtGlobal>

namespace streamview::rules {

class DslExecutor final {
public:
    [[nodiscard]] static DslExecutionResult
    decodeStruct(const DslTypedProgram& program,
                 quint32 structureIndex,
                 core::BitReader& reader,
                 const core::SourceMapping& mapping,
                 quint64 logicalStart,
                 core::AnalysisTree& tree,
                 core::AnalysisNodeId parentId,
                 const DslExecutionOptions& options = {});

    [[nodiscard]] static DslExecutionResult
    decodeStruct(const DslTypedProgram& program,
                 const QString& structureName,
                 core::BitReader& reader,
                 const core::SourceMapping& mapping,
                 quint64 logicalStart,
                 core::AnalysisTree& tree,
                 core::AnalysisNodeId parentId,
                 const DslExecutionOptions& options = {});

    /// Compatibility entry point that compiles the parsed program before execution.
    [[nodiscard]] static DslExecutionResult
    decodeStruct(const DslProgram& program,
                 const QString& structureName,
                 core::BitReader& reader,
                 const core::SourceMapping& mapping,
                 quint64 logicalStart,
                 core::AnalysisTree& tree,
                 core::AnalysisNodeId parentId,
                 const DslExecutionOptions& options = {});
};

} // namespace streamview::rules
