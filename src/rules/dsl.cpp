#include <streamview/rules/dsl.h>

#include <QtGlobal>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace streamview::rules {

namespace {

[[nodiscard]] bool isIdentifierStart(QChar character) noexcept {
    return character == QLatin1Char('_') ||
           (character >= QLatin1Char('a') && character <= QLatin1Char('z')) ||
           (character >= QLatin1Char('A') && character <= QLatin1Char('Z'));
}

[[nodiscard]] bool isIdentifierPart(QChar character) noexcept {
    return isIdentifierStart(character) ||
           (character >= QLatin1Char('0') && character <= QLatin1Char('9'));
}

[[nodiscard]] bool isDecimalDigit(QChar character) noexcept {
    return character >= QLatin1Char('0') && character <= QLatin1Char('9');
}

[[nodiscard]] int hexDigit(QChar character) noexcept {
    if (character >= QLatin1Char('0') && character <= QLatin1Char('9')) {
        return character.unicode() - QLatin1Char('0').unicode();
    }
    if (character >= QLatin1Char('a') && character <= QLatin1Char('f')) {
        return character.unicode() - QLatin1Char('a').unicode() + 10;
    }
    if (character >= QLatin1Char('A') && character <= QLatin1Char('F')) {
        return character.unicode() - QLatin1Char('A').unicode() + 10;
    }
    return -1;
}

class Lexer final {
public:
    explicit Lexer(const QString& source) : source_(source) {}

    [[nodiscard]] DslLexResult run() {
        while (!atEnd()) {
            skipWhitespaceAndComments();
            if (atEnd()) {
                break;
            }
            lexToken();
        }

        DslToken end;
        end.kind = DslTokenKind::EndOfFile;
        end.range = positionRange();
        result_.tokens.push_back(std::move(end));
        return std::move(result_);
    }

private:
    [[nodiscard]] bool atEnd() const noexcept { return index_ >= source_.size(); }

    [[nodiscard]] DslSourcePosition position() const noexcept {
        return {static_cast<quint64>(index_), line_, column_};
    }

    [[nodiscard]] DslSourceRange positionRange() const noexcept {
        const DslSourcePosition current = position();
        return {current, current};
    }

    QChar current() const noexcept { return source_.at(index_); }

