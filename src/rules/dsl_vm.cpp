#include <streamview/rules/dsl_vm.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

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

[[nodiscard]] bool fitsUnsignedBits(quint64 value, quint8 bitWidth) noexcept {
    return bitWidth == 64 || (bitWidth != 0 && value < (quint64{1} << bitWidth));
}

[[nodiscard]] quint64 decodeValue(quint64 value, const DslValueType& type) noexcept {
    if (type.endian != DslEndian::Little) {
        return value;
    }

    // BitReader preserves source bit order; byte-order conversion belongs to value decoding.
    quint64 decoded = 0;
    const unsigned int byteCount = type.bitWidth / 8U;
    for (unsigned int index = 0; index < byteCount; ++index) {
        decoded = (decoded << 8U) | (value & 0xffU);
        value >>= 8U;
    }
    return decoded;
}

[[nodiscard]] bool enumContains(const DslTypedEnum& enumeration, quint64 value) noexcept {
    return std::any_of(enumeration.values.begin(),
                       enumeration.values.end(),
                       [value](const DslTypedEnumValue& member) {
                           return member.value == value;
                       });
}

struct ExpGolombReadResult final {
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
    quint64 unsignedValue = 0;
    qlonglong signedValue = 0;
    quint64 bitCount = 0;
    quint64 diagnosticBits = 0;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept {
        return status == DslExecutionStatus::Materialized;
    }
};

