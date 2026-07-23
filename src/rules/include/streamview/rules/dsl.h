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
    Equals,
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

struct DslStruct final {
    QString name;
    std::vector<DslAnnotation> annotations;
    std::vector<DslBitField> fields;
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

struct DslProgram final {
    std::vector<DslEnum> enums;
    std::vector<DslStruct> structs;
    std::vector<DslProgressiveScan> scans;
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
