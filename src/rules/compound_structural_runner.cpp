#include <streamview/rules/compound_structural_runner.h>

#include <streamview/core/bit_reader.h>

#include <limits>

namespace streamview::rules {

namespace {

[[nodiscard]] CompoundStructuralExecutionResult makeFailure(
    DslExecutionStatus status,
    const QString& message) {
    CompoundStructuralExecutionResult res;
    res.status = status;
    res.errorMessage = message;
    return res;
}

[[nodiscard]] bool validateMapping(const core::SourceMapping& mapping, QString* errorMessage) {
    const quint64 logicalBitLength = mapping.logicalBitLength();
    if (logicalBitLength == 0) {
        *errorMessage = QStringLiteral("Source mapping logical length is zero");
        return false;
    }
    if ((logicalBitLength % 8U) != 0) {
        *errorMessage = QStringLiteral("Source mapping logical length is not byte-aligned");
        return false;
    }
    constexpr quint64 maxByteCoordinate = std::numeric_limits<quint64>::max() / 8U;
    const quint64 byteLength = logicalBitLength / 8U;
    if (byteLength > maxByteCoordinate) {
        *errorMessage = QStringLiteral("Source mapping length exceeds coordinate limit");
        return false;
    }
    for (const auto& span : mapping.sourceSpans()) {
        if (span.start().bitOffsetInByte() != 0 || (span.bitLength() % 8U) != 0) {
            *errorMessage = QStringLiteral("Source mapping spans must be byte-aligned");
            return false;
        }
    }
    return true;
}

} // namespace

CompoundStructuralExecutionResult CompoundStructuralRunner::execute(
    const DslTypedProgram& program,
    const CompoundStructuralExecutionRequest& request) {
    CompoundStructuralExecutionResult result;

    if (request.source == nullptr) {
        return makeFailure(DslExecutionStatus::InvalidDefinition,
                           QStringLiteral("Source is null"));
    }
    if (request.headerMapping == nullptr) {
        return makeFailure(DslExecutionStatus::InvalidDefinition,
                           QStringLiteral("Header source mapping is null"));
    }
    if (request.tree == nullptr) {
        return makeFailure(DslExecutionStatus::InvalidDefinition,
                           QStringLiteral("Analysis tree is null"));
    }
    if (request.headerStructureIndex >= program.structs.size()) {
        return makeFailure(DslExecutionStatus::InvalidDefinition,
                           QStringLiteral("Header structure index is out of range"));
    }

    QString validationError;
    if (!validateMapping(*request.headerMapping, &validationError)) {
        return makeFailure(DslExecutionStatus::InvalidDefinition, validationError);
    }

    const auto parentNode = request.tree->node(request.parentId);
    if (!parentNode) {
        return makeFailure(DslExecutionStatus::InvalidDefinition,
                           QStringLiteral("Parent analysis node is invalid"));
    }
    if (parentNode->state() != core::MaterializationState::Indexing) {
        return makeFailure(DslExecutionStatus::InvalidDefinition,
                           QStringLiteral("Parent analysis node is not indexing"));
    }

    if (request.payloadStructureIndex.has_value()) {
        if (*request.payloadStructureIndex >= program.structs.size()) {
            return makeFailure(DslExecutionStatus::InvalidDefinition,
                               QStringLiteral("Payload structure index is out of range"));
        }
        if (request.payloadMapping == nullptr) {
            return makeFailure(DslExecutionStatus::InvalidDefinition,
                               QStringLiteral("Payload source mapping is null"));
        }
        if (request.payloadLogicalStart > request.payloadMapping->logicalBitLength()) {
            return makeFailure(DslExecutionStatus::InvalidDefinition,
                               QStringLiteral("Payload logical start is out of range"));
        }
        if (!validateMapping(*request.payloadMapping, &validationError)) {
            return makeFailure(DslExecutionStatus::InvalidDefinition, validationError);
        }
    }

    if (request.options.cancellation &&
        request.options.cancellation->isCancellationRequested()) {
        return makeFailure(DslExecutionStatus::Cancelled,
                           QStringLiteral("Compound execution was cancelled before starting"));
    }

    // 1. Header Execution Phase (keeps header in Indexing state)
    DslExecutionOptions headerOptions = request.options;
    headerOptions.deferMaterialization = true;

    core::BitReader headerReader(*request.source, *request.headerMapping);

    const DslExecutionResult headerResult = DslVirtualMachine::execute(
        program,
        request.headerStructureIndex,
        headerReader,
        *request.headerMapping,
        0,
        *request.tree,
        request.parentId,
        headerOptions);

    result.headerNodeId = headerResult.structureNode;
    result.headerBitsConsumed = headerResult.bitsConsumed;
    result.instructionsExecuted = headerResult.instructionsExecuted;
    result.nodesCreated = headerResult.nodesCreated;
    result.headerFieldValues = headerResult.fieldValues;

    if (!headerResult.materialized()) {
        result.status = headerResult.status;
        result.errorMessage = headerResult.errorMessage;
        if (request.transactionHooks.onRollback) {
            request.transactionHooks.onRollback();
        }
        return result;
    }

    if (!result.headerNodeId) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Header execution did not produce a structure node");
        if (request.transactionHooks.onRollback) {
            request.transactionHooks.onRollback();
        }
        return result;
    }

