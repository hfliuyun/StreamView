#include <streamview/rules/dsl_executor.h>

#include <QVariant>

#include <limits>

namespace streamview::rules {

namespace {

[[nodiscard]] const DslStruct* findStruct(const DslProgram& program, const QString& name) {
    for (const DslStruct& structure : program.structs) {
        if (structure.name == name) {
            return &structure;
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<quint64> equalsConstraint(const DslBitField& field) {
    for (const DslAnnotation& annotation : field.annotations) {
        if (annotation.name == QStringLiteral("equals") && annotation.arguments.size() == 1 &&
            annotation.arguments.front().kind == DslAnnotationValueKind::Integer) {
            return annotation.arguments.front().integerValue;
        }
    }
    return std::nullopt;
}

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
    if (status == DslExecutionStatus::SourceError) {
        return core::DiagnosticCode::SourceError;
    }
    if (status == DslExecutionStatus::InvalidDefinition ||
        status == DslExecutionStatus::InvalidSyntax) {
        return core::DiagnosticCode::InvalidSyntax;
    }
    return core::DiagnosticCode::TruncatedSource;
}

} // namespace

DslExecutionResult DslExecutor::decodeStruct(const DslProgram& program,
                                             const QString& structureName,
                                             core::BitReader& reader,
                                             const core::SourceMapping& mapping,
                                             quint64 logicalStart,
                                             core::AnalysisTree& tree,
                                             core::AnalysisNodeId parentId) {
    DslExecutionResult result;
    const DslStruct* structure = findStruct(program, structureName);
    if (structure == nullptr || structure->fields.empty()) {
        result.errorMessage = QStringLiteral("Structure is not declared or has no fields");
        return result;
    }

    core::AnalysisNodeSpec structureSpec;
    structureSpec.kind = core::AnalysisNodeKind::Structure;
    structureSpec.name = structure->name;
    structureSpec.state = core::MaterializationState::Indexing;
    result.structureNode = tree.appendChild(parentId, std::move(structureSpec));
    if (!result.structureNode) {
        result.errorMessage = QStringLiteral("Unable to append structure to analysis tree");
        return result;
    }

    for (const DslBitField& field : structure->fields) {
        const quint64 fieldStart = reader.position();
        const core::BitReadResult readResult = reader.readBits(field.width);
        if (!readResult.complete()) {
            result.status = statusForRead(readResult.status);
            result.bitsConsumed = reader.position();
            result.errorMessage = readResult.errorMessage.isEmpty()
                                      ? QStringLiteral("Unable to read complete syntax field")
                                      : readResult.errorMessage;
            core::ParseDiagnostic diagnostic;
            diagnostic.code = diagnosticForStatus(result.status);
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = result.errorMessage;
            diagnostic.fieldPath = structure->name + QLatin1Char('.') + field.name;
            (void)tree.markPartial(
                *result.structureNode, core::MaterializationState::Invalid, std::move(diagnostic));
            return result;
        }

        if (addWouldOverflow(logicalStart, fieldStart)) {
            result.errorMessage = QStringLiteral("Logical field offset overflow");
            result.bitsConsumed = reader.position();
            (void)tree.transition(*result.structureNode, core::MaterializationState::Invalid);
            return result;
        }
        const auto logicalRange = core::LogicalRange::create(
            core::LogicalBitAddress(mapping.viewId(), logicalStart + fieldStart), field.width);
        const auto location = logicalRange ? mapping.locate(*logicalRange) : std::nullopt;
        if (!location) {
            result.errorMessage = QStringLiteral("Field range is not covered by the source mapping");
            result.bitsConsumed = reader.position();
            (void)tree.transition(*result.structureNode, core::MaterializationState::Invalid);
            return result;
        }
        const quint64 readerStart = reader.range().start().absoluteBitOffset();
        if (addWouldOverflow(readerStart, fieldStart) || location->sourceSpans().size() != 1 ||
            location->sourceSpans().front().start().absoluteBitOffset() !=
                readerStart + fieldStart ||
            location->sourceSpans().front().bitLength() != field.width) {
            result.errorMessage = QStringLiteral(
                "Minimum DSL executor requires a contiguous direct source mapping");
            result.bitsConsumed = reader.position();
            (void)tree.transition(*result.structureNode, core::MaterializationState::Invalid);
            return result;
        }

        core::AnalysisNodeSpec fieldSpec;
        fieldSpec.kind = core::AnalysisNodeKind::SyntaxField;
        fieldSpec.name = field.name;
        fieldSpec.state = core::MaterializationState::Materialized;
        fieldSpec.value = QVariant::fromValue<qulonglong>(readResult.value);
        fieldSpec.location = *location;
        if (!tree.appendChild(*result.structureNode, std::move(fieldSpec))) {
            result.errorMessage = QStringLiteral("Unable to append syntax field to analysis tree");
            result.bitsConsumed = reader.position();
            (void)tree.transition(*result.structureNode, core::MaterializationState::Invalid);
            return result;
        }

        const std::optional<quint64> expected = equalsConstraint(field);
        if (expected && *expected != readResult.value) {
            result.status = DslExecutionStatus::InvalidSyntax;
            result.bitsConsumed = reader.position();
            result.errorMessage = QStringLiteral("Field value violates @equals constraint");
            core::ParseDiagnostic diagnostic;
            diagnostic.code = core::DiagnosticCode::InvalidSyntax;
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = result.errorMessage;
            diagnostic.fieldPath = structure->name + QLatin1Char('.') + field.name;
            diagnostic.location = *location;
            (void)tree.markPartial(
                *result.structureNode, core::MaterializationState::Invalid, std::move(diagnostic));
            return result;
        }
    }

    result.bitsConsumed = reader.position();
    if (!tree.transition(*result.structureNode, core::MaterializationState::Materialized)) {
        result.errorMessage = QStringLiteral("Unable to materialize structure node");
        return result;
    }
    result.status = DslExecutionStatus::Materialized;
    return result;
}

} // namespace streamview::rules
