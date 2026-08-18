#include <streamview/rules/h264_rbsp_payload_transform_provider.h>

#include <streamview/rules/h264_ebsp_rbsp_mapper.h>

#include <limits>
#include <utility>

namespace streamview::rules {

namespace {

[[nodiscard]] QString issueMessage(H264EbspRbspIssueKind kind) {
    switch (kind) {
    case H264EbspRbspIssueKind::Prohibited000000:
        return QStringLiteral("H.264 EBSP contains prohibited byte sequence 00 00 00");
    case H264EbspRbspIssueKind::Prohibited000001:
        return QStringLiteral("H.264 EBSP contains prohibited byte sequence 00 00 01");
    case H264EbspRbspIssueKind::Prohibited000002:
        return QStringLiteral("H.264 EBSP contains prohibited byte sequence 00 00 02");
    case H264EbspRbspIssueKind::Prohibited000003xx:
        return QStringLiteral(
            "H.264 EBSP contains prohibited byte sequence 00 00 03 xx with xx greater than 03");
    case H264EbspRbspIssueKind::FinalZeroByte:
        return QStringLiteral("H.264 EBSP final byte must not be 00");
    }
    return QStringLiteral("H.264 EBSP violates byte-sequence conformance requirements");
}

} // namespace

PayloadTransformResult H264RbspPayloadTransformProvider::transform(
    const PayloadTransformRequest& request) const {
    PayloadTransformResult result;

    if (!request.source || !request.inputMapping) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral(
            "H.264 RBSP transform requires a valid source and input mapping");
        return result;
    }

    if (request.logicalBitStart % 8U != 0) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("H.264 RBSP input start must be byte-aligned");
        return result;
    }

    if (request.logicalBitLength == 0) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("H.264 RBSP input length must be greater than zero");
        return result;
    }

    if (request.logicalBitLength % 8U != 0) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("H.264 RBSP input length must be byte-aligned");
        return result;
    }

    if (request.logicalBitStart >
            std::numeric_limits<quint64>::max() - request.logicalBitLength ||
        request.logicalBitStart + request.logicalBitLength >
            request.inputMapping->logicalBitLength()) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("H.264 RBSP requested range exceeds input mapping");
        return result;
    }

    if (request.cancellation && request.cancellation->isCancellationRequested()) {
        result.status = DslExecutionStatus::Cancelled;
        result.errorMessage = QStringLiteral("H.264 RBSP transform was cancelled");
        return result;
    }

    const auto logicalAddress = core::LogicalBitAddress(
        request.inputMapping->viewId(), request.logicalBitStart);
    const auto logicalRange = core::LogicalRange::create(logicalAddress, request.logicalBitLength);
    if (!logicalRange) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Invalid logical range coordinates");
        return result;
    }

    const auto location = request.inputMapping->locate(*logicalRange);
    if (!location || location->sourceSpans().empty()) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Failed to locate requested range in input mapping");
        return result;
    }

    const auto& spans = location->sourceSpans();
    if (spans.size() > 1) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral(
            "Disjoint EBSP source spans are not supported by the H.264 RBSP mapper");
        return result;
    }

    const core::SourceSpan& ebspSpan = spans.front();
    if (ebspSpan.start().bitOffsetInByte() != 0 || ebspSpan.bitLength() % 8U != 0) {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Located EBSP span is not byte-aligned");
        return result;
    }

    H264EbspRbspMapLimits limits;
    H264EbspRbspMapper mapper(
        *request.source,
        request.inputMapping->viewId(),
        ebspSpan,
        limits,
        request.cancellation);

    const quint64 budget = request.maximumInspectedBytes > 0
                               ? request.maximumInspectedBytes
                               : std::numeric_limits<quint64>::max();
    const auto batch = mapper.mapBatch(budget);

    const quint64 startByte = ebspSpan.start().byteOffset();
    result.inspectedByteCount =
        mapper.sourceCursor() >= startByte ? (mapper.sourceCursor() - startByte) : 0;

    if (batch.status == H264EbspRbspMapStatus::Complete) {
        result.status = DslExecutionStatus::Materialized;
        result.forwardedMapping = mapper.mapping();

        result.excludedSpans.reserve(mapper.excludedSpans().size());
        for (const auto& excluded : mapper.excludedSpans()) {
            result.excludedSpans.push_back({excluded.sourceSpan, excluded.rbspBitOffset});
        }

        for (const auto& issue : mapper.issues()) {
            core::ParseDiagnostic diag;
            diag.code = core::DiagnosticCode::InvalidSyntax;
            diag.severity = core::DiagnosticSeverity::Error;
            diag.message = issueMessage(issue.kind);
            const auto diagRange = core::LogicalRange::create(
                core::LogicalBitAddress(request.inputMapping->viewId(), 0),
                issue.sourceSpan.bitLength());
            if (diagRange) {
                diag.location = core::FieldLocation::create(*diagRange, {issue.sourceSpan});
            }
            result.diagnostics.push_back(std::move(diag));
        }
    } else if (batch.status == H264EbspRbspMapStatus::Cancelled) {
        result.status = DslExecutionStatus::Cancelled;
        result.errorMessage = batch.errorMessage.isEmpty()
                                  ? QStringLiteral("H.264 RBSP transform was cancelled")
                                  : batch.errorMessage;
    } else if (batch.status == H264EbspRbspMapStatus::SourceError) {
        result.status = DslExecutionStatus::SourceError;
        result.errorMessage = batch.errorMessage;
    } else if (batch.status == H264EbspRbspMapStatus::ResourceLimit) {
        result.status = DslExecutionStatus::ResourceLimit;
        result.errorMessage = batch.errorMessage;
    } else if (batch.status == H264EbspRbspMapStatus::InProgress) {
        result.status = DslExecutionStatus::ResourceLimit;
        result.errorMessage = QStringLiteral("H.264 RBSP transform exceeded inspection budget");
    } else {
        result.status = DslExecutionStatus::InvalidDefinition;
        result.errorMessage = batch.errorMessage;
    }

    return result;
}

} // namespace streamview::rules
