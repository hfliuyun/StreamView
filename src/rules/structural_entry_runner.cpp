#include <streamview/rules/structural_entry_runner.h>

#include <streamview/core/bit_reader.h>
#include <streamview/core/bounded_source_view.h>
#include <streamview/rules/dsl_executor.h>

#include <limits>

namespace streamview::rules {

namespace {

[[nodiscard]] DslExecutionResult makeFailure(DslExecutionStatus status, const QString& message) {
    DslExecutionResult res;
    res.status = status;
    res.errorMessage = message;
    return res;
}

} // namespace

StructuralExecutionResult StructuralEntryRunner::execute(
    const core::RandomAccessSource& baseSource,
    const core::SourceMapping& sourceMapping,
    const DslTypedProgram& program,
    const StructuralExecutionOptions& options) {
    StructuralExecutionResult result;

    if (program.entry.kind != DslEntryKind::Structure) {
        result.execution = makeFailure(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Structural entry runner requires DslEntryKind::Structure"));
        return result;
    }

    if (program.entry.targetIndex >= program.structs.size()) {
        result.execution = makeFailure(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Structural entry targetIndex is out of range"));
        return result;
    }

    const quint64 logicalBitLength = sourceMapping.logicalBitLength();
    if (logicalBitLength == 0) {
        result.execution = makeFailure(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Source mapping logical length is zero"));
        return result;
    }

    if ((logicalBitLength % 8U) != 0) {
        result.execution = makeFailure(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Source mapping logical length is not byte-aligned"));
        return result;
    }

    if (sourceMapping.sourceSpans().empty()) {
        result.execution = makeFailure(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Source mapping has no source spans"));
        return result;
    }

    constexpr quint64 maxByteCoordinate = std::numeric_limits<quint64>::max() / 8U;
    const quint64 byteLength = logicalBitLength / 8U;
    if (byteLength > maxByteCoordinate) {
        result.execution = makeFailure(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Source mapping length exceeds coordinate limit"));
        return result;
    }

    for (const auto& span : sourceMapping.sourceSpans()) {
        if (span.start().bitOffsetInByte() != 0 || (span.bitLength() % 8U) != 0) {
            result.execution = makeFailure(
                DslExecutionStatus::InvalidDefinition,
                QStringLiteral("Source mapping spans must be byte-aligned"));
            return result;
        }
    }

    const auto& entryStruct = program.structs.at(program.entry.targetIndex);
    auto treeOpt = core::AnalysisTree::create(entryStruct.name);
    if (!treeOpt) {
        result.execution = makeFailure(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Failed to create analysis tree"));
        return result;
    }

    auto tree = std::make_shared<core::AnalysisTree>(std::move(*treeOpt));
    result.tree = tree;

    core::BitReader reader(baseSource, sourceMapping);

    DslExecutionOptions vmOptions;
    vmOptions.limits = options.limits;
    vmOptions.cancellation = options.cancellation;

    result.execution = DslExecutor::decodeStruct(
        program,
        program.entry.targetIndex,
        reader,
        sourceMapping,
        0,
        *tree,
        tree->rootId(),
        vmOptions);

    return result;
}

} // namespace streamview::rules
