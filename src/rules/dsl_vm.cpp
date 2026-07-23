#include <streamview/rules/dsl_vm.h>

#include <algorithm>
#include <cstddef>
#include <limits>

namespace streamview::rules {

namespace {

[[nodiscard]] bool addWouldOverflow(quint64 left, quint64 right) noexcept {
    return right > std::numeric_limits<quint64>::max() - left;
}

[[nodiscard]] DslExecutionStatus statusForRead(core::BitReadStatus status) noexcept {
    switch (status) {
    case core::BitReadStatus::EndOfRange:
    case core::BitReadStatus::EndOfSource:
        return DslExecutionStatus::TruncatedSource;
    case core::BitReadStatus::SourceError:
        return DslExecutionStatus::SourceError;
    case core::BitReadStatus::InvalidBitCount:
        return DslExecutionStatus::InvalidDefinition;
    case core::BitReadStatus::Complete:
        return DslExecutionStatus::Materialized;
    }
    return DslExecutionStatus::InvalidDefinition;
}

[[nodiscard]] core::DiagnosticCode diagnosticForStatus(DslExecutionStatus status) noexcept {
    switch (status) {
    case DslExecutionStatus::SourceError:
        return core::DiagnosticCode::SourceError;
    case DslExecutionStatus::Cancelled:
        return core::DiagnosticCode::Cancelled;
    case DslExecutionStatus::ResourceLimit:
        return core::DiagnosticCode::ResourceLimit;
    case DslExecutionStatus::InvalidSyntax:
    case DslExecutionStatus::InvalidDefinition:
        return core::DiagnosticCode::InvalidSyntax;
    case DslExecutionStatus::TruncatedSource:
    case DslExecutionStatus::Materialized:
        return core::DiagnosticCode::TruncatedSource;
    }
    return core::DiagnosticCode::InvalidSyntax;
}

[[nodiscard]] std::optional<core::FieldLocation> locationAt(
    const core::SourceMapping& mapping,
    quint64 logicalStart,
    quint64 position,
    quint64 availableBits,
    quint64 requestedBits) {
    if (addWouldOverflow(logicalStart, position)) {
        return std::nullopt;
    }
    const auto range = core::LogicalRange::create(
        core::LogicalBitAddress(mapping.viewId(), logicalStart + position),
        std::min(availableBits, requestedBits));
    return range ? mapping.locate(*range) : std::nullopt;
}

[[nodiscard]] quint32 nodeDepth(const core::AnalysisTree& tree,
                                core::AnalysisNodeId id) noexcept {
    quint32 depth = 0;
    auto current = tree.node(id);
    while (current) {
        if (depth == std::numeric_limits<quint32>::max()) {
            return depth;
        }
        ++depth;
        current = current->parentId() ? tree.node(*current->parentId()) : std::nullopt;
    }
    return depth;
}

} // namespace

DslExecutionResult DslVirtualMachine::execute(
    const DslTypedProgram& program,
    quint32 structureIndex,
    core::BitReader& reader,
    const core::SourceMapping& mapping,
    quint64 logicalStart,
    core::AnalysisTree& tree,
    core::AnalysisNodeId parentId,
    const DslExecutionOptions& options) {
    DslExecutionResult result;
    if (structureIndex >= program.structs.size()) {
        result.errorMessage = QStringLiteral("Typed IR structure index is out of range");
        return result;
    }

    const DslTypedStruct& structure = program.structs.at(structureIndex);
    const auto markFailure = [&](DslExecutionStatus status,
                                 const QString& message,
                                 const DslTypedField* field) {
        result.status = status;
        result.errorMessage = message;
        core::ParseDiagnostic diagnostic;
        diagnostic.code = diagnosticForStatus(status);
        diagnostic.severity = core::DiagnosticSeverity::Error;
        diagnostic.message = message;
        diagnostic.fieldPath = structure.name;
        quint64 requestedBits = 0;
        if (field != nullptr) {
            diagnostic.fieldPath += QLatin1Char('.') + field->name;
            requestedBits = field->type.bitWidth;
        }
        diagnostic.location = locationAt(mapping,
                                          logicalStart,
                                          reader.position(),
                                          reader.remainingBits(),
                                          requestedBits);
        const auto state = status == DslExecutionStatus::Cancelled
                               ? core::MaterializationState::Cancelled
                               : core::MaterializationState::Invalid;
        if (result.structureNode) {
            (void)tree.markPartial(*result.structureNode, state, std::move(diagnostic));
        } else {
            (void)tree.markPartial(parentId, state, std::move(diagnostic));
        }
    };

    if (options.limits.maximumCallDepth == 0 || options.limits.maximumViewDepth == 0 ||
        options.limits.maximumNodeDepth == 0 || options.limits.maximumMaterializedNodes == 0 ||
        options.limits.maximumInstructions == 0 ||
        options.limits.cancellationCheckInterval == 0) {
        markFailure(DslExecutionStatus::ResourceLimit,
                    QStringLiteral("DSL execution limits must be greater than zero"),
                    nullptr);
        return result;
    }

    const quint32 parentDepth = nodeDepth(tree, parentId);
    if (parentDepth == 0) {
        result.errorMessage = QStringLiteral("DSL analysis parent node is invalid");
        return result;
    }
    if (parentDepth >= options.limits.maximumNodeDepth) {
        markFailure(DslExecutionStatus::ResourceLimit,
                    QStringLiteral("DSL analysis node depth limit exceeded"),
                    nullptr);
        return result;
    }

    if (structure.bytecodeOffset > program.bytecode.size() ||
        structure.bytecodeLength > program.bytecode.size() - structure.bytecodeOffset) {
        result.errorMessage = QStringLiteral("Typed IR bytecode range is invalid");
        return result;
    }

    const auto consumeInstruction = [&]() -> bool {
        if (result.instructionsExecuted >= options.limits.maximumInstructions) {
            markFailure(DslExecutionStatus::ResourceLimit,
                        QStringLiteral("DSL instruction budget exceeded"),
                        nullptr);
            return false;
        }
        if (result.instructionsExecuted % options.limits.cancellationCheckInterval == 0 &&
            options.cancellation && options.cancellation->isCancellationRequested()) {
            markFailure(DslExecutionStatus::Cancelled,
                        QStringLiteral("DSL execution was cancelled"),
                        nullptr);
            return false;
        }
        ++result.instructionsExecuted;
        return true;
    };

    const std::size_t begin = structure.bytecodeOffset;
    const std::size_t end = begin + structure.bytecodeLength;
    std::optional<quint32> lastField;
    std::optional<quint64> lastValue;
    quint32 nextFieldIndex = 0;
    bool ended = false;

    for (std::size_t programCounter = begin; programCounter < end; ++programCounter) {
        const DslInstruction& instruction = program.bytecode.at(programCounter);
        if (!consumeInstruction()) {
            return result;
        }
        switch (instruction.opcode) {
        case DslOpcode::BeginStructure: {
            if (result.structureNode || instruction.operand != structureIndex) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR begin instruction is invalid"),
                            nullptr);
                return result;
            }
            if (result.nodesCreated >= options.limits.maximumMaterializedNodes) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL materialized-node budget exceeded"),
                            nullptr);
                return result;
            }
            core::AnalysisNodeSpec spec;
            spec.kind = core::AnalysisNodeKind::Structure;
            spec.name = structure.name;
            spec.state = core::MaterializationState::Indexing;
            spec.metadata = structure.metadata;
            result.structureNode = tree.appendChild(parentId, std::move(spec));
            if (!result.structureNode) {
                result.status = DslExecutionStatus::InvalidDefinition;
                result.errorMessage = QStringLiteral("Unable to append typed structure node");
                return result;
            }
            ++result.nodesCreated;
            break;
        }
        case DslOpcode::ReadUnsignedBits: {
            if (!result.structureNode || instruction.operand != nextFieldIndex ||
                instruction.operand >= structure.fields.size()) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR field instruction is invalid"),
                            nullptr);
                return result;
            }
            const DslTypedField& field = structure.fields.at(instruction.operand);
            const quint32 structureDepth = parentDepth + 1U;
            if (structureDepth >= options.limits.maximumNodeDepth) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL analysis node depth limit exceeded"),
                            &field);
                return result;
            }
            if (result.nodesCreated >= options.limits.maximumMaterializedNodes) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL materialized-node budget exceeded"),
                            &field);
                return result;
            }
            const quint64 fieldStart = reader.position();
            const core::BitReadResult readResult = reader.readBits(field.type.bitWidth);
            result.bitsConsumed = reader.position();
            if (!readResult.complete()) {
                const DslExecutionStatus status = statusForRead(readResult.status);
                markFailure(status,
                            readResult.errorMessage.isEmpty()
                                ? QStringLiteral("Unable to read complete syntax field")
                                : readResult.errorMessage,
                            &field);
                return result;
            }
            if (addWouldOverflow(logicalStart, fieldStart)) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Logical field offset overflow"),
                            &field);
                return result;
            }
            const auto location = locationAt(mapping,
                                             logicalStart,
                                             fieldStart,
                                             field.type.bitWidth,
                                             field.type.bitWidth);
            const quint64 readerStart = reader.range().start().absoluteBitOffset();
            if (addWouldOverflow(readerStart, fieldStart) || !location ||
                location->sourceSpans().size() != 1 ||
                location->sourceSpans().front().start().absoluteBitOffset() !=
                    readerStart + fieldStart ||
                location->sourceSpans().front().bitLength() != field.type.bitWidth) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral(
                                "Minimum DSL executor requires a contiguous direct source mapping"),
                            &field);
                return result;
            }
            core::AnalysisNodeSpec fieldSpec;
            fieldSpec.kind = core::AnalysisNodeKind::SyntaxField;
            fieldSpec.name = field.name;
            fieldSpec.state = core::MaterializationState::Materialized;
            fieldSpec.value = QVariant::fromValue<qulonglong>(readResult.value);
            fieldSpec.location = *location;
            fieldSpec.metadata = field.metadata;
            if (!tree.appendChild(*result.structureNode, std::move(fieldSpec))) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to append typed field node"),
                            &field);
                return result;
            }
            ++result.nodesCreated;
            lastField = instruction.operand;
            lastValue = readResult.value;
            ++nextFieldIndex;
            break;
        }
        case DslOpcode::AssertEquals: {
            const DslTypedField* field = instruction.operand < structure.fields.size()
                                             ? &structure.fields.at(instruction.operand)
                                             : nullptr;
            if (!result.structureNode || !lastField || !lastValue ||
                *lastField != instruction.operand || field == nullptr ||
                !field->equalsConstraint || *field->equalsConstraint != instruction.immediate) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR equality instruction is invalid"),
                            nullptr);
                return result;
            }
            if (*lastValue == instruction.immediate) {
                break;
            }
            result.status = DslExecutionStatus::InvalidSyntax;
            result.errorMessage = QStringLiteral("Field value violates @equals constraint");
            core::ParseDiagnostic diagnostic;
            diagnostic.code = core::DiagnosticCode::InvalidSyntax;
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = result.errorMessage;
            diagnostic.fieldPath = structure.name + QLatin1Char('.') + field->name;
            diagnostic.location = locationAt(mapping,
                                             logicalStart,
                                             reader.position() - field->type.bitWidth,
                                             field->type.bitWidth,
                                             field->type.bitWidth);
            (void)tree.markPartial(*result.structureNode,
                                   core::MaterializationState::Invalid,
                                   std::move(diagnostic));
            return result;
        }
        case DslOpcode::EndStructure:
            if (!result.structureNode || instruction.operand != structureIndex ||
                nextFieldIndex != structure.fields.size() ||
                !tree.transition(*result.structureNode,
                                 core::MaterializationState::Materialized)) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR end instruction is invalid"),
                            nullptr);
                return result;
            }
            result.status = DslExecutionStatus::Materialized;
            ended = true;
            break;
        }
        if (ended) {
            break;
        }
    }

    if (!ended) {
        markFailure(DslExecutionStatus::InvalidDefinition,
                    QStringLiteral("Typed IR did not terminate a structure"),
                    nullptr);
    }
    return result;
}

} // namespace streamview::rules
