#include <streamview/rules/dsl.h>

#include <QtGlobal>

#include <limits>
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
        case ';':
            punctuation(DslTokenKind::Semicolon);
            return;
        case ',':
            punctuation(DslTokenKind::Comma);
            return;
        case '=':
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
            if (isIdentifier(QStringLiteral("struct"))) {
                parseStruct(annotations);
            } else if (isIdentifier(QStringLiteral("sequence"))) {
                parseScan(annotations);
            } else if (isIdentifier(QStringLiteral("entry"))) {
                parseEntry(annotations);
            } else {
                error(DslDiagnosticCode::UnexpectedToken,
                      QStringLiteral("Expected struct, sequence, or entry declaration"));
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

        while (!at(DslTokenKind::RightBrace) && !at(DslTokenKind::EndOfFile)) {
            const std::vector<DslAnnotation> fieldAnnotations = parseAnnotations();
            const DslSourcePosition fieldStart = current().range.start;
            if (!matchIdentifier(QStringLiteral("bits"))) {
                error(DslDiagnosticCode::UnexpectedToken,
                      QStringLiteral("Only bits<N> fields are supported in the minimum DSL"));
                recoverField();
                continue;
            }
            expect(DslTokenKind::Less, QStringLiteral("'<' after bits"));
            quint64 width = 0;
            if (at(DslTokenKind::IntegerLiteral)) {
                width = consume().integerValue;
            } else {
                error(DslDiagnosticCode::MissingToken, QStringLiteral("Expected bit width"));
            }
            expect(DslTokenKind::Greater, QStringLiteral("'>' after bit width"));

            DslBitField field;
            field.annotations = fieldAnnotations;
            if (!expectIdentifier(&field.name, QStringLiteral("field name"))) {
                recoverField();
                continue;
            }
            const std::vector<DslAnnotation> trailingAnnotations = parseAnnotations();
            field.annotations.insert(
                field.annotations.end(), trailingAnnotations.begin(), trailingAnnotations.end());
            expect(DslTokenKind::Semicolon, QStringLiteral("';' after field"));
            field.width = width >= 1 && width <= 64 ? static_cast<quint8>(width) : 0;
            field.range = {fieldStart, lexResult_.tokens.at(index_ - 1).range.end};
            if (field.width == 0) {
                result_.diagnostics.push_back(
                    {DslDiagnosticCode::InvalidBitWidth,
                     QStringLiteral("Bit field width must be in the range 1..64"),
                     field.range});
            }
            structure.fields.push_back(std::move(field));
        }

        const bool closed = expect(DslTokenKind::RightBrace, QStringLiteral("'}' after fields"));
        match(DslTokenKind::Semicolon);
        structure.range = {start, lexResult_.tokens.at(index_ - 1).range.end};
        if (structure.fields.empty() && closed) {
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
            for (std::size_t fieldIndex = 0; fieldIndex < structure.fields.size(); ++fieldIndex) {
                const DslBitField& field = structure.fields.at(fieldIndex);
                validatePresentationAnnotations(field.annotations);
                for (std::size_t previous = 0; previous < fieldIndex; ++previous) {
                    if (field.name == structure.fields.at(previous).name) {
                        result_.diagnostics.push_back({DslDiagnosticCode::DuplicateName,
                                                       QStringLiteral("Duplicate field name"),
                                                       field.range});
                        break;
                    }
                }
                for (const DslAnnotation& annotation : field.annotations) {
                    if (annotation.name == QStringLiteral("equals") &&
                        (annotation.arguments.size() != 1 ||
                         annotation.arguments.front().kind !=
                             DslAnnotationValueKind::Integer)) {
                        result_.diagnostics.push_back({DslDiagnosticCode::InvalidAnnotation,
                                                       QStringLiteral("@equals requires one integer argument"),
                                                       annotation.range});
                    }
                }
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
