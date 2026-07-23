#include <streamview/rules/dsl_executor.h>

namespace streamview::rules {

namespace {

[[nodiscard]] DslExecutionResult invalidDefinition(const QString& message) {
    DslExecutionResult result;
    result.status = DslExecutionStatus::InvalidDefinition;
    result.errorMessage = message;
    return result;
}

} // namespace

DslExecutionResult DslExecutor::decodeStruct(
    const DslTypedProgram& program,
    quint32 structureIndex,
    core::BitReader& reader,
    const core::SourceMapping& mapping,
    quint64 logicalStart,
    core::AnalysisTree& tree,
    core::AnalysisNodeId parentId,
    const DslExecutionOptions& options) {
    return DslVirtualMachine::execute(program,
                                      structureIndex,
                                      reader,
                                      mapping,
                                      logicalStart,
                                      tree,
                                      parentId,
                                      options);
}

DslExecutionResult DslExecutor::decodeStruct(
    const DslTypedProgram& program,
    const QString& structureName,
    core::BitReader& reader,
    const core::SourceMapping& mapping,
    quint64 logicalStart,
    core::AnalysisTree& tree,
    core::AnalysisNodeId parentId,
    const DslExecutionOptions& options) {
    const auto structureIndex = program.structureIndex(structureName);
    if (!structureIndex) {
        return invalidDefinition(QStringLiteral("Structure is not declared in typed IR"));
    }
    return decodeStruct(program,
                        *structureIndex,
                        reader,
                        mapping,
                        logicalStart,
                        tree,
                        parentId,
                        options);
}

DslExecutionResult DslExecutor::decodeStruct(
    const DslProgram& program,
    const QString& structureName,
    core::BitReader& reader,
    const core::SourceMapping& mapping,
    quint64 logicalStart,
    core::AnalysisTree& tree,
    core::AnalysisNodeId parentId,
    const DslExecutionOptions& options) {
    const DslCompileResult compiled = DslCompiler::compile(program);
    if (!compiled.succeeded()) {
        const QString message = compiled.diagnostics.empty()
                                    ? QStringLiteral("Unable to compile DSL program")
                                    : compiled.diagnostics.front().message;
        return invalidDefinition(message);
    }
    return decodeStruct(*compiled.program,
                        structureName,
                        reader,
                        mapping,
                        logicalStart,
                        tree,
                        parentId,
                        options);
}

} // namespace streamview::rules