    QChar advance() noexcept {
        const QChar character = source_.at(index_++);
        if (character == QLatin1Char('\n')) {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return character;
    }

    void addDiagnostic(DslDiagnosticCode code,
                       const QString& message,
                       DslSourcePosition start,
                       DslSourcePosition end) {
        result_.diagnostics.push_back({code, message, {start, end}});
    }

    void skipWhitespaceAndComments() {
        bool skipped = true;
        while (skipped && !atEnd()) {
            skipped = false;
            while (!atEnd() && current().isSpace()) {
                advance();
                skipped = true;
            }
            if (atEnd() || current() != QLatin1Char('/')) {
                continue;
            }

            const DslSourcePosition commentStart = position();
            if (index_ + 1 < source_.size() && source_.at(index_ + 1) == QLatin1Char('/')) {
                advance();
                advance();
                while (!atEnd() && current() != QLatin1Char('\n')) {
                    advance();
                }
                skipped = true;
                continue;
            }
            if (index_ + 1 >= source_.size() || source_.at(index_ + 1) != QLatin1Char('*')) {
                continue;
            }

            advance();
            advance();
            bool closed = false;
            while (!atEnd()) {
                if (current() == QLatin1Char('*') && index_ + 1 < source_.size() &&
                    source_.at(index_ + 1) == QLatin1Char('/')) {
                    advance();
                    advance();
                    closed = true;
                    break;
                }
                advance();
            }
            if (!closed) {
                addDiagnostic(DslDiagnosticCode::UnterminatedComment,
                              QStringLiteral("Unterminated block comment"),
                              commentStart,
                              position());
            }
            skipped = true;
        }
    }

    void lexToken() {
        const DslSourcePosition start = position();
        const QChar character = current();
        if (isIdentifierStart(character)) {
            const qsizetype startIndex = index_;
            advance();
            while (!atEnd() && isIdentifierPart(current())) {
                advance();
            }
            DslToken token;
            token.kind = DslTokenKind::Identifier;
            token.lexeme = source_.mid(startIndex, index_ - startIndex);
            token.range = {start, position()};
            result_.tokens.push_back(std::move(token));
            return;
        }
        if (isDecimalDigit(character)) {
            lexInteger(start);
            return;
        }
        if (character == QLatin1Char('"')) {
            lexString(start);
            return;
        }

        const auto punctuation = [this, start](DslTokenKind kind) {
            const QString lexeme(1, advance());
            result_.tokens.push_back({kind, lexeme, 0, {start, position()}});
        };
        switch (character.unicode()) {
        case '@':
            punctuation(DslTokenKind::At);
            return;
        case '<':
            punctuation(DslTokenKind::Less);
            return;
        case '>':
            punctuation(DslTokenKind::Greater);
            return;
        case '{':
            punctuation(DslTokenKind::LeftBrace);
            return;
        case '}':
            punctuation(DslTokenKind::RightBrace);
            return;
        case '(':
            punctuation(DslTokenKind::LeftParen);
            return;
        case ')':
            punctuation(DslTokenKind::RightParen);
            return;
        case '[':
            punctuation(DslTokenKind::LeftBracket);
            return;
        case ']':
            punctuation(DslTokenKind::RightBracket);
            return;
        case ';':
            punctuation(DslTokenKind::Semicolon);
            return;
        case ',':
            punctuation(DslTokenKind::Comma);
            return;
        case ':':
            punctuation(DslTokenKind::Colon);
            return;
        case '=':
            if (index_ + 1 < source_.size() &&
                source_.at(index_ + 1) == QLatin1Char('=')) {
                advance();
                advance();
                result_.tokens.push_back(
                    {DslTokenKind::EqualEqual, QStringLiteral("=="), 0, {start, position()}});
                return;
            }
            punctuation(DslTokenKind::Equals);
            return;
        default:
            advance();
            addDiagnostic(DslDiagnosticCode::InvalidCharacter,
                          QStringLiteral("Invalid character in DSL source"),
                          start,
                          position());
            result_.tokens.push_back(
                {DslTokenKind::Invalid,
                 source_.mid(static_cast<qsizetype>(start.offset), 1),
                 0,
                 {start, position()}});
            return;
        }
    }

    void lexInteger(DslSourcePosition start) {
        const qsizetype startIndex = index_;
        int base = 10;
        if (current() == QLatin1Char('0') && index_ + 1 < source_.size() &&
            (source_.at(index_ + 1) == QLatin1Char('x') ||
             source_.at(index_ + 1) == QLatin1Char('X'))) {
            base = 16;
            advance();
            advance();
        }

        const qsizetype digitsStart = index_;
        quint64 value = 0;
        bool valid = true;
        while (!atEnd()) {
            const int digit = base == 10 ? (isDecimalDigit(current())
                                                ? current().unicode() - QLatin1Char('0').unicode()
                                                : -1)
                                        : hexDigit(current());
            if (digit < 0) {
                break;
            }
            if (value > (std::numeric_limits<quint64>::max() - static_cast<quint64>(digit)) /
                            static_cast<quint64>(base)) {
                valid = false;
            } else {
                value = value * static_cast<quint64>(base) + static_cast<quint64>(digit);
            }
            advance();
        }
        if (digitsStart == index_) {
            valid = false;
        }
        if (base == 16 && !atEnd() && isIdentifierPart(current())) {
            valid = false;
            while (!atEnd() && isIdentifierPart(current())) {
                advance();
            }
        }

        DslToken token;
        token.kind = valid ? DslTokenKind::IntegerLiteral : DslTokenKind::Invalid;
        token.lexeme = source_.mid(startIndex, index_ - startIndex);
        token.integerValue = value;
        token.range = {start, position()};
        result_.tokens.push_back(std::move(token));
        if (!valid) {
            addDiagnostic(DslDiagnosticCode::InvalidInteger,
                          QStringLiteral("Invalid integer literal"),
                          start,
                          position());
        }
    }

    void lexString(DslSourcePosition start) {
        const qsizetype startIndex = index_;
        advance();
        QString value;
        bool valid = true;
        while (!atEnd() && current() != QLatin1Char('"')) {
            if (current() == QLatin1Char('\n') || current() == QLatin1Char('\r')) {
                valid = false;
                break;
            }
            if (current() != QLatin1Char('\\')) {
                value.append(advance());
                continue;
            }

            advance();
            if (atEnd()) {
                valid = false;
                break;
            }
            const QChar escaped = advance();
            switch (escaped.unicode()) {
            case '"':
                value.append(QLatin1Char('"'));
                break;
            case '\\':
                value.append(QLatin1Char('\\'));
                break;
            case 'n':
                value.append(QLatin1Char('\n'));
                break;
            case 'r':
                value.append(QLatin1Char('\r'));
                break;
            case 't':
                value.append(QLatin1Char('\t'));
                break;
            default:
                valid = false;
                break;
            }
            if (!valid) {
                addDiagnostic(DslDiagnosticCode::InvalidEscape,
                              QStringLiteral("Unsupported string escape"),
                              start,
                              position());
                while (!atEnd() && current() != QLatin1Char('"') &&
                       current() != QLatin1Char('\n')) {
                    advance();
                }
                break;
            }
        }

        if (atEnd() || current() != QLatin1Char('"')) {
            addDiagnostic(DslDiagnosticCode::UnterminatedString,
                          QStringLiteral("Unterminated string literal"),
                          start,
                          position());
            valid = false;
        } else {
            advance();
        }

        DslToken token;
        token.kind = valid ? DslTokenKind::StringLiteral : DslTokenKind::Invalid;
        token.lexeme = std::move(value);
        token.range = {start, position()};
        result_.tokens.push_back(std::move(token));
        Q_UNUSED(startIndex);
    }

    QString source_;
    qsizetype index_ = 0;
    quint32 line_ = 1;
    quint32 column_ = 1;
    DslLexResult result_;
};

class Parser final {
public:
    explicit Parser(const QString& source) : lexResult_(DslLexer::lex(source)) {
        result_.diagnostics = lexResult_.diagnostics;
    }

    [[nodiscard]] DslParseResult run() {
        while (!at(DslTokenKind::EndOfFile)) {
            const std::vector<DslAnnotation> annotations = parseAnnotations();
            if (at(DslTokenKind::EndOfFile)) {
                if (!annotations.empty()) {
                    error(DslDiagnosticCode::UnexpectedToken,
                          QStringLiteral("Expected declaration after annotation"));
                }
                break;
            }
            if (isIdentifier(QStringLiteral("enum"))) {
                parseEnum(annotations);
            } else if (isIdentifier(QStringLiteral("struct"))) {
                parseStruct(annotations);
            } else if (isIdentifier(QStringLiteral("sequence"))) {
                parseScan(annotations);
            } else if (isIdentifier(QStringLiteral("entry"))) {
                parseEntry(annotations);
            } else {
                error(DslDiagnosticCode::UnexpectedToken,
                      QStringLiteral("Expected enum, struct, sequence, or entry declaration"));
                recoverDeclaration();
            }
        }
        validateProgram();
        return std::move(result_);
    }

private:
    [[nodiscard]] const DslToken& current() const { return lexResult_.tokens.at(index_); }
    [[nodiscard]] bool at(DslTokenKind kind) const { return current().kind == kind; }

    [[nodiscard]] bool isIdentifier(const QString& value) const {
        return at(DslTokenKind::Identifier) && current().lexeme == value;
    }

    const DslToken& consume() {
        const DslToken& token = current();
        if (!at(DslTokenKind::EndOfFile)) {
            ++index_;
        }
        return token;
    }

    bool match(DslTokenKind kind) {
        if (!at(kind)) {
            return false;
        }
        consume();
        return true;
    }

    bool matchIdentifier(const QString& value) {
        if (!isIdentifier(value)) {
            return false;
        }
        consume();
        return true;
    }

    void error(DslDiagnosticCode code, const QString& message) {
        result_.diagnostics.push_back({code, message, current().range});
    }

    bool expect(DslTokenKind kind, const QString& description) {
        if (match(kind)) {
            return true;
        }
        error(DslDiagnosticCode::MissingToken, QStringLiteral("Expected ") + description);
        return false;
    }

