#include <streamview/rules/sample_payload_runner.h>

#include <streamview/core/bit_reader.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace streamview::rules {

namespace {

constexpr quint64 kMaxByteCoordinate = std::numeric_limits<quint64>::max() / 8U;

[[nodiscard]] bool isLegalPrefixLength(quint32 prefixLengthBytes) noexcept {
    return prefixLengthBytes == 1U || prefixLengthBytes == 2U || prefixLengthBytes == 4U;
}

[[nodiscard]] SamplePayloadFramingResult framingFailure(SamplePayloadFramingStatus status,
                                                        const QString& message) {
    SamplePayloadFramingResult result;
    result.status = status;
    result.errorMessage = message;
    return result;
}

[[nodiscard]] SamplePayloadRunResult runFailure(SamplePayloadRunStatus status,
                                                const QString& message) {
    SamplePayloadRunResult result;
    result.status = status;
    result.errorMessage = message;
    return result;
}

/// Framing failures describe the sample's bytes, so they map onto run statuses
/// one-for-one rather than collapsing into a single generic error.
[[nodiscard]] SamplePayloadRunStatus runStatusForFraming(SamplePayloadFramingStatus status) noexcept {
    switch (status) {
    case SamplePayloadFramingStatus::Framed:
        return SamplePayloadRunStatus::Executed;
    case SamplePayloadFramingStatus::InvalidRequest:
        return SamplePayloadRunStatus::InvalidRequest;
    case SamplePayloadFramingStatus::UnsupportedFraming:
        return SamplePayloadRunStatus::UnsupportedFraming;
    case SamplePayloadFramingStatus::TruncatedSample:
        return SamplePayloadRunStatus::TruncatedSample;
    case SamplePayloadFramingStatus::InvalidUnitLength:
        return SamplePayloadRunStatus::InvalidUnitLength;
    case SamplePayloadFramingStatus::ResourceLimit:
        return SamplePayloadRunStatus::ResourceLimit;
    case SamplePayloadFramingStatus::SourceError:
        return SamplePayloadRunStatus::SourceError;
    case SamplePayloadFramingStatus::Cancelled:
        return SamplePayloadRunStatus::Cancelled;
    }
    return SamplePayloadRunStatus::InvalidRequest;
}

[[nodiscard]] core::DiagnosticCode diagnosticCodeFor(SamplePayloadRunStatus status) noexcept {
    switch (status) {
    case SamplePayloadRunStatus::TruncatedSample:
        return core::DiagnosticCode::TruncatedSource;
    case SamplePayloadRunStatus::Cancelled:
        return core::DiagnosticCode::Cancelled;
    case SamplePayloadRunStatus::SourceError:
        return core::DiagnosticCode::SourceError;
    case SamplePayloadRunStatus::ResourceLimit:
        return core::DiagnosticCode::ResourceLimit;
    case SamplePayloadRunStatus::DependencyUnavailable:
        return core::DiagnosticCode::DependencyUnavailable;
    case SamplePayloadRunStatus::UnsupportedFraming:
        return core::DiagnosticCode::UnsupportedSyntax;
    case SamplePayloadRunStatus::Executed:
    case SamplePayloadRunStatus::InvalidRequest:
    case SamplePayloadRunStatus::InvalidUnitLength:
        break;
    }
    return core::DiagnosticCode::InvalidSyntax;
}

/// A unit that fails to execute must not take the sample down with it, so its
/// state is recorded on the unit node and the sample continues.
[[nodiscard]] core::MaterializationState unitStateFor(RuleExecutionStatus status) noexcept {
    switch (status) {
    case RuleExecutionStatus::Materialized:
        return core::MaterializationState::Materialized;
    case RuleExecutionStatus::Cancelled:
        return core::MaterializationState::Cancelled;
    case RuleExecutionStatus::Unsupported:
        return core::MaterializationState::Unsupported;
    case RuleExecutionStatus::DependencyUnavailable:
        return core::MaterializationState::WaitingDependency;
    case RuleExecutionStatus::TruncatedSource:
    case RuleExecutionStatus::InvalidSyntax:
    case RuleExecutionStatus::SourceError:
    case RuleExecutionStatus::ResourceLimit:
    case RuleExecutionStatus::InvalidDefinition:
        break;
    }
    return core::MaterializationState::Invalid;
}

[[nodiscard]] core::DiagnosticCode diagnosticCodeForUnit(RuleExecutionStatus status) noexcept {
    switch (status) {
    case RuleExecutionStatus::TruncatedSource:
        return core::DiagnosticCode::TruncatedSource;
    case RuleExecutionStatus::Cancelled:
        return core::DiagnosticCode::Cancelled;
    case RuleExecutionStatus::SourceError:
        return core::DiagnosticCode::SourceError;
    case RuleExecutionStatus::ResourceLimit:
        return core::DiagnosticCode::ResourceLimit;
    case RuleExecutionStatus::DependencyUnavailable:
        return core::DiagnosticCode::DependencyUnavailable;
    case RuleExecutionStatus::Unsupported:
        return core::DiagnosticCode::UnsupportedSyntax;
    case RuleExecutionStatus::Materialized:
    case RuleExecutionStatus::InvalidSyntax:
    case RuleExecutionStatus::InvalidDefinition:
        break;
    }
    return core::DiagnosticCode::InvalidSyntax;
}

/// Cancellation and source faults are conditions of the whole run, not of one
/// unit's syntax, so they stop the sample rather than being absorbed into it.
[[nodiscard]] bool isRunTerminating(RuleExecutionStatus status) noexcept {
    return status == RuleExecutionStatus::Cancelled ||
           status == RuleExecutionStatus::SourceError ||
           status == RuleExecutionStatus::ResourceLimit;
}

[[nodiscard]] SamplePayloadRunStatus runStatusForUnit(RuleExecutionStatus status) noexcept {
    switch (status) {
    case RuleExecutionStatus::Cancelled:
        return SamplePayloadRunStatus::Cancelled;
    case RuleExecutionStatus::SourceError:
        return SamplePayloadRunStatus::SourceError;
    case RuleExecutionStatus::ResourceLimit:
        return SamplePayloadRunStatus::ResourceLimit;
    default:
        break;
    }
    return SamplePayloadRunStatus::InvalidRequest;
}

/// Resolves a byte range of the sample view onto physical spans. Going through
/// the mapping keeps discontinuous samples correct and keeps span normalization
/// in one place: concatenating separately located spans would produce adjacent
/// spans, which `FieldLocation::create` rejects.
[[nodiscard]] std::optional<core::FieldLocation>
locateByteRange(const core::SourceMapping& mapping, quint64 byteOffset, quint64 byteLength) {
    if (byteOffset > kMaxByteCoordinate || byteLength > kMaxByteCoordinate) {
        return std::nullopt;
    }
    const auto range = core::LogicalRange::create(
        core::LogicalBitAddress(mapping.viewId(), byteOffset * 8U), byteLength * 8U);
    if (!range) {
        return std::nullopt;
    }
    return mapping.locate(*range);
}

[[nodiscard]] std::optional<core::AnalysisNodeId>
appendComputedField(core::AnalysisTree& tree, core::AnalysisNodeId parentId, const QString& name,
                    QVariant value, const QString& description) {
    core::AnalysisNodeSpec spec;
    spec.kind = core::AnalysisNodeKind::ComputedField;
    spec.name = name;
    spec.state = core::MaterializationState::Materialized;
    spec.value = std::move(value);
    spec.metadata.description = description;
    return tree.appendChild(parentId, std::move(spec));
}

} // namespace

