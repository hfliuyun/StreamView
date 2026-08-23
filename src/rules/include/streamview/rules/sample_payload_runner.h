#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/sample_descriptor.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl_vm.h>
#include <streamview/rules/payload_transform.h>
#include <streamview/rules/rule_execution_session.h>

#include <QString>
#include <QtGlobal>

#include <optional>
#include <vector>

namespace streamview::rules {

/// How the bytes of one media sample are divided into executable units.
///
/// Both members are container-framing shapes rather than codec identities: a
/// length-prefixed sample carries its own unit boundaries, while an opaque
/// access unit is presented whole. The concrete codec is decided by the rule
/// package the caller resolved, never by this component.
enum class SamplePayloadFraming : quint8 {
    LengthPrefixed,
    OpaqueAccessUnit,
};

enum class SamplePayloadFramingStatus : quint8 {
    Framed,
    InvalidRequest,
    UnsupportedFraming,
    TruncatedSample,
    InvalidUnitLength,
    ResourceLimit,
    SourceError,
    Cancelled,
};

/// One framed unit, expressed in byte offsets relative to the start of the
/// sample rather than absolute coordinates, so framing stays independent of how
/// the sample is physically laid out in the source.
struct SamplePayloadUnitRange final {
    quint64 unitIndex = 0;
    quint64 prefixByteOffset = 0;
    quint32 prefixByteLength = 0;
    quint64 payloadByteOffset = 0;
    quint64 payloadByteLength = 0;
};

struct SamplePayloadFramingOptions final {
    SamplePayloadFraming framing = SamplePayloadFraming::LengthPrefixed;
    /// Bytes occupied by each unit's length prefix. Only 1, 2, and 4 are legal.
    quint32 prefixLengthBytes = 4;
    quint64 maximumUnits = 1024;
    std::optional<core::CancellationToken> cancellation;
};

struct SamplePayloadFramingResult final {
    SamplePayloadFramingStatus status = SamplePayloadFramingStatus::InvalidRequest;
    std::vector<SamplePayloadUnitRange> units;
    /// Bytes accounted for by `units`; a truncated sample stops short of its length.
    quint64 framedByteCount = 0;
    QString errorMessage;

    [[nodiscard]] bool framed() const noexcept {
        return status == SamplePayloadFramingStatus::Framed;
    }
};

/// Splits one sample into units without copying payload: it reads only the
/// length prefixes and returns byte ranges into the sample it was given.
class SamplePayloadFramer final {
public:
    [[nodiscard]] static constexpr quint64 defaultMaximumUnits() noexcept { return 1024; }

    [[nodiscard]] static SamplePayloadFramingResult
    frame(const core::RandomAccessSource& source,
          const core::SourceMapping& sampleMapping,
          const SamplePayloadFramingOptions& options);
};

enum class SamplePayloadRunStatus : quint8 {
    /// A sample sub-tree was produced. Individual units may still carry their
    /// own diagnostics: a failing unit is isolated, not fatal to the sample.
    Executed,
    InvalidRequest,
    UnsupportedFraming,
    DependencyUnavailable,
    TruncatedSample,
    InvalidUnitLength,
    SourceError,
    Cancelled,
    ResourceLimit,
};

struct SamplePayloadUnitResult final {
    quint64 unitIndex = 0;
    std::vector<core::SourceSpan> payloadSpans;
    std::optional<core::AnalysisNodeId> unitNode;
    std::optional<core::AnalysisNodeId> structureNode;
    /// Emulation-prevention (or equivalent) records the rule-declared transform
    /// kept out of the decoded bit stream, per the ADR-0104 contract.
    std::vector<PayloadExcludedSpan> excludedSpans;
    RuleExecutionStatus executionStatus = RuleExecutionStatus::InvalidDefinition;
    QString errorMessage;

    [[nodiscard]] bool executed() const noexcept {
        return executionStatus == RuleExecutionStatus::Materialized;
    }
};

struct SamplePayloadRunRequest final {
    const core::RandomAccessSource* source = nullptr;
    const core::SampleDescriptor* sample = nullptr;

    SamplePayloadFraming framing = SamplePayloadFraming::LengthPrefixed;
    quint32 prefixLengthBytes = 4;
    quint64 maximumUnitsPerSample = SamplePayloadFramer::defaultMaximumUnits();

    core::LogicalViewId sampleViewId{1};

    core::AnalysisTree* tree = nullptr;
    core::AnalysisNodeId parentId;

    // Length-prefixed execution seam. The caller resolves the session and the
    // entry structure from whichever rule package declared the sample's target
    // format, so the runner needs no codec knowledge of its own. The transform
    // applied to each unit is likewise chosen by the rule's payload dispatch.
    RuleExecutionSession* executionSession = nullptr;
    quint32 unitStructureIndex = 0;
    const PayloadTransformRegistry* transformRegistry = nullptr;

    // Opaque access-unit envelope seam (ADR-0105 section 5). The configuration
    // this sample depends on is resolved by the caller and referenced by node
    // id plus an already-formatted summary; absent configuration is reported as
    // DependencyUnavailable instead of being guessed at.
    std::optional<core::AnalysisNodeId> configurationNode;
    QString configurationSummary;

    QString sampleNodeName;
    QString unitNodeName;

    DslExecutionOptions options;
};

struct SamplePayloadRunResult final {
    SamplePayloadRunStatus status = SamplePayloadRunStatus::InvalidRequest;
    std::optional<core::AnalysisNodeId> sampleNode;
    std::vector<SamplePayloadUnitResult> units;
    quint64 framedByteCount = 0;
    QString errorMessage;

    [[nodiscard]] bool executed() const noexcept {
        return status == SamplePayloadRunStatus::Executed;
    }

    [[nodiscard]] quint64 executedUnitCount() const noexcept;
};

/// Builds a navigable sub-tree for one media sample.
///
/// Length-prefixed samples are split into units and each unit is executed
/// through the caller's `RuleExecutionSession`, so parameter-set context
/// published by an earlier unit or sample stays visible to later ones. Opaque
/// samples get an access-unit envelope instead: metadata, the raw payload span,
/// and a reference to the configuration they decode against.
class SamplePayloadRunner final {
public:
    [[nodiscard]] static SamplePayloadRunResult run(const SamplePayloadRunRequest& request);
};

} // namespace streamview::rules
