#include <streamview/rules/dsl_vm.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace streamview::rules {

namespace {

constexpr quint64 maximumUnsignedExpGolombValue = std::numeric_limits<quint64>::max() - 1;

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
    case DslExecutionStatus::DependencyUnavailable:
        return core::DiagnosticCode::DependencyUnavailable;
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

[[nodiscard]] bool sameSourceSpans(const std::vector<core::SourceSpan>& left,
                                   const std::vector<core::SourceSpan>& right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(),
                      left.end(),
                      right.begin(),
                      [](const core::SourceSpan& leftSpan,
                         const core::SourceSpan& rightSpan) {
                          return leftSpan.start() == rightSpan.start() &&
                                 leftSpan.bitLength() == rightSpan.bitLength();
                      });
}

[[nodiscard]] bool readerBackingMatchesMapping(const core::BitReader& reader,
                                               const core::SourceMapping& mapping,
                                               quint64 logicalStart) {
    const auto range = core::LogicalRange::create(
        core::LogicalBitAddress(mapping.viewId(), logicalStart), reader.logicalBitLength());
    const auto location = range ? mapping.locate(*range) : std::nullopt;
    return location && sameSourceSpans(reader.backingSpans(), location->sourceSpans());
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

[[nodiscard]] bool validUnsignedExpGolombEnum(
    const DslTypedProgram& program,
    const std::optional<quint32>& enumIndex) noexcept {
    if (!enumIndex) {
        return true;
    }
    if (*enumIndex >= program.enums.size()) {
        return false;
    }
    const DslTypedEnum& enumeration = program.enums.at(*enumIndex);
    return !enumeration.values.empty() &&
           std::none_of(enumeration.values.begin(),
                        enumeration.values.end(),
                        [](const DslTypedEnumValue& value) {
                            return value.value > maximumUnsignedExpGolombValue;
                        });
}

[[nodiscard]] bool validScalarType(DslScalarType type) noexcept {
    return type == DslScalarType::Bool || type == DslScalarType::U64;
}

[[nodiscard]] bool validContextKind(core::ContextDefinitionKind kind) noexcept {
    switch (kind) {
    case core::ContextDefinitionKind::H264SequenceParameterSet:
    case core::ContextDefinitionKind::H264PictureParameterSet:
    case core::ContextDefinitionKind::AacAudioSpecificConfig:
    case core::ContextDefinitionKind::IsoBmffSampleDescription:
        return true;
    }
    return false;
}

[[nodiscard]] bool validContextField(const DslTypedStruct& structure,
                                     quint32 fieldIndex) noexcept {
    if (fieldIndex >= structure.fields.size()) {
        return false;
    }
    const DslTypedField& field = structure.fields.at(fieldIndex);
    const bool unsignedScalar =
        field.type.kind == DslValueTypeKind::UnsignedBits ||
        field.type.kind == DslValueTypeKind::Enum ||
        field.type.kind == DslValueTypeKind::UnsignedExpGolomb ||
        field.type.kind == DslValueTypeKind::ComputedUnsigned;
    return field.kind == DslTypedFieldKind::Declared && field.contextEligible &&
           field.conditions.empty() && unsignedScalar;
}

[[nodiscard]] bool validContextDefinitionHeader(
    const DslTypedStruct& structure) noexcept {
    if (!structure.contextDefinition) {
        return false;
    }
    const DslTypedContextDefinition& definition = *structure.contextDefinition;
    return definition.dependencies.size() <=
               DslTypedContextDefinition::maximumDependencies() &&
           definition.exportFieldIndices.size() <=
               DslTypedContextDefinition::maximumExports() &&
           validContextKind(definition.kind) &&
           validContextField(structure, definition.keyFieldIndex);
}

[[nodiscard]] bool validContextDefinitionExports(
    const DslTypedStruct& structure) noexcept {
    if (!structure.contextDefinition) {
        return false;
    }
    const std::vector<quint32>& exports =
        structure.contextDefinition->exportFieldIndices;
    for (std::size_t index = 0; index < exports.size(); ++index) {
        if (!validContextField(structure, exports.at(index))) {
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (exports.at(previous) == exports.at(index)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool validContextDefinitionDependencies(
    const DslTypedStruct& structure) noexcept {
    if (!structure.contextDefinition) {
        return false;
    }
    const std::vector<DslTypedContextDependency>& dependencies =
        structure.contextDefinition->dependencies;
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        const DslTypedContextDependency& dependency = dependencies.at(index);
        if (!validContextKind(dependency.kind) ||
            !validContextField(structure, dependency.keyFieldIndex)) {
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            const DslTypedContextDependency& candidate =
                dependencies.at(previous);
            if (candidate.kind == dependency.kind &&
                candidate.keyFieldIndex == dependency.keyFieldIndex) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool validContextDefinition(
    const DslTypedStruct& structure) noexcept {
    return validContextDefinitionHeader(structure) &&
           validContextDefinitionExports(structure) &&
           validContextDefinitionDependencies(structure);
}

[[nodiscard]] bool contextKindReachable(
    const DslTypedProgram& program,
    core::ContextDefinitionKind rootKind,
    core::ContextDefinitionKind targetKind) {
    std::vector<core::ContextDefinitionKind> reachable{rootKind};
    for (std::size_t index = 0; index < reachable.size(); ++index) {
        for (const DslTypedStruct& candidate : program.structs) {
            if (!candidate.contextDefinition ||
                candidate.contextDefinition->kind != reachable.at(index)) {
                continue;
            }
            for (const DslTypedContextDependency& dependency :
                 candidate.contextDefinition->dependencies) {
                if (std::find(reachable.begin(), reachable.end(), dependency.kind) ==
                    reachable.end()) {
                    reachable.push_back(dependency.kind);
                }
            }
        }
    }
    return std::find(reachable.begin(), reachable.end(), targetKind) !=
           reachable.end();
}

[[nodiscard]] std::optional<DslScalarType> scalarTypeForField(
    const DslTypedField& field) noexcept {
    switch (field.type.kind) {
    case DslValueTypeKind::UnsignedBits:
    case DslValueTypeKind::Enum:
    case DslValueTypeKind::UnsignedExpGolomb:
    case DslValueTypeKind::ComputedUnsigned:
        return DslScalarType::U64;
    case DslValueTypeKind::ComputedBool:
        return DslScalarType::Bool;
    case DslValueTypeKind::SignedExpGolomb:
    case DslValueTypeKind::LazyBytes:
    case DslValueTypeKind::CompressedPayload:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] bool sameTypedExpression(const DslTypedExpression& left,
                                       const DslTypedExpression& right) noexcept {
    if (left.kind != right.kind || left.type != right.type ||
        left.unaryOperator != right.unaryOperator ||
        left.binaryOperator != right.binaryOperator ||
        left.unsignedValue != right.unsignedValue ||
        left.booleanValue != right.booleanValue || left.fieldIndex != right.fieldIndex ||
        left.contextImportIndex != right.contextImportIndex ||
        left.contextDefinitionKind != right.contextDefinitionKind ||
        left.contextStructureIndex != right.contextStructureIndex ||
        left.contextExportIndex != right.contextExportIndex ||
        left.operands.size() != right.operands.size()) {
        return false;
    }
    return std::equal(left.operands.begin(), left.operands.end(), right.operands.begin(),
                      [](const DslTypedExpression& leftOperand,
                         const DslTypedExpression& rightOperand) {
                          return sameTypedExpression(leftOperand, rightOperand);
                      });
}

[[nodiscard]] bool sameCondition(const DslTypedFieldCondition& left,
                                 const DslTypedFieldCondition& right) noexcept {
    return left.fieldIndex == right.fieldIndex &&
           left.expectedValue == right.expectedValue && left.negated == right.negated &&
           left.op == right.op &&
           ((!left.expression && !right.expression) ||
            (left.expression && right.expression &&
             sameTypedExpression(*left.expression, *right.expression)));
}

struct TypedExpressionValidationState final {
    std::size_t nodeCount = 0;
    bool allowImportedContextReferences = false;
    QString errorMessage;
};

[[nodiscard]] bool validateTypedExpression(
    const DslTypedExpression& expression,
    const DslTypedProgram& program,
    const DslTypedStruct& structure,
    std::size_t subjectFieldIndex,
    const std::vector<DslTypedFieldCondition>& subjectConditions,
    std::size_t depth,
    TypedExpressionValidationState& state) {
    const auto fail = [&state](const QString& message) {
        if (state.errorMessage.isEmpty()) {
            state.errorMessage = message;
        }
        return false;
    };
    if (depth > 64) {
        return fail(QStringLiteral("Typed computed expression exceeds the depth limit"));
    }
    ++state.nodeCount;
    if (state.nodeCount > 256) {
        return fail(QStringLiteral("Typed computed expression exceeds the node limit"));
    }
    if (!validScalarType(expression.type)) {
        return fail(QStringLiteral("Typed computed expression has an invalid scalar type"));
    }

    switch (expression.kind) {
    case DslTypedExpressionKind::UnsignedLiteral:
        return expression.type == DslScalarType::U64 && expression.operands.empty()
                   ? true
                   : fail(QStringLiteral("Typed unsigned literal expression is invalid"));
    case DslTypedExpressionKind::BooleanLiteral:
        return expression.type == DslScalarType::Bool && expression.operands.empty()
                   ? true
                   : fail(QStringLiteral("Typed Boolean literal expression is invalid"));
    case DslTypedExpressionKind::FieldReference: {
        if (!expression.operands.empty() || expression.fieldIndex >= subjectFieldIndex ||
            expression.fieldIndex >= structure.fields.size()) {
            return fail(QStringLiteral("Typed computed field reference is invalid"));
        }
        const DslTypedField& dependency = structure.fields.at(expression.fieldIndex);
        const std::optional<DslScalarType> dependencyType = scalarTypeForField(dependency);
        if (!dependencyType || *dependencyType != expression.type) {
            return fail(QStringLiteral("Typed computed field reference type is invalid"));
        }
        const bool dependencyAvailable = std::all_of(
            dependency.conditions.begin(),
            dependency.conditions.end(),
            [&subjectConditions](const DslTypedFieldCondition& required) {
                return std::any_of(
                    subjectConditions.begin(),
                    subjectConditions.end(),
                    [&required](const DslTypedFieldCondition& candidate) {
                        return sameCondition(required, candidate);
                    });
            });
        return dependencyAvailable
                   ? true
                   : fail(QStringLiteral(
                         "Typed computed field dependency is not guaranteed"));
    }
    case DslTypedExpressionKind::ImportedContextReference: {
        if (!state.allowImportedContextReferences || !expression.operands.empty() ||
            expression.type != DslScalarType::U64 ||
            expression.contextImportIndex >= structure.contextImports.size()) {
            return fail(QStringLiteral("Typed imported context reference is invalid"));
        }
        const DslTypedContextImport& import =
            structure.contextImports.at(expression.contextImportIndex);
        if (import.keyFieldIndex >= subjectFieldIndex ||
            import.keyFieldIndex >= structure.fields.size() ||
            expression.contextStructureIndex >= program.structs.size()) {
            return fail(QStringLiteral("Typed imported context reference is out of range"));
        }
        const DslTypedStruct& publisher =
            program.structs.at(expression.contextStructureIndex);
        const std::size_t publisherCount = static_cast<std::size_t>(std::count_if(
            program.structs.begin(),
            program.structs.end(),
            [&expression](const DslTypedStruct& candidate) {
                return candidate.contextDefinition &&
                       candidate.contextDefinition->kind ==
                           expression.contextDefinitionKind;
            }));
        if (!validContextDefinition(publisher) ||
            !validContextKind(expression.contextDefinitionKind) ||
            publisherCount != 1 ||
            !contextKindReachable(program,
                                  import.kind,
                                  expression.contextDefinitionKind) ||
            publisher.contextDefinition->kind != expression.contextDefinitionKind ||
            expression.contextExportIndex >=
                publisher.contextDefinition->exportFieldIndices.size() ||
            !validContextField(
                publisher,
                publisher.contextDefinition->exportFieldIndices.at(
                    expression.contextExportIndex))) {
            return fail(QStringLiteral("Typed imported context export descriptor is invalid"));
        }
        return true;
    }
    case DslTypedExpressionKind::SequenceElementReference: {
        if (!state.allowImportedContextReferences || !expression.operands.empty() ||
            expression.type != DslScalarType::U64 || program.scans.empty()) {
            return fail(QStringLiteral("Typed sequence element reference is invalid"));
        }
        const quint32 elementStructIndex = program.scans.front().elementStructIndex;
        if (elementStructIndex >= program.structs.size()) {
            return fail(
                QStringLiteral("Typed sequence element reference is out of range"));
        }
        const DslTypedStruct& element = program.structs.at(elementStructIndex);
        if (expression.elementFieldIndex >= element.fields.size() ||
            !validContextField(element, expression.elementFieldIndex)) {
            return fail(QStringLiteral(
                "Typed sequence element reference descriptor is invalid"));
        }
        return true;
    }
    case DslTypedExpressionKind::Unary:
        if (expression.unaryOperator != DslUnaryOperator::LogicalNot ||
            expression.type != DslScalarType::Bool || expression.operands.size() != 1) {
            return fail(QStringLiteral("Typed unary computed expression is invalid"));
        }
        if (!validateTypedExpression(expression.operands.front(),
                                     program,
                                     structure,
                                     subjectFieldIndex,
                                     subjectConditions,
                                     depth + 1,
                                     state)) {
            return false;
        }
        return expression.operands.front().type == DslScalarType::Bool
                   ? true
                   : fail(QStringLiteral("Typed logical negation operand is invalid"));
    case DslTypedExpressionKind::Binary:
        break;
    default:
        return fail(QStringLiteral("Typed computed expression kind is invalid"));
    }

    if (expression.operands.size() != 2) {
        return fail(QStringLiteral("Typed binary computed expression is invalid"));
    }
    const DslTypedExpression& left = expression.operands.at(0);
    const DslTypedExpression& right = expression.operands.at(1);
    if (!validateTypedExpression(left,
                                 program,
                                 structure,
                                 subjectFieldIndex,
                                 subjectConditions,
                                 depth + 1,
                                 state) ||
        !validateTypedExpression(right,
                                 program,
                                 structure,
                                 subjectFieldIndex,
                                 subjectConditions,
                                 depth + 1,
                                 state)) {
        return false;
    }

    bool typesValid = false;
    switch (expression.binaryOperator) {
    case DslBinaryOperator::Multiply:
    case DslBinaryOperator::Divide:
    case DslBinaryOperator::Remainder:
    case DslBinaryOperator::Add:
    case DslBinaryOperator::Subtract:
        typesValid = expression.type == DslScalarType::U64 &&
                     left.type == DslScalarType::U64 && right.type == DslScalarType::U64;
        break;
    case DslBinaryOperator::Equal:
    case DslBinaryOperator::NotEqual:
        typesValid = expression.type == DslScalarType::Bool && left.type == right.type;
        break;
    case DslBinaryOperator::Less:
    case DslBinaryOperator::LessEqual:
    case DslBinaryOperator::Greater:
    case DslBinaryOperator::GreaterEqual:
        typesValid = expression.type == DslScalarType::Bool &&
                     left.type == DslScalarType::U64 && right.type == DslScalarType::U64;
        break;
    case DslBinaryOperator::LogicalAnd:
    case DslBinaryOperator::LogicalOr:
        typesValid = expression.type == DslScalarType::Bool &&
                     left.type == DslScalarType::Bool && right.type == DslScalarType::Bool;
        break;
    default:
        return fail(QStringLiteral("Typed computed binary operator is invalid"));
    }
    return typesValid
               ? true
               : fail(QStringLiteral("Typed computed binary operand types are invalid"));
}

struct ComputedScalarValue final {
    DslScalarType type = DslScalarType::U64;
    quint64 unsignedValue = 0;
    bool booleanValue = false;
};

struct ComputedEvaluationResult final {
    DslExecutionStatus status = DslExecutionStatus::InvalidDefinition;
    ComputedScalarValue value;
    QString errorMessage;
    std::optional<quint32> diagnosticFieldIndex;

    [[nodiscard]] bool complete() const noexcept {
        return status == DslExecutionStatus::Materialized;
    }
};

[[nodiscard]] ComputedEvaluationResult evaluateTypedExpression(
    const DslTypedExpression& expression,
    const DslTypedStruct& structure,
    const std::vector<std::optional<quint64>>& fieldValues,
    const DslContextValueResolver& contextValueResolver,
    const std::vector<std::optional<quint64>>* sequenceElementValues = nullptr) {
    const auto invalidDefinition = [](const QString& message) {
        return ComputedEvaluationResult{
            DslExecutionStatus::InvalidDefinition, {}, message, std::nullopt};
    };
    const auto invalidSyntax = [](const QString& message) {
        return ComputedEvaluationResult{
            DslExecutionStatus::InvalidSyntax, {}, message, std::nullopt};
    };
    const auto unsignedResult = [](quint64 value) {
        return ComputedEvaluationResult{DslExecutionStatus::Materialized,
                                        {DslScalarType::U64, value, false},
                                        {},
                                        std::nullopt};
    };
    const auto booleanResult = [](bool value) {
        return ComputedEvaluationResult{DslExecutionStatus::Materialized,
                                        {DslScalarType::Bool, 0, value},
                                        {},
                                        std::nullopt};
    };

    switch (expression.kind) {
    case DslTypedExpressionKind::UnsignedLiteral:
        return unsignedResult(expression.unsignedValue);
    case DslTypedExpressionKind::BooleanLiteral:
        return booleanResult(expression.booleanValue);
    case DslTypedExpressionKind::FieldReference: {
        if (expression.fieldIndex >= fieldValues.size() ||
            !fieldValues.at(expression.fieldIndex)) {
            return invalidDefinition(
                QStringLiteral("Computed expression dependency is unavailable"));
        }
        const quint64 value = *fieldValues.at(expression.fieldIndex);
        if (expression.type == DslScalarType::Bool) {
            if (value > 1) {
                return invalidDefinition(
                    QStringLiteral("Computed Boolean dependency value is invalid"));
            }
            return booleanResult(value != 0);
        }
        return unsignedResult(value);
    }
    case DslTypedExpressionKind::ImportedContextReference: {
        if (expression.contextImportIndex >= structure.contextImports.size()) {
            return invalidDefinition(
                QStringLiteral("Imported context reference is out of range"));
        }
        const DslTypedContextImport& import =
            structure.contextImports.at(expression.contextImportIndex);
        if (import.keyFieldIndex >= fieldValues.size() ||
            !fieldValues.at(import.keyFieldIndex)) {
            ComputedEvaluationResult failure = invalidDefinition(
                QStringLiteral("Imported context key is unavailable"));
            failure.diagnosticFieldIndex = import.keyFieldIndex;
            return failure;
        }
        if (!contextValueResolver) {
            ComputedEvaluationResult failure = invalidDefinition(
                QStringLiteral("Imported context value resolver is unavailable"));
            failure.diagnosticFieldIndex = import.keyFieldIndex;
            return failure;
        }
        const DslContextValueResolution resolution = contextValueResolver({
            expression.contextImportIndex,
            import.kind,
            *fieldValues.at(import.keyFieldIndex),
            expression.contextDefinitionKind,
            expression.contextStructureIndex,
            expression.contextExportIndex,
        });
        if (!resolution.resolved()) {
            ComputedEvaluationResult failure{
                resolution.status,
                {},
                resolution.errorMessage.isEmpty()
                    ? QStringLiteral("Imported context value is unavailable")
                    : resolution.errorMessage,
                import.keyFieldIndex,
            };
            return failure;
        }
        return unsignedResult(resolution.value);
    }
    case DslTypedExpressionKind::SequenceElementReference: {
        if (sequenceElementValues == nullptr) {
            return invalidDefinition(
                QStringLiteral("Sequence element values are unavailable"));
        }
        if (expression.elementFieldIndex >= sequenceElementValues->size()) {
            return invalidDefinition(
                QStringLiteral("Sequence element reference is out of range"));
        }
        const std::optional<quint64>& elementValue =
            sequenceElementValues->at(expression.elementFieldIndex);
        if (!elementValue) {
            return invalidDefinition(
                QStringLiteral("Sequence element value is unavailable"));
        }
        return unsignedResult(*elementValue);
    }
    case DslTypedExpressionKind::Unary: {
        const ComputedEvaluationResult operand =
            evaluateTypedExpression(expression.operands.front(),
                                    structure,
                                    fieldValues,
                                    contextValueResolver,
                                    sequenceElementValues);
        return operand.complete() ? booleanResult(!operand.value.booleanValue) : operand;
    }
    case DslTypedExpressionKind::Binary:
        break;
    default:
        return invalidDefinition(QStringLiteral("Computed expression kind is invalid"));
    }

    const ComputedEvaluationResult left =
        evaluateTypedExpression(expression.operands.at(0),
                                structure,
                                fieldValues,
                                contextValueResolver,
                                sequenceElementValues);
    if (!left.complete()) {
        return left;
    }
    if (expression.binaryOperator == DslBinaryOperator::LogicalAnd &&
        !left.value.booleanValue) {
        return booleanResult(false);
    }
    if (expression.binaryOperator == DslBinaryOperator::LogicalOr &&
        left.value.booleanValue) {
        return booleanResult(true);
    }
    const ComputedEvaluationResult right =
        evaluateTypedExpression(expression.operands.at(1),
                                structure,
                                fieldValues,
                                contextValueResolver,
                                sequenceElementValues);
    if (!right.complete()) {
        return right;
    }

    const quint64 leftUnsigned = left.value.unsignedValue;
    const quint64 rightUnsigned = right.value.unsignedValue;
    switch (expression.binaryOperator) {
    case DslBinaryOperator::Multiply:
        if (leftUnsigned != 0 &&
            rightUnsigned > std::numeric_limits<quint64>::max() / leftUnsigned) {
            return invalidSyntax(QStringLiteral("Computed expression multiplication overflow"));
        }
        return unsignedResult(leftUnsigned * rightUnsigned);
    case DslBinaryOperator::Divide:
        return rightUnsigned == 0
                   ? invalidSyntax(QStringLiteral("Computed expression division by zero"))
                   : unsignedResult(leftUnsigned / rightUnsigned);
    case DslBinaryOperator::Remainder:
        return rightUnsigned == 0
                   ? invalidSyntax(QStringLiteral("Computed expression remainder by zero"))
                   : unsignedResult(leftUnsigned % rightUnsigned);
    case DslBinaryOperator::Add:
        return addWouldOverflow(leftUnsigned, rightUnsigned)
                   ? invalidSyntax(QStringLiteral("Computed expression addition overflow"))
                   : unsignedResult(leftUnsigned + rightUnsigned);
    case DslBinaryOperator::Subtract:
        return leftUnsigned < rightUnsigned
                   ? invalidSyntax(QStringLiteral("Computed expression subtraction underflow"))
                   : unsignedResult(leftUnsigned - rightUnsigned);
    case DslBinaryOperator::Equal:
    case DslBinaryOperator::NotEqual: {
        const bool equal = left.value.type == DslScalarType::Bool
                               ? left.value.booleanValue == right.value.booleanValue
                               : leftUnsigned == rightUnsigned;
        return booleanResult(expression.binaryOperator == DslBinaryOperator::Equal
                                 ? equal
                                 : !equal);
    }
    case DslBinaryOperator::Less:
        return booleanResult(leftUnsigned < rightUnsigned);
    case DslBinaryOperator::LessEqual:
        return booleanResult(leftUnsigned <= rightUnsigned);
    case DslBinaryOperator::Greater:
        return booleanResult(leftUnsigned > rightUnsigned);
    case DslBinaryOperator::GreaterEqual:
        return booleanResult(leftUnsigned >= rightUnsigned);
    case DslBinaryOperator::LogicalAnd:
        return booleanResult(left.value.booleanValue && right.value.booleanValue);
    case DslBinaryOperator::LogicalOr:
        return booleanResult(left.value.booleanValue || right.value.booleanValue);
    }
    return invalidDefinition(QStringLiteral("Computed expression operator is invalid"));
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
    const quint64 rangeLength = reader.logicalBitLength();
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
    const DslExecutionOptions& options,
    const DslContextValueResolver& contextValueResolver) {
    DslExecutionResult result;
    if (structureIndex >= program.structs.size()) {
        result.errorMessage = QStringLiteral("Typed IR structure index is out of range");
        return result;
    }
    if (!readerBackingMatchesMapping(reader, mapping, logicalStart)) {
        result.errorMessage =
            QStringLiteral("DSL reader backing does not match the supplied source mapping");
        return result;
    }

    const DslTypedStruct& structure = program.structs.at(structureIndex);
    const auto markFailure = [&](DslExecutionStatus status,
                                 const QString& message,
                                 const DslTypedField* field,
                                 std::optional<quint64> diagnosticPosition = std::nullopt,
                                 std::optional<quint64> diagnosticBits = std::nullopt,
                                 bool includeLocation = true) {
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
        if (includeLocation) {
            const quint64 position = diagnosticPosition.value_or(reader.position());
            const quint64 availableBits = position <= reader.logicalBitLength()
                                               ? reader.logicalBitLength() - position
                                               : 0;
            diagnostic.location = locationAt(mapping,
                                              logicalStart,
                                              position,
                                              availableBits,
                                              diagnosticBits.value_or(requestedBits));
        }
        const auto state = status == DslExecutionStatus::Cancelled
                               ? core::MaterializationState::Cancelled
                           : status == DslExecutionStatus::DependencyUnavailable
                               ? core::MaterializationState::WaitingDependency
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
    if (options.cancellation && options.cancellation->isCancellationRequested()) {
        markFailure(DslExecutionStatus::Cancelled,
                    QStringLiteral("DSL execution was cancelled"),
                    nullptr);
        return result;
    }

    const auto validFixedController = [&program](const DslTypedField& controller) {
        const bool validUnsignedController = controller.kind == DslTypedFieldKind::Declared &&
            controller.type.kind == DslValueTypeKind::UnsignedBits &&
            !controller.type.enumIndex;
        const bool validEnumController = controller.kind == DslTypedFieldKind::Declared &&
            controller.type.kind == DslValueTypeKind::Enum && controller.type.enumIndex &&
            *controller.type.enumIndex < program.enums.size();
        const bool validEndian = controller.type.endian == DslEndian::Big ||
                                 controller.type.endian == DslEndian::Little;
        return (validUnsignedController || validEnumController) &&
               controller.type.bitWidth != 0 && controller.type.bitWidth <= 64 && validEndian &&
               (controller.type.endian != DslEndian::Little ||
                controller.type.bitWidth % 8 == 0);
    };
    const auto validUnsignedExpGolombController = [&program](
                                                       const DslTypedField& controller) {
        return controller.kind == DslTypedFieldKind::Declared &&
               controller.type.kind == DslValueTypeKind::UnsignedExpGolomb &&
               controller.type.bitWidth == 0 && controller.type.endian == DslEndian::Big &&
               validUnsignedExpGolombEnum(program, controller.type.enumIndex);
    };
    const auto validComputedController = [](const DslTypedField& controller,
                                            DslValueTypeKind expectedKind) {
        return controller.kind == DslTypedFieldKind::Declared &&
               controller.type.kind == expectedKind && controller.type.bitWidth == 0 &&
               controller.type.endian == DslEndian::Big && !controller.type.enumIndex &&
               !controller.equalsConstraint && !controller.rangeConstraint &&
               controller.computedExpression.has_value();
    };
    const auto validateConditions = [&](const std::vector<DslTypedFieldCondition>& conditions,
                                        std::size_t subjectFieldIndex,
                                        const DslTypedField* subject,
                                        const QString& subjectName) {
        for (std::size_t conditionIndex = 0; conditionIndex < conditions.size();
             ++conditionIndex) {
            const DslTypedFieldCondition& condition = conditions.at(conditionIndex);
            if (condition.expression) {
                TypedExpressionValidationState validation;
                validation.allowImportedContextReferences = true;
                if (condition.fieldIndex != 0 ||
                    condition.op != DslConditionOperator::Equal ||
                    (condition.expression->kind !=
                         DslTypedExpressionKind::ImportedContextReference &&
                     condition.expression->kind !=
                         DslTypedExpressionKind::SequenceElementReference) ||
                    !validateTypedExpression(*condition.expression,
                                             program,
                                             structure,
                                             subjectFieldIndex,
                                             conditions,
                                             1,
                                             validation) ||
                    condition.expression->type != DslScalarType::U64) {
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed IR %1 imported condition is invalid")
                                    .arg(subjectName),
                                subject);
                    return false;
                }
                continue;
            }
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
            const bool computedUnsignedController =
                validComputedController(controller, DslValueTypeKind::ComputedUnsigned);
            const bool computedBooleanController =
                validComputedController(controller, DslValueTypeKind::ComputedBool);
            const bool validController =
                condition.op == DslConditionOperator::Equal
                    ? fixedController || computedUnsignedController ||
                          unsignedExpGolombController ||
                          (computedBooleanController && condition.expectedValue == 1)
                    : fixedController || unsignedExpGolombController ||
                          computedUnsignedController;
            if (!validOperator || !validController ||
                (fixedController &&
                 !fitsUnsignedBits(condition.expectedValue, controller.type.bitWidth)) ||
                (unsignedExpGolombController &&
                 condition.expectedValue > maximumUnsignedExpGolombValue)) {
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
                [&conditions, availableEnd](
                    const DslTypedFieldCondition& required) {
                    return std::any_of(conditions.begin(),
                                       availableEnd,
                                       [&required](
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
        const bool computedBoolean =
            field.type.kind == DslValueTypeKind::ComputedBool;
        const bool computedUnsigned =
            field.type.kind == DslValueTypeKind::ComputedUnsigned;
        const bool lazyBytes = field.type.kind == DslValueTypeKind::LazyBytes;
        const bool compressedPayload =
            field.type.kind == DslValueTypeKind::CompressedPayload;
        if (compressedPayload) {
            if (fieldIndex + 1 != structure.fields.size() ||
                field.kind != DslTypedFieldKind::Declared || field.name.isEmpty() ||
                field.type.bitWidth != 0 || field.type.endian != DslEndian::Big ||
                field.type.enumIndex || field.contextEligible || field.equalsConstraint ||
                field.rangeConstraint || field.bitWidthExpression ||
                field.computedExpression || field.lazyByteCountExpression ||
                !field.conditions.empty() ||
                field.metadata.typeName != QStringLiteral("compressed_payload")) {
                markFailure(
                    DslExecutionStatus::InvalidDefinition,
                    QStringLiteral("Typed compressed payload definition is invalid"),
                    &field,
                    std::nullopt,
                    std::nullopt,
                    false);
                return result;
            }
            continue;
        }
        if (lazyBytes) {
            if (field.type.bitWidth != 0 || field.type.endian != DslEndian::Big ||
                field.type.enumIndex || field.equalsConstraint || field.rangeConstraint ||
                field.computedExpression || field.bitWidthExpression ||
                !field.lazyByteCountExpression) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed lazy byte region definition is invalid"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            TypedExpressionValidationState validation;
            if (!validateTypedExpression(*field.lazyByteCountExpression,
                                         program,
                                         structure,
                                         fieldIndex,
                                         field.conditions,
                                         1,
                                         validation)) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            validation.errorMessage,
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            if (field.lazyByteCountExpression->type != DslScalarType::U64) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed lazy byte-count expression result is invalid"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            continue;
        }
        if (field.lazyByteCountExpression) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Non-lazy typed field has a lazy byte-count expression"),
                        &field,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
        if (field.bitWidthExpression) {
            if (computedBoolean || computedUnsigned ||
                field.kind != DslTypedFieldKind::Declared ||
                field.type.kind != DslValueTypeKind::UnsignedBits ||
                field.type.bitWidth != 0 || field.type.endian != DslEndian::Big ||
                field.type.enumIndex || field.contextEligible ||
                field.equalsConstraint || field.rangeConstraint ||
                field.computedExpression) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed dynamic-width field definition is invalid"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            TypedExpressionValidationState validation;
            validation.allowImportedContextReferences = true;
            if (!validateTypedExpression(*field.bitWidthExpression,
                                         program,
                                         structure,
                                         fieldIndex,
                                         field.conditions,
                                         1,
                                         validation) ||
                field.bitWidthExpression->type != DslScalarType::U64) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            validation.errorMessage.isEmpty()
                                ? QStringLiteral(
                                      "Typed dynamic bit-width expression is invalid")
                                : validation.errorMessage,
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            continue;
        }
        if (!computedBoolean && !computedUnsigned) {
            if (field.computedExpression) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Source-backed typed field has a computed expression"),
                            &field);
                return result;
            }
            if ((field.type.kind == DslValueTypeKind::UnsignedExpGolomb &&
                 !validUnsignedExpGolombEnum(program, field.type.enumIndex)) ||
                (field.type.kind == DslValueTypeKind::SignedExpGolomb &&
                 field.type.enumIndex)) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed Exp-Golomb enum definition is invalid"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            if (field.type.kind == DslValueTypeKind::UnsignedExpGolomb &&
                field.equalsConstraint &&
                *field.equalsConstraint > maximumUnsignedExpGolombValue) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed ue equality constraint is out of range"),
                            &field);
                return result;
            }
            if (field.rangeConstraint &&
                (field.type.kind != DslValueTypeKind::UnsignedExpGolomb ||
                 field.rangeConstraint->minimum > field.rangeConstraint->maximum ||
                 field.rangeConstraint->maximum > maximumUnsignedExpGolombValue)) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed ue range constraint is out of range"),
                            &field);
                return result;
            }
            continue;
        }
        if (!validComputedController(field, field.type.kind)) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed computed field definition is invalid"),
                        &field,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
        TypedExpressionValidationState validation;
        if (!validateTypedExpression(*field.computedExpression,
                                     program,
                                     structure,
                                     fieldIndex,
                                     field.conditions,
                                     1,
                                     validation)) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        validation.errorMessage,
                        &field,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
        const DslScalarType expectedType = computedBoolean ? DslScalarType::Bool
                                                           : DslScalarType::U64;
        if (field.computedExpression->type != expectedType) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed computed expression result type is invalid"),
                        &field,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
    }
    for (std::size_t fieldIndex = 0; fieldIndex < structure.fields.size(); ++fieldIndex) {
        const DslTypedField& field = structure.fields.at(fieldIndex);
        if (!validateConditions(
                field.conditions, fieldIndex, &field, QStringLiteral("field"))) {
            return result;
        }
    }
    if (structure.contextDefinition) {
        if (!validContextDefinitionHeader(structure)) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed IR context definition is invalid"),
                        nullptr);
            return result;
        }
        if (!validContextDefinitionExports(structure)) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed IR context export is invalid"),
                        nullptr);
            return result;
        }
        if (!validContextDefinitionDependencies(structure)) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed IR context dependency is invalid"),
                        nullptr);
            return result;
        }
    }
    if (structure.contextImports.size() > DslTypedContextImport::maximumImports()) {
        markFailure(DslExecutionStatus::InvalidDefinition,
                    QStringLiteral("Typed IR declares too many context imports"),
                    nullptr);
        return result;
    }
    std::vector<std::pair<core::ContextDefinitionKind, quint32>> checkedImports;
    checkedImports.reserve(structure.contextImports.size());
    for (const DslTypedContextImport& import : structure.contextImports) {
        const auto identity = std::pair{import.kind, import.keyFieldIndex};
        if (!validContextKind(import.kind) ||
            !validContextField(structure, import.keyFieldIndex) ||
            std::find(checkedImports.begin(), checkedImports.end(), identity) !=
                checkedImports.end()) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed IR context import is invalid"),
                        nullptr);
            return result;
        }
        checkedImports.push_back(identity);
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
        const bool computedUnsignedController =
            validComputedController(controller, DslValueTypeKind::ComputedUnsigned);
        if ((!fixedController && !validUnsignedExpGolombController(controller) &&
             !computedUnsignedController) ||
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
            [&repeat](const DslTypedFieldCondition& required) {
                return std::any_of(repeat.conditions.begin(),
                                   repeat.conditions.end(),
                                   [&required](
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
    quint32 previousSentinelAssertionPosition = 0;
    for (std::size_t repeatIndex = 0;
         repeatIndex < structure.sentinelRepeats.size();
         ++repeatIndex) {
        const DslTypedSentinelRepeat& repeat =
            structure.sentinelRepeats.at(repeatIndex);
        const bool ordered =
            repeatIndex == 0 ||
            repeat.assertionFieldIndex >= previousSentinelAssertionPosition;
        if (!ordered || repeat.sentinelFieldIndices.empty() ||
            repeat.firstFieldIndices.size() !=
                repeat.sentinelFieldIndices.size() ||
            repeat.sentinelFieldIndices.size() >
                DslTypedSentinelRepeat::maximumIterations() ||
            repeat.assertionFieldIndex > structure.fields.size()) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed IR sentinel repeat is invalid"),
                        nullptr);
            return result;
        }
        std::vector<DslTypedFieldCondition> expectedConditions =
            repeat.conditions;
        quint32 previousSentinelFieldIndex = 0;
        for (std::size_t sentinelIndex = 0;
             sentinelIndex < repeat.sentinelFieldIndices.size();
             ++sentinelIndex) {
            const quint32 firstFieldIndex =
                repeat.firstFieldIndices.at(sentinelIndex);
            const quint32 fieldIndex =
                repeat.sentinelFieldIndices.at(sentinelIndex);
            const bool sentinelOrdered =
                sentinelIndex == 0 ||
                (firstFieldIndex > previousSentinelFieldIndex &&
                 fieldIndex > previousSentinelFieldIndex);
            const quint32 iterationEnd =
                sentinelIndex + 1 < repeat.firstFieldIndices.size()
                    ? repeat.firstFieldIndices.at(sentinelIndex + 1)
                    : repeat.assertionFieldIndex;
            if (!sentinelOrdered || firstFieldIndex > fieldIndex ||
                fieldIndex >= iterationEnd ||
                iterationEnd > repeat.assertionFieldIndex ||
                fieldIndex >= structure.fields.size()) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral(
                                "Typed IR sentinel repeat field index is invalid"),
                            nullptr);
                return result;
            }
            for (quint32 projectedFieldIndex = firstFieldIndex;
                 projectedFieldIndex < iterationEnd;
                 ++projectedFieldIndex) {
                const DslTypedField& projected =
                    structure.fields.at(projectedFieldIndex);
                if (projected.conditions.size() < expectedConditions.size() ||
                    !std::equal(expectedConditions.begin(),
                                expectedConditions.end(),
                                projected.conditions.begin(),
                                [](const DslTypedFieldCondition& left,
                                   const DslTypedFieldCondition& right) {
                                    return sameCondition(left, right);
                                })) {
                    markFailure(
                        DslExecutionStatus::InvalidDefinition,
                        QStringLiteral(
                            "Typed IR sentinel repeat projection guard is invalid"),
                        &projected);
                    return result;
                }
            }
            const DslTypedField& sentinel = structure.fields.at(fieldIndex);
            const bool fixedSentinel = validFixedController(sentinel);
            const bool unsignedExpGolombSentinel =
                validUnsignedExpGolombController(sentinel);
            const bool conditionsMatch =
                sentinel.conditions.size() == expectedConditions.size() &&
                std::equal(sentinel.conditions.begin(),
                           sentinel.conditions.end(),
                           expectedConditions.begin(),
                           [](const DslTypedFieldCondition& left,
                              const DslTypedFieldCondition& right) {
                               return sameCondition(left, right);
                           });
            if ((!fixedSentinel && !unsignedExpGolombSentinel) ||
                sentinel.contextEligible || !conditionsMatch ||
                (fixedSentinel &&
                 !fitsUnsignedBits(repeat.terminatingValue,
                                   sentinel.type.bitWidth)) ||
                (unsignedExpGolombSentinel &&
                 repeat.terminatingValue >
                     maximumUnsignedExpGolombValue)) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral(
                                "Typed IR sentinel repeat field is invalid"),
                            &sentinel);
                return result;
            }
            expectedConditions.push_back(
                {fieldIndex,
                 repeat.terminatingValue,
                 true,
                 DslConditionOperator::Equal,
                 std::nullopt});
            previousSentinelFieldIndex = fieldIndex;
        }
        const DslTypedField& firstSentinel =
            structure.fields.at(repeat.sentinelFieldIndices.front());
        if (!validateConditions(repeat.conditions,
                                repeat.sentinelFieldIndices.front(),
                                &firstSentinel,
                                QStringLiteral("sentinel repeat"))) {
            return result;
        }
        previousSentinelAssertionPosition = repeat.assertionFieldIndex;
    }

    if (structure.assertions.size() >
        DslTypedAssertion::maximumPerStructure()) {
        markFailure(DslExecutionStatus::InvalidDefinition,
                    QStringLiteral("Typed IR declares too many assertions"),
                    nullptr,
                    std::nullopt,
                    std::nullopt,
                    false);
        return result;
    }
    quint32 previousAssertionPosition = 0;
    for (std::size_t assertionIndex = 0;
         assertionIndex < structure.assertions.size();
         ++assertionIndex) {
        if (options.cancellation &&
            options.cancellation->isCancellationRequested()) {
            markFailure(DslExecutionStatus::Cancelled,
                        QStringLiteral("DSL execution was cancelled"),
                        nullptr);
            return result;
        }
        const DslTypedAssertion& assertion =
            structure.assertions.at(assertionIndex);
        const bool ordered = assertionIndex == 0 ||
                             assertion.assertionFieldIndex >=
                                 previousAssertionPosition;
        if (!ordered || assertion.assertionFieldIndex > structure.fields.size() ||
            assertion.anchorFieldIndex >= assertion.assertionFieldIndex ||
            assertion.anchorFieldIndex >= structure.fields.size()) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed assertion position or anchor is invalid"),
                        nullptr,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
        const DslTypedField& anchor =
            structure.fields.at(assertion.anchorFieldIndex);
        const bool sourceBacked =
            anchor.type.kind == DslValueTypeKind::UnsignedBits ||
            anchor.type.kind == DslValueTypeKind::Enum ||
            anchor.type.kind == DslValueTypeKind::UnsignedExpGolomb ||
            anchor.type.kind == DslValueTypeKind::SignedExpGolomb;
        if (anchor.kind != DslTypedFieldKind::Declared || !sourceBacked ||
            !anchor.conditions.empty()) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed assertion anchor is invalid"),
                        &anchor,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
        TypedExpressionValidationState validation;
        validation.allowImportedContextReferences = true;
        if (!validateTypedExpression(assertion.condition,
                                     program,
                                     structure,
                                     assertion.assertionFieldIndex,
                                     {},
                                     1,
                                     validation) ||
            assertion.condition.type != DslScalarType::Bool) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        validation.errorMessage.isEmpty()
                            ? QStringLiteral("Typed assertion condition is invalid")
                            : validation.errorMessage,
                        &anchor,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
        previousAssertionPosition = assertion.assertionFieldIndex;
    }

    const std::size_t bytecodeBegin = structure.bytecodeOffset;
    const std::size_t bytecodeEnd = bytecodeBegin + structure.bytecodeLength;
    std::size_t nextSentinelOpcodeIndex = 0;
    std::size_t nextAssertionOpcodeIndex = 0;
    std::size_t nextRepeatOpcodeIndex = 0;
    quint32 assertionFieldPosition = 0;
    bool assertionBytecodeBegan = false;
    bool assertionBytecodeEnded = false;
    const bool requiresPositionedAssertionPreflight = !structure.assertions.empty();
    const auto sentinelPendingAtOrBefore = [&]() {
        return nextSentinelOpcodeIndex < structure.sentinelRepeats.size() &&
               structure.sentinelRepeats.at(nextSentinelOpcodeIndex)
                       .assertionFieldIndex <= assertionFieldPosition;
    };
    const auto assertionPendingAtOrBefore = [&]() {
        return nextAssertionOpcodeIndex < structure.assertions.size() &&
               structure.assertions.at(nextAssertionOpcodeIndex)
                       .assertionFieldIndex <= assertionFieldPosition;
    };
    const auto repeatPendingAtOrBefore = [&]() {
        return nextRepeatOpcodeIndex < structure.repeatBounds.size() &&
               structure.repeatBounds.at(nextRepeatOpcodeIndex).firstFieldIndex <=
                   assertionFieldPosition;
    };
    const auto rejectPositionedAssertionBytecode = [&](const DslTypedField* field) {
        markFailure(DslExecutionStatus::InvalidDefinition,
                    QStringLiteral("Typed positioned assertion bytecode is invalid"),
                    field,
                    std::nullopt,
                    std::nullopt,
                    false);
    };
    for (std::size_t instructionIndex = bytecodeBegin;
         instructionIndex < bytecodeEnd;
         ++instructionIndex) {
        if ((instructionIndex - bytecodeBegin) %
                    options.limits.cancellationCheckInterval ==
                0 &&
            options.cancellation &&
            options.cancellation->isCancellationRequested()) {
            markFailure(DslExecutionStatus::Cancelled,
                        QStringLiteral("DSL execution was cancelled"),
                        nullptr);
            return result;
        }
        const DslInstruction& instruction = program.bytecode.at(instructionIndex);
        if (!requiresPositionedAssertionPreflight) {
            if (instruction.opcode == DslOpcode::AssertExpression) {
                rejectPositionedAssertionBytecode(nullptr);
                return result;
            }
            continue;
        }
        if (assertionBytecodeEnded) {
            rejectPositionedAssertionBytecode(nullptr);
            return result;
        }
        switch (instruction.opcode) {
        case DslOpcode::BeginStructure:
            if (assertionBytecodeBegan || instructionIndex != bytecodeBegin ||
                instruction.operand != structureIndex || instruction.immediate != 0) {
                rejectPositionedAssertionBytecode(nullptr);
                return result;
            }
            assertionBytecodeBegan = true;
            break;
        case DslOpcode::ReadUnsignedBits:
        case DslOpcode::ReadUnsignedExpGolomb:
        case DslOpcode::ReadSignedExpGolomb:
        case DslOpcode::EvaluateComputed:
        case DslOpcode::RegisterLazyBytes:
        case DslOpcode::RegisterCompressedPayload:
            if (!assertionBytecodeBegan || sentinelPendingAtOrBefore() ||
                assertionPendingAtOrBefore() || repeatPendingAtOrBefore()) {
                rejectPositionedAssertionBytecode(nullptr);
                return result;
            }
            ++assertionFieldPosition;
            break;
        case DslOpcode::ReadRbspTrailingBits:
            if (!assertionBytecodeBegan || sentinelPendingAtOrBefore() ||
                assertionPendingAtOrBefore() || repeatPendingAtOrBefore()) {
                rejectPositionedAssertionBytecode(nullptr);
                return result;
            }
            assertionFieldPosition += 8;
            break;
        case DslOpcode::AssertExpression: {
            const DslTypedAssertion* assertion =
                nextAssertionOpcodeIndex < structure.assertions.size()
                    ? &structure.assertions.at(nextAssertionOpcodeIndex)
                    : nullptr;
            if (!assertionBytecodeBegan || assertion == nullptr ||
                instruction.operand != nextAssertionOpcodeIndex ||
                instruction.immediate != 0 ||
                assertion->assertionFieldIndex != assertionFieldPosition ||
                sentinelPendingAtOrBefore()) {
                rejectPositionedAssertionBytecode(
                    assertion != nullptr
                        ? &structure.fields.at(assertion->anchorFieldIndex)
                        : nullptr);
                return result;
            }
            ++nextAssertionOpcodeIndex;
            break;
        }
        case DslOpcode::AssertRepeatCount: {
            const DslTypedRepeatBound* repeat =
                nextRepeatOpcodeIndex < structure.repeatBounds.size()
                    ? &structure.repeatBounds.at(nextRepeatOpcodeIndex)
                    : nullptr;
            if (!assertionBytecodeBegan || repeat == nullptr ||
                instruction.operand != nextRepeatOpcodeIndex ||
                instruction.immediate != repeat->maximumCount ||
                repeat->firstFieldIndex != assertionFieldPosition ||
                sentinelPendingAtOrBefore() || assertionPendingAtOrBefore()) {
                rejectPositionedAssertionBytecode(nullptr);
                return result;
            }
            ++nextRepeatOpcodeIndex;
            break;
        }
        case DslOpcode::AssertSentinelTerminated: {
            const DslTypedSentinelRepeat* repeat =
                nextSentinelOpcodeIndex < structure.sentinelRepeats.size()
                    ? &structure.sentinelRepeats.at(nextSentinelOpcodeIndex)
                    : nullptr;
            if (!assertionBytecodeBegan || repeat == nullptr ||
                instruction.operand != nextSentinelOpcodeIndex ||
                instruction.immediate != repeat->terminatingValue ||
                repeat->assertionFieldIndex != assertionFieldPosition) {
                rejectPositionedAssertionBytecode(nullptr);
                return result;
            }
            ++nextSentinelOpcodeIndex;
            break;
        }
        case DslOpcode::EndStructure:
            if (!assertionBytecodeBegan || instruction.operand != structureIndex ||
                instruction.immediate != 0 || sentinelPendingAtOrBefore() ||
                assertionPendingAtOrBefore() || repeatPendingAtOrBefore()) {
                rejectPositionedAssertionBytecode(nullptr);
                return result;
            }
            assertionBytecodeEnded = true;
            break;
        case DslOpcode::AssertEquals:
        case DslOpcode::AssertRangeMinimum:
        case DslOpcode::AssertRangeMaximum:
            break;
        }
    }
    if (requiresPositionedAssertionPreflight &&
        (!assertionBytecodeBegan || !assertionBytecodeEnded ||
         nextSentinelOpcodeIndex != structure.sentinelRepeats.size() ||
         nextAssertionOpcodeIndex != structure.assertions.size() ||
         nextRepeatOpcodeIndex != structure.repeatBounds.size())) {
        rejectPositionedAssertionBytecode(nullptr);
        return result;
    }
    std::vector<quint32> unsignedExpGolombReadCounts(structure.fields.size());
    std::vector<bool> conflictingEnumReads(structure.fields.size());
    for (std::size_t instructionIndex = bytecodeBegin;
         instructionIndex < bytecodeEnd;
         ++instructionIndex) {
        const DslInstruction& instruction = program.bytecode.at(instructionIndex);
        if (instruction.operand >= structure.fields.size()) {
            continue;
        }
        if (instruction.opcode == DslOpcode::ReadUnsignedExpGolomb) {
            ++unsignedExpGolombReadCounts.at(instruction.operand);
        } else if (instruction.opcode == DslOpcode::ReadUnsignedBits ||
                   instruction.opcode == DslOpcode::ReadSignedExpGolomb) {
            conflictingEnumReads.at(instruction.operand) = true;
        }
    }
    for (std::size_t fieldIndex = 0; fieldIndex < structure.fields.size(); ++fieldIndex) {
        const DslTypedField& field = structure.fields.at(fieldIndex);
        if (field.type.kind != DslValueTypeKind::UnsignedExpGolomb ||
            !field.type.enumIndex) {
            continue;
        }
        if (unsignedExpGolombReadCounts.at(fieldIndex) != 1 ||
            conflictingEnumReads.at(fieldIndex)) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral(
                            "Typed unsigned Exp-Golomb enum bytecode is invalid"),
                        &field,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
    }
    const auto compressedField = std::find_if(
        structure.fields.begin(),
        structure.fields.end(),
        [](const DslTypedField& field) {
            return field.type.kind == DslValueTypeKind::CompressedPayload;
        });
    const auto compressedOpcodes = static_cast<std::size_t>(std::count_if(
        program.bytecode.begin() + static_cast<std::ptrdiff_t>(bytecodeBegin),
        program.bytecode.begin() + static_cast<std::ptrdiff_t>(bytecodeEnd),
        [](const DslInstruction& instruction) {
            return instruction.opcode == DslOpcode::RegisterCompressedPayload;
        }));
    if (compressedField == structure.fields.end()) {
        if (compressedOpcodes != 0) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed compressed payload opcode has no field"),
                        nullptr,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
    } else {
        const quint32 fieldIndex = static_cast<quint32>(
            std::distance(structure.fields.begin(), compressedField));
        const bool validTail =
            structure.bytecodeLength >= 3 && compressedOpcodes == 1 &&
            program.bytecode.at(bytecodeEnd - 1).opcode == DslOpcode::EndStructure &&
            program.bytecode.at(bytecodeEnd - 1).operand == structureIndex &&
            program.bytecode.at(bytecodeEnd - 2).opcode ==
                DslOpcode::RegisterCompressedPayload &&
            program.bytecode.at(bytecodeEnd - 2).operand == fieldIndex &&
            program.bytecode.at(bytecodeEnd - 2).immediate == 0;
        if (!validTail) {
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed compressed payload bytecode is invalid"),
                        &*compressedField,
                        std::nullopt,
                        std::nullopt,
                        false);
            return result;
        }
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
    std::optional<DslExecutionContextValues> stagedContextValues;
    if (structure.contextDefinition) {
        stagedContextValues.emplace();
        stagedContextValues->dependencies.reserve(
            structure.contextDefinition->dependencies.size());
        stagedContextValues->exports.reserve(
            structure.contextDefinition->exportFieldIndices.size());
    }
    std::vector<DslExecutionContextImport> stagedContextImports;
    stagedContextImports.reserve(structure.contextImports.size());
    std::optional<quint32> lastField;
    std::optional<quint64> lastValue;
    std::optional<core::AnalysisNodeId> lastFieldNode;
    bool lastFieldSkipped = false;
    quint32 nextFieldIndex = 0;
    quint32 nextSentinelRepeatIndex = 0;
    quint32 nextAssertionIndex = 0;
    bool ended = false;
    const auto conditionsPresent = [&](const std::vector<DslTypedFieldCondition>& conditions,
                                       const DslTypedField* subject,
                                       const QString& subjectName) -> std::optional<bool> {
        for (const DslTypedFieldCondition& condition : conditions) {
            if (condition.expression) {
                const ComputedEvaluationResult evaluated = evaluateTypedExpression(
                    *condition.expression,
                    structure,
                    fieldValues,
                    contextValueResolver,
                    &options.sequenceElementValues);
                if (!evaluated.complete() || evaluated.value.type != DslScalarType::U64) {
                    const DslTypedField* diagnosticField = subject;
                    std::optional<quint64> diagnosticPosition;
                    std::optional<quint64> diagnosticBits;
                    if (evaluated.diagnosticFieldIndex &&
                        *evaluated.diagnosticFieldIndex < structure.fields.size()) {
                        diagnosticField =
                            &structure.fields.at(*evaluated.diagnosticFieldIndex);
                        if (const auto& range =
                                fieldRanges.at(*evaluated.diagnosticFieldIndex)) {
                            diagnosticPosition = range->start;
                            diagnosticBits = range->bitCount;
                        }
                    }
                    markFailure(evaluated.status,
                                evaluated.errorMessage.isEmpty()
                                    ? QStringLiteral("Imported condition value is unavailable")
                                    : evaluated.errorMessage,
                                diagnosticField,
                                diagnosticPosition,
                                diagnosticBits,
                                diagnosticPosition.has_value());
                    return std::nullopt;
                }
                const bool matches = evaluated.value.unsignedValue == condition.expectedValue;
                if (condition.negated ? matches : !matches) {
                    return false;
                }
                continue;
            }
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
            if (field.kind != DslTypedFieldKind::Declared) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR field instruction uses a generated field"),
                            &field);
                return result;
            }
            const bool readsFixedBits = instruction.opcode == DslOpcode::ReadUnsignedBits;
            const bool readsUnsignedExpGolomb =
                instruction.opcode == DslOpcode::ReadUnsignedExpGolomb;
            const DslTypedEnum* enumeration = nullptr;
            if (readsFixedBits) {
                const bool dynamicWidth = field.bitWidthExpression.has_value();
                if ((!dynamicWidth &&
                     (field.type.bitWidth == 0 || field.type.bitWidth > 64)) ||
                    (dynamicWidth &&
                     (field.type.bitWidth != 0 ||
                      field.type.endian != DslEndian::Big)) ||
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
                case DslValueTypeKind::ComputedBool:
                case DslValueTypeKind::ComputedUnsigned:
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed IR opcode does not match the field type"),
                                &field);
                    return result;
                default:
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed IR field type kind is invalid"),
                                &field);
                    return result;
                }
            } else {
                const DslValueTypeKind expectedKind =
                    readsUnsignedExpGolomb ? DslValueTypeKind::UnsignedExpGolomb
                                           : DslValueTypeKind::SignedExpGolomb;
                if (field.type.kind != expectedKind || field.type.bitWidth != 0 ||
                    field.type.endian != DslEndian::Big ||
                    (!readsUnsignedExpGolomb && field.type.enumIndex) ||
                    (readsUnsignedExpGolomb &&
                     !validUnsignedExpGolombEnum(program, field.type.enumIndex)) ||
                    (!readsUnsignedExpGolomb &&
                     (field.equalsConstraint || field.rangeConstraint))) {
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed Exp-Golomb field definition is invalid"),
                                &field);
                    return result;
                }
                if (readsUnsignedExpGolomb && field.type.enumIndex) {
                    enumeration = &program.enums.at(*field.type.enumIndex);
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
            quint64 bitWidth = field.type.bitWidth;
            if (readsFixedBits && field.bitWidthExpression) {
                const ComputedEvaluationResult evaluated =
                    evaluateTypedExpression(*field.bitWidthExpression,
                                            structure,
                                            fieldValues,
                                            contextValueResolver,
                                            &options.sequenceElementValues);
                if (!evaluated.complete()) {
                    const DslTypedField* diagnosticField = &field;
                    std::optional<quint64> diagnosticPosition;
                    std::optional<quint64> diagnosticBits;
                    if (evaluated.diagnosticFieldIndex &&
                        *evaluated.diagnosticFieldIndex < structure.fields.size() &&
                        fieldRanges.at(*evaluated.diagnosticFieldIndex)) {
                        diagnosticField =
                            &structure.fields.at(*evaluated.diagnosticFieldIndex);
                        diagnosticPosition =
                            fieldRanges.at(*evaluated.diagnosticFieldIndex)->start;
                        diagnosticBits =
                            fieldRanges.at(*evaluated.diagnosticFieldIndex)->bitCount;
                    }
                    markFailure(evaluated.status,
                                evaluated.errorMessage,
                                diagnosticField,
                                diagnosticPosition,
                                diagnosticBits);
                    return result;
                }
                bitWidth = evaluated.value.unsignedValue;
                if (bitWidth == 0 || bitWidth > 64) {
                    markFailure(DslExecutionStatus::InvalidSyntax,
                                QStringLiteral(
                                    "Dynamic bit width must be in the range 1..64"),
                                &field,
                                fieldStart,
                                std::min<quint64>(bitWidth,
                                                  reader.remainingBits()));
                    return result;
                }
            }
            if (readsFixedBits && field.type.endian == DslEndian::Little) {
                const bool hasReadableBit = reader.remainingBits() != 0;
                const auto firstBitLocation = hasReadableBit
                                                  ? locationAt(mapping,
                                                               logicalStart,
                                                               fieldStart,
                                                               1,
                                                               1)
                                                  : std::nullopt;
                const bool sourceStartMisaligned =
                    hasReadableBit &&
                    (!firstBitLocation || firstBitLocation->sourceSpans().empty() ||
                     firstBitLocation->sourceSpans()
                                 .front()
                                 .start()
                                 .absoluteBitOffset() %
                             8 !=
                         0);
                if (addWouldOverflow(logicalStart, fieldStart) ||
                    (logicalStart + fieldStart) % 8 != 0 ||
                    sourceStartMisaligned) {
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed IR field type is invalid"),
                                &field);
                    return result;
                }
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
                const core::BitReadResult readResult =
                    reader.readBits(static_cast<unsigned int>(bitWidth));
                result.bitsConsumed = reader.position();
                if (!readResult.complete()) {
                    const DslExecutionStatus status = statusForRead(readResult.status);
                    markFailure(status,
                                readResult.errorMessage.isEmpty()
                                    ? QStringLiteral("Unable to read complete syntax field")
                                    : readResult.errorMessage,
                                &field,
                                fieldStart,
                                bitWidth);
                    return result;
                }
                consumedBits = bitWidth;
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
            if (!location) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to map DSL field location"),
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
            const std::optional<core::AnalysisNodeId> fieldNode =
                tree.appendChild(*result.structureNode, std::move(fieldSpec));
            if (!fieldNode) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to append typed field node"),
                            &field);
                return result;
            }
            lastFieldNode = fieldNode;
            ++result.nodesCreated;
            fieldValues.at(instruction.operand) =
                readsFixedBits || readsUnsignedExpGolomb
                    ? std::optional<quint64>(unsignedValue)
                    : std::nullopt;
            fieldRanges.at(instruction.operand) =
                MaterializedFieldRange{fieldStart, consumedBits};
            lastField = instruction.operand;
            lastValue = readsFixedBits || readsUnsignedExpGolomb
                             ? std::optional<quint64>(unsignedValue)
                             : std::nullopt;
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
        case DslOpcode::ReadRbspTrailingBits: {
            constexpr quint32 reservedFieldCount = 8;
            if (!result.structureNode || instruction.operand != nextFieldIndex ||
                instruction.immediate != 0 || structure.fields.size() < reservedFieldCount ||
                instruction.operand != structure.fields.size() - reservedFieldCount) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR rbsp trailing-bits instruction is invalid"),
                            nullptr);
                return result;
            }

            const auto validGeneratedField = [&](const DslTypedField& field,
                                                 DslTypedFieldKind expectedKind,
                                                 quint32 index) {
                const QString expectedName =
                    index == 0 ? QStringLiteral("rbsp_stop_one_bit")
                               : QStringLiteral("rbsp_alignment_zero_bit[%1]").arg(index - 1);
                return field.kind == expectedKind && field.name == expectedName &&
                       field.type.kind == DslValueTypeKind::UnsignedBits &&
                       field.type.bitWidth == 1 && field.type.endian == DslEndian::Big &&
                       !field.type.enumIndex &&
                       field.equalsConstraint == std::optional<quint64>(index == 0 ? 1 : 0) &&
                       !field.computedExpression && !field.lazyByteCountExpression &&
                       field.conditions.empty() && field.metadata.typeName == QStringLiteral("bits") &&
                       field.metadata.specification &&
                       field.metadata.specification->standard == QStringLiteral("ITU-T H.264") &&
                       field.metadata.specification->clause == QStringLiteral("7.3.2.11");
            };
            for (quint32 index = 0; index < reservedFieldCount; ++index) {
                const DslTypedField& field = structure.fields.at(instruction.operand + index);
                const DslTypedFieldKind expectedKind =
                    index == 0 ? DslTypedFieldKind::RbspStopOneBit
                               : DslTypedFieldKind::RbspAlignmentZeroBit;
                if (!validGeneratedField(field, expectedKind, index)) {
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed IR rbsp trailing-bits fields are invalid"),
                                &field);
                    return result;
                }
            }

            const quint32 structureDepth = parentDepth + 1U;
            if (structureDepth >= options.limits.maximumNodeDepth) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL analysis node depth limit exceeded"),
                            &structure.fields.at(instruction.operand));
                return result;
            }

            quint32 fieldsToRead = 1;
            for (quint32 fieldCount = 0; fieldCount < fieldsToRead; ++fieldCount) {
                const DslTypedField& field = structure.fields.at(instruction.operand + fieldCount);
                if (result.nodesCreated >= options.limits.maximumMaterializedNodes) {
                    markFailure(DslExecutionStatus::ResourceLimit,
                                QStringLiteral("DSL materialized-node budget exceeded"),
                                &field);
                    return result;
                }
                const quint64 fieldStart = reader.position();
                const core::BitReadResult readResult = reader.readBits(1);
                result.bitsConsumed = reader.position();
                if (!readResult.complete()) {
                    markFailure(statusForRead(readResult.status),
                                readResult.errorMessage.isEmpty()
                                    ? QStringLiteral("Unable to read complete rbsp trailing bit")
                                    : readResult.errorMessage,
                                &field,
                                fieldStart);
                    return result;
                }
                const auto location = locationAt(mapping, logicalStart, fieldStart, 1, 1);
                if (!location) {
                    markFailure(DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Unable to map rbsp trailing-bit location"),
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
                                QStringLiteral("Unable to append rbsp trailing-bit node"),
                                &field);
                    return result;
                }
                ++result.nodesCreated;
                fieldValues.at(instruction.operand + fieldCount) = readResult.value;
                fieldRanges.at(instruction.operand + fieldCount) =
                    MaterializedFieldRange{fieldStart, 1};
                lastField = instruction.operand + fieldCount;
                lastValue = readResult.value;
                lastFieldSkipped = false;
                if (readResult.value != *field.equalsConstraint) {
                    markFailure(DslExecutionStatus::InvalidSyntax,
                                QStringLiteral("Field value violates rbsp trailing-bits constraint"),
                                &field,
                                fieldStart,
                                1);
                    return result;
                }
                if (fieldCount == 0) {
                    if (addWouldOverflow(logicalStart, reader.position())) {
                        markFailure(DslExecutionStatus::InvalidDefinition,
                                    QStringLiteral("Logical rbsp trailing-bit offset overflow"),
                                    &field);
                        return result;
                    }
                    const quint64 paddingCount =
                        (8U - ((logicalStart + reader.position()) % 8U)) % 8U;
                    fieldsToRead += static_cast<quint32>(paddingCount);
                }
            }
            for (quint32 skipped = fieldsToRead; skipped < reservedFieldCount; ++skipped) {
                fieldValues.at(instruction.operand + skipped).reset();
                fieldRanges.at(instruction.operand + skipped).reset();
            }
            nextFieldIndex += reservedFieldCount;
            break;
        }
        case DslOpcode::EvaluateComputed: {
            if (!result.structureNode || instruction.operand != nextFieldIndex ||
                instruction.operand >= structure.fields.size()) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR computed instruction is invalid"),
                            nullptr);
                return result;
            }
            const DslTypedField& field = structure.fields.at(instruction.operand);
            if (field.kind != DslTypedFieldKind::Declared) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR computed instruction uses a generated field"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const bool computedBoolean =
                field.type.kind == DslValueTypeKind::ComputedBool;
            const bool computedUnsigned =
                field.type.kind == DslValueTypeKind::ComputedUnsigned;
            if ((!computedBoolean && !computedUnsigned) || !field.computedExpression) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR computed field definition is invalid"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const std::optional<bool> fieldPresent =
                conditionsPresent(field.conditions, &field, QStringLiteral("computed field"));
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
            const quint32 structureDepth = parentDepth + 1U;
            if (structureDepth >= options.limits.maximumNodeDepth) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL analysis node depth limit exceeded"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            if (result.nodesCreated >= options.limits.maximumMaterializedNodes) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL materialized-node budget exceeded"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const ComputedEvaluationResult evaluated =
                evaluateTypedExpression(*field.computedExpression,
                                        structure,
                                        fieldValues,
                                        contextValueResolver,
                                        &options.sequenceElementValues);
            if (!evaluated.complete()) {
                markFailure(evaluated.status,
                            evaluated.errorMessage,
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            core::AnalysisNodeSpec fieldSpec;
            fieldSpec.kind = core::AnalysisNodeKind::ComputedField;
            fieldSpec.name = field.name;
            fieldSpec.state = core::MaterializationState::Materialized;
            fieldSpec.value = computedBoolean
                                  ? QVariant::fromValue<bool>(evaluated.value.booleanValue)
                                  : QVariant::fromValue<qulonglong>(
                                        evaluated.value.unsignedValue);
            fieldSpec.metadata = field.metadata;
            if (!tree.appendChild(*result.structureNode, std::move(fieldSpec))) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to append computed field node"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            ++result.nodesCreated;
            fieldValues.at(instruction.operand) =
                computedBoolean ? static_cast<quint64>(evaluated.value.booleanValue)
                                : evaluated.value.unsignedValue;
            fieldRanges.at(instruction.operand).reset();
            lastField = instruction.operand;
            lastValue.reset();
            lastFieldSkipped = false;
            ++nextFieldIndex;
            break;
        }
        case DslOpcode::RegisterLazyBytes: {
            if (!result.structureNode || instruction.operand != nextFieldIndex ||
                instruction.operand >= structure.fields.size()) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR lazy byte region instruction is invalid"),
                            nullptr);
                return result;
            }
            const DslTypedField& field = structure.fields.at(instruction.operand);
            if (field.kind != DslTypedFieldKind::Declared) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR lazy instruction uses a generated field"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            if (field.type.kind != DslValueTypeKind::LazyBytes ||
                !field.lazyByteCountExpression) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR lazy byte region definition is invalid"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const std::optional<bool> fieldPresent =
                conditionsPresent(field.conditions, &field, QStringLiteral("lazy byte region"));
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

            const ComputedEvaluationResult evaluated =
                evaluateTypedExpression(*field.lazyByteCountExpression,
                                        structure,
                                        fieldValues,
                                        contextValueResolver,
                                        &options.sequenceElementValues);
            if (!evaluated.complete()) {
                markFailure(evaluated.status,
                            evaluated.errorMessage,
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const quint64 byteCount = evaluated.value.unsignedValue;
            if (byteCount > std::numeric_limits<quint64>::max() / 8U) {
                markFailure(DslExecutionStatus::InvalidSyntax,
                            QStringLiteral("Lazy byte region bit length overflows"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const quint64 bitCount = byteCount * 8U;
            const quint64 fieldStart = reader.position();
            if (addWouldOverflow(logicalStart, fieldStart) ||
                (logicalStart + fieldStart) % 8U != 0) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed lazy byte region start is not byte-aligned"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            if (bitCount > reader.remainingBits()) {
                const quint64 availableBits = reader.remainingBits();
                markFailure(DslExecutionStatus::TruncatedSource,
                            QStringLiteral("Lazy byte region exceeds the available source range"),
                            &field,
                            fieldStart,
                            availableBits,
                            availableBits != 0);
                return result;
            }
            const quint64 absoluteStart = logicalStart + fieldStart;
            const auto range = core::LogicalRange::create(
                core::LogicalBitAddress(mapping.viewId(), absoluteStart), bitCount);
            const auto location = range ? mapping.locate(*range) : std::nullopt;
            if (!location) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to map DSL lazy byte region"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const quint32 structureDepth = parentDepth + 1U;
            if (structureDepth >= options.limits.maximumNodeDepth) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL analysis node depth limit exceeded"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            if (result.nodesCreated >= options.limits.maximumMaterializedNodes) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL materialized-node budget exceeded"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }

            core::AnalysisNodeSpec fieldSpec;
            fieldSpec.kind = core::AnalysisNodeKind::Region;
            fieldSpec.name = field.name;
            fieldSpec.state = bitCount == 0 ? core::MaterializationState::Materialized
                                            : core::MaterializationState::Lazy;
            fieldSpec.location = *location;
            fieldSpec.metadata = field.metadata;
            if (!tree.appendChild(*result.structureNode, std::move(fieldSpec))) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to append lazy byte region node"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            ++result.nodesCreated;
            const quint64 fieldEnd = fieldStart + bitCount;
            (void)reader.seek(fieldEnd);
            result.bitsConsumed = reader.position();
            fieldValues.at(instruction.operand).reset();
            fieldRanges.at(instruction.operand) = MaterializedFieldRange{fieldStart, bitCount};
            lastField = instruction.operand;
            lastValue.reset();
            lastFieldSkipped = false;
            ++nextFieldIndex;
            break;
        }
        case DslOpcode::RegisterCompressedPayload: {
            if (!result.structureNode || instruction.operand != nextFieldIndex ||
                instruction.operand >= structure.fields.size() ||
                instruction.operand + 1 != structure.fields.size() ||
                instruction.immediate != 0) {
                markFailure(
                    DslExecutionStatus::InvalidDefinition,
                    QStringLiteral("Typed IR compressed payload instruction is invalid"),
                    nullptr);
                return result;
            }
            const DslTypedField& field = structure.fields.at(instruction.operand);
            if (field.type.kind != DslValueTypeKind::CompressedPayload) {
                markFailure(
                    DslExecutionStatus::InvalidDefinition,
                    QStringLiteral("Typed IR compressed payload definition is invalid"),
                    &field,
                    std::nullopt,
                    std::nullopt,
                    false);
                return result;
            }

            const quint64 fieldStart = reader.position();
            const quint64 bitCount = reader.remainingBits();
            if (addWouldOverflow(logicalStart, fieldStart)) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Logical compressed payload offset overflows"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const auto range = core::LogicalRange::create(
                core::LogicalBitAddress(mapping.viewId(), logicalStart + fieldStart),
                bitCount);
            const auto location = range ? mapping.locate(*range) : std::nullopt;
            if (!location) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to map DSL compressed payload"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const quint32 structureDepth = parentDepth + 1U;
            if (structureDepth >= options.limits.maximumNodeDepth) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL analysis node depth limit exceeded"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            if (result.nodesCreated >= options.limits.maximumMaterializedNodes) {
                markFailure(DslExecutionStatus::ResourceLimit,
                            QStringLiteral("DSL materialized-node budget exceeded"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }

            core::AnalysisNodeSpec fieldSpec;
            fieldSpec.kind = core::AnalysisNodeKind::CompressedPayload;
            fieldSpec.name = field.name;
            fieldSpec.state = core::MaterializationState::Materialized;
            fieldSpec.location = *location;
            fieldSpec.metadata = field.metadata;
            if (!tree.appendChild(*result.structureNode, std::move(fieldSpec))) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to append compressed payload node"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            ++result.nodesCreated;
            if (!reader.seek(reader.logicalBitLength())) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to advance over compressed payload"),
                            &field,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            result.bitsConsumed = reader.position();
            fieldValues.at(instruction.operand).reset();
            fieldRanges.at(instruction.operand) =
                MaterializedFieldRange{fieldStart, bitCount};
            lastField = instruction.operand;
            lastValue.reset();
            lastFieldSkipped = false;
            ++nextFieldIndex;
            break;
        }
        case DslOpcode::AssertExpression: {
            const DslTypedAssertion* assertion =
                instruction.operand < structure.assertions.size()
                    ? &structure.assertions.at(instruction.operand)
                    : nullptr;
            if (!result.structureNode || assertion == nullptr ||
                instruction.operand != nextAssertionIndex ||
                instruction.immediate != 0 ||
                assertion->assertionFieldIndex != nextFieldIndex) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed assertion instruction is invalid"),
                            nullptr);
                return result;
            }
            ++nextAssertionIndex;
            const DslTypedField& anchor =
                structure.fields.at(assertion->anchorFieldIndex);
            const std::optional<MaterializedFieldRange>& anchorRange =
                fieldRanges.at(assertion->anchorFieldIndex);
            if (!anchorRange) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed assertion anchor range is unavailable"),
                            &anchor,
                            std::nullopt,
                            std::nullopt,
                            false);
                return result;
            }
            const ComputedEvaluationResult evaluated = evaluateTypedExpression(
                assertion->condition,
                structure,
                fieldValues,
                contextValueResolver,
                &options.sequenceElementValues);
            if (!evaluated.complete() || evaluated.value.type != DslScalarType::Bool) {
                const DslTypedField* diagnosticField = &anchor;
                std::optional<MaterializedFieldRange> diagnosticRange = anchorRange;
                if (evaluated.diagnosticFieldIndex &&
                    *evaluated.diagnosticFieldIndex < structure.fields.size()) {
                    diagnosticField =
                        &structure.fields.at(*evaluated.diagnosticFieldIndex);
                    diagnosticRange =
                        fieldRanges.at(*evaluated.diagnosticFieldIndex);
                }
                markFailure(
                    evaluated.complete() ? DslExecutionStatus::InvalidDefinition
                                         : evaluated.status,
                    evaluated.errorMessage.isEmpty()
                        ? QStringLiteral("Assertion condition evaluation failed")
                        : evaluated.errorMessage,
                    diagnosticField,
                    diagnosticRange
                        ? std::optional<quint64>(diagnosticRange->start)
                        : std::nullopt,
                    diagnosticRange
                        ? std::optional<quint64>(diagnosticRange->bitCount)
                        : std::nullopt,
                    diagnosticRange.has_value());
                return result;
            }
            if (evaluated.value.booleanValue) {
                break;
            }
            markFailure(DslExecutionStatus::InvalidSyntax,
                        QStringLiteral("Assertion condition is false"),
                        &anchor,
                        anchorRange->start,
                        anchorRange->bitCount,
                        true);
            return result;
        }
        case DslOpcode::AssertEquals: {
            const DslTypedField* field = instruction.operand < structure.fields.size()
                                             ? &structure.fields.at(instruction.operand)
                                             : nullptr;
            if (!result.structureNode || !lastField || *lastField != instruction.operand ||
                field == nullptr || !field->equalsConstraint ||
                field->kind != DslTypedFieldKind::Declared ||
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
            const auto range = fieldRanges.at(instruction.operand);
            if (!range) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR equality field range is unavailable"),
                            field);
                return result;
            }
            diagnostic.location = locationAt(mapping,
                                             logicalStart,
                                             range->start,
                                             range->bitCount,
                                             range->bitCount);
            (void)tree.markPartial(*result.structureNode,
                                   core::MaterializationState::Invalid,
                                   std::move(diagnostic));
            return result;
        }
        case DslOpcode::AssertRangeMinimum:
        case DslOpcode::AssertRangeMaximum: {
            const bool checksMinimum = instruction.opcode == DslOpcode::AssertRangeMinimum;
            const DslTypedField* field = instruction.operand < structure.fields.size()
                                             ? &structure.fields.at(instruction.operand)
                                             : nullptr;
            if (!result.structureNode || !lastField || *lastField != instruction.operand ||
                field == nullptr || !field->rangeConstraint ||
                field->kind != DslTypedFieldKind::Declared ||
                field->type.kind != DslValueTypeKind::UnsignedExpGolomb ||
                (checksMinimum ? field->rangeConstraint->minimum
                               : field->rangeConstraint->maximum) != instruction.immediate ||
                (lastFieldSkipped ? lastValue.has_value() : !lastValue.has_value())) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR range instruction is invalid"),
                            nullptr);
                return result;
            }
            if (lastFieldSkipped) {
                break;
            }
            if (checksMinimum ? *lastValue >= instruction.immediate
                              : *lastValue <= instruction.immediate) {
                break;
            }
            if (!lastFieldNode) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR range field node is unavailable"),
                            field);
                return result;
            }
            const auto range = fieldRanges.at(instruction.operand);
            if (!range) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR range field range is unavailable"),
                            field);
                return result;
            }
            core::ParseDiagnostic diagnostic;
            diagnostic.code = core::DiagnosticCode::InvalidSyntax;
            diagnostic.severity = core::DiagnosticSeverity::Warning;
            diagnostic.message =
                checksMinimum
                    ? QStringLiteral("Field value is below its @range minimum")
                    : QStringLiteral("Field value is above its @range maximum");
            diagnostic.fieldPath = structure.name + QLatin1Char('.') + field->name;
            diagnostic.location = locationAt(mapping,
                                             logicalStart,
                                             range->start,
                                             range->bitCount,
                                             range->bitCount);
            if (!tree.addDiagnostic(*lastFieldNode, std::move(diagnostic))) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Unable to attach @range diagnostic"),
                            field);
                return result;
            }
            break;
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
                            : std::nullopt,
                        controllerRange.has_value());
            return result;
        }
        case DslOpcode::AssertSentinelTerminated: {
            const DslTypedSentinelRepeat* repeat =
                instruction.operand < structure.sentinelRepeats.size()
                    ? &structure.sentinelRepeats.at(instruction.operand)
                    : nullptr;
            if (!result.structureNode || repeat == nullptr ||
                instruction.operand != nextSentinelRepeatIndex ||
                repeat->assertionFieldIndex != nextFieldIndex ||
                repeat->terminatingValue != instruction.immediate) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral(
                                "Typed IR sentinel repeat assertion is invalid"),
                            nullptr);
                return result;
            }
            ++nextSentinelRepeatIndex;
            const DslTypedField& firstSentinel =
                structure.fields.at(repeat->sentinelFieldIndices.front());
            const std::optional<bool> repeatPresent = conditionsPresent(
                repeat->conditions,
                &firstSentinel,
                QStringLiteral("sentinel repeat"));
            if (!repeatPresent) {
                return result;
            }
            if (!*repeatPresent) {
                break;
            }
            const DslTypedField* lastSentinel = nullptr;
            std::optional<MaterializedFieldRange> lastSentinelRange;
            bool terminated = false;
            for (const quint32 fieldIndex : repeat->sentinelFieldIndices) {
                const std::optional<quint64>& value = fieldValues.at(fieldIndex);
                if (!value) {
                    markFailure(
                        DslExecutionStatus::InvalidDefinition,
                        QStringLiteral(
                            "Typed IR sentinel repeat field is unavailable"),
                        &structure.fields.at(fieldIndex));
                    return result;
                }
                lastSentinel = &structure.fields.at(fieldIndex);
                lastSentinelRange = fieldRanges.at(fieldIndex);
                if (*value == repeat->terminatingValue) {
                    terminated = true;
                    break;
                }
            }
            if (terminated) {
                break;
            }
            if (lastSentinel == nullptr) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral(
                                "Typed IR sentinel repeat has no selected field"),
                            nullptr);
                return result;
            }
            markFailure(
                DslExecutionStatus::InvalidSyntax,
                QStringLiteral(
                    "Sentinel repeat did not terminate within its declared maximum"),
                lastSentinel,
                lastSentinelRange
                    ? std::optional<quint64>(lastSentinelRange->start)
                    : std::nullopt,
                lastSentinelRange
                    ? std::optional<quint64>(lastSentinelRange->bitCount)
                    : std::nullopt,
                lastSentinelRange.has_value());
            return result;
        }
        case DslOpcode::EndStructure: {
            if (!result.structureNode || instruction.operand != structureIndex ||
                nextFieldIndex != structure.fields.size() ||
                nextSentinelRepeatIndex != structure.sentinelRepeats.size() ||
                nextAssertionIndex != structure.assertions.size()) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR end instruction is invalid"),
                            nullptr);
                return result;
            }
            if (structure.contextDefinition || !structure.contextImports.empty()) {
                const auto materializedContextValue =
                    [&](quint32 fieldIndex) -> std::optional<DslExecutionContextValue> {
                    if (fieldIndex >= fieldValues.size() || !fieldValues.at(fieldIndex)) {
                        return std::nullopt;
                    }
                    DslExecutionContextValue value;
                    value.value = *fieldValues.at(fieldIndex);
                    if (fieldRanges.at(fieldIndex)) {
                        const MaterializedFieldRange& range = *fieldRanges.at(fieldIndex);
                        value.location = locationAt(mapping,
                                                    logicalStart,
                                                    range.start,
                                                    range.bitCount,
                                                    range.bitCount);
                        if (!value.location) {
                            return std::nullopt;
                        }
                    }
                    return value;
                };
                if (structure.contextDefinition) {
                    const DslTypedContextDefinition& definition =
                        *structure.contextDefinition;
                    const auto key = materializedContextValue(definition.keyFieldIndex);
                    if (!key) {
                        markFailure(DslExecutionStatus::InvalidDefinition,
                                    QStringLiteral("Typed IR context key value is unavailable"),
                                    nullptr);
                        return result;
                    }
                    stagedContextValues->key = *key;
                    for (const DslTypedContextDependency& dependency :
                         definition.dependencies) {
                        const auto value =
                            materializedContextValue(dependency.keyFieldIndex);
                        if (!value) {
                            markFailure(
                                DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed IR context dependency value is unavailable"),
                                nullptr);
                            return result;
                        }
                        stagedContextValues->dependencies.push_back(*value);
                    }
                    for (const quint32 fieldIndex : definition.exportFieldIndices) {
                        const auto value = materializedContextValue(fieldIndex);
                        if (!value) {
                            markFailure(
                                DslExecutionStatus::InvalidDefinition,
                                QStringLiteral("Typed IR context export value is unavailable"),
                                nullptr);
                            return result;
                        }
                        stagedContextValues->exports.push_back(*value);
                    }
                }
                for (const DslTypedContextImport& import : structure.contextImports) {
                    const auto key = materializedContextValue(import.keyFieldIndex);
                    if (!key) {
                        markFailure(DslExecutionStatus::InvalidDefinition,
                                    QStringLiteral("Typed IR context import key is unavailable"),
                                    nullptr);
                        return result;
                    }
                    stagedContextImports.push_back({import.kind, *key});
                }
            }
            if (!tree.transition(*result.structureNode,
                                 core::MaterializationState::Materialized)) {
                markFailure(DslExecutionStatus::InvalidDefinition,
                            QStringLiteral("Typed IR end instruction is invalid"),
                            nullptr);
                return result;
            }
            result.contextValues = std::move(stagedContextValues);
            result.contextImports = std::move(stagedContextImports);
            result.status = DslExecutionStatus::Materialized;
            ended = true;
            break;
        }
        default:
            markFailure(DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Typed IR opcode is invalid"),
                        nullptr);
            return result;
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
