#pragma once

#include <streamview/core/analysis_model.h>
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

struct DslTypedFieldCondition final {
    quint32 fieldIndex = 0;
    quint64 expectedValue = 0;
    bool negated = false;
    DslConditionOperator op = DslConditionOperator::Equal;
};

enum class DslTypedExpressionKind : quint8 {
    UnsignedLiteral,
    BooleanLiteral,
    FieldReference,
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
    std::vector<DslTypedExpression> operands;
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

struct DslTypedField final {
    DslTypedFieldKind kind = DslTypedFieldKind::Declared;
    QString name;
    DslValueType type;
    std::optional<quint64> equalsConstraint;
    std::optional<DslTypedUnsignedRange> rangeConstraint;
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

struct DslTypedStruct final {
    QString name;
    core::AnalysisNodeMetadata metadata;
    std::vector<DslTypedField> fields;
    std::vector<DslTypedRepeatBound> repeatBounds;
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
    ReadRbspTrailingBits,
    AssertEquals,
    AssertRangeMinimum,
    AssertRangeMaximum,
    AssertRepeatCount,
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