quint64 SamplePayloadRunResult::executedUnitCount() const noexcept {
    return static_cast<quint64>(
        std::count_if(units.begin(), units.end(), [](const SamplePayloadUnitResult& unit) {
            return unit.executed();
        }));
}

SamplePayloadFramingResult
SamplePayloadFramer::frame(const core::RandomAccessSource& source,
                           const core::SourceMapping& sampleMapping,
                           const SamplePayloadFramingOptions& options) {
    SamplePayloadFramingResult result;

    const quint64 sampleBitLength = sampleMapping.logicalBitLength();
    if (sampleBitLength == 0) {
        return framingFailure(SamplePayloadFramingStatus::InvalidRequest,
                              QStringLiteral("Sample mapping logical length is zero"));
    }
    if ((sampleBitLength % 8U) != 0) {
        return framingFailure(SamplePayloadFramingStatus::InvalidRequest,
                              QStringLiteral("Sample mapping length is not byte-aligned"));
    }
    for (const auto& span : sampleMapping.sourceSpans()) {
        if (span.start().bitOffsetInByte() != 0 || (span.bitLength() % 8U) != 0) {
            return framingFailure(SamplePayloadFramingStatus::InvalidRequest,
                                  QStringLiteral("Sample source spans must be byte-aligned"));
        }
    }

    const quint64 sampleByteLength = sampleBitLength / 8U;

    if (options.cancellation && options.cancellation->isCancellationRequested()) {
        return framingFailure(SamplePayloadFramingStatus::Cancelled,
                              QStringLiteral("Sample framing was cancelled"));
    }

    if (options.framing == SamplePayloadFraming::OpaqueAccessUnit) {
        // An opaque access unit is one unit with no prefix: the envelope is the
        // sample itself.
        result.status = SamplePayloadFramingStatus::Framed;
        result.units.push_back(SamplePayloadUnitRange{
            .unitIndex = 0,
            .prefixByteOffset = 0,
            .prefixByteLength = 0,
            .payloadByteOffset = 0,
            .payloadByteLength = sampleByteLength,
        });
        result.framedByteCount = sampleByteLength;
        return result;
    }

    if (options.framing != SamplePayloadFraming::LengthPrefixed) {
        return framingFailure(SamplePayloadFramingStatus::UnsupportedFraming,
                              QStringLiteral("Unsupported sample framing kind"));
    }

    if (!isLegalPrefixLength(options.prefixLengthBytes)) {
        // ISO/IEC 14496-15 admits only 1, 2, and 4 byte length prefixes; the
        // caller derives this from the sample description it selected.
        return framingFailure(
            SamplePayloadFramingStatus::UnsupportedFraming,
            QStringLiteral("Unsupported length prefix size: %1").arg(options.prefixLengthBytes));
    }

    if (options.maximumUnits == 0) {
        return framingFailure(SamplePayloadFramingStatus::InvalidRequest,
                              QStringLiteral("Maximum unit count must be greater than zero"));
    }

    core::BitReader reader(source, sampleMapping);
    const quint64 prefixByteLength = options.prefixLengthBytes;
    const auto prefixBitLength = static_cast<unsigned int>(prefixByteLength * 8U);

    quint64 cursorByte = 0;
    quint64 unitIndex = 0;
    while (cursorByte < sampleByteLength) {
        if (options.cancellation && options.cancellation->isCancellationRequested()) {
            return framingFailure(SamplePayloadFramingStatus::Cancelled,
                                  QStringLiteral("Sample framing was cancelled"));
        }

        if (unitIndex >= options.maximumUnits) {
            return framingFailure(
                SamplePayloadFramingStatus::ResourceLimit,
                QStringLiteral("Sample contains more than %1 units").arg(options.maximumUnits));
        }

        const quint64 remainingBytes = sampleByteLength - cursorByte;
        if (remainingBytes < prefixByteLength) {
            return framingFailure(
                SamplePayloadFramingStatus::TruncatedSample,
                QStringLiteral("Sample ends inside a %1-byte length prefix").arg(prefixByteLength));
        }

        if (!reader.seek(cursorByte * 8U)) {
            return framingFailure(SamplePayloadFramingStatus::SourceError,
                                  QStringLiteral("Failed to seek to a sample length prefix"));
        }

        const auto prefix = reader.readBits(prefixBitLength);
        if (!prefix.complete()) {
            switch (prefix.status) {
            case core::BitReadStatus::EndOfRange:
            case core::BitReadStatus::EndOfSource:
                return framingFailure(SamplePayloadFramingStatus::TruncatedSample,
                                      QStringLiteral("Sample length prefix is truncated"));
            default:
                return framingFailure(SamplePayloadFramingStatus::SourceError,
                                      prefix.errorMessage.isEmpty()
                                          ? QStringLiteral("Failed to read a sample length prefix")
                                          : prefix.errorMessage);
            }
        }

        const quint64 payloadByteOffset = cursorByte + prefixByteLength;
        const quint64 unitByteLength = prefix.value;
        if (unitByteLength == 0) {
            // A zero-length unit cannot carry a header, and accepting it would
            // let a malformed sample frame unbounded empty units.
            return framingFailure(
                SamplePayloadFramingStatus::InvalidUnitLength,
                QStringLiteral("Unit %1 declares a zero byte length").arg(unitIndex));
        }

        if (unitByteLength > sampleByteLength - payloadByteOffset) {
            return framingFailure(
                SamplePayloadFramingStatus::TruncatedSample,
                QStringLiteral("Unit %1 declares %2 bytes but only %3 remain in the sample")
                    .arg(unitIndex)
                    .arg(unitByteLength)
                    .arg(sampleByteLength - payloadByteOffset));
        }

        result.units.push_back(SamplePayloadUnitRange{
            .unitIndex = unitIndex,
            .prefixByteOffset = cursorByte,
            .prefixByteLength = options.prefixLengthBytes,
            .payloadByteOffset = payloadByteOffset,
            .payloadByteLength = unitByteLength,
        });

        cursorByte = payloadByteOffset + unitByteLength;
        ++unitIndex;
    }

    if (result.units.empty()) {
        return framingFailure(SamplePayloadFramingStatus::InvalidRequest,
                              QStringLiteral("Sample framing produced no units"));
    }

    result.status = SamplePayloadFramingStatus::Framed;
    result.framedByteCount = cursorByte;
    return result;
}

