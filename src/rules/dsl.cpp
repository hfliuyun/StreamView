#include <streamview/rules/dsl.h>

#include <QtGlobal>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace streamview::rules {

namespace {

[[nodiscard]] qint64 signedAnnotationBound(const DslAnnotationValue& argument) noexcept {
    const quint64 magnitude = argument.integerValue;
    return argument.negative ? -static_cast<qint64>(magnitude)
                             : static_cast<qint64>(magnitude);
}

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
            if (index_ + 1 < source_.size() &&
                source_.at(index_ + 1) == QLatin1Char('=')) {
                advance();
                advance();
                result_.tokens.push_back(
                    {DslTokenKind::LessEqual, QStringLiteral("<="), 0, {start, position()}});
                return;
            }
            punctuation(DslTokenKind::Less);
            return;
        case '>':
            if (index_ + 1 < source_.size() &&
                source_.at(index_ + 1) == QLatin1Char('=')) {
                advance();
                advance();
                result_.tokens.push_back(
                    {DslTokenKind::GreaterEqual, QStringLiteral(">="), 0, {start, position()}});
                return;
            }
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
        case '!':
            if (index_ + 1 < source_.size() &&
                source_.at(index_ + 1) == QLatin1Char('=')) {
                advance();
                advance();
                result_.tokens.push_back(
                    {DslTokenKind::BangEqual, QStringLiteral("!="), 0, {start, position()}});
                return;
            }
            punctuation(DslTokenKind::Bang);
            return;
        case '*':
            punctuation(DslTokenKind::Star);
            return;
        case '/':
            punctuation(DslTokenKind::Slash);
            return;
        case '%':
            punctuation(DslTokenKind::Percent);
            return;
        case '+':
            punctuation(DslTokenKind::Plus);
            return;
        case '-':
            punctuation(DslTokenKind::Minus);
            return;
        case '&':
            if (index_ + 1 < source_.size() &&
                source_.at(index_ + 1) == QLatin1Char('&')) {
                advance();
                advance();
                result_.tokens.push_back(
                    {DslTokenKind::AndAnd, QStringLiteral("&&"), 0, {start, position()}});
                return;
            }
            break;
        case '|':
            if (index_ + 1 < source_.size() &&
                source_.at(index_ + 1) == QLatin1Char('|')) {
                advance();
                advance();
                result_.tokens.push_back(
                    {DslTokenKind::OrOr, QStringLiteral("||"), 0, {start, position()}});
                return;
            }
            break;
        default:
            break;
        }
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
            if (isIdentifier(QStringLiteral("pure"))) {
                parsePureFunction(annotations);
            } else if (isIdentifier(QStringLiteral("enum"))) {
                parseEnum(annotations);
            } else if (isIdentifier(QStringLiteral("struct"))) {
                parseStruct(annotations);
            } else if (isIdentifier(QStringLiteral("sequence"))) {
                parseScan(annotations);
            } else if (isPayloadDispatchIntroducer()) {
                parsePayloadDispatch(annotations);
            } else if (isIdentifier(QStringLiteral("entry"))) {
                parseEntry(annotations);
            } else {
                error(DslDiagnosticCode::UnexpectedToken,
                      QStringLiteral(
                          "Expected pure, enum, struct, sequence, payload, or entry "
                          "declaration"));
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

    [[nodiscard]] bool isLazyRegionIntroducer() const {
        if (!at(DslTokenKind::At) || index_ + 2 >= lexResult_.tokens.size()) {
            return false;
        }
        const DslToken& name = lexResult_.tokens.at(index_ + 1);
        const DslToken& leftParen = lexResult_.tokens.at(index_ + 2);
        return name.kind == DslTokenKind::Identifier && name.lexeme == QStringLiteral("lazy") &&
               leftParen.kind == DslTokenKind::LeftParen;
    }

    [[nodiscard]] bool isPayloadDispatchIntroducer() const {
        if (!isIdentifier(QStringLiteral("payload")) ||
            index_ + 1 >= lexResult_.tokens.size()) {
            return false;
        }
        return lexResult_.tokens.at(index_ + 1).kind == DslTokenKind::Less;
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

    bool parseScalarType(DslScalarType* type, const QString& description) {
        if (matchIdentifier(QStringLiteral("bool"))) {
            *type = DslScalarType::Bool;
            return true;
        }
        if (matchIdentifier(QStringLiteral("u64"))) {
            *type = DslScalarType::U64;
            return true;
        }
        if (at(DslTokenKind::Identifier)) {
            const DslToken invalid = consume();
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidType,
                 QStringLiteral("Scalar types must be bool or u64"),
                 invalid.range});
            return false;
        }
        error(DslDiagnosticCode::MissingToken, QStringLiteral("Expected ") + description);
        return false;
    }

    [[nodiscard]] DslExpression makeBinaryExpression(DslExpression left,
                                                     DslBinaryOperator binaryOperator,
                                                     DslExpression right) const {
        DslExpression expression;
        expression.kind = DslExpressionKind::Binary;
        expression.binaryOperator = binaryOperator;
        expression.range = {left.range.start, right.range.end};
        expression.operands.push_back(std::move(left));
        expression.operands.push_back(std::move(right));
        return expression;
    }

    DslExpression parseNestedExpression() {
        if (expressionParseDepth_ >= 64) {
            const DslSourceRange range = current().range;
            error(DslDiagnosticCode::InvalidExpression,
                  QStringLiteral("Expression nesting exceeds 64 levels"));
            recoverExpression();
            DslExpression expression;
            expression.range = range;
            return expression;
        }
        ++expressionParseDepth_;
        DslExpression expression = parseExpression();
        --expressionParseDepth_;
        return expression;
    }

    DslExpression parseNestedUnaryExpression() {
        if (expressionParseDepth_ >= 64) {
            const DslSourceRange range = current().range;
            error(DslDiagnosticCode::InvalidExpression,
                  QStringLiteral("Expression nesting exceeds 64 levels"));
            recoverExpression();
            DslExpression expression;
            expression.range = range;
            return expression;
        }
        ++expressionParseDepth_;
        DslExpression expression = parseUnaryExpression();
        --expressionParseDepth_;
        return expression;
    }

    DslExpression parsePrimaryExpression() {
        if (at(DslTokenKind::IntegerLiteral)) {
            const DslToken literal = consume();
            DslExpression expression;
            expression.kind = DslExpressionKind::UnsignedLiteral;
            expression.unsignedValue = literal.integerValue;
            expression.range = literal.range;
            return expression;
        }
        if (at(DslTokenKind::Identifier)) {
            const DslToken identifier = consume();
            if (match(DslTokenKind::LeftParen)) {
                DslExpression expression;
                expression.kind = DslExpressionKind::Call;
                expression.name = identifier.lexeme;
                while (!at(DslTokenKind::RightParen) &&
                       !at(DslTokenKind::EndOfFile)) {
                    expression.operands.push_back(parseNestedExpression());
                    if (!match(DslTokenKind::Comma)) {
                        break;
                    }
                    if (at(DslTokenKind::RightParen)) {
                        error(DslDiagnosticCode::UnexpectedToken,
                              QStringLiteral("Expected expression after ','"));
                        break;
                    }
                }
                expect(DslTokenKind::RightParen,
                       QStringLiteral("')' after function arguments"));
                expression.range = {
                    identifier.range.start,
                    lexResult_.tokens.at(index_ - 1).range.end,
                };
                return expression;
            }
            DslExpression expression;
            if (identifier.lexeme == QStringLiteral("true") ||
                identifier.lexeme == QStringLiteral("false")) {
                expression.kind = DslExpressionKind::BooleanLiteral;
                expression.booleanValue = identifier.lexeme == QStringLiteral("true");
            } else {
                expression.kind = DslExpressionKind::Identifier;
                expression.name = identifier.lexeme;
            }
            expression.range = identifier.range;
            return expression;
        }
        if (match(DslTokenKind::LeftParen)) {
            const DslSourcePosition start = lexResult_.tokens.at(index_ - 1).range.start;
            DslExpression expression = parseNestedExpression();
            expect(DslTokenKind::RightParen, QStringLiteral("')' after expression"));
            expression.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
            return expression;
        }

        const DslSourceRange range = current().range;
        error(DslDiagnosticCode::UnexpectedToken, QStringLiteral("Expected expression"));
        if (!at(DslTokenKind::Comma) && !at(DslTokenKind::RightParen) &&
            !at(DslTokenKind::Semicolon) && !at(DslTokenKind::RightBrace) &&
            !at(DslTokenKind::EndOfFile)) {
            consume();
        }
        DslExpression expression;
        expression.range = range;
        return expression;
    }

    DslExpression parseUnaryExpression() {
        if (!match(DslTokenKind::Bang)) {
            return parsePrimaryExpression();
        }
        const DslSourcePosition start = lexResult_.tokens.at(index_ - 1).range.start;
        DslExpression expression;
        expression.kind = DslExpressionKind::Unary;
        expression.unaryOperator = DslUnaryOperator::LogicalNot;
        expression.operands.push_back(parseNestedUnaryExpression());
        expression.range = {start, expression.operands.front().range.end};
        return expression;
    }

    DslExpression parseMultiplicativeExpression() {
        DslExpression expression = parseUnaryExpression();
        while (at(DslTokenKind::Star) || at(DslTokenKind::Slash) ||
               at(DslTokenKind::Percent)) {
            const DslTokenKind operation = consume().kind;
            const DslBinaryOperator binaryOperator =
                operation == DslTokenKind::Star
                    ? DslBinaryOperator::Multiply
                    : operation == DslTokenKind::Slash ? DslBinaryOperator::Divide
                                                       : DslBinaryOperator::Remainder;
            expression = makeBinaryExpression(
                std::move(expression), binaryOperator, parseUnaryExpression());
        }
        return expression;
    }

    DslExpression parseAdditiveExpression() {
        DslExpression expression = parseMultiplicativeExpression();
        while (at(DslTokenKind::Plus) || at(DslTokenKind::Minus)) {
            const DslBinaryOperator binaryOperator =
                consume().kind == DslTokenKind::Plus ? DslBinaryOperator::Add
                                                     : DslBinaryOperator::Subtract;
            expression = makeBinaryExpression(
                std::move(expression), binaryOperator, parseMultiplicativeExpression());
        }
        return expression;
    }

    DslExpression parseRelationalExpression() {
        DslExpression expression = parseAdditiveExpression();
        while (at(DslTokenKind::Less) || at(DslTokenKind::LessEqual) ||
               at(DslTokenKind::Greater) || at(DslTokenKind::GreaterEqual)) {
            const DslTokenKind operation = consume().kind;
            DslBinaryOperator binaryOperator = DslBinaryOperator::Less;
            if (operation == DslTokenKind::LessEqual) {
                binaryOperator = DslBinaryOperator::LessEqual;
            } else if (operation == DslTokenKind::Greater) {
                binaryOperator = DslBinaryOperator::Greater;
            } else if (operation == DslTokenKind::GreaterEqual) {
                binaryOperator = DslBinaryOperator::GreaterEqual;
            }
            expression = makeBinaryExpression(
                std::move(expression), binaryOperator, parseAdditiveExpression());
        }
        return expression;
    }

    DslExpression parseEqualityExpression() {
        DslExpression expression = parseRelationalExpression();
        while (at(DslTokenKind::EqualEqual) || at(DslTokenKind::BangEqual)) {
            const DslBinaryOperator binaryOperator =
                consume().kind == DslTokenKind::EqualEqual ? DslBinaryOperator::Equal
                                                           : DslBinaryOperator::NotEqual;
            expression = makeBinaryExpression(
                std::move(expression), binaryOperator, parseRelationalExpression());
        }
        return expression;
    }

    DslExpression parseLogicalAndExpression() {
        DslExpression expression = parseEqualityExpression();
        while (match(DslTokenKind::AndAnd)) {
            expression = makeBinaryExpression(std::move(expression),
                                              DslBinaryOperator::LogicalAnd,
                                              parseEqualityExpression());
        }
        return expression;
    }

    DslExpression parseExpression() {
        DslExpression expression = parseLogicalAndExpression();
        while (match(DslTokenKind::OrOr)) {
            expression = makeBinaryExpression(std::move(expression),
                                              DslBinaryOperator::LogicalOr,
                                              parseLogicalAndExpression());
        }
        return expression;
    }

    std::vector<DslAnnotation> parseAnnotations(bool stopBeforeLazyRegion = false) {
        std::vector<DslAnnotation> annotations;
        while (at(DslTokenKind::At) && !(stopBeforeLazyRegion && isLazyRegionIntroducer())) {
            consume();
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
                        const bool negated =
                            at(DslTokenKind::Minus) &&
                            index_ + 1 < lexResult_.tokens.size() &&
                            lexResult_.tokens.at(index_ + 1).kind ==
                                DslTokenKind::IntegerLiteral;
                        if (negated) {
                            consume();
                        }
                        if (at(DslTokenKind::IntegerLiteral)) {
                            argument.kind = DslAnnotationValueKind::Integer;
                            argument.integerValue = consume().integerValue;
                            argument.negative = negated && argument.integerValue != 0;
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

    void parsePureFunction(const std::vector<DslAnnotation>& annotations) {
        const DslSourcePosition start = consume().range.start;
        if (!annotations.empty()) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidAnnotation,
                 QStringLiteral("Pure functions do not accept annotations"),
                 annotations.front().range});
        }

        DslPureFunction function;
        parseScalarType(&function.returnType, QStringLiteral("pure function return type"));
        if (!expectIdentifier(&function.name, QStringLiteral("pure function name"))) {
            recoverDeclaration();
            return;
        }
        if (!expect(DslTokenKind::LeftParen,
                    QStringLiteral("'(' after pure function name"))) {
            recoverDeclaration();
            return;
        }
        if (!at(DslTokenKind::RightParen)) {
            while (true) {
                const DslSourcePosition parameterStart = current().range.start;
                DslFunctionParameter parameter;
                parseScalarType(&parameter.type, QStringLiteral("parameter type"));
                if (!expectIdentifier(&parameter.name, QStringLiteral("parameter name"))) {
                    recoverExpression();
                }
                parameter.range = {
                    parameterStart,
                    lexResult_.tokens.at(index_ - 1).range.end,
                };
                function.parameters.push_back(std::move(parameter));
                if (!match(DslTokenKind::Comma)) {
                    break;
                }
                if (at(DslTokenKind::RightParen)) {
                    error(DslDiagnosticCode::UnexpectedToken,
                          QStringLiteral("Expected parameter after ','"));
                    break;
                }
            }
        }
        expect(DslTokenKind::RightParen,
               QStringLiteral("')' after pure function parameters"));
        if (!expect(DslTokenKind::LeftBrace,
                    QStringLiteral("'{' before pure function body"))) {
            recoverDeclaration();
            return;
        }
        if (!matchIdentifier(QStringLiteral("return"))) {
            error(DslDiagnosticCode::UnexpectedToken,
                  QStringLiteral("Pure function body must contain one return expression"));
            while (!at(DslTokenKind::EndOfFile) && !at(DslTokenKind::RightBrace)) {
                consume();
            }
            expect(DslTokenKind::RightBrace,
                   QStringLiteral("'}' after pure function body"));
            return;
        }
        function.expression = parseExpression();
        expect(DslTokenKind::Semicolon,
               QStringLiteral("';' after pure function return expression"));
        if (!at(DslTokenKind::RightBrace)) {
            error(DslDiagnosticCode::UnexpectedToken,
                  QStringLiteral("Pure function body allows only one return expression"));
            while (!at(DslTokenKind::EndOfFile) && !at(DslTokenKind::RightBrace)) {
                consume();
            }
        }
        expect(DslTokenKind::RightBrace, QStringLiteral("'}' after pure function body"));
        function.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        if (function.parameters.size() > 16) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidType,
                 QStringLiteral("Pure functions may declare at most 16 parameters"),
                 function.range});
        }
        result_.program.pureFunctions.push_back(std::move(function));
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
            DslExpression widthExpression = parseAdditiveExpression();
            if (widthExpression.kind == DslExpressionKind::UnsignedLiteral) {
                width = widthExpression.unsignedValue;
            } else {
                field.widthExpression = std::move(widthExpression);
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
        } else if (matchIdentifier(QStringLiteral("ff_coded"))) {
            field.encoding = DslFieldEncoding::FfCoded;
            expect(DslTokenKind::Less, QStringLiteral("'<' after ff_coded"));
            if (at(DslTokenKind::IntegerLiteral)) {
                const DslToken maxBytesToken = consume();
                field.maxBytes = maxBytesToken.integerValue;
                if (field.maxBytes < 1 || field.maxBytes > 64) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidBitWidth,
                         QStringLiteral("ff_coded maximum bytes must be between 1 and 64"),
                         maxBytesToken.range});
                }
            } else {
                error(DslDiagnosticCode::MissingToken,
                      QStringLiteral("Expected maximum byte count literal for ff_coded"));
            }
            expect(DslTokenKind::Greater, QStringLiteral("'>' after ff_coded maximum bytes"));
        } else {
            error(DslDiagnosticCode::UnexpectedToken,
                  QStringLiteral("Expected bits<N[, endian]>, ue, se, or ff_coded<N> field type"));
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
        if (field.encoding == DslFieldEncoding::Bits && field.width == 0 &&
            !field.widthExpression) {
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

    void parseComputedField(std::vector<DslStructItem>& items,
                            const std::vector<DslAnnotation>& fieldAnnotations) {
        const DslSourcePosition start = consume().range.start;
        DslComputedField field;
        field.annotations = fieldAnnotations;
        expect(DslTokenKind::Less, QStringLiteral("'<' after computed"));
        parseScalarType(&field.type, QStringLiteral("computed field type"));
        expect(DslTokenKind::Greater, QStringLiteral("'>' after computed field type"));
        if (!expectIdentifier(&field.name, QStringLiteral("computed field name"))) {
            recoverField();
            return;
        }
        if (match(DslTokenKind::LeftBracket)) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidArrayLength,
                 QStringLiteral("Computed fields cannot be arrays"),
                 lexResult_.tokens.at(index_ - 1).range});
            while (!at(DslTokenKind::EndOfFile) &&
                   !at(DslTokenKind::RightBracket) &&
                   !at(DslTokenKind::Semicolon) &&
                   !at(DslTokenKind::RightBrace)) {
                consume();
            }
            match(DslTokenKind::RightBracket);
        }
        expect(DslTokenKind::Equals, QStringLiteral("'=' before computed expression"));
        field.expression = parseExpression();
        if (!at(DslTokenKind::At) && !at(DslTokenKind::Semicolon)) {
            error(DslDiagnosticCode::UnexpectedToken,
                  QStringLiteral("Unexpected token after computed expression"));
            recoverExpression();
        }
        const std::vector<DslAnnotation> trailingAnnotations = parseAnnotations();
        field.annotations.insert(field.annotations.end(),
                                 trailingAnnotations.begin(),
                                 trailingAnnotations.end());
        expect(DslTokenKind::Semicolon, QStringLiteral("';' after computed field"));
        field.range = {start, lexResult_.tokens.at(index_ - 1).range.end};

        DslStructItem item;
        item.kind = DslStructItemKind::Computed;
        item.computed = std::move(field);
        item.range = item.computed.range;
        items.push_back(std::move(item));
    }

    void parseLazyRegion(std::vector<DslStructItem>& items) {
        const DslSourcePosition start = consume().range.start;
        consume();
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after @lazy"));

        DslLazyRegion region;
        region.byteCountExpression = parseExpression();
        expect(DslTokenKind::RightParen, QStringLiteral("')' after lazy byte count expression"));
        if (!matchIdentifier(QStringLiteral("bytes"))) {
            error(DslDiagnosticCode::UnexpectedToken,
                  QStringLiteral("Expected bytes after @lazy(...)"));
            recoverField();
            return;
        }
        if (!expectIdentifier(&region.name, QStringLiteral("lazy byte region name"))) {
            recoverField();
            return;
        }
        if (match(DslTokenKind::LeftBracket)) {
            result_.diagnostics.push_back({DslDiagnosticCode::InvalidArrayLength,
                                           QStringLiteral("Lazy byte regions cannot be arrays"),
                                           lexResult_.tokens.at(index_ - 1).range});
            while (!at(DslTokenKind::EndOfFile) && !at(DslTokenKind::RightBracket) &&
                   !at(DslTokenKind::Semicolon) && !at(DslTokenKind::RightBrace)) {
                consume();
            }
            match(DslTokenKind::RightBracket);
        }
        region.annotations = parseAnnotations();
        expect(DslTokenKind::Semicolon, QStringLiteral("';' after lazy byte region"));
        region.range = {start, lexResult_.tokens.at(index_ - 1).range.end};

        DslStructItem item;
        item.kind = DslStructItemKind::LazyRegion;
        item.lazyRegion = std::move(region);
        item.range = item.lazyRegion.range;
        items.push_back(std::move(item));
    }

    void parseConditional(std::vector<DslStructItem>& items) {
        const DslSourcePosition start = consume().range.start;
        DslStructItem item;
        item.kind = DslStructItemKind::Conditional;
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after if"));
        const DslSourcePosition conditionStart = current().range.start;
        if (at(DslTokenKind::Identifier) &&
            lexResult_.tokens.at(index_ + 1).kind == DslTokenKind::RightParen) {
            item.condition.fieldName = consume().lexeme;
            item.condition.booleanShorthand = true;
            item.condition.expectedValue = 1;
        } else {
            if (at(DslTokenKind::Identifier) &&
                lexResult_.tokens.at(index_ + 1).kind == DslTokenKind::Equals) {
                error(DslDiagnosticCode::MissingToken,
                      QStringLiteral("Conditions require the '==' operator"));
            }
            DslExpression expression = parseNestedExpression();
            if (expression.kind == DslExpressionKind::Binary &&
                expression.binaryOperator == DslBinaryOperator::Equal &&
                expression.operands.size() == 2 &&
                expression.operands.front().kind == DslExpressionKind::Identifier &&
                expression.operands.back().kind == DslExpressionKind::UnsignedLiteral) {
                item.condition.fieldName = expression.operands.front().name;
                item.condition.expectedValue = expression.operands.back().unsignedValue;
            } else {
                const bool importedEquality =
                    expression.kind == DslExpressionKind::Binary &&
                    expression.binaryOperator == DslBinaryOperator::Equal &&
                    expression.operands.size() == 2 &&
                    expression.operands.front().kind == DslExpressionKind::Call &&
                    (expression.operands.front().name ==
                         QStringLiteral("context_value") ||
                     expression.operands.front().name ==
                         QStringLiteral("header_value")) &&
                    expression.operands.back().kind == DslExpressionKind::UnsignedLiteral;
                if (!importedEquality) {
                    error(DslDiagnosticCode::MissingToken,
                          QStringLiteral(
                              "Conditions require a field or context_value equality"));
                }
                item.condition.expression = std::move(expression);
            }
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
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after repeat"));
        if (at(DslTokenKind::Identifier)) {
            item.kind = DslStructItemKind::Repeat;
            item.repeatCountFieldRange = current().range;
            item.repeatCountFieldName = consume().lexeme;
            expect(DslTokenKind::Comma,
                   QStringLiteral("',' after repeat count field name"));
        } else if (at(DslTokenKind::IntegerLiteral)) {
            item.kind = DslStructItemKind::SentinelRepeat;
        } else {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected repeat count field name or maximum"));
        }
        if (at(DslTokenKind::IntegerLiteral)) {
            const DslToken maximum = consume();
            item.repeatMaximum = maximum.integerValue;
            item.repeatMaximumRange = maximum.range;
        } else {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected repeat maximum count"));
        }
        expect(DslTokenKind::RightParen, QStringLiteral("')' after repeat maximum"));
        if (item.kind == DslStructItemKind::SentinelRepeat &&
            isIdentifier(QStringLiteral("while"))) {
            item.kind = DslStructItemKind::WhileRepeat;
            consume();
            expect(DslTokenKind::LeftParen, QStringLiteral("'(' after while"));
            if (!matchIdentifier(QStringLiteral("more_rbsp_data"))) {
                error(DslDiagnosticCode::InvalidCondition,
                      QStringLiteral("Expected 'more_rbsp_data' in while condition"));
            } else {
                expect(DslTokenKind::LeftParen,
                       QStringLiteral("'(' after more_rbsp_data"));
                expect(DslTokenKind::RightParen,
                       QStringLiteral("')' after more_rbsp_data"));
            }
            expect(DslTokenKind::RightParen,
                   QStringLiteral("')' after while condition"));
            if (expect(DslTokenKind::LeftBrace,
                       QStringLiteral("'{' after while condition"))) {
                parseStructItems(item.repeatItems);
                expect(DslTokenKind::RightBrace,
                       QStringLiteral("'}' after while repeat body"));
            } else {
                recoverField();
            }
            match(DslTokenKind::Semicolon);
        } else {
            if (expect(DslTokenKind::LeftBrace,
                       QStringLiteral("'{' after repeat header"))) {
                parseStructItems(item.repeatItems);
                expect(DslTokenKind::RightBrace,
                       QStringLiteral("'}' after repeat body"));
            } else {
                recoverField();
            }
            if (item.kind == DslStructItemKind::SentinelRepeat) {
                if (!isIdentifier(QStringLiteral("until"))) {
                    error(DslDiagnosticCode::MissingToken,
                          QStringLiteral(
                              "Expected until clause after sentinel repeat body"));
                } else {
                    consume();
                }
                expect(DslTokenKind::LeftParen, QStringLiteral("'(' after until"));
                if (at(DslTokenKind::Identifier)) {
                    item.sentinelFieldRange = current().range;
                    item.sentinelFieldName = consume().lexeme;
                } else {
                    error(DslDiagnosticCode::MissingToken,
                          QStringLiteral("Expected sentinel field name"));
                }
                expect(DslTokenKind::EqualEqual,
                       QStringLiteral("'==' after sentinel field name"));
                if (at(DslTokenKind::IntegerLiteral)) {
                    const DslToken value = consume();
                    item.sentinelValue = value.integerValue;
                    item.sentinelValueRange = value.range;
                } else {
                    error(DslDiagnosticCode::MissingToken,
                          QStringLiteral("Expected sentinel termination value"));
                }
                expect(DslTokenKind::RightParen,
                       QStringLiteral("')' after sentinel termination value"));
                expect(DslTokenKind::Semicolon,
                       QStringLiteral("';' after sentinel repeat"));
            }
        }
        item.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        items.push_back(std::move(item));
    }

    void parseRbspTrailingBits(std::vector<DslStructItem>& items,
                               const std::vector<DslAnnotation>& annotations) {
        const DslSourcePosition start = consume().range.start;
        if (!annotations.empty()) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidAnnotation,
                 QStringLiteral("rbsp_trailing_bits does not accept annotations"),
                 annotations.front().range});
        }
        DslStructItem item;
        item.kind = DslStructItemKind::RbspTrailingBits;
        item.rbspTrailingBits.annotations = annotations;
        expect(DslTokenKind::Semicolon, QStringLiteral("';' after rbsp_trailing_bits"));
        item.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        item.rbspTrailingBits.range = item.range;
        items.push_back(std::move(item));
    }

    void parseCompressedPayload(std::vector<DslStructItem>& items,
                                const std::vector<DslAnnotation>& leadingAnnotations) {
        const DslSourcePosition start = consume().range.start;
        if (!leadingAnnotations.empty()) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidAnnotation,
                 QStringLiteral("compressed_payload does not accept leading annotations"),
                 leadingAnnotations.front().range});
        }

        DslCompressedPayload payload;
        if (!expectIdentifier(&payload.name, QStringLiteral("compressed payload name"))) {
            recoverField();
        }
        if (match(DslTokenKind::LeftBracket)) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidArrayLength,
                 QStringLiteral("Compressed payloads cannot be arrays"),
                 lexResult_.tokens.at(index_ - 1).range});
            while (!at(DslTokenKind::EndOfFile) && !at(DslTokenKind::RightBracket) &&
                   !at(DslTokenKind::Semicolon) && !at(DslTokenKind::RightBrace)) {
                consume();
            }
            match(DslTokenKind::RightBracket);
        }
        payload.annotations = parseAnnotations();
        expect(DslTokenKind::Semicolon,
               QStringLiteral("';' after compressed payload"));
        payload.range = {start, lexResult_.tokens.at(index_ - 1).range.end};

        DslStructItem item;
        item.kind = DslStructItemKind::CompressedPayload;
        item.compressedPayload = std::move(payload);
        item.range = item.compressedPayload.range;
        items.push_back(std::move(item));
    }

    void parseAssertion(std::vector<DslStructItem>& items,
                        const std::vector<DslAnnotation>& leadingAnnotations) {
        const DslSourcePosition start = consume().range.start;
        if (!leadingAnnotations.empty()) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidAnnotation,
                 QStringLiteral("Annotations are not allowed before assertions"),
                 leadingAnnotations.front().range});
        }

        DslAssertion assertion;
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after assert"));
        assertion.condition = parseExpression();
        expect(DslTokenKind::RightParen, QStringLiteral("')' after assertion condition"));
        if (!matchIdentifier(QStringLiteral("at"))) {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected 'at' after assertion condition"));
        }
        if (at(DslTokenKind::Identifier)) {
            assertion.anchorFieldRange = current().range;
            assertion.anchorFieldName = consume().lexeme;
        } else {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected assertion anchor field name"));
        }
        expect(DslTokenKind::Semicolon, QStringLiteral("';' after assertion"));
        assertion.range = {start, lexResult_.tokens.at(index_ - 1).range.end};

        DslStructItem item;
        item.kind = DslStructItemKind::Assertion;
        item.assertion = std::move(assertion);
        item.range = item.assertion.range;
        items.push_back(std::move(item));
    }

    void parseUnsupported(std::vector<DslStructItem>& items,
                          const std::vector<DslAnnotation>& leadingAnnotations) {
        const DslSourcePosition start = consume().range.start;
        if (!leadingAnnotations.empty()) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidAnnotation,
                 QStringLiteral("Annotations are not allowed before unsupported statements"),
                 leadingAnnotations.front().range});
        }

        DslUnsupported unsupported;
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after unsupported"));
        if (at(DslTokenKind::StringLiteral)) {
            unsupported.reason = consume().lexeme;
        } else {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected a string reason for unsupported syntax"));
        }
        expect(DslTokenKind::RightParen, QStringLiteral("')' after unsupported reason"));
        if (!matchIdentifier(QStringLiteral("at"))) {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected 'at' after unsupported reason"));
        }
        if (at(DslTokenKind::Identifier)) {
            unsupported.anchorFieldRange = current().range;
            unsupported.anchorFieldName = consume().lexeme;
        } else {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected unsupported anchor field name"));
        }
        expect(DslTokenKind::Semicolon, QStringLiteral("';' after unsupported statement"));
        unsupported.range = {start, lexResult_.tokens.at(index_ - 1).range.end};

        DslStructItem item;
        item.kind = DslStructItemKind::Unsupported;
        item.unsupported = std::move(unsupported);
        item.range = item.unsupported.range;
        items.push_back(std::move(item));
    }

    void parseStructItems(std::vector<DslStructItem>& items) {
        while (!at(DslTokenKind::RightBrace) && !at(DslTokenKind::EndOfFile)) {
            const std::vector<DslAnnotation> annotations = parseAnnotations(true);
            if (isIdentifier(QStringLiteral("rbsp_trailing_bits"))) {
                parseRbspTrailingBits(items, annotations);
            } else if (isIdentifier(QStringLiteral("compressed_payload"))) {
                parseCompressedPayload(items, annotations);
            } else if (isIdentifier(QStringLiteral("assert"))) {
                parseAssertion(items, annotations);
            } else if (isIdentifier(QStringLiteral("unsupported"))) {
                parseUnsupported(items, annotations);
            } else if (isLazyRegionIntroducer()) {
                if (!annotations.empty()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidAnnotation,
                         QStringLiteral("Annotations are not allowed before @lazy"),
                         annotations.front().range});
                }
                parseLazyRegion(items);
            } else if (isIdentifier(QStringLiteral("if"))) {
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
            } else if (isIdentifier(QStringLiteral("computed"))) {
                parseComputedField(items, annotations);
            } else if (isIdentifier(QStringLiteral("bytes"))) {
                error(DslDiagnosticCode::UnexpectedToken,
                      QStringLiteral("bytes fields require immediately preceding @lazy(...)"));
                recoverField();
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
        quint32 trailingBitsCount = 0;
        quint32 compressedPayloadCount = 0;
        const auto validateTerminals = [&](const auto& self,
                                           const std::vector<DslStructItem>& items,
                                           bool topLevel,
                                           bool inBranch,
                                           bool repeatLocal) -> void {
            for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
                const DslStructItem& item = items.at(itemIndex);
                if (item.kind == DslStructItemKind::RbspTrailingBits) {
                    ++trailingBitsCount;
                    if ((!topLevel && !inBranch) || itemIndex + 1 != items.size()) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidRbspTrailingBits,
                             QStringLiteral(
                                 "rbsp_trailing_bits must occur as the final item of a branch or structure"),
                             item.range});
                    }
                    continue;
                }
                if (item.kind == DslStructItemKind::CompressedPayload) {
                    ++compressedPayloadCount;
                    if (!topLevel || itemIndex + 1 != items.size() ||
                        compressedPayloadCount > 1) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidCompressedPayload,
                             QStringLiteral(
                                 "compressed_payload must occur once as the final top-level item"),
                             item.range});
                    }
                    continue;
                }
                if (item.kind == DslStructItemKind::Assertion) {
                    if (!topLevel && !repeatLocal) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidCondition,
                             QStringLiteral(
                                 "Assertions must be top-level or repeat-local items"),
                             item.range});
                    }
                    continue;
                }
                if (item.kind == DslStructItemKind::Conditional) {
                    self(self, item.thenItems, false, true, repeatLocal);
                    self(self, item.elseItems, false, true, repeatLocal);
                } else if (item.kind == DslStructItemKind::Switch) {
                    for (const DslStructItem::SwitchArm& arm : item.switchArms) {
                        self(self, arm.items, false, true, repeatLocal);
                    }
                } else if (item.kind == DslStructItemKind::Repeat ||
                           item.kind == DslStructItemKind::SentinelRepeat ||
                           item.kind == DslStructItemKind::WhileRepeat) {
                    self(self, item.repeatItems, false, false, true);
                }
            }
        };
        validateTerminals(validateTerminals, structure.items, true, false, false);
        if (trailingBitsCount != 0 && compressedPayloadCount != 0) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidCompressedPayload,
                 QStringLiteral(
                     "compressed_payload and rbsp_trailing_bits are mutually exclusive"),
                 structure.range});
        }
        if (trailingBitsCount != 0) {
            for (const QString& reservedName : {QStringLiteral("rbsp_stop_one_bit"),
                                                QStringLiteral("rbsp_alignment_zero_bit")}) {
                if (declaresName(structure.items, reservedName)) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::DuplicateName,
                         QStringLiteral("rbsp_trailing_bits reserves the field name %1")
                             .arg(reservedName),
                         structure.range});
                }
            }
        }
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

    void recoverPayloadCase() {
        while (!at(DslTokenKind::EndOfFile) && !at(DslTokenKind::RightBrace) &&
               !isIdentifier(QStringLiteral("case")) &&
               !isIdentifier(QStringLiteral("default")) && !match(DslTokenKind::Semicolon)) {
            consume();
        }
    }

    void parsePayloadCase(DslPayloadDispatch& dispatch) {
        if (isIdentifier(QStringLiteral("default"))) {
            const DslSourceRange range = current().range;
            consume();
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidPayloadDispatch,
                 QStringLiteral("A payload dispatch may not declare a default arm"),
                 range});
            recoverPayloadCase();
            return;
        }
        const DslSourcePosition start = current().range.start;
        if (!matchIdentifier(QStringLiteral("case"))) {
            error(DslDiagnosticCode::UnexpectedToken,
                  QStringLiteral("Expected a payload dispatch case"));
            recoverPayloadCase();
            return;
        }
        if (!at(DslTokenKind::IntegerLiteral)) {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected an integer payload case value"));
            recoverPayloadCase();
            return;
        }
        DslPayloadCase payloadCase;
        const DslToken& value = consume();
        payloadCase.value = value.integerValue;
        payloadCase.valueRange = value.range;
        if (!expect(DslTokenKind::Colon, QStringLiteral("':' after a payload case value"))) {
            recoverPayloadCase();
            return;
        }
        if (matchIdentifier(QStringLiteral("empty"))) {
            payloadCase.kind = DslPayloadCaseKind::Empty;
        } else if (!expectIdentifier(&payloadCase.targetName,
                                     QStringLiteral("a payload case target structure"))) {
            recoverPayloadCase();
            return;
        }
        expect(DslTokenKind::Semicolon, QStringLiteral("';' after a payload case"));
        payloadCase.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        dispatch.cases.push_back(std::move(payloadCase));
    }

    void parsePayloadDispatch(const std::vector<DslAnnotation>& annotations) {
        const DslSourcePosition start = consume().range.start;
        DslPayloadDispatch dispatch;
        dispatch.annotations = annotations;
        validateAnnotations(dispatch.annotations, DslAnnotationTarget::PayloadDispatch);
        expect(DslTokenKind::Less, QStringLiteral("'<' after payload"));
        expectIdentifier(&dispatch.viewKind, QStringLiteral("payload view kind"));
        expect(DslTokenKind::Greater, QStringLiteral("'>' after payload view kind"));
        expectIdentifier(&dispatch.sequenceName, QStringLiteral("payload sequence name"));
        if (!matchIdentifier(QStringLiteral("switch"))) {
            error(DslDiagnosticCode::MissingToken,
                  QStringLiteral("Expected switch after the payload sequence name"));
        }
        expect(DslTokenKind::LeftParen, QStringLiteral("'(' after switch"));
        dispatch.controllerRange = current().range;
        expectIdentifier(&dispatch.controllerFieldName,
                         QStringLiteral("payload controller field"));
        expect(DslTokenKind::RightParen,
               QStringLiteral("')' after the payload controller field"));
        if (expect(DslTokenKind::LeftBrace,
                   QStringLiteral("'{' after the payload controller"))) {
            while (!at(DslTokenKind::RightBrace) && !at(DslTokenKind::EndOfFile)) {
                parsePayloadCase(dispatch);
            }
            expect(DslTokenKind::RightBrace, QStringLiteral("'}' after the payload cases"));
            match(DslTokenKind::Semicolon);
        } else {
            recoverDeclaration();
        }
        dispatch.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        if (result_.program.payloadDispatch) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::DuplicateName,
                 QStringLiteral("A DSL program may contain only one payload dispatch"),
                 dispatch.range});
            return;
        }
        result_.program.payloadDispatch = std::move(dispatch);
    }

    void parseEntry(const std::vector<DslAnnotation>& annotations) {
        const DslSourcePosition start = consume().range.start;
        if (!annotations.empty()) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidAnnotation,
                 QStringLiteral("Entry declarations do not accept annotations"),
                 annotations.front().range});
        }
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

    void recoverExpression() {
        while (!at(DslTokenKind::EndOfFile) && !at(DslTokenKind::Comma) &&
               !at(DslTokenKind::RightParen) && !at(DslTokenKind::Semicolon) &&
               !at(DslTokenKind::RightBrace)) {
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

    enum class DslAnnotationTarget : quint32 {
        BitField          = 1U << 0,
        ComputedField     = 1U << 1,
        LazyRegion        = 1U << 2,
        CompressedPayload = 1U << 3,
        Struct            = 1U << 4,
        Enum              = 1U << 5,
        Scan              = 1U << 6,
        PayloadDispatch   = 1U << 7,
    };

    struct DslAnnotationDescriptor {
        QStringView name;
        quint32 allowedTargets;
    };

    static constexpr DslAnnotationDescriptor knownAnnotations[] = {
        {u"spec",
         static_cast<quint32>(DslAnnotationTarget::BitField) |
             static_cast<quint32>(DslAnnotationTarget::ComputedField) |
             static_cast<quint32>(DslAnnotationTarget::LazyRegion) |
             static_cast<quint32>(DslAnnotationTarget::CompressedPayload) |
             static_cast<quint32>(DslAnnotationTarget::Struct) |
             static_cast<quint32>(DslAnnotationTarget::Enum) |
             static_cast<quint32>(DslAnnotationTarget::Scan) |
             static_cast<quint32>(DslAnnotationTarget::PayloadDispatch)},
        {u"description",
         static_cast<quint32>(DslAnnotationTarget::BitField) |
             static_cast<quint32>(DslAnnotationTarget::ComputedField) |
             static_cast<quint32>(DslAnnotationTarget::LazyRegion) |
             static_cast<quint32>(DslAnnotationTarget::CompressedPayload) |
             static_cast<quint32>(DslAnnotationTarget::Struct) |
             static_cast<quint32>(DslAnnotationTarget::Enum) |
             static_cast<quint32>(DslAnnotationTarget::Scan) |
             static_cast<quint32>(DslAnnotationTarget::PayloadDispatch)},
        {u"equals", static_cast<quint32>(DslAnnotationTarget::BitField)},
        {u"range", static_cast<quint32>(DslAnnotationTarget::BitField)},
        {u"enum", static_cast<quint32>(DslAnnotationTarget::BitField)},
        {u"lazy", 0U}, // @lazy is a dedicated field introducer, not a general annotation
        {u"index", static_cast<quint32>(DslAnnotationTarget::Scan)},
        {u"context", static_cast<quint32>(DslAnnotationTarget::Struct)},
        {u"context_export",
         static_cast<quint32>(DslAnnotationTarget::BitField) |
             static_cast<quint32>(DslAnnotationTarget::ComputedField)},
        {u"context_import", static_cast<quint32>(DslAnnotationTarget::Struct)},
        {u"context_dependency", static_cast<quint32>(DslAnnotationTarget::Struct)},
        {u"container", static_cast<quint32>(DslAnnotationTarget::LazyRegion)},
        {u"target_format", static_cast<quint32>(DslAnnotationTarget::LazyRegion)},
        {u"window", static_cast<quint32>(DslAnnotationTarget::LazyRegion)},
    };

    void validateAnnotations(const std::vector<DslAnnotation>& annotations,
                             DslAnnotationTarget host) {
        for (const DslAnnotation& annotation : annotations) {
            const auto it = std::find_if(
                std::begin(knownAnnotations),
                std::end(knownAnnotations),
                [&](const DslAnnotationDescriptor& desc) {
                    return desc.name == annotation.name;
                });
            if (it == std::end(knownAnnotations)) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidAnnotation,
                     QStringLiteral("Unknown annotation '@%1'").arg(annotation.name),
                     annotation.range});
                continue;
            }
            const bool isAllowed = (it->allowedTargets & static_cast<quint32>(host)) != 0;
            if (!isAllowed) {
                if (host == DslAnnotationTarget::ComputedField) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidAnnotation,
                         QStringLiteral(
                             "Computed fields accept only @description, @spec, and @context_export"),
                         annotation.range});
                } else if (host == DslAnnotationTarget::LazyRegion) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidAnnotation,
                         QStringLiteral("Lazy byte regions accept only @description, @spec, @container, @target_format, and @window"),
                         annotation.range});
                } else if (host == DslAnnotationTarget::CompressedPayload) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidAnnotation,
                         QStringLiteral(
                             "Compressed payloads accept only @description and @spec"),
                         annotation.range});
                } else {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidAnnotation,
                         QStringLiteral("@%1 is not supported on this declaration")
                             .arg(annotation.name),
                         annotation.range});
                }
                continue;
            }

            if (annotation.name == QStringLiteral("spec") &&
                (annotation.arguments.size() != 2 ||
                 annotation.arguments.at(0).kind != DslAnnotationValueKind::String ||
                 annotation.arguments.at(1).kind != DslAnnotationValueKind::String)) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidAnnotation,
                     QStringLiteral("@spec requires two string arguments"),
                     annotation.range});
            }
            if (annotation.name == QStringLiteral("description") &&
                (annotation.arguments.size() != 1 ||
                 annotation.arguments.front().kind != DslAnnotationValueKind::String)) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidAnnotation,
                     QStringLiteral("@description requires one string argument"),
                     annotation.range});
            }
            if (annotation.name == QStringLiteral("container") &&
                (annotation.arguments.size() != 1 ||
                 annotation.arguments.front().kind != DslAnnotationValueKind::Identifier)) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidAnnotation,
                     QStringLiteral("@container requires one struct identifier argument"),
                     annotation.range});
            }
            if (annotation.name == QStringLiteral("target_format") &&
                (annotation.arguments.size() != 1 ||
                 annotation.arguments.front().kind != DslAnnotationValueKind::String ||
                 annotation.arguments.front().text.isEmpty())) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidAnnotation,
                     QStringLiteral("@target_format requires one non-empty string argument"),
                     annotation.range});
            }
            if (annotation.name == QStringLiteral("window") &&
                (annotation.arguments.size() != 2 ||
                 annotation.arguments.at(0).kind != DslAnnotationValueKind::Identifier ||
                 annotation.arguments.at(1).kind != DslAnnotationValueKind::Identifier)) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidAnnotation,
                     QStringLiteral("@window requires two identifier arguments: entry struct and count field"),
                     annotation.range});
            }
        }
    }

    void validateProgram() {
        // An empty resolver rejects optional_value, which is how a
        // pure-function body excludes the form. See ADR-0066.
        using OptionalDependencyResolver = std::function<std::optional<DslScalarType>(
            const QString&, const DslSourceRange&)>;
        const auto validateExpression = [&](const auto& self,
                                            const DslExpression& expression,
                                            const auto& resolveIdentifier,
                                            std::size_t availableFunctionCount,
                                            bool allowImportedContextReference,
                                            bool allowSourceStateReference,
                                            const OptionalDependencyResolver&
                                                resolveOptionalDependency,
                                            std::size_t depth,
                                            std::size_t& nodeCount)
            -> std::optional<DslScalarType> {
            ++nodeCount;
            if (nodeCount > 256) {
                if (nodeCount == 257) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidExpression,
                         QStringLiteral("Expressions may contain at most 256 nodes"),
                         expression.range});
                }
                return std::nullopt;
            }
            if (depth > 64) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidExpression,
                     QStringLiteral("Expressions may have depth at most 64"),
                     expression.range});
                return std::nullopt;
            }

            switch (expression.kind) {
            case DslExpressionKind::UnsignedLiteral:
                return DslScalarType::U64;
            case DslExpressionKind::BooleanLiteral:
                return DslScalarType::Bool;
            case DslExpressionKind::Identifier:
                return resolveIdentifier(expression.name, expression.range);
            case DslExpressionKind::Call: {
                if (expression.name == QStringLiteral("power_of_two")) {
                    if (expression.operands.size() != 1) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidExpression,
                             QStringLiteral(
                                 "power_of_two requires one unsigned exponent"),
                             expression.range});
                        return std::nullopt;
                    }
                    const auto exponentType = self(self,
                                                   expression.operands.front(),
                                                   resolveIdentifier,
                                                   availableFunctionCount,
                                                   allowImportedContextReference,
                                                   allowSourceStateReference,
                                                   resolveOptionalDependency,
                                                   depth + 1,
                                                   nodeCount);
                    if (!exponentType) {
                        return std::nullopt;
                    }
                    if (*exponentType != DslScalarType::U64) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidType,
                             QStringLiteral("power_of_two exponent must be unsigned"),
                             expression.operands.front().range});
                        return std::nullopt;
                    }
                    return DslScalarType::U64;
                }
                if (expression.name == QStringLiteral("more_rbsp_data")) {
                    if (!allowSourceStateReference) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::UnknownReference,
                             QStringLiteral(
                                 "more_rbsp_data is unavailable in pure functions"),
                             expression.range});
                        return std::nullopt;
                    }
                    if (!expression.operands.empty()) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidExpression,
                             QStringLiteral("more_rbsp_data requires no arguments"),
                             expression.range});
                        return std::nullopt;
                    }
                    return DslScalarType::Bool;
                }
                if (expression.name == QStringLiteral("byte_aligned")) {
                    if (!allowSourceStateReference) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::UnknownReference,
                             QStringLiteral(
                                 "byte_aligned is unavailable in pure functions"),
                             expression.range});
                        return std::nullopt;
                    }
                    if (!expression.operands.empty()) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidExpression,
                             QStringLiteral("byte_aligned requires no arguments"),
                             expression.range});
                        return std::nullopt;
                    }
                    return DslScalarType::Bool;
                }
                if (expression.name == QStringLiteral("available_bytes")) {
                    if (!allowSourceStateReference) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::UnknownReference,
                             QStringLiteral(
                                 "available_bytes is unavailable in pure functions"),
                             expression.range});
                        return std::nullopt;
                    }
                    if (!expression.operands.empty()) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidExpression,
                             QStringLiteral("available_bytes requires no arguments"),
                             expression.range});
                        return std::nullopt;
                    }
                    return DslScalarType::U64;
                }
                if (allowImportedContextReference &&
                    expression.name == QStringLiteral("context_value")) {
                    if ((expression.operands.size() != 2 && expression.operands.size() != 3) ||
                        std::any_of(expression.operands.begin(),
                                    expression.operands.end(),
                                    [](const DslExpression& operand) {
                                        return operand.kind !=
                                               DslExpressionKind::Identifier;
                                    })) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidContext,
                             QStringLiteral(
                                 "context_value requires two or three identifier arguments"),
                             expression.range});
                        return std::nullopt;
                    }
                    return DslScalarType::U64;
                }
                if (allowImportedContextReference &&
                    expression.name == QStringLiteral("header_value")) {
                    if (expression.operands.size() != 1 ||
                        expression.operands.front().kind !=
                            DslExpressionKind::Identifier) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidContext,
                             QStringLiteral(
                                 "header_value requires one identifier argument"),
                             expression.range});
                        return std::nullopt;
                    }
                    return DslScalarType::U64;
                }
                if (resolveOptionalDependency &&
                    expression.name == QStringLiteral("optional_value")) {
                    if (expression.operands.size() != 2 ||
                        expression.operands.front().kind !=
                            DslExpressionKind::Identifier) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidExpression,
                             QStringLiteral("optional_value requires an identifier "
                                            "and a fallback expression"),
                             expression.range});
                        return std::nullopt;
                    }
                    // The named field is exempt from the branch-guarantee rule.
                    // The fallback is an ordinary expression and keeps it.
                    const auto fieldType =
                        resolveOptionalDependency(expression.operands.front().name,
                                                  expression.operands.front().range);
                    const auto fallbackType = self(self,
                                                   expression.operands.at(1),
                                                   resolveIdentifier,
                                                   availableFunctionCount,
                                                   allowImportedContextReference,
                                                   allowSourceStateReference,
                                                   resolveOptionalDependency,
                                                   depth + 1,
                                                   nodeCount);
                    if (!fieldType || !fallbackType) {
                        return std::nullopt;
                    }
                    if (*fallbackType != DslScalarType::U64) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidType,
                             QStringLiteral(
                                 "optional_value requires an unsigned fallback"),
                             expression.operands.at(1).range});
                        return std::nullopt;
                    }
                    return DslScalarType::U64;
                }
                const auto functionsEnd = result_.program.pureFunctions.begin() +
                                          static_cast<std::ptrdiff_t>(availableFunctionCount);
                const auto found = std::find_if(
                    result_.program.pureFunctions.begin(),
                    functionsEnd,
                    [&expression](const DslPureFunction& function) {
                        return function.name == expression.name;
                    });
                std::vector<std::optional<DslScalarType>> argumentTypes;
                argumentTypes.reserve(expression.operands.size());
                for (const DslExpression& argument : expression.operands) {
                    argumentTypes.push_back(self(self,
                                                 argument,
                                                 resolveIdentifier,
                                                 availableFunctionCount,
                                                 allowImportedContextReference,
                                                 allowSourceStateReference,
                                                 resolveOptionalDependency,
                                                 depth + 1,
                                                 nodeCount));
                }
                if (found == functionsEnd) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::UnknownReference,
                         QStringLiteral("Pure function is not declared before this call"),
                         expression.range});
                    return std::nullopt;
                }
                if (found->parameters.size() != argumentTypes.size()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidType,
                         QStringLiteral("Pure function argument count does not match"),
                         expression.range});
                    return found->returnType;
                }
                for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
                    if (argumentTypes.at(index) &&
                        *argumentTypes.at(index) != found->parameters.at(index).type) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidType,
                             QStringLiteral("Pure function argument type does not match"),
                             expression.operands.at(index).range});
                    }
                }
                return found->returnType;
            }
            case DslExpressionKind::Unary: {
                if (expression.operands.size() != 1) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidExpression,
                         QStringLiteral("Unary expressions require one operand"),
                         expression.range});
                    return std::nullopt;
                }
                const auto operandType = self(self,
                                              expression.operands.front(),
                                              resolveIdentifier,
                                              availableFunctionCount,
                                              allowImportedContextReference,
                                              allowSourceStateReference,
                                              resolveOptionalDependency,
                                              depth + 1,
                                              nodeCount);
                if (operandType && *operandType != DslScalarType::Bool) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidType,
                         QStringLiteral("Logical negation requires a bool operand"),
                         expression.range});
                }
                return DslScalarType::Bool;
            }
            case DslExpressionKind::Binary:
                break;
            }

            if (expression.operands.size() != 2) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidExpression,
                     QStringLiteral("Binary expressions require two operands"),
                     expression.range});
                return std::nullopt;
            }
            const auto leftType = self(self,
                                       expression.operands.at(0),
                                       resolveIdentifier,
                                       availableFunctionCount,
                                       allowImportedContextReference,
                                       allowSourceStateReference,
                                       resolveOptionalDependency,
                                       depth + 1,
                                       nodeCount);
            const auto rightType = self(self,
                                        expression.operands.at(1),
                                        resolveIdentifier,
                                        availableFunctionCount,
                                        allowImportedContextReference,
                                        allowSourceStateReference,
                                        resolveOptionalDependency,
                                        depth + 1,
                                        nodeCount);
            const auto requireOperands = [&](DslScalarType required,
                                             const QString& message) {
                if ((leftType && *leftType != required) ||
                    (rightType && *rightType != required)) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidType, message, expression.range});
                }
            };
            switch (expression.binaryOperator) {
            case DslBinaryOperator::Add:
            case DslBinaryOperator::Multiply:
                if ((leftType && *leftType != DslScalarType::U64 && *leftType != DslScalarType::Bool) ||
                    (rightType && *rightType != DslScalarType::U64 && *rightType != DslScalarType::Bool)) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidType,
                         QStringLiteral("Add and multiply operators require u64 or bool operands"),
                         expression.range});
                }
                return DslScalarType::U64;
            case DslBinaryOperator::Divide:
            case DslBinaryOperator::Remainder:
            case DslBinaryOperator::Subtract:
                requireOperands(DslScalarType::U64,
                                QStringLiteral("Arithmetic operators require u64 operands"));
                return DslScalarType::U64;
            case DslBinaryOperator::Equal:
            case DslBinaryOperator::NotEqual:
                if (leftType && rightType && *leftType != *rightType) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidType,
                         QStringLiteral("Equality operands must have the same type"),
                         expression.range});
                }
                return DslScalarType::Bool;
            case DslBinaryOperator::Less:
            case DslBinaryOperator::LessEqual:
            case DslBinaryOperator::Greater:
            case DslBinaryOperator::GreaterEqual:
                requireOperands(DslScalarType::U64,
                                QStringLiteral("Ordering operators require u64 operands"));
                return DslScalarType::Bool;
            case DslBinaryOperator::LogicalAnd:
            case DslBinaryOperator::LogicalOr:
                requireOperands(DslScalarType::Bool,
                                QStringLiteral("Logical operators require bool operands"));
                return DslScalarType::Bool;
            }
            return std::nullopt;
        };

        for (std::size_t index = 0; index < result_.program.pureFunctions.size(); ++index) {
            const DslPureFunction& function = result_.program.pureFunctions.at(index);
            const bool duplicateFunction = std::any_of(
                result_.program.pureFunctions.begin(),
                result_.program.pureFunctions.begin() + static_cast<std::ptrdiff_t>(index),
                [&function](const DslPureFunction& previous) {
                    return previous.name == function.name;
                });
            const bool conflictsWithDeclaration =
                std::any_of(result_.program.enums.begin(),
                            result_.program.enums.end(),
                            [&function](const DslEnum& enumeration) {
                                return enumeration.name == function.name;
                            }) ||
                std::any_of(result_.program.structs.begin(),
                            result_.program.structs.end(),
                            [&function](const DslStruct& structure) {
                                return structure.name == function.name;
                            }) ||
                std::any_of(result_.program.scans.begin(),
                            result_.program.scans.end(),
                            [&function](const DslProgressiveScan& scan) {
                                return scan.name == function.name;
                            });
            const bool conflictsWithReservedExpression =
                function.name == QStringLiteral("power_of_two") ||
                function.name == QStringLiteral("more_rbsp_data") ||
                function.name == QStringLiteral("byte_aligned") ||
                function.name == QStringLiteral("available_bytes");
            if (duplicateFunction || conflictsWithDeclaration ||
                conflictsWithReservedExpression) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::DuplicateName,
                     conflictsWithReservedExpression
                         ? QStringLiteral("%1 is a reserved expression name")
                               .arg(function.name)
                         : QStringLiteral(
                               "Pure function names share the top-level namespace"),
                     function.range});
            }
            for (std::size_t parameterIndex = 0;
                 parameterIndex < function.parameters.size();
                 ++parameterIndex) {
                const DslFunctionParameter& parameter =
                    function.parameters.at(parameterIndex);
                const bool duplicateParameter = std::any_of(
                    function.parameters.begin(),
                    function.parameters.begin() +
                        static_cast<std::ptrdiff_t>(parameterIndex),
                    [&parameter](const DslFunctionParameter& previous) {
                        return previous.name == parameter.name;
                    });
                if (duplicateParameter) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::DuplicateName,
                         QStringLiteral("Pure function parameter names must be unique"),
                         parameter.range});
                }
            }
            const auto resolveParameter = [&](const QString& name,
                                              const DslSourceRange& range)
                -> std::optional<DslScalarType> {
                const auto found = std::find_if(
                    function.parameters.begin(),
                    function.parameters.end(),
                    [&name](const DslFunctionParameter& parameter) {
                        return parameter.name == name;
                    });
                if (found == function.parameters.end()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::UnknownReference,
                         QStringLiteral("Pure function expressions may reference only parameters"),
                         range});
                    return std::nullopt;
                }
                return found->type;
            };
            std::size_t nodeCount = 0;
            const auto expressionType = validateExpression(validateExpression,
                                                           function.expression,
                                                           resolveParameter,
                                                           index,
                                                           false,
                                                           false,
                                                           OptionalDependencyResolver{},
                                                           1,
                                                           nodeCount);
            if (expressionType && *expressionType != function.returnType) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidType,
                     QStringLiteral("Pure function return expression type does not match"),
                     function.expression.range});
            }
        }

        for (std::size_t index = 0; index < result_.program.enums.size(); ++index) {
            const DslEnum& enumeration = result_.program.enums.at(index);
            validateAnnotations(enumeration.annotations, DslAnnotationTarget::Enum);
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
            validateAnnotations(structure.annotations, DslAnnotationTarget::Struct);
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
                QString name;
                const DslBitField* syntax = nullptr;
                const DslComputedField* computed = nullptr;
                DslScalarType type = DslScalarType::U64;
                std::vector<ActiveCondition> conditions;
            };
            enum class ControllerUse : quint8 {
                Equality,
                Repeat,
                Boolean,
            };
            std::vector<QString> declaredFieldNames;
            std::vector<DeclaredField> declaredFields;
            const auto sameCondition = [](const ActiveCondition& left,
                                          const ActiveCondition& right) {
                return left.fieldName == right.fieldName &&
                       left.expectedValue == right.expectedValue &&
                       left.negated == right.negated;
            };
            // Resolves the first argument of optional_value. It applies every
            // dependency rule except the branch-guarantee check, which is the
            // one restriction the form exists to lift. See ADR-0066.
            const auto resolveOptionalDependency =
                [&](const QString& name,
                    const DslSourceRange& range) -> std::optional<DslScalarType> {
                const auto found = std::find_if(
                    declaredFields.rbegin(),
                    declaredFields.rend(),
                    [&name](const DeclaredField& declared) {
                        return declared.name == name;
                    });
                if (found == declaredFields.rend()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::UnknownReference,
                         QStringLiteral(
                             "Optional field dependency must be declared earlier"),
                         range});
                    return std::nullopt;
                }
                if (found->computed != nullptr) {
                    if (found->type != DslScalarType::U64) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidType,
                             QStringLiteral("Optional field values require scalar "
                                            "unsigned fields"),
                             range});
                        return std::nullopt;
                    }
                    return DslScalarType::U64;
                }
                if (found->syntax == nullptr || found->syntax->arrayLength ||
                    found->syntax->encoding == DslFieldEncoding::SignedExpGolomb) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidType,
                         QStringLiteral(
                             "Optional field values require scalar unsigned fields"),
                         range});
                    return std::nullopt;
                }
                return DslScalarType::U64;
            };
            const auto validateController = [&](const QString& fieldName,
                                                const DslSourceRange& range,
                                                const std::vector<ActiveCondition>& active,
                                                ControllerUse use)
                -> const DeclaredField* {
                const auto found = std::find_if(
                    declaredFields.rbegin(),
                    declaredFields.rend(),
                    [&fieldName](const DeclaredField& declared) {
                        return declared.name == fieldName;
                    });
                if (found == declaredFields.rend()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::UnknownReference,
                         QStringLiteral(
                             "Controller field must be declared before the statement"),
                         range});
                    return nullptr;
                }
                const bool syntaxScalar = found->syntax != nullptr &&
                                          !found->syntax->arrayLength;
                const bool supported =
                    use == ControllerUse::Boolean
                        ? found->computed != nullptr &&
                              found->type == DslScalarType::Bool
                        : found->computed != nullptr
                              ? found->type == DslScalarType::U64
                              : syntaxScalar &&
                                    (found->syntax->encoding == DslFieldEncoding::Bits ||
                                     found->syntax->encoding ==
                                         DslFieldEncoding::UnsignedExpGolomb ||
                                     found->syntax->encoding ==
                                         DslFieldEncoding::FfCoded);
                if (!supported) {
                    QString message = QStringLiteral(
                        "Controllers require a previous scalar bits, enum, ue, or "
                        "computed<u64> field");
                    if (use == ControllerUse::Repeat) {
                        message = QStringLiteral(
                            "Repeat counts require a previous scalar bits, enum, ue, or "
                            "computed<u64> field");
                    } else if (use == ControllerUse::Boolean) {
                        message = QStringLiteral(
                            "Boolean conditions require a previous computed<bool> field");
                    }
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::InvalidType, message, range});
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
                return &*found;
            };
            const auto validateConditionValue = [&](const DeclaredField* controller,
                                                    quint64 expectedValue,
                                                    const DslSourceRange& range) {
                if (controller != nullptr && controller->syntax != nullptr &&
                    controller->syntax->width != 0 && controller->syntax->width < 64 &&
                    expectedValue >= (quint64{1} << controller->syntax->width)) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::ConstraintOutOfRange,
                         QStringLiteral("Condition value does not fit the controlling field"),
                         range});
                } else if (controller != nullptr && controller->syntax != nullptr &&
                           controller->syntax->encoding ==
                               DslFieldEncoding::UnsignedExpGolomb &&
                           expectedValue == std::numeric_limits<quint64>::max()) {
                    result_.diagnostics.push_back(
                        {DslDiagnosticCode::ConstraintOutOfRange,
                         QStringLiteral(
                             "Condition value exceeds the supported ue domain"),
                         range});
                }
            };
            const auto validateCondition = [&](const DslEqualityCondition& condition,
                                               const std::vector<ActiveCondition>& active) {
                if (condition.expression) {
                    return;
                }
                const ControllerUse use = condition.booleanShorthand
                                              ? ControllerUse::Boolean
                                              : ControllerUse::Equality;
                const DeclaredField* controller = validateController(
                    condition.fieldName, condition.range, active, use);
                if (!condition.booleanShorthand) {
                    validateConditionValue(
                        controller, condition.expectedValue, condition.range);
                }
            };
            const auto validateFieldAnnotations = [&](const DslBitField& field) {
                validateAnnotations(field.annotations, DslAnnotationTarget::BitField);
                bool equalsSeen = false;
                bool rangeSeen = false;
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
                        if (field.encoding != DslFieldEncoding::Bits &&
                            field.encoding != DslFieldEncoding::UnsignedExpGolomb) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidAnnotation,
                                 QStringLiteral("@enum is only supported on bits and ue fields"),
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
                        } else if (field.encoding == DslFieldEncoding::UnsignedExpGolomb) {
                            const auto outsideDomain = std::find_if(
                                found->values.begin(),
                                found->values.end(),
                                [](const DslEnumValue& value) {
                                    return value.value ==
                                           std::numeric_limits<quint64>::max();
                                });
                            if (outsideDomain != found->values.end()) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::EnumValueOutOfRange,
                                     QStringLiteral(
                                         "Enum member value exceeds the supported ue domain"),
                                     annotation.range});
                            }
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
                    if (annotation.name == QStringLiteral("range")) {
                        if (rangeSeen) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidAnnotation,
                                 QStringLiteral("@range may appear at most once on a field"),
                                 annotation.range});
                        }
                        rangeSeen = true;
                        const bool signedRangeField =
                            field.encoding == DslFieldEncoding::SignedExpGolomb;
                        const bool isUnsignedScalar =
                            field.encoding == DslFieldEncoding::Bits ||
                            field.encoding == DslFieldEncoding::UnsignedExpGolomb ||
                            field.encoding == DslFieldEncoding::FfCoded;
                        if (!isUnsignedScalar && !signedRangeField) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidAnnotation,
                                 QStringLiteral(
                                     "@range is only supported on bits, ue, se, and ff_coded fields"),
                                 annotation.range});
                            continue;
                        }
                        if (annotation.arguments.size() != 2 ||
                            std::any_of(annotation.arguments.begin(),
                                        annotation.arguments.end(),
                                        [](const DslAnnotationValue& argument) {
                                            return argument.kind != DslAnnotationValueKind::Integer;
                                        })) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidAnnotation,
                                 QStringLiteral("@range requires two integer arguments"),
                                 annotation.range});
                            continue;
                        }
                        const DslAnnotationValue& minimumArgument = annotation.arguments.at(0);
                        const DslAnnotationValue& maximumArgument = annotation.arguments.at(1);
                        if (!signedRangeField &&
                            (minimumArgument.negative || maximumArgument.negative)) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::ConstraintOutOfRange,
                                 QStringLiteral(
                                     "@range bounds cannot be negative on unsigned fields"),
                                 annotation.range});
                            continue;
                        }
                        if (signedRangeField) {
                            constexpr quint64 maximumSignedExpGolombMagnitude =
                                static_cast<quint64>(std::numeric_limits<qint64>::max());
                            if (minimumArgument.integerValue >
                                    maximumSignedExpGolombMagnitude ||
                                maximumArgument.integerValue >
                                    maximumSignedExpGolombMagnitude) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::ConstraintOutOfRange,
                                     QStringLiteral(
                                         "@range bound exceeds the supported se value domain"),
                                     annotation.range});
                            } else if (signedAnnotationBound(minimumArgument) >
                                       signedAnnotationBound(maximumArgument)) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::ConstraintOutOfRange,
                                     QStringLiteral("@range minimum cannot exceed its maximum"),
                                     annotation.range});
                            }
                            continue;
                        }
                        const quint64 minimum = minimumArgument.integerValue;
                        const quint64 maximum = maximumArgument.integerValue;
                        if (minimum > maximum) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::ConstraintOutOfRange,
                                 QStringLiteral("@range minimum cannot exceed its maximum"),
                                 annotation.range});
                        }
                        constexpr quint64 maximumUnsignedExpGolombValue =
                            std::numeric_limits<quint64>::max() - 1;
                        if (field.encoding == DslFieldEncoding::UnsignedExpGolomb &&
                            maximum > maximumUnsignedExpGolombValue) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::ConstraintOutOfRange,
                                 QStringLiteral(
                                     "@range maximum exceeds the largest supported ue value"),
                                 annotation.range});
                        } else if (field.encoding == DslFieldEncoding::Bits &&
                                   !field.widthExpression && field.width != 0 &&
                                   field.width < 64 &&
                                   maximum >= (quint64{1} << field.width)) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::ConstraintOutOfRange,
                                 QStringLiteral(
                                     "@range maximum does not fit the field width"),
                                 annotation.range});
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
                    if (field.encoding != DslFieldEncoding::Bits &&
                        field.encoding != DslFieldEncoding::UnsignedExpGolomb &&
                        field.encoding != DslFieldEncoding::FfCoded) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidAnnotation,
                             QStringLiteral("@equals is only supported on bits, ue, and ff_coded fields"),
                             annotation.range});
                        continue;
                    }
                    if (annotation.arguments.size() != 1 ||
                        annotation.arguments.front().kind != DslAnnotationValueKind::Integer) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidAnnotation,
                             QStringLiteral("@equals requires one integer argument"),
                             annotation.range});
                    } else if (annotation.arguments.front().negative) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::ConstraintOutOfRange,
                             QStringLiteral("@equals value cannot be negative"),
                             annotation.range});
                    }
                }
            };
            const auto validateComputedAnnotations =
                [&](const DslComputedField& field) {
                    validateAnnotations(field.annotations, DslAnnotationTarget::ComputedField);
                };
            const auto validateLazyAnnotations = [&](const DslLazyRegion& region) {
                validateAnnotations(region.annotations, DslAnnotationTarget::LazyRegion);
            };
            const auto validateCompressedPayloadAnnotations =
                [&](const DslCompressedPayload& payload) {
                    validateAnnotations(payload.annotations, DslAnnotationTarget::CompressedPayload);
                };
            const auto containsField = [](const auto& self,
                                          const std::vector<DslStructItem>& items) -> bool {
                for (const DslStructItem& item : items) {
                    if (item.kind == DslStructItemKind::Field ||
                        item.kind == DslStructItemKind::Computed ||
                        item.kind == DslStructItemKind::LazyRegion ||
                        item.kind == DslStructItemKind::RbspTrailingBits ||
                        item.kind == DslStructItemKind::CompressedPayload) {
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
                    if ((item.kind == DslStructItemKind::Repeat ||
                         item.kind == DslStructItemKind::SentinelRepeat ||
                         item.kind == DslStructItemKind::WhileRepeat) &&
                        self(self, item.repeatItems)) {
                        return true;
                    }
                }
                return false;
            };
            struct OffsetTracker {
                std::optional<quint64> exactBitOffset = 0;
                bool byteAligned = true;
            };
            const auto validateItems = [&](const auto& self,
                                           const std::vector<DslStructItem>& items,
                                           const std::vector<ActiveCondition>& active,
                                           OffsetTracker tracker)
                -> OffsetTracker {
                for (const DslStructItem& item : items) {
                    if (item.kind == DslStructItemKind::Assertion) {
                        const DslAssertion& assertion = item.assertion;
                        const auto anchor = std::find_if(
                            declaredFields.rbegin(),
                            declaredFields.rend(),
                            [&assertion](const DeclaredField& declared) {
                                return declared.name == assertion.anchorFieldName;
                            });
                        if (anchor == declaredFields.rend()) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::UnknownReference,
                                 QStringLiteral(
                                     "Assertion anchor field must be declared earlier"),
                                 assertion.anchorFieldRange});
                        } else if (anchor->syntax == nullptr ||
                                   anchor->syntax->arrayLength) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidType,
                                 QStringLiteral(
                                     "Assertion anchors require a source-backed scalar field"),
                                 assertion.anchorFieldRange});
                        } else {
                            const bool anchorAvailable = std::all_of(
                                anchor->conditions.begin(),
                                anchor->conditions.end(),
                                [&active, &sameCondition](const ActiveCondition& required) {
                                    return std::any_of(
                                        active.begin(),
                                        active.end(),
                                        [&required, &sameCondition](
                                            const ActiveCondition& candidate) {
                                            return sameCondition(required, candidate);
                                        });
                                });
                            if (!anchorAvailable) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::InvalidCondition,
                                     QStringLiteral(
                                         "Assertion anchor field is not guaranteed on the current branch"),
                                     assertion.anchorFieldRange});
                            }
                        }

                        const auto resolveAssertionIdentifier =
                            [&](const QString& name,
                                const DslSourceRange& range)
                            -> std::optional<DslScalarType> {
                            const auto found = std::find_if(
                                declaredFields.rbegin(),
                                declaredFields.rend(),
                                [&name](const DeclaredField& declared) {
                                    return declared.name == name;
                                });
                            if (found == declaredFields.rend()) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::UnknownReference,
                                     QStringLiteral(
                                         "Assertion dependency must be declared earlier"),
                                     range});
                                return std::nullopt;
                            }
                            const bool available = std::all_of(
                                found->conditions.begin(),
                                found->conditions.end(),
                                [&active, &sameCondition](const ActiveCondition& required) {
                                    return std::any_of(
                                        active.begin(),
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
                                         "Assertion dependency is not guaranteed on the current branch"),
                                     range});
                                return std::nullopt;
                            }
                            if (found->computed != nullptr) {
                                return found->type;
                            }
                            if (found->syntax == nullptr || found->syntax->arrayLength ||
                                found->syntax->encoding ==
                                    DslFieldEncoding::SignedExpGolomb) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::InvalidType,
                                     QStringLiteral(
                                         "Assertion expressions require scalar unsigned fields"),
                                     range});
                                return std::nullopt;
                            }
                            return DslScalarType::U64;
                        };
                        std::size_t nodeCount = 0;
                        const auto expressionType = validateExpression(
                            validateExpression,
                            assertion.condition,
                            resolveAssertionIdentifier,
                            result_.program.pureFunctions.size(),
                            true,
                            true,
                            resolveOptionalDependency,
                            1,
                            nodeCount);
                        if (expressionType && *expressionType != DslScalarType::Bool) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidType,
                                 QStringLiteral("Assertion conditions must be bool"),
                                 assertion.condition.range});
                        }
                        continue;
                    }
                    if (item.kind == DslStructItemKind::CompressedPayload) {
                        const DslCompressedPayload& payload = item.compressedPayload;
                        validateCompressedPayloadAnnotations(payload);
                        if (std::find(declaredFieldNames.begin(),
                                      declaredFieldNames.end(),
                                      payload.name) != declaredFieldNames.end()) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::DuplicateName,
                                 QStringLiteral("Duplicate field name"),
                                 payload.range});
                        }
                        declaredFieldNames.push_back(payload.name);
                        tracker.exactBitOffset = std::nullopt;
                        tracker.byteAligned = false;
                        continue;
                    }
                    if (item.kind == DslStructItemKind::Conditional) {
                        validateCondition(item.condition, active);
                        const QString conditionName = item.condition.expression
                                                          ? QStringLiteral("@context:%1")
                                                                .arg(item.condition.range.start.offset)
                                                          : item.condition.fieldName;
                        std::vector<ActiveCondition> thenConditions = active;
                        thenConditions.push_back({conditionName,
                                                  item.condition.expectedValue,
                                                  false});
                        const auto thenTracker =
                            self(self, item.thenItems, thenConditions, tracker);
                        std::vector<ActiveCondition> elseConditions = active;
                        elseConditions.push_back({conditionName,
                                                  item.condition.expectedValue,
                                                  true});
                        const auto elseTracker = item.elseItems.empty()
                                                    ? tracker
                                                    : self(self,
                                                           item.elseItems,
                                                           elseConditions,
                                                           tracker);
                        tracker.exactBitOffset = thenTracker.exactBitOffset && elseTracker.exactBitOffset && *thenTracker.exactBitOffset == *elseTracker.exactBitOffset
                                          ? thenTracker.exactBitOffset
                                          : std::nullopt;
                        tracker.byteAligned =
                            thenTracker.byteAligned && elseTracker.byteAligned;
                        continue;
                    }
                    if (item.kind == DslStructItemKind::Switch) {
                        const DeclaredField* controller = validateController(
                            item.switchFieldName,
                            item.switchFieldRange,
                            active,
                            ControllerUse::Equality);
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

                        std::vector<OffsetTracker> armTrackers;
                        armTrackers.reserve(item.switchArms.size() +
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
                            armTrackers.push_back(
                                self(self, arm.items, armConditions, tracker));
                        }
                        if (!defaultSeen) {
                            armTrackers.push_back(tracker);
                        }
                        const auto firstKnown = std::find_if(
                            armTrackers.begin(),
                            armTrackers.end(),
                            [](const OffsetTracker& t) {
                                return t.exactBitOffset.has_value();
                            });
                        if (firstKnown == armTrackers.end() ||
                            std::any_of(
                                armTrackers.begin(),
                                armTrackers.end(),
                                [&firstKnown](const OffsetTracker& t) {
                                    return !t.exactBitOffset ||
                                           *t.exactBitOffset !=
                                               *firstKnown->exactBitOffset;
                                })) {
                            tracker.exactBitOffset = std::nullopt;
                        } else {
                            tracker.exactBitOffset = firstKnown->exactBitOffset;
                        }
                        tracker.byteAligned = std::all_of(
                            armTrackers.begin(),
                            armTrackers.end(),
                            [](const OffsetTracker& t) { return t.byteAligned; });
                        continue;
                    }
                    if (item.kind == DslStructItemKind::Repeat) {
                        const DeclaredField* controller = validateController(
                            item.repeatCountFieldName,
                            item.repeatCountFieldRange,
                            active,
                            ControllerUse::Repeat);
                        if (item.repeatMaximum == 0) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidArrayLength,
                                 QStringLiteral(
                                     "Bounded repeat maximum must be at least one"),
                                 item.repeatMaximumRange});
                        } else if (controller != nullptr &&
                                   controller->syntax != nullptr &&
                                   controller->syntax->encoding == DslFieldEncoding::Bits &&
                                   controller->syntax->width != 0 &&
                                   controller->syntax->width < 64 &&
                                   item.repeatMaximum >=
                                       (quint64{1} << controller->syntax->width)) {
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
                        (void)self(self, item.repeatItems, active, tracker);
                        declaredFields.resize(scopeStart);
                        tracker.exactBitOffset = std::nullopt;
                        tracker.byteAligned = false;
                        continue;
                    }
                    if (item.kind == DslStructItemKind::SentinelRepeat) {
                        if (item.repeatMaximum == 0 ||
                            item.repeatMaximum >
                                DslStructItem::maximumSentinelRepeatIterations()) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidArrayLength,
                                 QStringLiteral(
                                     "Sentinel repeat maximum must be in the range 1..64"),
                                 item.repeatMaximumRange});
                        }
                        if (!containsField(containsField, item.repeatItems)) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidCondition,
                                 QStringLiteral(
                                     "A sentinel repeat body must contain at least one field"),
                                 item.range});
                        }
                        const DslBitField* sentinel = nullptr;
                        for (const DslStructItem& bodyItem : item.repeatItems) {
                            if (bodyItem.kind == DslStructItemKind::Field &&
                                bodyItem.field.name == item.sentinelFieldName) {
                                sentinel = &bodyItem.field;
                                break;
                            }
                        }
                        if (sentinel == nullptr) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::UnknownReference,
                                 QStringLiteral(
                                     "Sentinel field must be declared directly in the repeat "
                                     "body"),
                                 item.sentinelFieldRange});
                        } else {
                            const bool supported =
                                !sentinel->arrayLength &&
                                ((sentinel->encoding == DslFieldEncoding::Bits &&
                                  sentinel->widthExpression == std::nullopt &&
                                  sentinel->width != 0) ||
                                 sentinel->encoding ==
                                     DslFieldEncoding::UnsignedExpGolomb);
                            if (!supported) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::InvalidType,
                                     QStringLiteral(
                                         "Sentinel fields require a fixed-width scalar bits, "
                                         "enum, or ue field"),
                                     item.sentinelFieldRange});
                            } else if (sentinel->encoding == DslFieldEncoding::Bits &&
                                       sentinel->width < 64 &&
                                       item.sentinelValue >=
                                           (quint64{1} << sentinel->width)) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::ConstraintOutOfRange,
                                     QStringLiteral(
                                         "Sentinel value does not fit the sentinel field"),
                                     item.sentinelValueRange});
                            } else if (
                                sentinel->encoding ==
                                    DslFieldEncoding::UnsignedExpGolomb &&
                                item.sentinelValue >
                                    std::numeric_limits<quint64>::max() - 1) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::ConstraintOutOfRange,
                                     QStringLiteral(
                                         "Sentinel value exceeds the supported ue domain"),
                                     item.sentinelValueRange});
                            }
                        }
                        const std::size_t scopeStart = declaredFields.size();
                        (void)self(self, item.repeatItems, active, tracker);
                        declaredFields.resize(scopeStart);
                        tracker.exactBitOffset = std::nullopt;
                        tracker.byteAligned = false;
                        continue;
                    }
                    if (item.kind == DslStructItemKind::WhileRepeat) {
                        if (item.repeatMaximum == 0 ||
                            item.repeatMaximum >
                                DslStructItem::maximumWhileRepeatIterations()) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidArrayLength,
                                 QStringLiteral(
                                     "While repeat maximum must be in the range 1..1024"),
                                 item.repeatMaximumRange});
                        }
                        if (!containsField(containsField, item.repeatItems)) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidCondition,
                                 QStringLiteral(
                                     "A while repeat body must contain at least one field"),
                                 item.range});
                        }
                        const std::size_t scopeStart = declaredFields.size();
                        (void)self(self, item.repeatItems, active, tracker);
                        declaredFields.resize(scopeStart);
                        tracker.exactBitOffset = std::nullopt;
                        tracker.byteAligned = false;
                        continue;
                    }
                    if (item.kind == DslStructItemKind::LazyRegion) {
                        const DslLazyRegion& region = item.lazyRegion;
                        validateLazyAnnotations(region);
                        if (std::find(declaredFieldNames.begin(),
                                      declaredFieldNames.end(),
                                      region.name) != declaredFieldNames.end()) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::DuplicateName,
                                 QStringLiteral("Duplicate field name"),
                                 region.range});
                        }
                        const auto resolveLazyIdentifier =
                            [&](const QString& name,
                                const DslSourceRange& range)
                            -> std::optional<DslScalarType> {
                            const auto found = std::find_if(
                                declaredFields.rbegin(),
                                declaredFields.rend(),
                                [&name](const DeclaredField& declared) {
                                    return declared.name == name;
                                });
                            if (found == declaredFields.rend()) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::UnknownReference,
                                     QStringLiteral(
                                         "Lazy byte-count dependency must be declared earlier"),
                                     range});
                                return std::nullopt;
                            }
                            const bool available = std::all_of(
                                found->conditions.begin(),
                                found->conditions.end(),
                                [&active, &sameCondition](const ActiveCondition& required) {
                                    return std::any_of(
                                        active.begin(),
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
                                         "Lazy byte-count dependency is not guaranteed on the "
                                         "current branch"),
                                     range});
                                return std::nullopt;
                            }
                            if (found->computed != nullptr) {
                                return found->type;
                            }
                            if (found->syntax == nullptr || found->syntax->arrayLength ||
                                found->syntax->encoding ==
                                    DslFieldEncoding::SignedExpGolomb) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::InvalidType,
                                     QStringLiteral(
                                         "Lazy byte counts require scalar unsigned fields"),
                                     range});
                                return std::nullopt;
                            }
                            return DslScalarType::U64;
                        };
                        std::size_t nodeCount = 0;
                        const auto expressionType = validateExpression(
                            validateExpression,
                            region.byteCountExpression,
                            resolveLazyIdentifier,
                            result_.program.pureFunctions.size(),
                            false,
                            true,
                            resolveOptionalDependency,
                            1,
                            nodeCount);
                        if (expressionType && *expressionType != DslScalarType::U64) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidType,
                                 QStringLiteral("Lazy byte-count expression must be u64"),
                                 region.byteCountExpression.range});
                        }
                        if (!tracker.byteAligned) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidEndian,
                                 QStringLiteral(
                                     "Lazy byte regions must begin at a byte boundary within the "
                                     "structure"),
                                 region.range});
                        }
                        declaredFieldNames.push_back(region.name);
                        tracker.exactBitOffset = std::nullopt;
                        tracker.byteAligned = false;
                        continue;
                    }

                    if (item.kind == DslStructItemKind::Computed) {
                        const DslComputedField& field = item.computed;
                        validateComputedAnnotations(field);
                        if (std::find(declaredFieldNames.begin(),
                                      declaredFieldNames.end(),
                                      field.name) != declaredFieldNames.end()) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::DuplicateName,
                                 QStringLiteral("Duplicate field name"),
                                 field.range});
                        }
                        const auto resolveComputedIdentifier =
                            [&](const QString& name,
                                const DslSourceRange& range)
                            -> std::optional<DslScalarType> {
                            const auto found = std::find_if(
                                declaredFields.rbegin(),
                                declaredFields.rend(),
                                [&name](const DeclaredField& declared) {
                                    return declared.name == name;
                                });
                            if (found == declaredFields.rend()) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::UnknownReference,
                                     QStringLiteral(
                                         "Computed dependency must be declared earlier"),
                                     range});
                                return std::nullopt;
                            }
                            const bool available = std::all_of(
                                found->conditions.begin(),
                                found->conditions.end(),
                                [&active, &sameCondition](const ActiveCondition& required) {
                                    return std::any_of(
                                        active.begin(),
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
                                         "Computed dependency is not guaranteed on the current "
                                         "branch"),
                                     range});
                                return std::nullopt;
                            }
                            if (found->computed != nullptr) {
                                return found->type;
                            }
                            if (found->syntax == nullptr || found->syntax->arrayLength ||
                                found->syntax->encoding ==
                                    DslFieldEncoding::SignedExpGolomb) {
                                result_.diagnostics.push_back(
                                    {DslDiagnosticCode::InvalidType,
                                     QStringLiteral(
                                         "Computed expressions require scalar unsigned fields"),
                                     range});
                                return std::nullopt;
                            }
                            return DslScalarType::U64;
                        };
                        std::size_t nodeCount = 0;
                        const auto expressionType = validateExpression(
                            validateExpression,
                            field.expression,
                            resolveComputedIdentifier,
                            result_.program.pureFunctions.size(),
                            true,
                            true,
                            resolveOptionalDependency,
                            1,
                            nodeCount);
                        if (expressionType && *expressionType != field.type) {
                            result_.diagnostics.push_back(
                                {DslDiagnosticCode::InvalidType,
                                 QStringLiteral(
                                     "Computed expression does not match declared type"),
                                 field.expression.range});
                        }
                        DeclaredField declared;
                        declared.name = field.name;
                        declared.type = field.type;
                        declared.conditions = active;
                        declared.computed = &field;
                        declaredFields.push_back(std::move(declared));
                        declaredFieldNames.push_back(field.name);
                        continue;
                    }

                    if (item.kind == DslStructItemKind::RbspTrailingBits) {
                        declaredFieldNames.push_back(
                            QStringLiteral("rbsp_trailing_bits"));
                        tracker.exactBitOffset = std::nullopt;
                        tracker.byteAligned = true;
                        continue;
                    }

                    if (item.kind != DslStructItemKind::Field) {
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
                    DeclaredField declared;
                    declared.name = field.name;
                    declared.type = DslScalarType::U64;
                    declared.conditions = active;
                    declared.syntax = &field;
                    declaredFields.push_back(std::move(declared));
                    declaredFieldNames.push_back(field.name);

                    if (field.endian == DslEndian::Little &&
                        (!tracker.exactBitOffset || *tracker.exactBitOffset % 8 != 0)) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidEndian,
                             QStringLiteral(
                                 "Little-endian fields must begin at a byte boundary within the "
                                 "structure"),
                             field.range});
                    }
                    if (field.encoding == DslFieldEncoding::FfCoded) {
                        tracker.exactBitOffset = std::nullopt;
                        // byteAligned remains unchanged
                        continue;
                    }
                    if (field.encoding != DslFieldEncoding::Bits || field.width == 0 ||
                        (field.arrayLength && *field.arrayLength == 0)) {
                        tracker.exactBitOffset = std::nullopt;
                        tracker.byteAligned = false;
                        continue;
                    }
                    const quint64 elementCount = field.arrayLength.value_or(1);
                    if (elementCount >
                        std::numeric_limits<quint64>::max() / field.width) {
                        result_.diagnostics.push_back(
                            {DslDiagnosticCode::InvalidArrayLength,
                             QStringLiteral("Fixed array bit width is too large"),
                             field.range});
                        tracker.exactBitOffset = std::nullopt;
                        tracker.byteAligned = false;
                        continue;
                    }
                    const quint64 totalWidth = elementCount * field.width;
                    if (tracker.exactBitOffset &&
                        *tracker.exactBitOffset <=
                            std::numeric_limits<quint64>::max() - totalWidth) {
                        *tracker.exactBitOffset += totalWidth;
                        tracker.byteAligned = (*tracker.exactBitOffset % 8 == 0);
                    } else {
                        tracker.exactBitOffset = std::nullopt;
                        if (totalWidth % 8 != 0) {
                            tracker.byteAligned = false;
                        }
                    }
                }
                return tracker;
            };
            (void)validateItems(validateItems, structure.items, {}, OffsetTracker{quint64(0), true});
            if (!structure.items.empty() && declaredFieldNames.empty() &&
                !containsField(containsField, structure.items)) {
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
            if (scan.scannerName != QStringLiteral("h264_start_code") &&
                scan.scannerName != QStringLiteral("adts_frame") &&
                scan.scannerName != QStringLiteral("mp4_box")) {
                result_.diagnostics.push_back({DslDiagnosticCode::UnsupportedScanner,
                                               QStringLiteral("Only h264_start_code, adts_frame, and mp4_box are supported"),
                                               scan.range});
            }
            validateAnnotations(scan.annotations, DslAnnotationTarget::Scan);
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
        if (result_.program.payloadDispatch) {
            validatePayloadDispatch(*result_.program.payloadDispatch);
        }
    }

    [[nodiscard]] static bool declaresName(const std::vector<DslStructItem>& items,
                                           const QString& name) {
        for (const DslStructItem& item : items) {
            switch (item.kind) {
            case DslStructItemKind::Field:
                if (item.field.name == name) {
                    return true;
                }
                break;
            case DslStructItemKind::Computed:
                if (item.computed.name == name) {
                    return true;
                }
                break;
            case DslStructItemKind::LazyRegion:
                if (item.lazyRegion.name == name) {
                    return true;
                }
                break;
            case DslStructItemKind::RbspTrailingBits:
                break;
            case DslStructItemKind::CompressedPayload:
                if (item.compressedPayload.name == name) {
                    return true;
                }
                break;
            case DslStructItemKind::Assertion:
            case DslStructItemKind::Unsupported:
                break;
            case DslStructItemKind::Conditional:
                if (declaresName(item.thenItems, name) || declaresName(item.elseItems, name)) {
                    return true;
                }
                break;
            case DslStructItemKind::Switch:
                for (const DslStructItem::SwitchArm& arm : item.switchArms) {
                    if (declaresName(arm.items, name)) {
                        return true;
                    }
                }
                break;
            case DslStructItemKind::Repeat:
            case DslStructItemKind::SentinelRepeat:
            case DslStructItemKind::WhileRepeat:
                if (declaresName(item.repeatItems, name)) {
                    return true;
                }
                break;
            }
        }
        return false;
    }

    void validatePayloadDispatch(const DslPayloadDispatch& dispatch) {
        if (dispatch.viewKind != QStringLiteral("rbsp")) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidPayloadDispatch,
                 QStringLiteral("The only accepted payload view kind is rbsp"),
                 dispatch.range});
        }
        if (dispatch.cases.empty()) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidPayloadDispatch,
                 QStringLiteral("A payload dispatch must declare at least one case"),
                 dispatch.range});
        }

        const DslProgressiveScan* scan = nullptr;
        for (const DslProgressiveScan& candidate : result_.program.scans) {
            if (candidate.name == dispatch.sequenceName) {
                scan = &candidate;
                break;
            }
        }
        if (scan == nullptr) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::UnknownReference,
                 QStringLiteral("A payload dispatch must name a declared sequence"),
                 dispatch.range});
            return;
        }
        if (result_.program.entry.targetName != scan->name) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidPayloadDispatch,
                 QStringLiteral("A payload dispatch requires an entry naming its sequence"),
                 dispatch.range});
        }

        const DslStruct* element = nullptr;
        for (const DslStruct& candidate : result_.program.structs) {
            if (candidate.name == scan->elementType) {
                element = &candidate;
                break;
            }
        }
        if (element == nullptr) {
            return;
        }

        const DslBitField* controller = nullptr;
        for (const DslStructItem& item : element->items) {
            if (item.kind == DslStructItemKind::Field &&
                item.field.name == dispatch.controllerFieldName) {
                controller = &item.field;
                break;
            }
        }
        if (controller == nullptr) {
            const auto code = declaresName(element->items, dispatch.controllerFieldName)
                                  ? DslDiagnosticCode::InvalidPayloadDispatch
                                  : DslDiagnosticCode::UnknownReference;
            result_.diagnostics.push_back(
                {code,
                 QStringLiteral("A payload controller must be an unsigned scalar bits field "
                                "declared unconditionally at the top level of the sequence "
                                "element structure"),
                 dispatch.controllerRange});
        } else if (controller->encoding != DslFieldEncoding::Bits || controller->arrayLength) {
            result_.diagnostics.push_back(
                {DslDiagnosticCode::InvalidPayloadDispatch,
                 QStringLiteral("A payload controller must be an unsigned scalar bits field "
                                "declared unconditionally at the top level of the sequence "
                                "element structure"),
                 dispatch.controllerRange});
            controller = nullptr;
        }

        std::vector<quint64> caseValues;
        for (const DslPayloadCase& payloadCase : dispatch.cases) {
            if (std::find(caseValues.begin(), caseValues.end(), payloadCase.value) !=
                caseValues.end()) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidPayloadDispatch,
                     QStringLiteral("Duplicate payload dispatch case value"),
                     payloadCase.valueRange});
            }
            caseValues.push_back(payloadCase.value);
            if (controller != nullptr && controller->width != 0 && controller->width < 64 &&
                payloadCase.value >= (quint64{1} << controller->width)) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::ConstraintOutOfRange,
                     QStringLiteral("Payload case value does not fit the controlling field"),
                     payloadCase.valueRange});
            }
            if (payloadCase.kind != DslPayloadCaseKind::Structure) {
                continue;
            }
            if (payloadCase.targetName == scan->elementType) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidPayloadDispatch,
                     QStringLiteral(
                         "A payload case target may not be the sequence element structure"),
                     payloadCase.range});
                continue;
            }
            bool targetFound = false;
            for (const DslStruct& candidate : result_.program.structs) {
                targetFound = targetFound || candidate.name == payloadCase.targetName;
            }
            if (!targetFound) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::UnknownReference,
                     QStringLiteral("A payload case target must name a declared structure"),
                     payloadCase.range});
            }
        }
    }

    DslLexResult lexResult_;
    std::size_t index_ = 0;
    std::size_t expressionParseDepth_ = 0;
    DslParseResult result_;
};

} // namespace

DslLexResult DslLexer::lex(const QString& source) { return Lexer(source).run(); }

DslParseResult DslParser::parse(const QString& source) { return Parser(source).run(); }

} // namespace streamview::rules
