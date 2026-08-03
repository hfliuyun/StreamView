#pragma once

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <vector>

namespace streamview::rules {

struct DslSourcePosition final {
    quint64 offset = 0;
    quint32 line = 1;
    quint32 column = 1;
};

struct DslSourceRange final {
    DslSourcePosition start;
    DslSourcePosition end;
};

enum class DslTokenKind : quint8 {
    Identifier,
    IntegerLiteral,
    StringLiteral,
    At,
    Less,
    Greater,
    LeftBrace,
    RightBrace,
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    Semicolon,
    Comma,
    Colon,
    Equals,
    EqualEqual,
    Bang,
    BangEqual,
    Star,
    Slash,
    Percent,
    Plus,
    Minus,
    LessEqual,
    GreaterEqual,
    AndAnd,
    OrOr,
    EndOfFile,
    Invalid,
};

struct DslToken final {
    DslTokenKind kind = DslTokenKind::Invalid;
    QString lexeme;
    quint64 integerValue = 0;
    DslSourceRange range;
};

enum class DslDiagnosticCode : quint8 {
    InvalidCharacter,
    UnterminatedComment,
    UnterminatedString,
    InvalidEscape,
    InvalidInteger,
    UnexpectedToken,
    MissingToken,
    InvalidBitWidth,
    EmptyStruct,
    DuplicateName,
    MissingEntry,
    UnknownReference,
    UnsupportedScanner,
    InvalidProgressiveAnnotation,
    InvalidAnnotation,
    InvalidType,
    ConstraintOutOfRange,
    EmptyEnum,
    InvalidEndian,
    EnumValueOutOfRange,
    InvalidArrayLength,
    InvalidCondition,
    InvalidExpression,
    InvalidPayloadDispatch,
    InvalidRbspTrailingBits,
    InvalidContext,
};

struct DslDiagnostic final {
    DslDiagnosticCode code = DslDiagnosticCode::UnexpectedToken;
    QString message;
    DslSourceRange range;
};

struct DslLexResult final {
    std::vector<DslToken> tokens;
    std::vector<DslDiagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const noexcept { return diagnostics.empty(); }
};

class DslLexer final {
public:
    [[nodiscard]] static DslLexResult lex(const QString& source);
};

enum class DslAnnotationValueKind : quint8 {
    Integer,
    String,
    Identifier,
};

struct DslAnnotationValue final {
    DslAnnotationValueKind kind = DslAnnotationValueKind::Identifier;
    quint64 integerValue = 0;
    QString text;
};

struct DslAnnotation final {
    QString name;
    std::vector<DslAnnotationValue> arguments;
    DslSourceRange range;
};

enum class DslEndian : quint8 {
    Big,
    Little,
};

enum class DslFieldEncoding : quint8 {
    Bits,
    UnsignedExpGolomb,
    SignedExpGolomb,
};

enum class DslScalarType : quint8 {
    Bool,
    U64,
};

enum class DslExpressionKind : quint8 {
    UnsignedLiteral,
    BooleanLiteral,
    Identifier,
    Call,
    Unary,
    Binary,
};

enum class DslUnaryOperator : quint8 {
    LogicalNot,
};

enum class DslBinaryOperator : quint8 {
    Multiply,
    Divide,
    Remainder,
    Add,
    Subtract,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    LogicalAnd,
    LogicalOr,
};

struct DslExpression final {
    DslExpressionKind kind = DslExpressionKind::UnsignedLiteral;
    DslUnaryOperator unaryOperator = DslUnaryOperator::LogicalNot;
    DslBinaryOperator binaryOperator = DslBinaryOperator::Add;
    quint64 unsignedValue = 0;
    bool booleanValue = false;
    QString name;
    std::vector<DslExpression> operands;
    DslSourceRange range;
};

struct DslFunctionParameter final {
    DslScalarType type = DslScalarType::U64;
    QString name;
    DslSourceRange range;
};

struct DslPureFunction final {
    DslScalarType returnType = DslScalarType::U64;
    QString name;
    std::vector<DslFunctionParameter> parameters;
    DslExpression expression;
    DslSourceRange range;
};

struct DslEnumValue final {
    QString name;
    quint64 value = 0;
    DslSourceRange range;
};

struct DslEnum final {
    QString name;
    std::vector<DslAnnotation> annotations;
    std::vector<DslEnumValue> values;
    DslSourceRange range;
};

struct DslBitField final {
    QString name;
    DslFieldEncoding encoding = DslFieldEncoding::Bits;
    quint8 width = 0;
    DslEndian endian = DslEndian::Big;
    std::optional<quint64> arrayLength;
    std::vector<DslAnnotation> annotations;
    DslSourceRange range;
};

struct DslComputedField final {
    DslScalarType type = DslScalarType::U64;
    QString name;
    DslExpression expression;
    std::vector<DslAnnotation> annotations;
    DslSourceRange range;
};

struct DslLazyRegion final {
    QString name;
    DslExpression byteCountExpression;
    std::vector<DslAnnotation> annotations;
    DslSourceRange range;
};

struct DslRbspTrailingBits final {
    std::vector<DslAnnotation> annotations;
    DslSourceRange range;
};

struct DslEqualityCondition final {
    QString fieldName;
    quint64 expectedValue = 0;
    bool booleanShorthand = false;
    DslSourceRange range;
};

enum class DslStructItemKind : quint8 {
    Field,
    Computed,
    LazyRegion,
    RbspTrailingBits,
    Conditional,
    Switch,
    Repeat,
};

enum class DslSwitchArmKind : quint8 {
    Case,
    Default,
};

struct DslStructItem final {
    struct SwitchArm final {
        DslSwitchArmKind kind = DslSwitchArmKind::Case;
        quint64 caseValue = 0;
        DslSourceRange valueRange;
        std::vector<DslStructItem> items;
        DslSourceRange range;
    };