    // 2. If no payload structure is requested
    if (!request.payloadStructureIndex.has_value()) {
        if (request.requireExactConsumption &&
            headerResult.bitsConsumed != request.headerMapping->logicalBitLength()) {
            core::ParseDiagnostic diag;
            diag.code = core::DiagnosticCode::InvalidSyntax;
            diag.severity = core::DiagnosticSeverity::Error;
            diag.message = QStringLiteral("Header did not consume all available logical bits");
            (void)request.tree->markPartial(
                *result.headerNodeId,
                core::MaterializationState::Invalid,
                std::move(diag));

            if (request.transactionHooks.onRollback) {
                request.transactionHooks.onRollback();
            }
            result.status = DslExecutionStatus::InvalidSyntax;
            result.errorMessage = QStringLiteral("Header did not consume all available logical bits");
            return result;
        }

        if (!request.tree->transition(*result.headerNodeId, core::MaterializationState::Materialized)) {
            result.status = DslExecutionStatus::InvalidDefinition;
            result.errorMessage = QStringLiteral("Failed to materialize header node");
            if (request.transactionHooks.onRollback) {
                request.transactionHooks.onRollback();
            }
            return result;
        }

        if (request.transactionHooks.onCommit) {
            request.transactionHooks.onCommit();
        }
        result.status = DslExecutionStatus::Materialized;
        return result;
    }

    // 3. Intermediate Checks between Header and Payload
    if (request.options.cancellation &&
        request.options.cancellation->isCancellationRequested()) {
        core::ParseDiagnostic diag;
        diag.code = core::DiagnosticCode::Cancelled;
        diag.severity = core::DiagnosticSeverity::Error;
        diag.message = QStringLiteral("Execution was cancelled between header and payload");
        (void)request.tree->markPartial(
            *result.headerNodeId,
            core::MaterializationState::Cancelled,
            std::move(diag));

        if (request.transactionHooks.onRollback) {
            request.transactionHooks.onRollback();
        }
        result.status = DslExecutionStatus::Cancelled;
        result.errorMessage = QStringLiteral("Execution was cancelled between header and payload");
        return result;
    }

    const quint64 maxInstructions = request.options.limits.maximumInstructions;
    const quint64 instUsed = headerResult.instructionsExecuted;
    if (instUsed >= maxInstructions) {
        core::ParseDiagnostic diag;
        diag.code = core::DiagnosticCode::ResourceLimit;
        diag.severity = core::DiagnosticSeverity::Error;
        diag.message = QStringLiteral("Instruction budget exhausted after header execution");
        (void)request.tree->markPartial(
            *result.headerNodeId,
            core::MaterializationState::Invalid,
            std::move(diag));

        if (request.transactionHooks.onRollback) {
            request.transactionHooks.onRollback();
        }
        result.status = DslExecutionStatus::ResourceLimit;
        result.errorMessage = QStringLiteral("Instruction budget exhausted after header execution");
        return result;
    }
    const quint64 instRemaining = maxInstructions - instUsed;

    const quint64 maxNodes = request.options.limits.maximumMaterializedNodes;
    const quint64 nodesUsed = headerResult.nodesCreated;
    if (nodesUsed >= maxNodes) {
        core::ParseDiagnostic diag;
        diag.code = core::DiagnosticCode::ResourceLimit;
        diag.severity = core::DiagnosticSeverity::Error;
        diag.message = QStringLiteral("Materialized node budget exhausted after header execution");
        (void)request.tree->markPartial(
            *result.headerNodeId,
            core::MaterializationState::Invalid,
            std::move(diag));

        if (request.transactionHooks.onRollback) {
            request.transactionHooks.onRollback();
        }
        result.status = DslExecutionStatus::ResourceLimit;
        result.errorMessage = QStringLiteral("Materialized node budget exhausted after header execution");
        return result;
    }
    const quint64 nodesRemaining = maxNodes - nodesUsed;

