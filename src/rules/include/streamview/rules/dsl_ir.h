#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/context_directory.h>
#include <streamview/rules/dsl.h>

#include <QString>
#include <QtGlobal>

#include <optional>
#include <vector>

namespace streamview::rules {

enum class DslValueTypeKind : quint8 {
    UnsignedBits,
    Enum,
    UnsignedExpGolomb,
    SignedExpGolomb,
    ComputedBool,
    ComputedUnsigned,
    LazyBytes,
    CompressedPayload,
};

struct DslValueType final {
    DslValueTypeKind kind = DslValueTypeKind::UnsignedBits;
    quint8 bitWidth = 0;
    DslEndian endian = DslEndian::Big;
    std::optional<quint32> enumIndex;
};

struct DslTypedEnumValue final {
    QString name;
    quint64 value = 0;
};

struct DslTypedEnum final {
    QString name;
    std::vector<DslTypedEnumValue> values;
    core::AnalysisNodeMetadata metadata;
    DslSourceRange range;
};

enum class DslConditionOperator : quint8 {
    Equal,
    GreaterThan,
};
enum class DslTypedExpressionKind : quint8 {
    UnsignedLiteral,
    BooleanLiteral,
    FieldReference,
    ImportedContextReference,
    SequenceElementReference,
    PowerOfTwo,
    MoreRbspData,
    // Yields the named field when the executed path materialized it and
    // evaluates operands[0] as the fallback otherwise. See ADR-0066.
    OptionalFieldReference,
    Unary,
    Binary,
};

struct DslTypedExpression final {
    DslTypedExpressionKind kind = DslTypedExpressionKind::UnsignedLiteral;
    DslScalarType type = DslScalarType::U64;
    DslUnaryOperator unaryOperator = DslUnaryOperator::LogicalNot;
    DslBinaryOperator binaryOperator = DslBinaryOperator::Add;
    quint64 unsignedValue = 0;
    bool booleanValue = false;
    quint32 fieldIndex = 0;
    quint32 contextImportIndex = 0;
    core::ContextDefinitionKind contextDefinitionKind =
        core::ContextDefinitionKind::H264SequenceParameterSet;
    quint32 contextStructureIndex = 0;
    quint32 contextExportIndex = 0;
    quint32 elementFieldIndex = 0;
    std::vector<DslTypedExpression> operands;
};

struct DslTypedFieldCondition final {
    quint32 fieldIndex = 0;
    quint64 expectedValue = 0;
    bool negated = false;
    DslConditionOperator op = DslConditionOperator::Equal;
    std::optional<DslTypedExpression> expression;
};

enum class DslTypedFieldKind : quint8 {
    Declared,
    RbspStopOneBit,
    RbspAlignmentZeroBit,
};

struct DslTypedUnsignedRange final {
    quint64 minimum = 0;
    quint64 maximum = 0;
};

struct DslTypedSignedRange final {
    qint64 minimum = 0;
    qint64 maximum = 0;
};

struct DslTypedField final {
    DslTypedFieldKind kind = DslTypedFieldKind::Declared;
    QString name;
    DslValueType type;
    bool contextEligible = false;
    std::optional<DslTypedExpression> bitWidthExpression;
    std::optional<quint64> equalsConstraint;
    std::optional<DslTypedUnsignedRange> rangeConstraint;
    std::optional<DslTypedSignedRange> signedRangeConstraint;
    std::optional<DslTypedExpression> computedExpression;
    std::optional<DslTypedExpression> lazyByteCountExpression;
    std::vector<DslTypedFieldCondition> conditions;
    core::AnalysisNodeMetadata metadata;
    DslSourceRange range;
};

struct DslTypedRepeatBound final {
    quint32 controllerFieldIndex = 0;
    quint32 firstFieldIndex = 0;
    quint64 maximumCount = 0;
    std::vector<DslTypedFieldCondition> conditions;
    DslSourceRange range;
};

struct DslTypedSentinelRepeat final {
    [[nodiscard]] static constexpr quint64 maximumIterations() noexcept { return 64; }

    std::vector<quint32> firstFieldIndices;
    std::vector<quint32> sentinelFieldIndices;
    quint32 assertionFieldIndex = 0;
    quint64 terminatingValue = 0;
    std::vector<DslTypedFieldCondition> conditions;
    DslSourceRange range;
};

struct DslTypedAssertion final {
    [[nodiscard]] static constexpr std::size_t maximumPerStructure() noexcept {
        return 1'024;
    }