    bool expectIdentifier(QString* value, const QString& description) {
        if (!at(DslTokenKind::Identifier)) {
            error(DslDiagnosticCode::MissingToken, QStringLiteral("Expected ") + description);
            return false;
        }
        *value = consume().lexeme;
        return true;
    }

    std::vector<DslAnnotation> parseAnnotations() {
        std::vector<DslAnnotation> annotations;
        while (match(DslTokenKind::At)) {
            const DslSourcePosition start = lexResult_.tokens.at(index_ - 1).range.start;
            DslAnnotation annotation;
            if (!expectIdentifier(&annotation.name, QStringLiteral("annotation name"))) {
                recoverAnnotation();
                continue;
            }
            if (match(DslTokenKind::LeftParen)) {
                if (!at(DslTokenKind::RightParen)) {
                    while (true) {
                        DslAnnotationValue argument;
                        if (at(DslTokenKind::IntegerLiteral)) {
                            argument.kind = DslAnnotationValueKind::Integer;
                            argument.integerValue = consume().integerValue;
                        } else if (at(DslTokenKind::StringLiteral)) {
                            argument.kind = DslAnnotationValueKind::String;
                            argument.text = consume().lexeme;
                        } else if (at(DslTokenKind::Identifier)) {
                            argument.kind = DslAnnotationValueKind::Identifier;
                            argument.text = consume().lexeme;
                        } else {
                            error(DslDiagnosticCode::UnexpectedToken,
                                  QStringLiteral("Expected annotation argument"));
                            break;
                        }
                        annotation.arguments.push_back(std::move(argument));
                        if (!match(DslTokenKind::Comma)) {
                            break;
                        }
                    }
                }
                expect(DslTokenKind::RightParen, QStringLiteral("')' after annotation arguments"));
            }
            annotation.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
            annotations.push_back(std::move(annotation));
        }
        return annotations;
    }