    // 4. Payload Execution Phase (appends payload under header node)
    DslExecutionOptions payloadOptions = request.options;
    payloadOptions.limits.maximumInstructions = instRemaining;
    payloadOptions.limits.maximumMaterializedNodes = nodesRemaining;
    payloadOptions.sequenceElementValues = headerResult.fieldValues;
    payloadOptions.deferMaterialization = false;

    core::BitReader payloadReader(*request.source, *request.payloadMapping);
    if (request.payloadLogicalStart > 0) {
        if (!payloadReader.seek(request.payloadLogicalStart)) {
            core::ParseDiagnostic diag;
            diag.code = core::DiagnosticCode::TruncatedSource;
            diag.severity = core::DiagnosticSeverity::Error;
            diag.message = QStringLiteral("Unable to seek to payload logical start");
            (void)request.tree->markPartial(
                *result.headerNodeId,
                core::MaterializationState::Invalid,
                std::move(diag));

            if (request.transactionHooks.onRollback) {
                request.transactionHooks.onRollback();
            }
            result.status = DslExecutionStatus::TruncatedSource;
            result.errorMessage = QStringLiteral("Unable to seek to payload logical start");
            return result;
        }
    }

    const DslExecutionResult payloadResult = DslVirtualMachine::execute(
        program,
        *request.payloadStructureIndex,
        payloadReader,
        *request.payloadMapping,
        request.payloadLogicalStart,
        *request.tree,
        *result.headerNodeId,
        payloadOptions);

    result.payloadNodeId = payloadResult.structureNode;
    result.payloadBitsConsumed = payloadResult.bitsConsumed;

    constexpr quint64 maxVal = std::numeric_limits<quint64>::max();
    if (maxVal - result.instructionsExecuted < payloadResult.instructionsExecuted) {
        result.instructionsExecuted = maxVal;
    } else {
        result.instructionsExecuted += payloadResult.instructionsExecuted;
    }
    if (maxVal - result.nodesCreated < payloadResult.nodesCreated) {
        result.nodesCreated = maxVal;
    } else {
        result.nodesCreated += payloadResult.nodesCreated;
    }

    if (!payloadResult.materialized()) {
        const auto headerTerminalState =
            payloadResult.status == DslExecutionStatus::Cancelled
                ? core::MaterializationState::Cancelled
            : payloadResult.status == DslExecutionStatus::Unsupported
                ? core::MaterializationState::Unsupported
            : payloadResult.status == DslExecutionStatus::DependencyUnavailable
                ? core::MaterializationState::WaitingDependency
                : core::MaterializationState::Invalid;

        (void)request.tree->transition(*result.headerNodeId, headerTerminalState);

        if (request.transactionHooks.onRollback) {
            request.transactionHooks.onRollback();
        }
        result.status = payloadResult.status;
        result.errorMessage = payloadResult.errorMessage;
        return result;
    }

    if (request.requireExactConsumption) {
        const quint64 expectedPayloadBits =
            request.payloadMapping->logicalBitLength() - request.payloadLogicalStart;
        if (payloadResult.bitsConsumed != expectedPayloadBits) {
            core::ParseDiagnostic diag;
            diag.code = core::DiagnosticCode::InvalidSyntax;
            diag.severity = core::DiagnosticSeverity::Error;
            diag.message = QStringLiteral("Payload did not consume all available logical bits");
            if (result.payloadNodeId) {
                (void)request.tree->markPartial(
                    *result.payloadNodeId,
                    core::MaterializationState::Invalid,
                    diag);
            }
            (void)request.tree->transition(*result.headerNodeId, core::MaterializationState::Invalid);

            if (request.transactionHooks.onRollback) {
                request.transactionHooks.onRollback();
            }
            result.status = DslExecutionStatus::InvalidSyntax;
            result.errorMessage = QStringLiteral("Payload did not consume all available logical bits");
            return result;
        }
    }

    if (!request.tree->transition(*result.headerNodeId, core::MaterializationState::Materialized)) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Failed to materialize header node after payload");
        if (request.transactionHooks.onRollback) {
            request.transactionHooks.onRollback();
        }
        return result;
    }

    if (request.transactionHooks.onCommit) {
        request.transactionHooks.onCommit();
    }
    result.status = DslExecutionStatus::Materialized;
    return result;
}

} // namespace streamview::rules