    DslTypedExpression condition;
    quint32 anchorFieldIndex = 0;
    quint32 assertionFieldIndex = 0;
    std::vector<DslTypedFieldCondition> conditions;
    DslSourceRange range;
};

struct DslTypedContextDependency final {
    core::ContextDefinitionKind kind =
        core::ContextDefinitionKind::H264SequenceParameterSet;
    quint32 keyFieldIndex = 0;
};

struct DslTypedContextDefinition final {
    [[nodiscard]] static constexpr std::size_t maximumDependencies() noexcept { return 16; }
    [[nodiscard]] static constexpr std::size_t maximumExports() noexcept { return 64; }

    core::ContextDefinitionKind kind =
        core::ContextDefinitionKind::H264SequenceParameterSet;
    quint32 keyFieldIndex = 0;
    std::vector<DslTypedContextDependency> dependencies;
    std::vector<quint32> exportFieldIndices;
};

struct DslTypedContextImport final {
    [[nodiscard]] static constexpr std::size_t maximumImports() noexcept { return 16; }

    core::ContextDefinitionKind kind =
        core::ContextDefinitionKind::H264SequenceParameterSet;
    quint32 keyFieldIndex = 0;
};

struct DslTypedStruct final {
    QString name;
    core::AnalysisNodeMetadata metadata;
    std::vector<DslTypedField> fields;
    std::vector<DslTypedRepeatBound> repeatBounds;
    std::vector<DslTypedSentinelRepeat> sentinelRepeats;
    std::vector<DslTypedAssertion> assertions;
    std::optional<DslTypedContextDefinition> contextDefinition;
    std::vector<DslTypedContextImport> contextImports;
    quint32 bytecodeOffset = 0;
    quint32 bytecodeLength = 0;
};

enum class DslScannerKind : quint8 {
    H264StartCode,
};

struct DslTypedScan final {
    QString name;
    quint32 elementStructIndex = 0;
    DslScannerKind scanner = DslScannerKind::H264StartCode;
    DslSourceRange range;
};

enum class DslEntryKind : quint8 {
    None,
    Structure,
    Sequence,
};

struct DslTypedEntry final {
    DslEntryKind kind = DslEntryKind::None;
    quint32 targetIndex = 0;
};

struct DslTypedPayloadCase final {
    quint64 value = 0;
    std::optional<quint32> structureIndex;
};

struct DslTypedPayloadDispatch final {
    quint32 scanIndex = 0;
    quint32 controllerFieldIndex = 0;
    std::vector<DslTypedPayloadCase> cases;
    core::AnalysisNodeMetadata metadata;

    [[nodiscard]] const DslTypedPayloadCase* find(quint64 value) const noexcept;
};

enum class DslOpcode : quint8 {
    BeginStructure,
    ReadUnsignedBits,
    ReadUnsignedExpGolomb,
    ReadSignedExpGolomb,
    EvaluateComputed,
    RegisterLazyBytes,
    RegisterCompressedPayload,
    ReadRbspTrailingBits,
    AssertEquals,
    AssertRangeMinimum,
    AssertRangeMaximum,
    AssertRepeatCount,
    AssertSentinelTerminated,
    AssertExpression,
    EndStructure,
};

struct DslInstruction final {
    DslOpcode opcode = DslOpcode::BeginStructure;
    quint32 operand = 0;
    quint64 immediate = 0;
};

struct DslTypedProgram final {
    std::vector<DslTypedEnum> enums;
    std::vector<DslTypedStruct> structs;
    std::vector<DslTypedScan> scans;
    std::vector<DslInstruction> bytecode;
    DslTypedEntry entry;
    std::optional<DslTypedPayloadDispatch> payloadDispatch;

    [[nodiscard]] std::optional<quint32> enumIndex(const QString& name) const;
    [[nodiscard]] std::optional<quint32> structureIndex(const QString& name) const;
    [[nodiscard]] std::optional<quint32> scanIndex(const QString& name) const;
};

struct DslCompileResult final {
    std::optional<DslTypedProgram> program;
    std::vector<DslDiagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const noexcept {
        return diagnostics.empty() && program.has_value();
    }
};

class DslCompiler final {
public:
    [[nodiscard]] static DslCompileResult compile(const DslProgram& program);
};

} // namespace streamview::rules