[[nodiscard]] ExpGolombReadResult readExpGolomb(core::BitReader& reader,
                                                 bool signedValue) {
    const quint64 start = reader.position();
    const quint64 rangeLength = reader.range().bitLength();
    const quint64 availableAtStart = rangeLength >= start ? rangeLength - start : 0;
    const auto fail = [&](DslExecutionStatus status,
                          const QString& message,
                          quint64 diagnosticBits) {
        (void)reader.seek(start);
        return ExpGolombReadResult{status, 0, 0, 0,
                                   std::min(availableAtStart, diagnosticBits), message};
    };
    const auto readFailure = [&](const core::BitReadResult& readResult,
                                 quint64 bitsRead,
                                 quint64 requestedBits) {
        const DslExecutionStatus status = statusForRead(readResult.status);
        quint64 diagnosticBits = bitsRead;
        if (readResult.status == core::BitReadStatus::EndOfRange) {
            diagnosticBits = availableAtStart;
        } else if (readResult.status == core::BitReadStatus::EndOfSource) {
            diagnosticBits = bitsRead < std::numeric_limits<quint64>::max()
                                  ? bitsRead + std::max<quint64>(requestedBits, 1)
                                  : bitsRead;
        } else if (diagnosticBits == 0) {
            diagnosticBits = 1;
        }
        return fail(status,
                    readResult.errorMessage.isEmpty()
                        ? QStringLiteral("Unable to read complete Exp-Golomb codeword")
                        : readResult.errorMessage,
                    diagnosticBits);
    };

    quint64 leadingZeroBits = 0;
    while (true) {
        const core::BitReadResult prefix = reader.readBits(1);
        if (!prefix.complete()) {
            return readFailure(prefix, reader.position() - start, 1);
        }
        if (prefix.value != 0) {
            break;
        }
        ++leadingZeroBits;
        if (leadingZeroBits >= 64) {
            return fail(DslExecutionStatus::InvalidSyntax,
                        QStringLiteral("Exp-Golomb codeword exceeds the 64-bit value range"),
                        leadingZeroBits);
        }
    }

    quint64 suffix = 0;
    if (leadingZeroBits != 0) {
        const core::BitReadResult suffixResult =
            reader.readBits(static_cast<unsigned int>(leadingZeroBits));
        if (!suffixResult.complete()) {
            return readFailure(suffixResult, reader.position() - start, leadingZeroBits);
        }
        suffix = suffixResult.value;
    }

    const quint64 base = (quint64{1} << leadingZeroBits) - 1U;
    const quint64 codeNumber = base + suffix;
    const quint64 bitCount = reader.position() - start;
    if (!signedValue) {
        return {DslExecutionStatus::Materialized, codeNumber, 0, bitCount, 0, {}};
    }

    qlonglong decoded = 0;
    if (codeNumber != 0) {
        const quint64 magnitude = (codeNumber + 1U) / 2U;
        decoded = (codeNumber & 1U) != 0 ? static_cast<qlonglong>(magnitude)
                                        : -static_cast<qlonglong>(magnitude);
    }
    return {DslExecutionStatus::Materialized, 0, decoded, bitCount, 0, {}};
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
                                 const DslTypedField* field,
                                 std::optional<quint64> diagnosticPosition = std::nullopt,
                                 std::optional<quint64> diagnosticBits = std::nullopt) {
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
        const quint64 position = diagnosticPosition.value_or(reader.position());
        const quint64 availableBits = position <= reader.range().bitLength()
                                           ? reader.range().bitLength() - position
                                           : 0;
        diagnostic.location = locationAt(mapping,
                                          logicalStart,
                                          position,
                                          availableBits,
                                          diagnosticBits.value_or(requestedBits));
        const auto state = status == DslExecutionStatus::Cancelled
                               ? core::MaterializationState::Cancelled
                               : core::MaterializationState::Invalid;
        if (result.structureNode) {
            (void)tree.markPartial(*result.structureNode, state, std::move(diagnostic));
        } else {
            (void)tree.markPartial(parentId, state, std::move(diagnostic));
        }
    };

    const DslExecutionLimits& limits = options.limits;
    if (limits.maximumCallDepth == 0 ||
        limits.maximumCallDepth > DslExecutionLimits::defaultMaximumCallDepth() ||
        limits.maximumViewDepth == 0 ||
        limits.maximumViewDepth > DslExecutionLimits::defaultMaximumViewDepth() ||
        limits.maximumNodeDepth == 0 ||
        limits.maximumNodeDepth > DslExecutionLimits::defaultMaximumNodeDepth() ||
        limits.maximumMaterializedNodes == 0 ||
        limits.maximumMaterializedNodes > DslExecutionLimits::defaultMaximumMaterializedNodes() ||
        limits.maximumInstructions == 0 ||
        limits.maximumInstructions > DslExecutionLimits::defaultMaximumInstructions() ||
        limits.cancellationCheckInterval == 0 ||
        limits.cancellationCheckInterval > DslExecutionLimits::defaultCancellationCheckInterval()) {
        markFailure(DslExecutionStatus::ResourceLimit,
                    QStringLiteral("DSL execution limits exceed the documented sandbox bounds"),
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

    const auto sameCondition = [](const DslTypedFieldCondition& left,
                                  const DslTypedFieldCondition& right) {
        return left.fieldIndex == right.fieldIndex &&
               left.expectedValue == right.expectedValue && left.negated == right.negated &&
               left.op == right.op;
    };
    const auto validFixedController = [&program](const DslTypedField& controller) {
        const bool validUnsignedController =
            controller.type.kind == DslValueTypeKind::UnsignedBits &&
            !controller.type.enumIndex;
        const bool validEnumController =
            controller.type.kind == DslValueTypeKind::Enum && controller.type.enumIndex &&
            *controller.type.enumIndex < program.enums.size();
        const bool validEndian = controller.type.endian == DslEndian::Big ||
                                 controller.type.endian == DslEndian::Little;
        return (validUnsignedController || validEnumController) &&
               controller.type.bitWidth != 0 && controller.type.bitWidth <= 64 && validEndian &&
               (controller.type.endian != DslEndian::Little ||
                controller.type.bitWidth % 8 == 0);
    };
    const auto validUnsignedExpGolombController = [](const DslTypedField& controller) {
        return controller.type.kind == DslValueTypeKind::UnsignedExpGolomb &&
               controller.type.bitWidth == 0 && controller.type.endian == DslEndian::Big &&
               !controller.type.enumIndex && !controller.equalsConstraint;
    };
    const auto validateConditions = [&](const std::vector<DslTypedFieldCondition>& conditions,
                                        std::size_t subjectFieldIndex,
                                        const DslTypedField* subject,
                                        const QString& subjectName) {
        for (std::size_t conditionIndex = 0; conditionIndex < conditions.size();
             ++conditionIndex) {
            const DslTypedFieldCondition& condition = conditions.at(conditionIndex);
            if (condition.fieldIndex >= subjectFieldIndex ||
                condition.fieldIndex >= structure.fields.size()) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR %1 condition is invalid").arg(subjectName),
                            subject);
                return false;
            }
            const DslTypedField& controller = structure.fields.at(condition.fieldIndex);
            const bool validOperator = condition.op == DslConditionOperator::Equal ||
                                       condition.op == DslConditionOperator::GreaterThan;
            const bool fixedController = validFixedController(controller);
            const bool unsignedExpGolombController =
                validUnsignedExpGolombController(controller);
            const bool validController =
                condition.op == DslConditionOperator::Equal
                    ? fixedController
                    : fixedController || unsignedExpGolombController;
            if (!validOperator || !validController ||
                (fixedController &&
                 !fitsUnsignedBits(condition.expectedValue, controller.type.bitWidth))) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR %1 condition has an invalid controller")
                                .arg(subjectName),
                            subject);
                return false;
            }
            const auto availableEnd =
                conditions.begin() + static_cast<std::ptrdiff_t>(conditionIndex);
            const bool controllerAvailable = std::all_of(
                controller.conditions.begin(),
                controller.conditions.end(),
                [&conditions, availableEnd, &sameCondition](
                    const DslTypedFieldCondition& required) {
                    return std::any_of(conditions.begin(),
                                       availableEnd,
                                       [&required, &sameCondition](
                                           const DslTypedFieldCondition& candidate) {
                                           return sameCondition(required, candidate);
                                       });
                });
            if (!controllerAvailable) {
                markFailure(
                    DslExecutionStatus::InvalidDefinition,
                    QStringLiteral("Typed IR %1 condition controller is not guaranteed")
                        .arg(subjectName),
                    subject);
                return false;
            }
        }
        return true;
    };
    for (std::size_t fieldIndex = 0; fieldIndex < structure.fields.size(); ++fieldIndex) {
        const DslTypedField& field = structure.fields.at(fieldIndex);
        if (!validateConditions(
                field.conditions, fieldIndex, &field, QStringLiteral("field"))) {
            return result;
        }
    }
    quint32 previousRepeatPosition = 0;
    for (std::size_t repeatIndex = 0; repeatIndex < structure.repeatBounds.size();
         ++repeatIndex) {
        const DslTypedRepeatBound& repeat = structure.repeatBounds.at(repeatIndex);
        const bool ordered = repeatIndex == 0 ||
                             repeat.firstFieldIndex >= previousRepeatPosition;
        if (!ordered || repeat.maximumCount == 0 ||
            repeat.firstFieldIndex >= structure.fields.size() ||
            repeat.controllerFieldIndex >= repeat.firstFieldIndex ||
            repeat.controllerFieldIndex >= structure.fields.size()) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed IR repeat bound is invalid"),
                        nullptr);
            return result;
        }
        const DslTypedField& controller =
            structure.fields.at(repeat.controllerFieldIndex);
        const bool fixedController = validFixedController(controller);
        if ((!fixedController && !validUnsignedExpGolombController(controller)) ||
            (fixedController &&
             !fitsUnsignedBits(repeat.maximumCount, controller.type.bitWidth))) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed IR repeat bound has an invalid controller"),
                        &controller);
            return result;
        }
        if (!validateConditions(repeat.conditions,
                                repeat.firstFieldIndex,
                                &controller,
                                QStringLiteral("repeat bound"))) {
            return result;
        }
        const bool controllerAvailable = std::all_of(
            controller.conditions.begin(),
            controller.conditions.end(),
            [&repeat, &sameCondition](const DslTypedFieldCondition& required) {
                return std::any_of(repeat.conditions.begin(),
                                   repeat.conditions.end(),
                                   [&required, &sameCondition](
                                       const DslTypedFieldCondition& candidate) {
                                       return sameCondition(required, candidate);
                                   });
            });
        if (!controllerAvailable) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed IR repeat bound controller is not guaranteed"),
                        &controller);
            return result;
        }
        previousRepeatPosition = repeat.firstFieldIndex;
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
    std::vector<std::optional<quint64>> fieldValues(structure.fields.size());
    struct MaterializedFieldRange final {
        quint64 start = 0;
        quint64 bitCount = 0;
    };
    std::vector<std::optional<MaterializedFieldRange>> fieldRanges(
        structure.fields.size());
    std::optional<quint32> lastField;
    std::optional<quint64> lastValue;
    bool lastFieldSkipped = false;
    quint32 nextFieldIndex = 0;
    bool ended = false;
    const auto conditionsPresent = [&](const std::vector<DslTypedFieldCondition>& conditions,
                                       const DslTypedField* subject,
                                       const QString& subjectName) -> std::optional<bool> {
        for (const DslTypedFieldCondition& condition : conditions) {
            const std::optional<quint64>& controllerValue =
                fieldValues.at(condition.fieldIndex);
            if (!controllerValue) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR %1 condition controller is unavailable")
                                .arg(subjectName),
                            subject);
                return std::nullopt;
            }
            const bool matches = condition.op == DslConditionOperator::Equal
                                     ? *controllerValue == condition.expectedValue
                                     : *controllerValue > condition.expectedValue;
            if (condition.negated ? matches : !matches) {
                return false;
            }
        }
        return true;
    };

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
        case DslOpcode::ReadUnsignedBits:
        case DslOpcode::ReadUnsignedExpGolomb:
        case DslOpcode::ReadSignedExpGolomb: {
            if (!result.structureNode || instruction.operand != nextFieldIndex ||
                instruction.operand >= structure.fields.size()) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR field instruction is invalid"),
                            nullptr);
                return result;
            }
            const DslTypedField& field = structure.fields.at(instruction.operand);
            const bool readsFixedBits = instruction.opcode == DslOpcode::ReadUnsignedBits;
            const bool readsUnsignedExpGolomb =
                instruction.opcode == DslOpcode::ReadUnsignedExpGolomb;
            const DslTypedEnum* enumeration = nullptr;
            if (readsFixedBits) {
                if (field.type.bitWidth == 0 || field.type.bitWidth > 64 ||
                    (field.type.endian != DslEndian::Big &&
                     field.type.endian != DslEndian::Little) ||
                    (field.type.endian == DslEndian::Little && field.type.bitWidth % 8 != 0)) {
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed IR field type is invalid"),
                                &field);
                    return result;
                }
                switch (field.type.kind) {
                case DslValueTypeKind::UnsignedBits:
                    if (field.type.enumIndex) {
                        markFailure(DslExecutionStatus::InvalidDefinition,
                                    QStringLiteral("Typed unsigned field has an enum reference"),
                                    &field);
                        return result;
                    }
                    break;
                case DslValueTypeKind::Enum:
                    if (!field.type.enumIndex || *field.type.enumIndex >= program.enums.size()) {
                        markFailure(
                            DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed enum field has an invalid enum reference"),
                            &field);
                        return result;
                    }
                    enumeration = &program.enums.at(*field.type.enumIndex);
                    if (enumeration->values.empty() ||
                        std::any_of(enumeration->values.begin(),
                                    enumeration->values.end(),
                                    [&field](const DslTypedEnumValue& value) {
                                        return !fitsUnsignedBits(value.value,
                                                                 field.type.bitWidth);
                                    })) {
                        markFailure(
                            DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed enum definition is invalid for the field"),
                            &field);
                        return result;
                    }
                    break;
                case DslValueTypeKind::UnsignedExpGolomb:
                case DslValueTypeKind::SignedExpGolomb:
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed IR opcode does not match the field type"),
                                &field);
                    return result;
                }
            } else {
                const DslValueTypeKind expectedKind =
                    readsUnsignedExpGolomb ? DslValueTypeKind::UnsignedExpGolomb
                                           : DslValueTypeKind::SignedExpGolomb;
                if (field.type.kind != expectedKind || field.type.bitWidth != 0 ||
                    field.type.endian != DslEndian::Big || field.type.enumIndex ||
                    field.equalsConstraint) {
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed Exp-Golomb field definition is invalid"),
                                &field);
                    return result;
                }
            }
            const std::optional<bool> fieldPresent =
                conditionsPresent(field.conditions, &field, QStringLiteral("field"));
            if (!fieldPresent) {
                return result;
            }
            if (!*fieldPresent) {
                lastField = instruction.operand;
                lastValue.reset();
                lastFieldSkipped = true;
                ++nextFieldIndex;
                break;
            }
            const quint64 fieldStart = reader.position();
            const quint64 readerStart = reader.range().start().absoluteBitOffset();
            if (readsFixedBits && field.type.endian == DslEndian::Little &&
                (addWouldOverflow(readerStart, fieldStart) ||
                 (readerStart + fieldStart) % 8 != 0)) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR field type is invalid"),
                            &field);
                return result;
            }
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
            quint64 consumedBits = 0;
            quint64 unsignedValue = 0;
            qlonglong signedValue = 0;
            if (readsFixedBits) {
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
                consumedBits = field.type.bitWidth;
                unsignedValue = decodeValue(readResult.value, field.type);
            } else {
                const ExpGolombReadResult readResult =
                    readExpGolomb(reader, !readsUnsignedExpGolomb);
                result.bitsConsumed = reader.position();
                if (!readResult.complete()) {
                    markFailure(readResult.status,
                                readResult.errorMessage,
                                &field,
                                fieldStart,
                                readResult.diagnosticBits);
                    return result;
                }
                consumedBits = readResult.bitCount;
                unsignedValue = readResult.unsignedValue;
                signedValue = readResult.signedValue;
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
                                             consumedBits,
                                             consumedBits);
            if (addWouldOverflow(readerStart, fieldStart) || !location ||
                location->sourceSpans().size() != 1 ||
                location->sourceSpans().front().start().absoluteBitOffset() !=
                    readerStart + fieldStart ||
                location->sourceSpans().front().bitLength() != consumedBits) {
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
            fieldSpec.value = readsFixedBits || readsUnsignedExpGolomb
                                  ? QVariant::fromValue<qulonglong>(unsignedValue)
                                  : QVariant::fromValue<qlonglong>(signedValue);
            fieldSpec.location = *location;
            fieldSpec.metadata = field.metadata;
            if (!tree.appendChild(*result.structureNode, std::move(fieldSpec))) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to append typed field node"),
                            &field);
                return result;
            }
            ++result.nodesCreated;
            fieldValues.at(instruction.operand) =
                readsFixedBits || readsUnsignedExpGolomb
                    ? std::optional<quint64>(unsignedValue)
                    : std::nullopt;
            fieldRanges.at(instruction.operand) =
                MaterializedFieldRange{fieldStart, consumedBits};
            lastField = instruction.operand;
            lastValue = readsFixedBits ? std::optional<quint64>(unsignedValue) : std::nullopt;
            lastFieldSkipped = false;
            ++nextFieldIndex;
            if (enumeration != nullptr && !enumContains(*enumeration, unsignedValue)) {
                result.status = DslExecutionStatus::InvalidSyntax;
                result.errorMessage =
                    QStringLiteral("Field value is not declared by its enum type");
                core::ParseDiagnostic diagnostic;
                diagnostic.code = core::DiagnosticCode::InvalidSyntax;
                diagnostic.severity = core::DiagnosticSeverity::Error;
                diagnostic.message = result.errorMessage;
                diagnostic.fieldPath = structure.name + QLatin1Char('.') + field.name;
                diagnostic.location = locationAt(mapping,
                                                 logicalStart,
                                                 fieldStart,
                                                 consumedBits,
                                                 consumedBits);
                (void)tree.markPartial(*result.structureNode,
                                       core::MaterializationState::Invalid,
                                       std::move(diagnostic));
                return result;
            }
            break;
        }
        case DslOpcode::AssertEquals: {
            const DslTypedField* field = instruction.operand < structure.fields.size()
                                             ? &structure.fields.at(instruction.operand)
                                             : nullptr;
            if (!result.structureNode || !lastField || *lastField != instruction.operand ||
                field == nullptr || !field->equalsConstraint ||
                *field->equalsConstraint != instruction.immediate ||
                (lastFieldSkipped ? lastValue.has_value() : !lastValue.has_value())) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR equality instruction is invalid"),
                            nullptr);
                return result;
            }
            if (lastFieldSkipped) {
                break;
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
        case DslOpcode::AssertRepeatCount: {
            const DslTypedRepeatBound* repeat =
                instruction.operand < structure.repeatBounds.size()
                    ? &structure.repeatBounds.at(instruction.operand)
                    : nullptr;
            if (!result.structureNode || repeat == nullptr ||
                repeat->firstFieldIndex != nextFieldIndex ||
                repeat->maximumCount != instruction.immediate) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR repeat assertion is invalid"),
                            nullptr);
                return result;
            }
            const DslTypedField& controller =
                structure.fields.at(repeat->controllerFieldIndex);
            const std::optional<bool> repeatPresent = conditionsPresent(
                repeat->conditions, &controller, QStringLiteral("repeat bound"));
            if (!repeatPresent) {
                return result;
            }
            if (!*repeatPresent) {
                break;
            }
            const std::optional<quint64>& count =
                fieldValues.at(repeat->controllerFieldIndex);
            if (!count) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral(
                                "Typed IR repeat bound controller is unavailable"),
                            &controller);
                return result;
            }
            if (*count <= repeat->maximumCount) {
                break;
            }
            const std::optional<MaterializedFieldRange>& controllerRange =
                fieldRanges.at(repeat->controllerFieldIndex);
            markFailure(DslExecutionStatus::InvalidSyntax,
                        QStringLiteral("Repeat count exceeds its declared maximum"),
                        &controller,
                        controllerRange
                            ? std::optional<quint64>(controllerRange->start)
                            : std::nullopt,
                        controllerRange
                            ? std::optional<quint64>(controllerRange->bitCount)
                            : std::nullopt);
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