    DslStructItemKind kind = DslStructItemKind::Field;
    DslBitField field;
    DslComputedField computed;
    DslLazyRegion lazyRegion;
    DslRbspTrailingBits rbspTrailingBits;
    DslEqualityCondition condition;
    std::vector<DslStructItem> thenItems;
    std::vector<DslStructItem> elseItems;
    QString switchFieldName;
    DslSourceRange switchFieldRange;
    std::vector<SwitchArm> switchArms;
    QString repeatCountFieldName;
    DslSourceRange repeatCountFieldRange;
    quint64 repeatMaximum = 0;
    DslSourceRange repeatMaximumRange;
    std::vector<DslStructItem> repeatItems;
    DslSourceRange range;
};

struct DslStruct final {
    QString name;
    std::vector<DslAnnotation> annotations;
    std::vector<DslStructItem> items;
    DslSourceRange range;
};

struct DslProgressiveScan final {
    QString elementType;
    QString name;
    QString scannerName;
    std::vector<DslAnnotation> annotations;
    DslSourceRange range;
};

struct DslEntry final {
    QString targetName;
    std::vector<DslAnnotation> annotations;
    DslSourceRange range;
};

enum class DslPayloadCaseKind : quint8 {
    Structure,
    Empty,
};

struct DslPayloadCase final {
    DslPayloadCaseKind kind = DslPayloadCaseKind::Structure;
    quint64 value = 0;
    DslSourceRange valueRange;
    QString targetName;
    DslSourceRange range;
};

struct DslPayloadDispatch final {
    QString viewKind;
    QString sequenceName;
    QString controllerFieldName;
    DslSourceRange controllerRange;
    std::vector<DslPayloadCase> cases;
    std::vector<DslAnnotation> annotations;
    DslSourceRange range;
};

struct DslProgram final {
    std::vector<DslPureFunction> pureFunctions;
    std::vector<DslEnum> enums;
    std::vector<DslStruct> structs;
    std::vector<DslProgressiveScan> scans;
    std::optional<DslPayloadDispatch> payloadDispatch;
    DslEntry entry;
    bool hasEntry = false;
};

struct DslParseResult final {
    DslProgram program;
    std::vector<DslDiagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const noexcept { return diagnostics.empty(); }
};

class DslParser final {
public:
    [[nodiscard]] static DslParseResult parse(const QString& source);
};

} // namespace streamview::rules
