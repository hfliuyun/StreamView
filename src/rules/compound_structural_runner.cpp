#include <streamview/rules/compound_structural_runner.h>

#include <streamview/core/bit_reader.h>

#include <algorithm>
#include <exception>
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

[[nodiscard]] bool spanContainedInMapping(const core::SourceMapping& mapping,
                                           const core::SourceSpan& candidate) noexcept {
    const auto candidateEnd = candidate.endExclusive();
    for (const auto& span : mapping.sourceSpans()) {
        if (candidate.start() >= span.start() && candidateEnd <= span.endExclusive()) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool spansOverlap(const core::SourceSpan& left,
                                const core::SourceSpan& right) noexcept {
    return left.start() < right.endExclusive() && right.start() < left.endExclusive();
}

[[nodiscard]] std::optional<core::SourceMapping> requestedInputMapping(
    const PayloadTransformRequest& request) {
    if (request.inputMapping == nullptr || request.logicalBitLength == 0 ||
        request.logicalBitStart > request.inputMapping->logicalBitLength() ||
        request.logicalBitLength >
            request.inputMapping->logicalBitLength() - request.logicalBitStart) {
        return std::nullopt;
    }
    const auto logicalAddress = core::LogicalBitAddress(
        request.inputMapping->viewId(), request.logicalBitStart);
    const auto logicalRange = core::LogicalRange::create(
        logicalAddress, request.logicalBitLength);
    if (!logicalRange) {
        return std::nullopt;
    }
    const auto location = request.inputMapping->locate(*logicalRange);
    if (!location) {
        return std::nullopt;
    }
    return core::SourceMapping::create(
        request.inputMapping->viewId(), location->sourceSpans());
}

[[nodiscard]] bool validateTransformResult(const PayloadTransformRequest& request,
                                           const PayloadTransformResult& transformResult,
                                           QString* errorMessage) {
    if (request.maximumInspectedBytes != 0 &&
        transformResult.inspectedByteCount > request.maximumInspectedBytes) {
        *errorMessage = QStringLiteral("Payload transform exceeded its inspection budget");
        return false;
    }
    if (transformResult.status != DslExecutionStatus::Materialized) {
        return true;
    }
    if (!transformResult.forwardedMapping) {
        *errorMessage = QStringLiteral("Materialized transform did not return a forwarded mapping");
        return false;
    }
    const auto allowedMapping = requestedInputMapping(request);
    if (!allowedMapping) {
        *errorMessage = QStringLiteral("Payload logical range cannot be mapped to source spans");
        return false;
    }
    if (transformResult.forwardedMapping->viewId() != allowedMapping->viewId()) {
        *errorMessage = QStringLiteral("Forwarded mapping view does not match the input mapping");
        return false;
    }
    if (!validateMapping(*transformResult.forwardedMapping, errorMessage)) {
        return false;
    }

    for (const auto& forwardedSpan : transformResult.forwardedMapping->sourceSpans()) {
        if (!spanContainedInMapping(*allowedMapping, forwardedSpan)) {
            *errorMessage = QStringLiteral(
                "Forwarded mapping escapes the requested payload logical range");
            return false;
        }
    }

    quint64 excludedBits = 0;
    for (const PayloadExcludedSpan& excluded : transformResult.excludedSpans) {
        if (excluded.sourceSpan.bitLength() == 0 ||
            excluded.sourceSpan.start().bitOffsetInByte() != 0 ||
            (excluded.sourceSpan.bitLength() % 8U) != 0) {
            *errorMessage = QStringLiteral("Excluded spans must be non-empty and byte-aligned");
            return false;
        }
        if (!spanContainedInMapping(*allowedMapping, excluded.sourceSpan)) {
            *errorMessage = QStringLiteral(
                "Excluded span escapes the requested payload logical range");
            return false;
        }
        if ((excluded.outputBitOffset % 8U) != 0 ||
            excluded.outputBitOffset > transformResult.forwardedMapping->logicalBitLength()) {
            *errorMessage = QStringLiteral("Excluded span output offset is out of range");
            return false;
        }
        if (std::numeric_limits<quint64>::max() - excludedBits <
            excluded.sourceSpan.bitLength()) {
            *errorMessage = QStringLiteral("Excluded span length arithmetic overflow");
            return false;
        }
        excludedBits += excluded.sourceSpan.bitLength();
    }

    for (std::size_t i = 0; i < transformResult.excludedSpans.size(); ++i) {
        const auto& left = transformResult.excludedSpans[i].sourceSpan;
        for (std::size_t j = i + 1; j < transformResult.excludedSpans.size(); ++j) {
            if (spansOverlap(left, transformResult.excludedSpans[j].sourceSpan)) {
                *errorMessage = QStringLiteral("Excluded spans overlap");
                return false;
            }
        }
        for (const auto& forwarded : transformResult.forwardedMapping->sourceSpans()) {
            if (spansOverlap(left, forwarded)) {
                *errorMessage = QStringLiteral("Excluded span is also forwarded");
                return false;
            }
        }
    }

    const quint64 forwardedBits = transformResult.forwardedMapping->logicalBitLength();
    if (std::numeric_limits<quint64>::max() - forwardedBits < excludedBits ||
        forwardedBits + excludedBits != request.logicalBitLength) {
        *errorMessage = QStringLiteral(
            "Forwarded and excluded spans do not cover the input logical range");
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<QString> invokeTransactionHook(
    const std::function<void()>& hook,
    const QString& hookName) noexcept {
    if (!hook) {
        return std::nullopt;
    }
    try {
        hook();
        return std::nullopt;
    } catch (const std::exception& error) {
        return QStringLiteral("%1 hook failed: %2")
            .arg(hookName, QString::fromUtf8(error.what()));
    } catch (...) {
        return QStringLiteral("%1 hook failed with an unknown exception").arg(hookName);
    }
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
        if ((request.payloadLogicalStart % 8U) != 0) {
            return makeFailure(DslExecutionStatus::InvalidDefinition,
                               QStringLiteral("Payload logical start is not byte-aligned"));
        }
        if (!validateMapping(*request.payloadMapping, &validationError)) {
            return makeFailure(DslExecutionStatus::InvalidDefinition, validationError);
        }
    } else if (request.payloadMapping != nullptr || request.payloadLogicalStart != 0) {
        return makeFailure(DslExecutionStatus::InvalidDefinition,
                           QStringLiteral("Payload mapping requires a payload structure"));
    }

    if (request.options.cancellation &&
        request.options.cancellation->isCancellationRequested()) {
        return makeFailure(DslExecutionStatus::Cancelled,
                           QStringLiteral("Compound execution was cancelled before starting"));
    }

    const auto rollbackAndReturn = [&](DslExecutionStatus status,
                                       const QString& message) {
        result.status = status;
        result.errorMessage = message;
        if (const auto hookError = invokeTransactionHook(
                request.transactionHooks.onRollback, QStringLiteral("Rollback"))) {
            result.status = DslExecutionStatus::InvalidDefinition;
            if (!result.errorMessage.isEmpty()) {
                result.errorMessage += QStringLiteral("; ");
            }
            result.errorMessage += *hookError;
        }
        return result;
    };

    const auto commitTransaction = [&]() {
        const auto hookError = invokeTransactionHook(
            request.transactionHooks.onCommit, QStringLiteral("Commit"));
        if (!hookError) {
            return true;
        }
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = *hookError;
        if (const auto rollbackError = invokeTransactionHook(
                request.transactionHooks.onRollback, QStringLiteral("Rollback"))) {
            result.errorMessage += QStringLiteral("; ") + *rollbackError;
        }
        return false;
    };

    // 1. Header Execution Phase (keeps header in Indexing state)
    core::BitReader headerReader(*request.source, *request.headerMapping);

    const DslExecutionResult headerResult = DslVirtualMachine::executeDeferred(
        program,
        request.headerStructureIndex,
        headerReader,
        *request.headerMapping,
        0,
        *request.tree,
        request.parentId,
        request.options);

    result.headerNodeId = headerResult.structureNode;
    result.headerBitsConsumed = headerResult.bitsConsumed;
    result.instructionsExecuted = headerResult.instructionsExecuted;
    result.nodesCreated = headerResult.nodesCreated;
    result.headerFieldValues = headerResult.fieldValues;

    if (!headerResult.materialized()) {
        return rollbackAndReturn(headerResult.status, headerResult.errorMessage);
    }

    if (!result.headerNodeId) {
        return rollbackAndReturn(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Header execution did not produce a structure node"));
    }

    if (request.requireExactConsumption &&
        headerResult.bitsConsumed != request.headerMapping->logicalBitLength()) {
        core::ParseDiagnostic diag;
        diag.code = core::DiagnosticCode::InvalidSyntax;
        diag.severity = core::DiagnosticSeverity::Error;
        diag.message = QStringLiteral("Header did not consume all available logical bits");
        if (!request.tree->markPartial(*result.headerNodeId,
                                       core::MaterializationState::Invalid,
                                       std::move(diag))) {
            return rollbackAndReturn(
                DslExecutionStatus::InvalidDefinition,
                QStringLiteral("Failed to invalidate an incompletely consumed header"));
        }
        return rollbackAndReturn(
            DslExecutionStatus::InvalidSyntax,
            QStringLiteral("Header did not consume all available logical bits"));
    }

    // 2. If no payload structure is requested
    if (!request.payloadStructureIndex.has_value()) {
        if (!commitTransaction()) {
            core::ParseDiagnostic diag;
            diag.code = core::DiagnosticCode::InvalidSyntax;
            diag.severity = core::DiagnosticSeverity::Error;
            diag.message = result.errorMessage;
            (void)request.tree->markPartial(*result.headerNodeId,
                                            core::MaterializationState::Invalid,
                                            std::move(diag));
            return result;
        }

        if (!request.tree->transition(*result.headerNodeId, core::MaterializationState::Materialized)) {
            return rollbackAndReturn(DslExecutionStatus::InvalidDefinition,
                                     QStringLiteral("Failed to materialize header node"));
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

        return rollbackAndReturn(
            DslExecutionStatus::Cancelled,
            QStringLiteral("Execution was cancelled between header and payload"));
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

        return rollbackAndReturn(
            DslExecutionStatus::ResourceLimit,
            QStringLiteral("Instruction budget exhausted after header execution"));
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

        return rollbackAndReturn(
            DslExecutionStatus::ResourceLimit,
            QStringLiteral("Materialized node budget exhausted after header execution"));
    }
    const quint64 nodesRemaining = maxNodes - nodesUsed;

    // 4. Payload Transform and Execution Phase (appends payload under header node)
    const auto& registry = request.transformRegistry ? *request.transformRegistry
                                                     : PayloadTransformRegistry::instance();
    const auto provider = registry.findProvider(request.transformProviderId);
    if (!provider) {
        core::ParseDiagnostic diag;
        diag.code = core::DiagnosticCode::InvalidSyntax;
        diag.severity = core::DiagnosticSeverity::Error;
        diag.message = QStringLiteral("Unknown payload transform provider: %1")
            .arg(request.transformProviderId);
        (void)request.tree->markPartial(
            *result.headerNodeId,
            core::MaterializationState::Invalid,
            std::move(diag));
        return rollbackAndReturn(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Unknown payload transform provider: %1").arg(request.transformProviderId));
    }

    const quint64 payloadInputBitLength =
        request.payloadMapping->logicalBitLength() - request.payloadLogicalStart;
    if (payloadInputBitLength == 0) {
        core::ParseDiagnostic diag;
        diag.code = core::DiagnosticCode::InvalidSyntax;
        diag.severity = core::DiagnosticSeverity::Error;
        diag.message = QStringLiteral("Payload logical length is zero");
        (void)request.tree->markPartial(
            *result.headerNodeId,
            core::MaterializationState::Invalid,
            std::move(diag));
        return rollbackAndReturn(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Payload logical length is zero"));
    }

    PayloadTransformRequest transformReq;
    transformReq.source = request.source;
    transformReq.inputMapping = request.payloadMapping;
    transformReq.logicalBitStart = request.payloadLogicalStart;
    transformReq.logicalBitLength = payloadInputBitLength;
    transformReq.cancellation = request.options.cancellation;
    transformReq.maximumInspectedBytes = request.options.limits.maximumInspectedBytes;

    const PayloadTransformResult transformRes = provider->transform(transformReq);
    result.inspectedByteCount = transformRes.inspectedByteCount;
    result.excludedSpans = transformRes.excludedSpans;
    result.transformDiagnostics = transformRes.diagnostics;
    for (const auto& diagnostic : transformRes.diagnostics) {
        (void)request.tree->addDiagnostic(*result.headerNodeId, diagnostic);
    }

    QString transformValidationError;
    if (transformRes.status == DslExecutionStatus::Materialized &&
        !validateTransformResult(transformReq, transformRes, &transformValidationError)) {
        core::ParseDiagnostic diag;
        diag.code = core::DiagnosticCode::InvalidSyntax;
        diag.severity = core::DiagnosticSeverity::Error;
        diag.message = transformValidationError;
        (void)request.tree->markPartial(
            *result.headerNodeId,
            core::MaterializationState::Invalid,
            std::move(diag));
        return rollbackAndReturn(DslExecutionStatus::InvalidDefinition,
                                 transformValidationError);
    }

    if (!transformRes.succeeded()) {
        const auto headerTerminalState =
            transformRes.status == DslExecutionStatus::Cancelled
                ? core::MaterializationState::Cancelled
            : transformRes.status == DslExecutionStatus::Unsupported
                ? core::MaterializationState::Unsupported
            : transformRes.status == DslExecutionStatus::DependencyUnavailable
                ? core::MaterializationState::WaitingDependency
                : core::MaterializationState::Invalid;

        core::ParseDiagnostic diag;
        diag.code =
            transformRes.status == DslExecutionStatus::Cancelled
                ? core::DiagnosticCode::Cancelled
            : transformRes.status == DslExecutionStatus::Unsupported
                ? core::DiagnosticCode::UnsupportedSyntax
            : transformRes.status == DslExecutionStatus::DependencyUnavailable
                ? core::DiagnosticCode::DependencyUnavailable
            : transformRes.status == DslExecutionStatus::TruncatedSource
                ? core::DiagnosticCode::TruncatedSource
            : transformRes.status == DslExecutionStatus::SourceError
                ? core::DiagnosticCode::SourceError
            : transformRes.status == DslExecutionStatus::ResourceLimit
                ? core::DiagnosticCode::ResourceLimit
                : core::DiagnosticCode::InvalidSyntax;
        diag.severity = core::DiagnosticSeverity::Error;
        diag.message = transformRes.errorMessage.isEmpty()
            ? QStringLiteral("Payload transform failed")
            : transformRes.errorMessage;

        (void)request.tree->markPartial(
            *result.headerNodeId,
            headerTerminalState,
            std::move(diag));

        const QString errorMessage = transformRes.errorMessage.isEmpty()
            ? QStringLiteral("Payload transform failed")
            : transformRes.errorMessage;
        return rollbackAndReturn(transformRes.status, errorMessage);
    }

    const core::SourceMapping& transformedMapping = *transformRes.forwardedMapping;
    result.forwardedPayloadMapping = transformedMapping;

    DslExecutionOptions payloadOptions = request.options;
    payloadOptions.limits.maximumInstructions = instRemaining;
    payloadOptions.limits.maximumMaterializedNodes = nodesRemaining;
    payloadOptions.sequenceElementValues = headerResult.fieldValues;

    core::BitReader payloadReader(*request.source, transformedMapping);

    const DslExecutionResult payloadResult = DslVirtualMachine::executeDeferred(
        program,
        *request.payloadStructureIndex,
        payloadReader,
        transformedMapping,
        0,
        *request.tree,
        *result.headerNodeId,
        payloadOptions);

    result.payloadNodeId = payloadResult.structureNode;
    result.payloadBitsConsumed = payloadResult.bitsConsumed;

    if (payloadResult.instructionsExecuted > instRemaining ||
        payloadResult.nodesCreated > nodesRemaining) {
        (void)request.tree->transition(*result.headerNodeId,
                                       core::MaterializationState::Invalid);
        return rollbackAndReturn(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Payload execution exceeded its remaining compound budget"));
    }
    result.instructionsExecuted += payloadResult.instructionsExecuted;
    result.nodesCreated += payloadResult.nodesCreated;

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

        return rollbackAndReturn(payloadResult.status, payloadResult.errorMessage);
    }

    if (request.requireExactConsumption) {
        if (payloadResult.bitsConsumed != transformedMapping.logicalBitLength()) {
            core::ParseDiagnostic diag;
            diag.code = core::DiagnosticCode::InvalidSyntax;
            diag.severity = core::DiagnosticSeverity::Error;
            diag.message = QStringLiteral("Payload did not consume all available logical bits");
            if (result.payloadNodeId) {
                if (!request.tree->markPartial(*result.payloadNodeId,
                                               core::MaterializationState::Invalid,
                                               diag)) {
                    return rollbackAndReturn(
                        DslExecutionStatus::InvalidDefinition,
                        QStringLiteral("Failed to invalidate an incompletely consumed payload"));
                }
            }
            (void)request.tree->transition(*result.headerNodeId, core::MaterializationState::Invalid);
            return rollbackAndReturn(
                DslExecutionStatus::InvalidSyntax,
                QStringLiteral("Payload did not consume all available logical bits"));
        }
    }

    if (!result.payloadNodeId) {
        (void)request.tree->transition(*result.headerNodeId,
                                       core::MaterializationState::Invalid);
        return rollbackAndReturn(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Payload execution did not produce a structure node"));
    }

    if (!commitTransaction()) {
        core::ParseDiagnostic diag;
        diag.code = core::DiagnosticCode::InvalidSyntax;
        diag.severity = core::DiagnosticSeverity::Error;
        diag.message = result.errorMessage;
        (void)request.tree->markPartial(*result.payloadNodeId,
                                        core::MaterializationState::Invalid,
                                        diag);
        (void)request.tree->markPartial(*result.headerNodeId,
                                        core::MaterializationState::Invalid,
                                        std::move(diag));
        return result;
    }

    if (!request.tree->transition(*result.payloadNodeId,
                                  core::MaterializationState::Materialized)) {
        (void)request.tree->transition(*result.headerNodeId,
                                       core::MaterializationState::Invalid);
        return rollbackAndReturn(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Failed to materialize payload node"));
    }
    if (!request.tree->transition(*result.headerNodeId, core::MaterializationState::Materialized)) {
        return rollbackAndReturn(
            DslExecutionStatus::InvalidDefinition,
            QStringLiteral("Failed to materialize header node after payload"));
    }
    result.status = DslExecutionStatus::Materialized;
    return result;
}

} // namespace streamview::rules