    void parseEnum(const std::vector<DslAnnotation>& annotations) {
        const DslSourcePosition start = consume().range.start;
        DslEnum enumeration;
        enumeration.annotations = annotations;
        if (!expectIdentifier(&enumeration.name, QStringLiteral("enum name"))) {
            recoverDeclaration();
            return;
        }
        if (!expect(DslTokenKind::LeftBrace, QStringLiteral("'{' after enum name"))) {
            recoverDeclaration();
            return;
        }

        while (!at(DslTokenKind::RightBrace) && !at(DslTokenKind::EndOfFile)) {
            const DslSourcePosition valueStart = current().range.start;
            DslEnumValue value;
            if (!expectIdentifier(&value.name, QStringLiteral("enum member name"))) {
                recoverField();
                continue;
            }
            expect(DslTokenKind::Equals, QStringLiteral("'=' after enum member name"));
            if (at(DslTokenKind::IntegerLiteral)) {
                value.value = consume().integerValue;
            } else {
                error(DslDiagnosticCode::MissingToken,
                      QStringLiteral("Expected integer enum member value"));
            }
            expect(DslTokenKind::Semicolon, QStringLiteral("';' after enum member"));
            value.range = {valueStart, lexResult_.tokens.at(index_ - 1).range.end};
            enumeration.values.push_back(std::move(value));
        }

        const bool closed =
            expect(DslTokenKind::RightBrace, QStringLiteral("'}' after enum members"));
        match(DslTokenKind::Semicolon);
        enumeration.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        if (enumeration.values.empty() && closed) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::EmptyEnum,
                 QStringLiteral("An enum must contain at least one member"),
                 enumeration.range});
        }
        result_.program.enums.push_back(std::move(enumeration));
    }

    void parseField(std::vector<DslStructItem>& items,
                    const std::vector<DslAnnotation>& fieldAnnotations) {
        const DslSourcePosition fieldStart = current().range.start;
        DslBitField field;
        field.annotations = fieldAnnotations;
        quint64 width = 0;
        if (matchIdentifier(QStringLiteral("bits"))) {
            field.encoding = DslFieldEncoding::Bits;
            expect(DslTokenKind::Less, QStringLiteral("'<' after bits"));
            if (at(DslTokenKind::IntegerLiteral)) {
                width = consume().integerValue;
            } else {
                error(DslDiagnosticCode::MissingToken, QStringLiteral("Expected bit width"));
            }
            if (match(DslTokenKind::Comma)) {
                QString endianName;
                if (expectIdentifier(&endianName,
                                     QStringLiteral("byte order (big or little)"))) {
                    if (endianName == QStringLiteral("little")) {
                        field.endian = DslEndian::Little;
                    } else if (endianName != QStringLiteral("big")) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidEndian,
                             QStringLiteral("Byte order must be 'big' or 'little'"),
                             lexResult_.tokens.at(index_ - 1).range});
                    }
                }
            }
            expect(DslTokenKind::Greater,
                   QStringLiteral("'>' after bit width and byte order"));
        } else if (matchIdentifier(QStringLiteral("ue"))) {
            field.encoding = DslFieldEncoding::UnsignedExpGolomb;
        } else if (matchIdentifier(QStringLiteral("se"))) {
            field.encoding = DslFieldEncoding::SignedExpGolomb;
        } else {
            error(DslDiagnosticCode::UnexpectedToken,
                  QStringLiteral("Expected bits<N[, endian]>, ue, or se field type"));
            recoverField();
            return;
        }
        if (!expectIdentifier(&field.name, QStringLiteral("field name"))) {
            recoverField();
            return;
        }
        if (match(DslTokenKind::LeftBracket)) {
            if (at(DslTokenKind::IntegerLiteral)) {
                const DslToken length = consume();
                field.arrayLength = length.integerValue;
                if (*field.arrayLength == 0) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidArrayLength,
                         QStringLiteral("Fixed array length must be at least one"),
                         length.range});
                }
            } else {
                field.arrayLength = 0;
                error(DslDiagnosticCode::MissingToken,
                      QStringLiteral("Expected fixed array length"));
            }
            expect(DslTokenKind::RightBracket,
                   QStringLiteral("']' after fixed array length"));
        }
        const std::vector<DslAnnotation> trailingAnnotations = parseAnnotations();
        field.annotations.insert(
            field.annotations.end(), trailingAnnotations.begin(), trailingAnnotations.end());
        expect(DslTokenKind::Semicolon, QStringLiteral("';' after field"));
        field.width = width >= 1 && width <= 64 ? static_cast<quint8>(width) : 0;
        field.range = {fieldStart, lexResult_.tokens.at(index_ - 1).range.end};
        if (field.encoding == DslFieldEncoding::Bits && field.width == 0) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidBitWidth,
                 QStringLiteral("Bit field width must be in the range 1..64"),
                 field.range});
        }
        if (field.encoding == DslFieldEncoding::Bits &&
            field.endian == DslEndian::Little && field.width != 0 && field.width % 8 != 0) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidEndian,
                 QStringLiteral(
                     "Little-endian fields must have a width that is a multiple of 8"),
                 field.range});
        }
        DslStructItem item;
        item.kind = DslStructItemKind::Field;
        item.field = std::move(field);
        item.range = item.field.range;
        items.push_back(std::move(item));
    }

    void parseConditional(std::vector<DslStructItem>& items) {
        const DslSourcePosition start = consume().range.start;
        DslStructItem item;
        item.kind = DslStructItemKind::Conditional;
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after if"));
        const DslSourcePosition conditionStart = current().range.start;
        expectIdentifier(&item.condition.fieldName, QStringLiteral("condition field name"));
        if (!match(DslTokenKind::EqualEqual)) {
            if (match(DslTokenKind::Equals)) {
                error(DslDiagnosticCode::UnexpectedToken,
                      QStringLiteral("Conditions require the '==' operator"));
            } else {
                error(DslDiagnosticCode::MissingToken, QStringLiteral("'==' in condition"));
            }
        }
        if (at(DslTokenKind::IntegerLiteral)) {
            item.condition.expectedValue = consume().integerValue;
        } else {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected integer condition value"));
        }
        item.condition.range = {conditionStart, lexResult_.tokens.at(index_ - 1).range.end};
        expect(DslTokenKind::RightParen, QStringLiteral("')' after condition"));
        if (expect(DslTokenKind::LeftBrace, QStringLiteral("'{' after condition"))) {
            parseStructItems(item.thenItems);
            expect(DslTokenKind::RightBrace, QStringLiteral("'}' after conditional body"));
        } else {
            recoverField();
        }
        if (matchIdentifier(QStringLiteral("else"))) {
            if (expect(DslTokenKind::LeftBrace, QStringLiteral("'{' after else"))) {
                parseStructItems(item.elseItems);
                expect(DslTokenKind::RightBrace,
                       QStringLiteral("'}' after alternative conditional body"));
            } else {
                recoverField();
            }
        }
        item.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        items.push_back(std::move(item));
    }

    void parseSwitch(std::vector<DslStructItem>& items) {
        const DslSourcePosition start = consume().range.start;
        DslStructItem item;
        item.kind = DslStructItemKind::Switch;
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after switch"));
        if (at(DslTokenKind::Identifier)) {
            item.switchFieldRange = current().range;
            item.switchFieldName = consume().lexeme;
        } else {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected switch field name"));
        }
        expect(DslTokenKind::RightParen, QStringLiteral("')' after switch field name"));
        if (!expect(DslTokenKind::LeftBrace, QStringLiteral("'{' after switch controller"))) {
            recoverField();
            item.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
            items.push_back(std::move(item));
            return;
        }

        while (!at(DslTokenKind::RightBrace) && !at(DslTokenKind::EndOfFile)) {
            DslStructItem::SwitchArm arm;
            const DslSourcePosition armStart = current().range.start;
            if (matchIdentifier(QStringLiteral("case"))) {
                arm.kind = DslSwitchArmKind::Case;
                if (at(DslTokenKind::IntegerLiteral)) {
                    const DslToken value = consume();
                    arm.caseValue = value.integerValue;
                    arm.valueRange = value.range;
                } else {
                    error(DslDiagnosticCode::MissingToken,
                          QStringLiteral("Expected integer switch case value"));
                }
            } else if (matchIdentifier(QStringLiteral("default"))) {
                arm.kind = DslSwitchArmKind::Default;
                arm.valueRange = lexResult_.tokens.at(index_ - 1).range;
            } else {
                error(DslDiagnosticCode::UnexpectedToken,
                      QStringLiteral("Expected case or default switch label"));
                recoverSwitchArm();
                continue;
            }

            expect(DslTokenKind::Colon, QStringLiteral("':' after switch label"));
            if (expect(DslTokenKind::LeftBrace, QStringLiteral("'{' after switch label"))) {
                parseStructItems(arm.items);
                expect(DslTokenKind::RightBrace, QStringLiteral("'}' after switch arm"));
            } else {
                recoverSwitchArm();
            }
            arm.range = {armStart, lexResult_.tokens.at(index_ - 1).range.end};
            item.switchArms.push_back(std::move(arm));
        }

        expect(DslTokenKind::RightBrace, QStringLiteral("'}' after switch arms"));
        item.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        items.push_back(std::move(item));
    }

    void parseRepeat(std::vector<DslStructItem>& items) {
        const DslSourcePosition start = consume().range.start;
        DslStructItem item;
        item.kind = DslStructItemKind::Repeat;
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after repeat"));
        if (at(DslTokenKind::Identifier)) {
            item.repeatCountFieldRange = current().range;
            item.repeatCountFieldName = consume().lexeme;
        } else {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected repeat count field name"));
        }
        expect(DslTokenKind::Comma, QStringLiteral("',' after repeat count field name"));
        if (at(DslTokenKind::IntegerLiteral)) {
            const DslToken maximum = consume();
            item.repeatMaximum = maximum.integerValue;
            item.repeatMaximumRange = maximum.range;
        } else {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected repeat maximum count"));
        }
        expect(DslTokenKind::RightParen, QStringLiteral("')' after repeat maximum"));
        if (expect(DslTokenKind::LeftBrace, QStringLiteral("'{' after repeat header"))) {
            parseStructItems(item.repeatItems);
            expect(DslTokenKind::RightBrace, QStringLiteral("'}' after repeat body"));
        } else {
            recoverField();
        }
        item.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        items.push_back(std::move(item));
    }

    void parseStructItems(std::vector<DslStructItem>& items) {
        while (!at(DslTokenKind::RightBrace) && !at(DslTokenKind::EndOfFile)) {
            const std::vector<DslAnnotation> annotations = parseAnnotations();
            if (isIdentifier(QStringLiteral("if"))) {
                if (!annotations.empty()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidAnnotation,
                         QStringLiteral("Annotations are not allowed before if statements"),
                         annotations.front().range});
                }
                parseConditional(items);
            } else if (isIdentifier(QStringLiteral("switch"))) {
                if (!annotations.empty()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidAnnotation,
                         QStringLiteral("Annotations are not allowed before switch statements"),
                         annotations.front().range});
                }
                parseSwitch(items);
            } else if (isIdentifier(QStringLiteral("repeat"))) {
                if (!annotations.empty()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidAnnotation,
                         QStringLiteral("Annotations are not allowed before repeat statements"),
                         annotations.front().range});
                }
                parseRepeat(items);
            } else {
                parseField(items, annotations);
            }
        }
    }

    void parseStruct(const std::vector<DslAnnotation>& annotations) {
        const DslSourcePosition start = consume().range.start;
        DslStruct structure;
        structure.annotations = annotations;
        if (!expectIdentifier(&structure.name, QStringLiteral("structure name"))) {
            recoverDeclaration();
            return;
        }
        if (!expect(DslTokenKind::LeftBrace, QStringLiteral("'{' after structure name"))) {
            recoverDeclaration();
            return;
        }

        parseStructItems(structure.items);
        const bool closed = expect(DslTokenKind::RightBrace, QStringLiteral("'}' after fields"));
        match(DslTokenKind::Semicolon);
        structure.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        if (structure.items.empty() && closed) {
            result_.diagnostics.push_back({DslDiagnosticCode::EmptyStruct,
                                           QStringLiteral("A structure must contain at least one field"),
                                           structure.range});
        }
        result_.program.structs.push_back(std::move(structure));
    }

    void parseScan(const std::vector<DslAnnotation>& annotations) {
        const DslSourcePosition start = consume().range.start;
        DslProgressiveScan scan;
        scan.annotations = annotations;
        expect(DslTokenKind::Less, QStringLiteral("'<' after sequence"));
        expectIdentifier(&scan.elementType, QStringLiteral("sequence element type"));
        expect(DslTokenKind::Greater, QStringLiteral("'>' after sequence element type"));
        expectIdentifier(&scan.name, QStringLiteral("sequence name"));
        expect(DslTokenKind::Equals, QStringLiteral("'=' after sequence name"));
        if (!matchIdentifier(QStringLiteral("scan"))) {
            error(DslDiagnosticCode::UnexpectedToken, QStringLiteral("Expected scan(...) initializer"));
        }
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after scan"));
        expectIdentifier(&scan.scannerName, QStringLiteral("scanner name"));
        expect(DslTokenKind::RightParen, QStringLiteral("')' after scanner name"));
        expect(DslTokenKind::Semicolon, QStringLiteral("';' after sequence"));
        scan.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        result_.program.scans.push_back(std::move(scan));
    }

    void parseEntry(const std::vector<DslAnnotation>& annotations) {
        const DslSourcePosition start = consume().range.start;
        DslEntry entry;
        entry.annotations = annotations;
        expectIdentifier(&entry.targetName, QStringLiteral("entry target"));
        expect(DslTokenKind::Semicolon, QStringLiteral("';' after entry"));
        entry.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        if (result_.program.hasEntry) {
            result_.diagnostics.push_back({DslDiagnosticCode::DuplicateName,
                                           QStringLiteral("A DSL program may contain only one entry"),
                                           entry.range});
        } else {
            result_.program.entry = std::move(entry);
            result_.program.hasEntry = true;
        }
    }

    void recoverDeclaration() {
        while (!at(DslTokenKind::EndOfFile) && !match(DslTokenKind::Semicolon)) {
            consume();
        }
    }

    void recoverField() {
        while (!at(DslTokenKind::EndOfFile) && !at(DslTokenKind::RightBrace) &&
               !match(DslTokenKind::Semicolon)) {
            consume();
        }
    }

    void recoverSwitchArm() {
        while (!at(DslTokenKind::EndOfFile) && !at(DslTokenKind::RightBrace) &&
               !isIdentifier(QStringLiteral("case")) &&
               !isIdentifier(QStringLiteral("default"))) {
            consume();
        }
    }

    void recoverAnnotation() {
        while (!at(DslTokenKind::EndOfFile) && !at(DslTokenKind::RightParen) &&
               !at(DslTokenKind::Semicolon)) {
            consume();
        }
        match(DslTokenKind::RightParen);
    }

    void validatePresentationAnnotations(const std::vector<DslAnnotation>& annotations) {
        for (const DslAnnotation& annotation : annotations) {
            if (annotation.name == QStringLiteral("spec") &&
                (annotation.arguments.size() != 2 ||
                 annotation.arguments.at(0).kind != DslAnnotationValueKind::String ||
                 annotation.arguments.at(1).kind != DslAnnotationValueKind::String)) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidAnnotation,
                     QStringLiteral("@spec requires two string arguments"), annotation.range});
            }
            if (annotation.name == QStringLiteral("description") &&
                (annotation.arguments.size() != 1 ||
                 annotation.arguments.front().kind != DslAnnotationValueKind::String)) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidAnnotation,
                     QStringLiteral("@description requires one string argument"),
                     annotation.range});
            }
        }
    }

    void validateProgram() {
        for (std::size_t index = 0; index < result_.program.enums.size(); ++index) {
            const DslEnum& enumeration = result_.program.enums.at(index);
            validatePresentationAnnotations(enumeration.annotations);
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (enumeration.name == result_.program.enums.at(previous).name) {
                    result_.diagnostics.push_back({DslDiagnosticCode::DuplicateName,
                                                   QStringLiteral("Duplicate enum name"),
                                                   enumeration.range});
                    break;
                }
            }
            for (const DslStruct& structure : result_.program.structs) {
                if (enumeration.name == structure.name) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::DuplicateName,
                         QStringLiteral("Enum, structure, and sequence names must be unique"),
                         enumeration.range});
                    break;
                }
            }
            for (const DslProgressiveScan& scan : result_.program.scans) {
                if (enumeration.name == scan.name) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::DuplicateName,
                         QStringLiteral("Enum, structure, and sequence names must be unique"),
                         enumeration.range});
                    break;
                }
            }
            for (std::size_t valueIndex = 0; valueIndex < enumeration.values.size(); ++valueIndex) {
                const DslEnumValue& value = enumeration.values.at(valueIndex);
                for (std::size_t previous = 0; previous < valueIndex; ++previous) {
                    if (value.name == enumeration.values.at(previous).name) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::DuplicateName,
                             QStringLiteral("Duplicate enum member name"),
                             value.range});
                        break;
                    }
                }
            }
        }

        for (std::size_t index = 0; index < result_.program.structs.size(); ++index) {
            const DslStruct& structure = result_.program.structs.at(index);
            validatePresentationAnnotations(structure.annotations);
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (structure.name == result_.program.structs.at(previous).name) {
                    result_.diagnostics.push_back({DslDiagnosticCode::DuplicateName,
                                                   QStringLiteral("Duplicate structure name"),
                                                   structure.range});
                    break;
                }
            }
            for (const DslProgressiveScan& scan : result_.program.scans) {
                if (structure.name == scan.name) {
                    result_.diagnostics.push_back({DslDiagnosticCode::DuplicateName,
                                                   QStringLiteral("Structure and sequence names must be unique"),
                                                   structure.range});
                    break;
                }
            }
            for (const DslEnum& enumeration : result_.program.enums) {
                if (structure.name == enumeration.name) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::DuplicateName,
                         QStringLiteral("Enum, structure, and sequence names must be unique"),
                         structure.range});
                    break;
                }
            }
            struct ActiveCondition final {
                QString fieldName;
                quint64 expectedValue = 0;
                bool negated = false;
            };
            struct DeclaredField final {
                const DslBitField* field = nullptr;
                std::vector<ActiveCondition> conditions;
            };
            std::vector<QString> declaredFieldNames;
            std::vector<DeclaredField> declaredFields;
            const auto sameCondition = [](const ActiveCondition& left,
                                          const ActiveCondition& right) {
                return left.fieldName == right.fieldName &&
                       left.expectedValue == right.expectedValue &&
                       left.negated == right.negated;
            };
            const auto validateController = [&](const QString& fieldName,
                                                const DslSourceRange& range,
                                                const std::vector<ActiveCondition>& active,
                                                bool allowUnsignedExpGolomb)
                -> const DslBitField* {
                const auto found = std::find_if(
                    declaredFields.rbegin(),
                    declaredFields.rend(),
                    [&fieldName](const DeclaredField& declared) {
                        return declared.field->name == fieldName;
                    });
                if (found == declaredFields.rend()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::UnknownReference,
                         QStringLiteral(
                             "Controller field must be declared before the statement"),
                         range});
                    return nullptr;
                }
                const bool supportedEncoding =
                    found->field->encoding == DslFieldEncoding::Bits ||
                    (allowUnsignedExpGolomb &&
                     found->field->encoding == DslFieldEncoding::UnsignedExpGolomb);
                if (!supportedEncoding || found->field->arrayLength) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidType,
                         allowUnsignedExpGolomb
                             ? QStringLiteral(
                                   "Repeat counts require a previous scalar bits, enum, or ue "
                                   "field")
                             : QStringLiteral(
                                   "Controllers require a previous scalar bits or enum field"),
                         range});
                    return nullptr;
                }
                const bool available = std::all_of(
                    found->conditions.begin(),
                    found->conditions.end(),
                    [&active, &sameCondition](const ActiveCondition& required) {
                        return std::any_of(active.begin(),
                                           active.end(),
                                           [&required, &sameCondition](
                                               const ActiveCondition& candidate) {
                                               return sameCondition(required, candidate);
                                           });
                    });
                if (!available) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidCondition,
                         QStringLiteral(
                             "Controller field is not guaranteed on the current branch"),
                         range});
                    return nullptr;
                }
                return found->field;
            };
            const auto validateConditionValue = [&](const DslBitField* controller,
                                                    quint64 expectedValue,
                                                    const DslSourceRange& range) {
                if (controller != nullptr && controller->width != 0 &&
                    controller->width < 64 &&
                    expectedValue >= (quint64{1} << controller->width)) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::ConstraintOutOfRange,
                         QStringLiteral("Condition value does not fit the controlling field"),
                         range});
                }
            };
            const auto validateCondition = [&](const DslEqualityCondition& condition,
                                               const std::vector<ActiveCondition>& active) {
                const DslBitField* controller = validateController(
                    condition.fieldName, condition.range, active, false);
                validateConditionValue(
                    controller, condition.expectedValue, condition.range);
            };
            const auto validateFieldAnnotations = [&](const DslBitField& field) {
                validatePresentationAnnotations(field.annotations);
                bool equalsSeen = false;
                bool enumSeen = false;
                for (const DslAnnotation& annotation : field.annotations) {
                    if (annotation.name == QStringLiteral("enum")) {
                        if (enumSeen) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidAnnotation,
                                 QStringLiteral("@enum may appear at most once on a field"),
                                 annotation.range});
                        }
                        enumSeen = true;
                        if (field.encoding != DslFieldEncoding::Bits) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidAnnotation,
                                 QStringLiteral("@enum is only supported on bits fields"),
                                 annotation.range});
                            continue;
                        }
                        if (annotation.arguments.size() != 1 ||
                            annotation.arguments.front().kind !=
                                DslAnnotationValueKind::Identifier) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidAnnotation,
                                 QStringLiteral("@enum requires one enum type name"),
                                 annotation.range});
                            continue;
                        }
                        const QString& enumName = annotation.arguments.front().text;
                        const auto found = std::find_if(
                            result_.program.enums.begin(),
                            result_.program.enums.end(),
                            [&enumName](const DslEnum& enumeration) {
                                return enumeration.name == enumName;
                            });
                        if (found == result_.program.enums.end()) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::UnknownReference,
                                 QStringLiteral("Field enum type is not declared"),
                                 annotation.range});
                        } else if (field.width != 0 && field.width < 64) {
                            const quint64 exclusiveLimit = quint64{1} << field.width;
                            for (const DslEnumValue& value : found->values) {
                                if (value.value >= exclusiveLimit) {
                                    result_.diagnostics.push_back(
                                        {DslDiagnosticCode::EnumValueOutOfRange,
                                         QStringLiteral(
                                             "Enum member value does not fit the field width"),
                                         annotation.range});
                                    break;
                                }
                            }
                        }
                        continue;
                    }
                    if (annotation.name != QStringLiteral("equals")) {
                        continue;
                    }
                    if (equalsSeen) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidAnnotation,
                             QStringLiteral("@equals may appear at most once on a field"),
                             annotation.range});
                    }
                    equalsSeen = true;
                    if (field.encoding != DslFieldEncoding::Bits) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidAnnotation,
                             QStringLiteral("@equals is only supported on bits fields"),
                             annotation.range});
                        continue;
                    }
                    if (annotation.arguments.size() != 1 ||
                        annotation.arguments.front().kind != DslAnnotationValueKind::Integer) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidAnnotation,
                             QStringLiteral("@equals requires one integer argument"),
                             annotation.range});
                    }
                }
            };
            const auto containsField = [](const auto& self,
                                          const std::vector<DslStructItem>& items) -> bool {
                for (const DslStructItem& item : items) {
                    if (item.kind == DslStructItemKind::Field) {
                        return true;
                    }
                    if (item.kind == DslStructItemKind::Conditional &&
                        (self(self, item.thenItems) || self(self, item.elseItems))) {
                        return true;
                    }
                    if (item.kind == DslStructItemKind::Switch) {
                        for (const DslStructItem::SwitchArm& arm : item.switchArms) {
                            if (self(self, arm.items)) {
                                return true;
                            }
                        }
                    }
                    if (item.kind == DslStructItemKind::Repeat &&
                        self(self, item.repeatItems)) {
                        return true;
                    }
                }
                return false;
            };
            const auto validateItems = [&](const auto& self,
                                           const std::vector<DslStructItem>& items,
                                           const std::vector<ActiveCondition>& active,
                                           std::optional<quint64> fieldOffset)
                -> std::optional<quint64> {
                for (const DslStructItem& item : items) {
                    if (item.kind == DslStructItemKind::Conditional) {
                        validateCondition(item.condition, active);
                        std::vector<ActiveCondition> thenConditions = active;
                        thenConditions.push_back({item.condition.fieldName,
                                                  item.condition.expectedValue,
                                                  false});
                        const auto thenOffset =
                            self(self, item.thenItems, thenConditions, fieldOffset);
                        std::vector<ActiveCondition> elseConditions = active;
                        elseConditions.push_back({item.condition.fieldName,
                                                  item.condition.expectedValue,
                                                  true});
                        const auto elseOffset = item.elseItems.empty()
                                                    ? fieldOffset
                                                    : self(self,
                                                           item.elseItems,
                                                           elseConditions,
                                                           fieldOffset);
                        fieldOffset = thenOffset && elseOffset && *thenOffset == *elseOffset
                                          ? thenOffset
                                          : std::nullopt;
                        continue;
                    }
                    if (item.kind == DslStructItemKind::Switch) {
                        const DslBitField* controller = validateController(
                            item.switchFieldName, item.switchFieldRange, active, false);
                        std::vector<const DslStructItem::SwitchArm*> caseArms;
                        std::vector<quint64> caseValues;
                        bool defaultSeen = false;
                        for (std::size_t armIndex = 0;
                             armIndex < item.switchArms.size();
                             ++armIndex) {
                            const DslStructItem::SwitchArm& arm =
                                item.switchArms.at(armIndex);
                            if (arm.kind == DslSwitchArmKind::Default) {
                                if (defaultSeen) {
                                    result_.diagnostics.push_back(
                                        {DslDiagnosticCode::InvalidCondition,
                                         QStringLiteral(
                                             "A switch may contain at most one default arm"),
                                         arm.range});
                                }
                                defaultSeen = true;
                                if (armIndex + 1 != item.switchArms.size()) {
                                    result_.diagnostics.push_back(
                                        {DslDiagnosticCode::InvalidCondition,
                                         QStringLiteral(
                                             "The default switch arm must appear last"),
                                         arm.range});
                                }
                                continue;
                            }
                            if (defaultSeen) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::InvalidCondition,
                                     QStringLiteral(
                                         "Switch case arms may not follow the default arm"),
                                     arm.range});
                            }
                            if (std::find(caseValues.begin(),
                                          caseValues.end(),
                                          arm.caseValue) != caseValues.end()) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::InvalidCondition,
                                     QStringLiteral("Duplicate switch case value"),
                                     arm.valueRange});
                            }
                            caseValues.push_back(arm.caseValue);
                            caseArms.push_back(&arm);
                            validateConditionValue(
                                controller, arm.caseValue, arm.valueRange);
                        }
                        if (caseArms.empty()) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidCondition,
                                 QStringLiteral("A switch must contain at least one case arm"),
                                 item.range});
                        }

                        std::vector<std::optional<quint64>> armOffsets;
                        armOffsets.reserve(item.switchArms.size() +
                                           (defaultSeen ? 0 : 1));
                        for (const DslStructItem::SwitchArm& arm : item.switchArms) {
                            std::vector<ActiveCondition> armConditions = active;
                            if (arm.kind == DslSwitchArmKind::Case) {
                                armConditions.push_back(
                                    {item.switchFieldName, arm.caseValue, false});
                            } else {
                                for (const DslStructItem::SwitchArm* caseArm : caseArms) {
                                    armConditions.push_back(
                                        {item.switchFieldName, caseArm->caseValue, true});
                                }
                            }
                            armOffsets.push_back(
                                self(self, arm.items, armConditions, fieldOffset));
                        }
                        if (!defaultSeen) {
                            armOffsets.push_back(fieldOffset);
                        }
                        const auto firstKnown = std::find_if(
                            armOffsets.begin(),
                            armOffsets.end(),
                            [](const std::optional<quint64>& offset) {
                                return offset.has_value();
                            });
                        if (firstKnown == armOffsets.end() ||
                            std::any_of(
                                armOffsets.begin(),
                                armOffsets.end(),
                                [&firstKnown](const std::optional<quint64>& offset) {
                                    return !offset || *offset != **firstKnown;
                                })) {
                            fieldOffset = std::nullopt;
                        } else {
                            fieldOffset = *firstKnown;
                        }
                        continue;
                    }
                    if (item.kind == DslStructItemKind::Repeat) {
                        const DslBitField* controller = validateController(
                            item.repeatCountFieldName,
                            item.repeatCountFieldRange,
                            active,
                            true);
                        if (item.repeatMaximum == 0) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidArrayLength,
                                 QStringLiteral(
                                     "Bounded repeat maximum must be at least one"),
                                 item.repeatMaximumRange});
                        } else if (controller != nullptr &&
                                   controller->encoding == DslFieldEncoding::Bits &&
                                   controller->width != 0 && controller->width < 64 &&
                                   item.repeatMaximum >=
                                       (quint64{1} << controller->width)) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::ConstraintOutOfRange,
                                 QStringLiteral(
                                     "Repeat maximum does not fit the controlling field"),
                                 item.repeatMaximumRange});
                        }
                        if (!containsField(containsField, item.repeatItems)) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidCondition,
                                 QStringLiteral(
                                     "A bounded repeat body must contain at least one field"),
                                 item.range});
                        }
                        const std::size_t scopeStart = declaredFields.size();
                        (void)self(self, item.repeatItems, active, fieldOffset);
                        declaredFields.resize(scopeStart);
                        fieldOffset = std::nullopt;
                        continue;
                    }

                    const DslBitField& field = item.field;
                    validateFieldAnnotations(field);
                    if (std::find(declaredFieldNames.begin(),
                                  declaredFieldNames.end(),
                                  field.name) != declaredFieldNames.end()) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::DuplicateName,
                             QStringLiteral("Duplicate field name"),
                             field.range});
                    }
                    declaredFieldNames.push_back(field.name);
                    declaredFields.push_back({&field, active});
                    if (field.endian == DslEndian::Little &&
                        (!fieldOffset || *fieldOffset % 8 != 0)) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidEndian,
                             QStringLiteral(
                                 "Little-endian fields must begin at a byte boundary within the "
                                 "structure"),
                             field.range});
                    }
                    if (field.encoding != DslFieldEncoding::Bits || field.width == 0 ||
                        !fieldOffset || (field.arrayLength && *field.arrayLength == 0)) {
                        fieldOffset = std::nullopt;
                        continue;
                    }
                    const quint64 elementCount = field.arrayLength.value_or(1);
                    if (elementCount >
                        std::numeric_limits<quint64>::max() / field.width) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidArrayLength,
                             QStringLiteral("Fixed array bit width is too large"),
                             field.range});
                        fieldOffset = std::nullopt;
                        continue;
                    }
                    const quint64 totalWidth = elementCount * field.width;
                    if (*fieldOffset <=
                        std::numeric_limits<quint64>::max() - totalWidth) {
                        *fieldOffset += totalWidth;
                    } else {
                        fieldOffset = std::nullopt;
                    }
                }
                return fieldOffset;
            };
            (void)validateItems(validateItems, structure.items, {}, quint64(0));
            if (!structure.items.empty() && declaredFieldNames.empty()) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::EmptyStruct,
                     QStringLiteral("A structure must contain at least one field"),
                     structure.range});
            }
        }

        for (std::size_t index = 0; index < result_.program.scans.size(); ++index) {
            const DslProgressiveScan& scan = result_.program.scans.at(index);
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (scan.name == result_.program.scans.at(previous).name) {
                    result_.diagnostics.push_back({DslDiagnosticCode::DuplicateName,
                                                   QStringLiteral("Duplicate sequence name"),
                                                   scan.range});
                    break;
                }
            }
            for (const DslStruct& structure : result_.program.structs) {
                if (scan.name == structure.name) {
                    result_.diagnostics.push_back({DslDiagnosticCode::DuplicateName,
                                                   QStringLiteral("Structure and sequence names must be unique"),
                                                   scan.range});
                    break;
                }
            }
            for (const DslEnum& enumeration : result_.program.enums) {
                if (scan.name == enumeration.name) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::DuplicateName,
                         QStringLiteral("Enum, structure, and sequence names must be unique"),
                         scan.range});
                    break;
                }
            }
            bool elementFound = false;
            for (const DslStruct& structure : result_.program.structs) {
                elementFound = elementFound || scan.elementType == structure.name;
            }
            if (!elementFound) {
                result_.diagnostics.push_back({DslDiagnosticCode::UnknownReference,
                                               QStringLiteral("Sequence element type is not declared"),
                                               scan.range});
            }
            if (scan.scannerName != QStringLiteral("h264_start_code")) {
                result_.diagnostics.push_back({DslDiagnosticCode::UnsupportedScanner,
                                               QStringLiteral("Only h264_start_code is supported"),
                                               scan.range});
            }
            bool progressive = false;
            for (const DslAnnotation& annotation : scan.annotations) {
                if (annotation.name != QStringLiteral("index")) {
                    continue;
                }
                progressive = annotation.arguments.size() == 1 &&
                              annotation.arguments.front().kind ==
                                  DslAnnotationValueKind::Identifier &&
                              annotation.arguments.front().text == QStringLiteral("progressive");
            }
            if (!progressive) {
                result_.diagnostics.push_back({DslDiagnosticCode::InvalidProgressiveAnnotation,
                                               QStringLiteral("A scan requires @index(progressive)"),
                                               scan.range});
            }
        }

        if (!result_.program.hasEntry) {
            result_.diagnostics.push_back({DslDiagnosticCode::MissingEntry,
                                           QStringLiteral("A DSL program must declare one entry"),
                                           current().range});
            return;
        }
        bool entryFound = false;
        for (const DslStruct& structure : result_.program.structs) {
            entryFound = entryFound || result_.program.entry.targetName == structure.name;
        }
        for (const DslProgressiveScan& scan : result_.program.scans) {
            entryFound = entryFound || result_.program.entry.targetName == scan.name;
        }
        if (!entryFound) {
            result_.diagnostics.push_back({DslDiagnosticCode::UnknownReference,
                                           QStringLiteral("Entry target is not declared"),
                                           result_.program.entry.range});
        }
    }

    DslLexResult lexResult_;
    std::size_t index_ = 0;
    DslParseResult result_;
};

} // namespace

DslLexResult DslLexer::lex(const QString& source) { return Lexer(source).run(); }

DslParseResult DslParser::parse(const QString& source) { return Parser(source).run(); }

} // namespace streamview::rules