SamplePayloadRunResult SamplePayloadRunner::run(const SamplePayloadRunRequest& request) {
    if (request.source == nullptr || request.sample == nullptr || request.tree == nullptr) {
        return runFailure(SamplePayloadRunStatus::InvalidRequest,
                          QStringLiteral("Sample payload runner requires a source, sample, and tree"));
    }

    const core::SampleDescriptor& sample = *request.sample;
    if (sample.sourceSpans.empty()) {
        return runFailure(SamplePayloadRunStatus::InvalidRequest,
                          QStringLiteral("Sample descriptor carries no source spans"));
    }

    const auto sampleMapping =
        core::SourceMapping::create(request.sampleViewId, sample.sourceSpans);
    if (!sampleMapping) {
        return runFailure(SamplePayloadRunStatus::InvalidRequest,
                          QStringLiteral("Failed to build a mapping for the sample spans"));
    }

    const quint64 sampleBitLength = sampleMapping->logicalBitLength();
    if (sampleBitLength == 0 || (sampleBitLength % 8U) != 0) {
        return runFailure(SamplePayloadRunStatus::InvalidRequest,
                          QStringLiteral("Sample length is zero or not byte-aligned"));
    }
    if (sampleBitLength / 8U > kMaxByteCoordinate) {
        return runFailure(SamplePayloadRunStatus::InvalidRequest,
                          QStringLiteral("Sample length exceeds coordinate limit"));
    }

    if (request.options.cancellation && request.options.cancellation->isCancellationRequested()) {
        return runFailure(SamplePayloadRunStatus::Cancelled,
                          QStringLiteral("Sample execution was cancelled"));
    }

    if (request.framing == SamplePayloadFraming::LengthPrefixed &&
        (request.executionSession == nullptr || request.transformRegistry == nullptr)) {
        return runFailure(
            SamplePayloadRunStatus::InvalidRequest,
            QStringLiteral("Length-prefixed execution requires a rule session and transform registry"));
    }

    // ADR-0105 section 5: an opaque access unit must reference the configuration
    // it decodes against rather than guessing one.
    if (request.framing == SamplePayloadFraming::OpaqueAccessUnit &&
        !request.configurationNode.has_value()) {
        return runFailure(
            SamplePayloadRunStatus::DependencyUnavailable,
            QStringLiteral("Access unit envelope requires a bound configuration node"));
    }

    SamplePayloadFramingOptions framingOptions;
    framingOptions.framing = request.framing;
    framingOptions.prefixLengthBytes = request.prefixLengthBytes;
    framingOptions.maximumUnits = request.maximumUnitsPerSample;
    framingOptions.cancellation = request.options.cancellation;

    const auto framing = SamplePayloadFramer::frame(*request.source, *sampleMapping, framingOptions);

    // The sample node is created even when framing failed, so the failure is
    // reported against a real node in the tree and the parent container stays
    // materialized (ADR-0105 section 6 item 4).
    const auto sampleLocation = locateByteRange(*sampleMapping, 0, sampleBitLength / 8U);
    if (!sampleLocation) {
        return runFailure(SamplePayloadRunStatus::InvalidRequest,
                          QStringLiteral("Failed to map the sample span"));
    }

    core::AnalysisNodeSpec sampleSpec;
    sampleSpec.kind = core::AnalysisNodeKind::Structure;
    sampleSpec.name = request.sampleNodeName.isEmpty()
                          ? QStringLiteral("Sample %1").arg(sample.sampleIndex)
                          : request.sampleNodeName;
    // The sample node stays in Indexing while its units are appended: the tree
    // only accepts children under an Indexing parent, and Materialized is
    // terminal. It is sealed on every exit path below.
    sampleSpec.state = core::MaterializationState::Indexing;
    sampleSpec.location = sampleLocation;

    const auto sampleNodeId = request.tree->appendChild(request.parentId, std::move(sampleSpec));
    if (!sampleNodeId) {
        return runFailure(SamplePayloadRunStatus::InvalidRequest,
                          QStringLiteral("Failed to append the sample node"));
    }

    SamplePayloadRunResult result;
    result.sampleNode = sampleNodeId;
    result.framedByteCount = framing.framedByteCount;

    const auto failSample = [&](SamplePayloadRunStatus status,
                                const QString& message) -> SamplePayloadRunResult {
        core::ParseDiagnostic diagnostic;
        diagnostic.code = diagnosticCodeFor(status);
        diagnostic.severity = core::DiagnosticSeverity::Error;
        diagnostic.message = message;
        diagnostic.location = sampleLocation;
        const auto state = status == SamplePayloadRunStatus::Cancelled
                               ? core::MaterializationState::Cancelled
                               : (status == SamplePayloadRunStatus::UnsupportedFraming
                                      ? core::MaterializationState::Unsupported
                                      : (status == SamplePayloadRunStatus::DependencyUnavailable
                                             ? core::MaterializationState::WaitingDependency
                                             : core::MaterializationState::Invalid));
        (void)request.tree->markPartial(*sampleNodeId, state, std::move(diagnostic));
        result.status = status;
        result.errorMessage = message;
        return result;
    };

    if (!framing.framed()) {
        return failSample(runStatusForFraming(framing.status), framing.errorMessage);
    }

    // Sample-level metadata comes straight from the descriptor, so the timeline
    // shown here is the same one the index derived.
    (void)appendComputedField(*request.tree, *sampleNodeId, QStringLiteral("sample_index"),
                              QVariant::fromValue(sample.sampleIndex),
                              QStringLiteral("Zero-based sample ordinal within the track."));
    (void)appendComputedField(*request.tree, *sampleNodeId, QStringLiteral("dts"),
                              QVariant::fromValue(sample.dts),
                              QStringLiteral("Decoding timestamp in timescale units."));
    (void)appendComputedField(*request.tree, *sampleNodeId, QStringLiteral("pts"),
                              QVariant::fromValue(sample.pts),
                              QStringLiteral("Presentation timestamp in timescale units."));
    (void)appendComputedField(*request.tree, *sampleNodeId, QStringLiteral("duration"),
                              QVariant::fromValue(sample.duration),
                              QStringLiteral("Sample duration in timescale units."));
    (void)appendComputedField(*request.tree, *sampleNodeId, QStringLiteral("timescale"),
                              QVariant::fromValue(sample.timescale),
                              QStringLiteral("Track timescale."));
    (void)appendComputedField(*request.tree, *sampleNodeId, QStringLiteral("is_sync_sample"),
                              QVariant::fromValue(sample.isSyncSample),
                              QStringLiteral("Whether the sample is a random access point."));
    (void)appendComputedField(*request.tree, *sampleNodeId, QStringLiteral("byte_length"),
                              QVariant::fromValue(sampleBitLength / 8U),
                              QStringLiteral("Physical sample length in bytes."));

    const auto locateUnit = [&](quint64 byteOffset,
                                quint64 byteLength) -> std::optional<core::FieldLocation> {
        return locateByteRange(*sampleMapping, byteOffset, byteLength);
    };

    if (request.framing == SamplePayloadFraming::OpaqueAccessUnit) {
        const auto& unitRange = framing.units.front();
        const auto payloadLocation = locateUnit(unitRange.payloadByteOffset,
                                                unitRange.payloadByteLength);
        if (!payloadLocation) {
            return failSample(SamplePayloadRunStatus::InvalidRequest,
                              QStringLiteral("Failed to map the access unit payload span"));
        }

        // v0.1 does not decode inside the access unit, so the payload is a
        // single compressed record whose physical extent stays exact.
        core::AnalysisNodeSpec payloadSpec;
        payloadSpec.kind = core::AnalysisNodeKind::CompressedPayload;
        payloadSpec.name = request.unitNodeName.isEmpty() ? QStringLiteral("access_unit")
                                                          : request.unitNodeName;
        payloadSpec.state = core::MaterializationState::Materialized;
        payloadSpec.location = payloadLocation;
        payloadSpec.metadata.description =
            QStringLiteral("Compressed access unit payload presented without spectral decoding.");

        const auto payloadNodeId =
            request.tree->appendChild(*sampleNodeId, std::move(payloadSpec));
        if (!payloadNodeId) {
            return failSample(SamplePayloadRunStatus::InvalidRequest,
                              QStringLiteral("Failed to append the access unit payload node"));
        }

        (void)appendComputedField(
            *request.tree, *sampleNodeId, QStringLiteral("configuration_node"),
            QVariant::fromValue(request.configurationNode->value()),
            QStringLiteral("Identifier of the configuration this access unit decodes against."));
        if (!request.configurationSummary.isEmpty()) {
            (void)appendComputedField(*request.tree, *sampleNodeId,
                                      QStringLiteral("configuration_summary"),
                                      QVariant::fromValue(request.configurationSummary),
                                      QStringLiteral("Formatted summary of the bound configuration."));
        }

        SamplePayloadUnitResult unitResult;
        unitResult.unitIndex = unitRange.unitIndex;
        unitResult.payloadSpans = payloadLocation->sourceSpans();
        unitResult.unitNode = payloadNodeId;
        unitResult.executionStatus = RuleExecutionStatus::Materialized;
        result.units.push_back(std::move(unitResult));

        if (!request.tree->transition(*sampleNodeId, core::MaterializationState::Materialized)) {
            return failSample(SamplePayloadRunStatus::InvalidRequest,
                              QStringLiteral("Failed to materialize the sample node"));
        }

        result.status = SamplePayloadRunStatus::Executed;
        return result;
    }

    for (const auto& unitRange : framing.units) {
        if (request.options.cancellation &&
            request.options.cancellation->isCancellationRequested()) {
            return failSample(SamplePayloadRunStatus::Cancelled,
                              QStringLiteral("Sample execution was cancelled"));
        }

        const auto prefixLocation = locateUnit(unitRange.prefixByteOffset,
                                               unitRange.prefixByteLength);
        const auto payloadLocation = locateUnit(unitRange.payloadByteOffset,
                                                unitRange.payloadByteLength);
        if (!prefixLocation || !payloadLocation) {
            return failSample(SamplePayloadRunStatus::InvalidRequest,
                              QStringLiteral("Failed to map unit %1 within the sample")
                                  .arg(unitRange.unitIndex));
        }

        core::AnalysisNodeSpec unitSpec;
        unitSpec.kind = core::AnalysisNodeKind::Structure;
        unitSpec.name = request.unitNodeName.isEmpty()
                            ? QStringLiteral("Unit %1").arg(unitRange.unitIndex)
                            : QStringLiteral("%1 %2").arg(request.unitNodeName)
                                  .arg(unitRange.unitIndex);
        // Indexing until the unit's own children are in place; the compound
        // runner requires an indexing parent as well.
        unitSpec.state = core::MaterializationState::Indexing;
        // The unit spans prefix and payload as one contiguous logical run, so it
        // is located as a single range rather than as concatenated pieces.
        unitSpec.location = locateUnit(unitRange.prefixByteOffset,
                                       unitRange.prefixByteLength + unitRange.payloadByteLength);
        if (!unitSpec.location) {
            return failSample(SamplePayloadRunStatus::InvalidRequest,
                              QStringLiteral("Failed to map unit %1 extent")
                                  .arg(unitRange.unitIndex));
        }

        const auto unitNodeId = request.tree->appendChild(*sampleNodeId, std::move(unitSpec));
        if (!unitNodeId) {
            return failSample(SamplePayloadRunStatus::InvalidRequest,
                              QStringLiteral("Failed to append unit %1")
                                  .arg(unitRange.unitIndex));
        }

        // The length prefix is container framing, not codec syntax, so it is
        // recorded as its own field with exact physical coordinates.
        core::AnalysisNodeSpec prefixSpec;
        prefixSpec.kind = core::AnalysisNodeKind::SyntaxField;
        prefixSpec.name = QStringLiteral("unit_length");
        prefixSpec.state = core::MaterializationState::Materialized;
        prefixSpec.value = QVariant::fromValue(unitRange.payloadByteLength);
        prefixSpec.location = prefixLocation;
        prefixSpec.metadata.description =
            QStringLiteral("Length-prefixed unit size in bytes, excluding the prefix itself.");
        (void)request.tree->appendChild(*unitNodeId, std::move(prefixSpec));

        SamplePayloadUnitResult unitResult;
        unitResult.unitIndex = unitRange.unitIndex;
        unitResult.payloadSpans = payloadLocation->sourceSpans();
        unitResult.unitNode = unitNodeId;

        // Each unit is executed as header + dispatched payload against the
        // caller's session, so parameter sets published earlier remain visible.
        // The payload's transform is chosen by the rule's own payload dispatch.
        const auto unitMapping =
            core::SourceMapping::create(request.sampleViewId, payloadLocation->sourceSpans());
        if (!unitMapping) {
            unitResult.executionStatus = RuleExecutionStatus::InvalidDefinition;
            unitResult.errorMessage = QStringLiteral("Failed to build a mapping for unit %1")
                                          .arg(unitRange.unitIndex);
        } else {
            CompoundRuleExecutionRequest unitRequest;
            unitRequest.source = request.source;
            unitRequest.headerMapping = &*unitMapping;
            unitRequest.headerStructureIndex = request.unitStructureIndex;
            unitRequest.payloadMapping = &*unitMapping;
            unitRequest.payloadLogicalStart = 0;
            unitRequest.transformRegistry = request.transformRegistry;
            unitRequest.tree = request.tree;
            unitRequest.parentId = *unitNodeId;
            unitRequest.enclosingSourceSpans = payloadLocation->sourceSpans();
            unitRequest.options = request.options;
            unitRequest.requireExactConsumption = true;
            unitRequest.autoDispatchPayload = true;

            const auto execution = request.executionSession->runCompound(unitRequest);
            unitResult.executionStatus = execution.status;
            unitResult.errorMessage = execution.errorMessage;
            unitResult.structureNode = execution.execution.headerNodeId;
            unitResult.excludedSpans = execution.execution.excludedSpans;
        }

        if (!unitResult.executed()) {
            core::ParseDiagnostic diagnostic;
            diagnostic.code = diagnosticCodeForUnit(unitResult.executionStatus);
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message =
                unitResult.errorMessage.isEmpty()
                    ? QStringLiteral("Unit %1 failed to execute").arg(unitRange.unitIndex)
                    : unitResult.errorMessage;
            diagnostic.location = payloadLocation;
            (void)request.tree->markPartial(
                *unitNodeId, unitStateFor(unitResult.executionStatus), std::move(diagnostic));

            if (isRunTerminating(unitResult.executionStatus)) {
                const auto status = runStatusForUnit(unitResult.executionStatus);
                const QString message = unitResult.errorMessage;
                result.units.push_back(std::move(unitResult));
                return failSample(status, message);
            }
        } else if (!request.tree->transition(*unitNodeId,
                                            core::MaterializationState::Materialized)) {
            return failSample(SamplePayloadRunStatus::InvalidRequest,
                              QStringLiteral("Failed to materialize unit %1")
                                  .arg(unitRange.unitIndex));
        }

        result.units.push_back(std::move(unitResult));
    }

    // The sample closes only after every unit reached a terminal state, so a
    // half-built sub-tree never presents itself as complete.
    if (!request.tree->transition(*sampleNodeId, core::MaterializationState::Materialized)) {
        return failSample(SamplePayloadRunStatus::InvalidRequest,
                          QStringLiteral("Failed to materialize the sample node"));
    }

    result.status = SamplePayloadRunStatus::Executed;
    return result;
}

} // namespace streamview::rules
