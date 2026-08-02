#include <streamview/rules/dsl.h>

#include <QTest>

#include <algorithm>
#include <optional>

using streamview::rules::DslAnnotationValueKind;
using streamview::rules::DslBinaryOperator;
using streamview::rules::DslDiagnosticCode;
using streamview::rules::DslEndian;
using streamview::rules::DslExpressionKind;
using streamview::rules::DslFieldEncoding;
using streamview::rules::DslLexer;
using streamview::rules::DslParser;
using streamview::rules::DslPayloadCaseKind;
using streamview::rules::DslScalarType;
using streamview::rules::DslSwitchArmKind;
using streamview::rules::DslStructItemKind;
using streamview::rules::DslTokenKind;

namespace {

[[nodiscard]] bool hasDiagnostic(const streamview::rules::DslParseResult& result,
                                 DslDiagnosticCode code) {
    return std::any_of(result.diagnostics.begin(),
                       result.diagnostics.end(),
                       [code](const auto& diagnostic) { return diagnostic.code == code; });
}

[[nodiscard]] QString payloadSource(const QString& dispatch) {
    return QStringLiteral(
               "struct NalUnitHeader { bits<5> nal_unit_type; }\n"
               "struct AccessUnitDelimiterRbsp { bits<8> value; }\n"
               "@index(progressive)\n"
               "sequence<NalUnitHeader> nal_units = scan(h264_start_code);\n") +
           dispatch + QStringLiteral("entry nal_units;\n");
}

const QString kPayloadDispatchSource = payloadSource(
    QStringLiteral("@spec(\"ITU-T H.264\", \"7.3.1\")\n"
                   "payload<rbsp> nal_units switch (nal_unit_type) {\n"
                   "    case 9: AccessUnitDelimiterRbsp;\n"
                   "    case 10: empty;\n"
                   "    case 11: empty;\n"
                   "}\n"));

} // namespace

class DslTest final : public QObject {
    Q_OBJECT

private slots:
    void lexesCommentsLiteralsAndPositions() {
        const auto result = DslLexer::lex(QStringLiteral(
            "// heading\n@tag(0x10, \"line\\nvalue\", progressive)"));

        QVERIFY(result.succeeded());
        QCOMPARE(result.tokens.at(0).kind, DslTokenKind::At);
        QCOMPARE(result.tokens.at(0).range.start.line, quint32(2));
        QCOMPARE(result.tokens.at(1).lexeme, QStringLiteral("tag"));
        QCOMPARE(result.tokens.at(3).kind, DslTokenKind::IntegerLiteral);
        QCOMPARE(result.tokens.at(3).integerValue, quint64(16));
        QCOMPARE(result.tokens.at(5).kind, DslTokenKind::StringLiteral);
        QCOMPARE(result.tokens.at(5).lexeme, QStringLiteral("line\nvalue"));
        QCOMPARE(result.tokens.back().kind, DslTokenKind::EndOfFile);
    }

    void lexesComputedExpressionOperators() {
        const auto result = DslLexer::lex(
            QStringLiteral("! != * / % + - < <= > >= == && ||"));

        QVERIFY(result.succeeded());
        const std::vector<DslTokenKind> expected{
            DslTokenKind::Bang,
            DslTokenKind::BangEqual,
            DslTokenKind::Star,
            DslTokenKind::Slash,
            DslTokenKind::Percent,
            DslTokenKind::Plus,
            DslTokenKind::Minus,
            DslTokenKind::Less,
            DslTokenKind::LessEqual,
            DslTokenKind::Greater,
            DslTokenKind::GreaterEqual,
            DslTokenKind::EqualEqual,
            DslTokenKind::AndAnd,
            DslTokenKind::OrOr,
            DslTokenKind::EndOfFile,
        };
        QCOMPARE(result.tokens.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(result.tokens.at(index).kind, expected.at(index));
        }
    }

    void parsesMinimumProgramIntoTypedIr() {
        const auto result = DslParser::parse(QStringLiteral(R"(
            @spec("ITU-T H.264", "7.3.1")
            struct NalUnitHeader {
                bits<1> forbidden_zero_bit @equals(0);
                bits<2> nal_ref_idc;
                bits<5> nal_unit_type;
            }

            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            entry nal_units;
        )"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.structs.size(), std::size_t(1));
        const auto& structure = result.program.structs.front();
        QCOMPARE(structure.name, QStringLiteral("NalUnitHeader"));
        QCOMPARE(structure.annotations.size(), std::size_t(1));
        QCOMPARE(structure.annotations.front().arguments.size(), std::size_t(2));
        QCOMPARE(structure.annotations.front().arguments.front().kind,
                 DslAnnotationValueKind::String);
        QCOMPARE(structure.items.size(), std::size_t(3));
        QCOMPARE(structure.items.at(0).field.width, quint8(1));
        QCOMPARE(structure.items.at(1).field.width, quint8(2));
        QCOMPARE(structure.items.at(2).field.width, quint8(5));
        QCOMPARE(structure.items.front().field.annotations.front().name,
                 QStringLiteral("equals"));
        QCOMPARE(structure.items.front().field.annotations.front().arguments.front().integerValue,
                 quint64(0));

        QCOMPARE(result.program.scans.size(), std::size_t(1));
        QCOMPARE(result.program.scans.front().elementType, QStringLiteral("NalUnitHeader"));
        QCOMPARE(result.program.scans.front().name, QStringLiteral("nal_units"));
        QCOMPARE(result.program.scans.front().scannerName,
                 QStringLiteral("h264_start_code"));
        QVERIFY(result.program.hasEntry);
        QCOMPARE(result.program.entry.targetName, QStringLiteral("nal_units"));
    }

    void parsesEnumsAndExplicitEndianFields() {
        const auto result = DslParser::parse(QStringLiteral(R"(
            enum NalUnitType {
                non_idr = 1;
                idr = 5;
            }
            struct Header {
                bits<16, little> value;
                bits<5> nal_unit_type @enum(NalUnitType);
            }
            entry Header;
        )"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.enums.size(), std::size_t(1));
        QCOMPARE(result.program.enums.front().name, QStringLiteral("NalUnitType"));
        QCOMPARE(result.program.enums.front().values.size(), std::size_t(2));
        QCOMPARE(result.program.enums.front().values.at(1).name, QStringLiteral("idr"));
        QCOMPARE(result.program.enums.front().values.at(1).value, quint64(5));
        QCOMPARE(result.program.structs.front().items.at(0).field.endian, DslEndian::Little);
        QCOMPARE(result.program.structs.front().items.at(1).field.endian, DslEndian::Big);
        QCOMPARE(result.program.structs.front().items.at(1).field.annotations.back().name,
                 QStringLiteral("enum"));
    }

    void parsesUnsignedAndSignedExpGolombFields() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct SliceHeader { ue first_mb_in_slice; "
            "se slice_qp_delta @description(\"QP delta.\"); } entry SliceHeader;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.structs.front().items.size(), std::size_t(2));
        QCOMPARE(result.program.structs.front().items.at(0).field.name,
                 QStringLiteral("first_mb_in_slice"));
        QCOMPARE(result.program.structs.front().items.at(0).field.encoding,
                 DslFieldEncoding::UnsignedExpGolomb);
        QCOMPARE(result.program.structs.front().items.at(1).field.name,
                 QStringLiteral("slice_qp_delta"));
        QCOMPARE(result.program.structs.front().items.at(1).field.encoding,
                 DslFieldEncoding::SignedExpGolomb);
    }

    void rejectsFixedWidthAnnotationsAndAlignmentAfterExpGolombFields() {
        const auto annotations = DslParser::parse(QStringLiteral(
            "enum Type { value = 1; } struct Header { "
            "se first @equals(0); se second @enum(Type); } entry Header;"));
        const auto alignment = DslParser::parse(QStringLiteral(
            "struct Header { ue prefix; bits<16, little> value; } entry Header;"));

        QVERIFY(hasDiagnostic(annotations, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(alignment, DslDiagnosticCode::InvalidEndian));
    }

    void acceptsEqualsOnUnsignedExpGolombFields() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { ue pic_order_cnt_type @equals(0); } entry Header;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.structs.front().items.front().field.annotations.front().name,
                 QStringLiteral("equals"));
    }

    void acceptsRangeOnUnsignedExpGolombFields() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { ue log2_max_frame_num_minus4 @range(0, 12); } entry Header;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        const auto& annotation =
            result.program.structs.front().items.front().field.annotations.front();
        QCOMPARE(annotation.name, QStringLiteral("range"));
        QCOMPARE(annotation.arguments.size(), std::size_t{2});
        QCOMPARE(annotation.arguments.at(0).integerValue, quint64{0});
        QCOMPARE(annotation.arguments.at(1).integerValue, quint64{12});
    }

    void rejectsInvalidRangeAnnotations() {
        const auto repeated = DslParser::parse(QStringLiteral(
            "struct Header { ue value @range(0, 1) @range(0, 2); } entry Header;"));
        const auto onBits = DslParser::parse(
            QStringLiteral("struct Header { bits<8> value @range(0, 12); } entry Header;"));
        const auto onSigned = DslParser::parse(
            QStringLiteral("struct Header { se value @range(0, 12); } entry Header;"));
        const auto oneArgument = DslParser::parse(
            QStringLiteral("struct Header { ue value @range(12); } entry Header;"));
        const auto threeArguments = DslParser::parse(
            QStringLiteral("struct Header { ue value @range(0, 1, 2); } entry Header;"));
        const auto nonInteger = DslParser::parse(QStringLiteral(
            "struct Header { ue value @range(0, \"twelve\"); } entry Header;"));
        const auto inverted = DslParser::parse(
            QStringLiteral("struct Header { ue value @range(12, 0); } entry Header;"));

        QVERIFY(hasDiagnostic(repeated, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(onBits, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(onSigned, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(oneArgument, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(threeArguments, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(nonInteger, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(inverted, DslDiagnosticCode::ConstraintOutOfRange));
    }

    void acceptsCoincidentRangeAndEqualsOnUnsignedExpGolombFields() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { ue value @equals(4) @range(0, 12); } entry Header;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.structs.front().items.front().field.annotations.size(),
                 std::size_t{2});
    }

    void parsesFixedLengthArraysForAllScalarFieldEncodings() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> flags[3] @description(\"Flags.\"); "
            "ue codes[2]; se deltas[2]; } entry Header;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.structs.front().items.size(), std::size_t(3));
        const auto& flags = result.program.structs.front().items.at(0).field;
        QCOMPARE(flags.name, QStringLiteral("flags"));
        QCOMPARE(flags.arrayLength, std::optional<quint64>(3));
        QCOMPARE(flags.annotations.back().name, QStringLiteral("description"));
        QCOMPARE(result.program.structs.front().items.at(1).field.arrayLength,
                 std::optional<quint64>(2));
        QCOMPARE(result.program.structs.front().items.at(2).field.arrayLength,
                 std::optional<quint64>(2));
    }

    void rejectsZeroAndMalformedFixedLengthArrays() {
        const auto zero = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flags[0]; } entry Header;"));
        const auto missingLength = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flags[]; } entry Header;"));
        const auto missingBracket = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flags[2; } entry Header;"));

        QCOMPARE(zero.diagnostics.size(), std::size_t(1));
        QVERIFY(hasDiagnostic(zero, DslDiagnosticCode::InvalidArrayLength));
        QCOMPARE(missingLength.diagnostics.size(), std::size_t(1));
        QVERIFY(hasDiagnostic(missingLength, DslDiagnosticCode::MissingToken));
        QCOMPARE(missingBracket.diagnostics.size(), std::size_t(1));
        QVERIFY(hasDiagnostic(missingBracket, DslDiagnosticCode::MissingToken));
    }

    void parsesATerminalRbspTrailingBitsItem() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Payload { bits<3> primary_pic_type; rbsp_trailing_bits; } "
            "entry Payload;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.structs.size(), std::size_t(1));
        QCOMPARE(result.program.structs.front().items.size(), std::size_t(2));
        QCOMPARE(result.program.structs.front().items.back().kind,
                 DslStructItemKind::RbspTrailingBits);
        QVERIFY(result.program.structs.front().items.back().range.end.offset >
                result.program.structs.front().items.back().range.start.offset);

        const auto terminalOnly = DslParser::parse(QStringLiteral(
            "struct Payload { rbsp_trailing_bits; } entry Payload;"));
        QVERIFY(terminalOnly.succeeded());
    }

    void rejectsInvalidRbspTrailingBitsPlacementAndAnnotations() {
        const std::vector<QString> invalidPlacements{
            QStringLiteral(
                "struct Payload { rbsp_trailing_bits; bits<1> tail; } entry Payload;"),
            QStringLiteral(
                "struct Payload { rbsp_trailing_bits; rbsp_trailing_bits; } entry Payload;"),
            QStringLiteral(
                "struct Payload { bits<1> flag; if (flag) { rbsp_trailing_bits; } } "
                "entry Payload;"),
            QStringLiteral(
                "struct Payload { bits<1> flag; switch (flag) { "
                "case 0: { rbsp_trailing_bits; } } } entry Payload;"),
            QStringLiteral(
                "struct Payload { bits<1> count; repeat (count, 1) { "
                "rbsp_trailing_bits; } } entry Payload;"),
        };

        for (const QString& source : invalidPlacements) {
            const auto result = DslParser::parse(source);
            QVERIFY(!result.succeeded());
            QVERIFY(hasDiagnostic(result, DslDiagnosticCode::InvalidRbspTrailingBits));
        }

        const auto annotated = DslParser::parse(QStringLiteral(
            "struct Payload { @spec(\"ITU-T H.264\", \"7.3.2.11\") "
            "rbsp_trailing_bits; } entry Payload;"));
        QVERIFY(!annotated.succeeded());
        QVERIFY(hasDiagnostic(annotated, DslDiagnosticCode::InvalidAnnotation));

        const std::vector<QString> reservedNames{
            QStringLiteral("rbsp_stop_one_bit"),
            QStringLiteral("rbsp_alignment_zero_bit"),
        };
        for (const QString& name : reservedNames) {
            const auto conflict = DslParser::parse(QStringLiteral(
                "struct Payload { bits<1> %1; rbsp_trailing_bits; } entry Payload;")
                                                      .arg(name));
            QVERIFY(!conflict.succeeded());
            QVERIFY(hasDiagnostic(conflict, DslDiagnosticCode::DuplicateName));
        }
    }

    void computesStaticAlignmentAcrossFixedLengthArrays() {
        const auto aligned = DslParser::parse(QStringLiteral(
            "struct Header { bits<4> prefix[2]; bits<16, little> value; } entry Header;"));
        const auto unaligned = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> prefix[2]; bits<16, little> value; } entry Header;"));

        QVERIFY2(aligned.succeeded(),
                 aligned.diagnostics.empty()
                     ? ""
                     : qPrintable(aligned.diagnostics.front().message));
        QVERIFY(hasDiagnostic(unaligned, DslDiagnosticCode::InvalidEndian));
    }

    void parsesNestedEqualityConditionalBlocksInDeclarationOrder() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; "
            "if (kind == 1) { bits<3> payload; "
            "if (kind == 1) { ue code; } } "
            "else { bits<3> reserved; } bits<2> tail; } entry Header;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        const auto& items = result.program.structs.front().items;
        QCOMPARE(items.size(), std::size_t(3));
        QCOMPARE(items.at(0).kind, DslStructItemKind::Field);
        QCOMPARE(items.at(0).field.name, QStringLiteral("kind"));
        QCOMPARE(items.at(1).kind, DslStructItemKind::Conditional);
        QCOMPARE(items.at(1).condition.fieldName, QStringLiteral("kind"));
        QCOMPARE(items.at(1).condition.expectedValue, quint64(1));
        QCOMPARE(items.at(1).thenItems.size(), std::size_t(2));
        QCOMPARE(items.at(1).thenItems.at(0).field.name, QStringLiteral("payload"));
        QCOMPARE(items.at(1).thenItems.at(1).kind, DslStructItemKind::Conditional);
        QCOMPARE(items.at(1).thenItems.at(1).thenItems.front().field.name,
                 QStringLiteral("code"));
        QCOMPARE(items.at(1).elseItems.size(), std::size_t(1));
        QCOMPARE(items.at(1).elseItems.front().field.name, QStringLiteral("reserved"));
        QCOMPARE(items.at(2).field.name, QStringLiteral("tail"));
        QVERIFY(items.at(1).range.end.offset > items.at(1).range.start.offset);
    }

    void rejectsInvalidOrUnavailableConditionalControlFields() {
        const auto unknown = DslParser::parse(QStringLiteral(
            "struct Header { if (missing == 1) { bits<1> value; } } entry Header;"));
        const auto future = DslParser::parse(QStringLiteral(
            "struct Header { if (kind == 1) { bits<1> value; } bits<1> kind; } "
            "entry Header;"));
        const auto array = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flags[2]; if (flags == 1) { bits<1> value; } } "
            "entry Header;"));
        const auto variable = DslParser::parse(QStringLiteral(
            "struct Header { ue code; if (code == 1) { bits<1> value; } } entry Header;"));
        const auto outOfRange = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag; if (flag == 2) { bits<1> value; } } "
            "entry Header;"));
        const auto unavailable = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag; if (flag == 1) { bits<1> local; } "
            "if (local == 1) { bits<1> value; } } entry Header;"));

        QVERIFY(hasDiagnostic(unknown, DslDiagnosticCode::UnknownReference));
        QVERIFY(hasDiagnostic(future, DslDiagnosticCode::UnknownReference));
        QVERIFY(hasDiagnostic(array, DslDiagnosticCode::InvalidType));
        QVERIFY(hasDiagnostic(variable, DslDiagnosticCode::InvalidType));
        QVERIFY(hasDiagnostic(outOfRange, DslDiagnosticCode::ConstraintOutOfRange));
        QVERIFY(hasDiagnostic(unavailable, DslDiagnosticCode::InvalidCondition));
    }

    void parsesNestedEqualitySwitchArmsInDeclarationOrder() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 1: { bits<3> compact_value; } "
            "case 2: { if (kind == 2) { bits<5> extended_value; } } "
            "default: { switch (kind) { case 3: { bits<4> unknown_value; } } } "
            "} bits<1> tail; } entry Header;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        const auto& items = result.program.structs.front().items;
        QCOMPARE(items.size(), std::size_t(3));
        QCOMPARE(items.at(1).kind, DslStructItemKind::Switch);
        QCOMPARE(items.at(1).switchFieldName, QStringLiteral("kind"));
        QCOMPARE(items.at(1).switchArms.size(), std::size_t(3));
        QCOMPARE(items.at(1).switchArms.at(0).kind, DslSwitchArmKind::Case);
        QCOMPARE(items.at(1).switchArms.at(0).caseValue, quint64(1));
        QCOMPARE(items.at(1).switchArms.at(0).items.front().field.name,
                 QStringLiteral("compact_value"));
        QCOMPARE(items.at(1).switchArms.at(1).kind, DslSwitchArmKind::Case);
        QCOMPARE(items.at(1).switchArms.at(1).caseValue, quint64(2));
        QCOMPARE(items.at(1).switchArms.at(1).items.front().kind,
                 DslStructItemKind::Conditional);
        QCOMPARE(items.at(1).switchArms.at(2).kind, DslSwitchArmKind::Default);
        QCOMPARE(items.at(1).switchArms.at(2).items.front().kind,
                 DslStructItemKind::Switch);
        QVERIFY(items.at(1).switchFieldRange.end.offset >
                items.at(1).switchFieldRange.start.offset);
        QVERIFY(items.at(1).range.end.offset > items.at(1).range.start.offset);
        QCOMPARE(items.at(2).field.name, QStringLiteral("tail"));
    }

    void rejectsInvalidEqualitySwitchControllersAndLabels() {
        struct Case final {
            QString source;
            DslDiagnosticCode diagnostic;
        };
        const std::vector<Case> cases{
            {QStringLiteral(
                 "struct Header { switch (missing) { case 0: { bits<1> value; } } } "
                 "entry Header;"),
             DslDiagnosticCode::UnknownReference},
            {QStringLiteral(
                 "struct Header { bits<1> flags[2]; switch (flags) { "
                 "case 0: { bits<1> value; } } } entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { ue code; switch (code) { case 0: { bits<1> value; } } } "
                 "entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { bits<1> kind; switch (kind) { "
                 "case 2: { bits<1> value; } } } entry Header;"),
             DslDiagnosticCode::ConstraintOutOfRange},
            {QStringLiteral(
                 "struct Header { bits<2> kind; switch (kind) { "
                 "case 1: { bits<1> first; } case 1: { bits<1> second; } } } "
                 "entry Header;"),
             DslDiagnosticCode::InvalidCondition},
            {QStringLiteral(
                 "struct Header { bits<2> kind; switch (kind) { "
                 "default: { bits<1> fallback; } case 1: { bits<1> value; } } } "
                 "entry Header;"),
             DslDiagnosticCode::InvalidCondition},
            {QStringLiteral(
                 "struct Header { bits<2> kind; switch (kind) { "
                 "default: { bits<1> first; } default: { bits<1> second; } } } "
                 "entry Header;"),
             DslDiagnosticCode::InvalidCondition},
            {QStringLiteral(
                 "struct Header { bits<2> kind; switch (kind) { "
                 "default: { bits<1> value; } } } entry Header;"),
             DslDiagnosticCode::InvalidCondition},
            {QStringLiteral(
                 "struct Header { bits<1> kind; if (kind == 1) { bits<1> local; } "
                 "switch (local) { case 0: { bits<1> value; } } } entry Header;"),
             DslDiagnosticCode::InvalidCondition},
            {QStringLiteral(
                 "struct Header { bits<1> kind; switch (kind) { "
                 "case 0: { bits<1> value; } case 1: { bits<1> value; } } } "
                 "entry Header;"),
             DslDiagnosticCode::DuplicateName},
        };

        for (const Case& testCase : cases) {
            const auto result = DslParser::parse(testCase.source);
            QVERIFY(!result.succeeded());
            QVERIFY(hasDiagnostic(result, testCase.diagnostic));
        }

        const auto missingColon = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> kind; switch (kind) { "
            "case 0 { bits<1> value; } } } entry Header;"));
        QVERIFY(hasDiagnostic(missingColon, DslDiagnosticCode::MissingToken));
    }

    void parsesNestedBoundedRepeatsAndPreservesSourceRanges() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 3) { "
            "bits<2> inner_count; repeat (inner_count, 2) { bits<1> value; } "
            "if (inner_count == 1) { ue code; } } bits<1> tail; } entry Header;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        const auto& items = result.program.structs.front().items;
        QCOMPARE(items.size(), std::size_t(3));
        QCOMPARE(items.at(1).kind, DslStructItemKind::Repeat);
        QCOMPARE(items.at(1).repeatCountFieldName, QStringLiteral("count"));
        QCOMPARE(items.at(1).repeatMaximum, quint64(3));
        QCOMPARE(items.at(1).repeatItems.size(), std::size_t(3));
        QCOMPARE(items.at(1).repeatItems.at(1).kind, DslStructItemKind::Repeat);
        QCOMPARE(items.at(1).repeatItems.at(1).repeatCountFieldName,
                 QStringLiteral("inner_count"));
        QCOMPARE(items.at(1).repeatItems.at(1).repeatMaximum, quint64(2));
        QCOMPARE(items.at(1).repeatItems.at(1).repeatItems.front().field.name,
                 QStringLiteral("value"));
        QCOMPARE(items.at(1).repeatItems.at(2).kind, DslStructItemKind::Conditional);
        QVERIFY(items.at(1).repeatCountFieldRange.end.offset >
                items.at(1).repeatCountFieldRange.start.offset);
        QVERIFY(items.at(1).repeatMaximumRange.end.offset >
                items.at(1).repeatMaximumRange.start.offset);
        QVERIFY(items.at(1).range.end.offset > items.at(1).range.start.offset);
    }

    void acceptsUnsignedExpGolombRepeatCountsAndScopesRepeatLocals() {
        const auto valid = DslParser::parse(QStringLiteral(
            "struct Header { ue count; repeat (count, 3) { bits<1> local; "
            "if (local == 1) { bits<1> value; } } bits<1> tail; } entry Header;"));
        const auto leaked = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 2) { bits<1> local; } "
            "if (local == 1) { bits<1> value; } } entry Header;"));
        const auto duplicate = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 2) { bits<1> local; } "
            "bits<1> local; } entry Header;"));

        QVERIFY2(valid.succeeded(),
                 valid.diagnostics.empty()
                     ? ""
                     : qPrintable(valid.diagnostics.front().message));
        QVERIFY(hasDiagnostic(leaked, DslDiagnosticCode::UnknownReference));
        QVERIFY(hasDiagnostic(duplicate, DslDiagnosticCode::DuplicateName));
    }

    void rejectsInvalidBoundedRepeatControllersAndMaxima() {
        struct Case final {
            QString source;
            DslDiagnosticCode diagnostic;
        };
        const std::vector<Case> cases{
            {QStringLiteral(
                 "struct Header { repeat (missing, 2) { bits<1> value; } } entry Header;"),
             DslDiagnosticCode::UnknownReference},
            {QStringLiteral(
                 "struct Header { repeat (count, 2) { bits<1> value; } bits<2> count; } "
                 "entry Header;"),
             DslDiagnosticCode::UnknownReference},
            {QStringLiteral(
                 "struct Header { bits<2> counts[2]; repeat (counts, 2) { bits<1> value; } "
                 "} entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { se count; repeat (count, 2) { bits<1> value; } } "
                 "entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { bits<2> count; repeat (count, 0) { bits<1> value; } } "
                 "entry Header;"),
             DslDiagnosticCode::InvalidArrayLength},
            {QStringLiteral(
                 "struct Header { bits<2> count; repeat (count, 4) { bits<1> value; } } "
                 "entry Header;"),
             DslDiagnosticCode::ConstraintOutOfRange},
            {QStringLiteral(
                 "struct Header { bits<1> flag; if (flag == 1) { bits<2> count; } "
                 "repeat (count, 2) { bits<1> value; } } entry Header;"),
             DslDiagnosticCode::InvalidCondition},
            {QStringLiteral(
                 "struct Header { bits<2> count; repeat (count, 2) { } } entry Header;"),
             DslDiagnosticCode::InvalidCondition},
            {QStringLiteral(
                 "struct Header { bits<2> count; @description(\"bad\") "
                 "repeat (count, 2) { bits<1> value; } } entry Header;"),
             DslDiagnosticCode::InvalidAnnotation},
        };

        for (const Case& testCase : cases) {
            const auto result = DslParser::parse(testCase.source);
            QVERIFY(!result.succeeded());
            QVERIFY(hasDiagnostic(result, testCase.diagnostic));
        }

        const auto missingMaximum = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count,) { bits<1> value; } } "
            "entry Header;"));
        QVERIFY(hasDiagnostic(missingMaximum, DslDiagnosticCode::MissingToken));
    }

    void parsesPureFunctionsComputedFieldsAndBooleanConditions() {
        const auto result = DslParser::parse(QStringLiteral(R"(
            pure bool between(u64 value, u64 low, u64 high) {
                return value >= low && value <= high;
            }
            pure u64 add(u64 value, u64 amount) {
                return value + amount;
            }
            struct Header {
                bits<5> type;
                @description("Video coding layer flag.")
                computed<bool> is_vcl = between(type, 1, 5) @spec("H.264", "7.4.1");
                computed<u64> adjusted = add(type, 1);
                if (is_vcl) { computed<u64> selected = adjusted + 1; }
                else { bits<1> reserved; }
            }
            entry Header;
        )"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.pureFunctions.size(), std::size_t(2));
        const auto& between = result.program.pureFunctions.front();
        QCOMPARE(between.name, QStringLiteral("between"));
        QCOMPARE(between.returnType, DslScalarType::Bool);
        QCOMPARE(between.parameters.size(), std::size_t(3));
        QCOMPARE(between.parameters.front().name, QStringLiteral("value"));
        QCOMPARE(between.parameters.front().type, DslScalarType::U64);
        QCOMPARE(between.expression.kind, DslExpressionKind::Binary);
        QCOMPARE(between.expression.binaryOperator, DslBinaryOperator::LogicalAnd);
        QCOMPARE(between.expression.operands.size(), std::size_t(2));

        const auto& items = result.program.structs.front().items;
        QCOMPARE(items.size(), std::size_t(4));
        QCOMPARE(items.at(1).kind, DslStructItemKind::Computed);
        QCOMPARE(items.at(1).computed.name, QStringLiteral("is_vcl"));
        QCOMPARE(items.at(1).computed.type, DslScalarType::Bool);
        QCOMPARE(items.at(1).computed.expression.kind, DslExpressionKind::Call);
        QCOMPARE(items.at(1).computed.expression.operands.size(), std::size_t(3));
        QCOMPARE(items.at(1).computed.annotations.size(), std::size_t(2));
        QCOMPARE(items.at(2).computed.type, DslScalarType::U64);
        QCOMPARE(items.at(3).kind, DslStructItemKind::Conditional);
        QVERIFY(items.at(3).condition.booleanShorthand);
        QCOMPARE(items.at(3).condition.fieldName, QStringLiteral("is_vcl"));
        QCOMPARE(items.at(3).condition.expectedValue, quint64(1));
        QCOMPARE(items.at(3).thenItems.front().kind, DslStructItemKind::Computed);
        QCOMPARE(items.at(3).elseItems.front().kind, DslStructItemKind::Field);
    }

    void parsesBooleanConditionsWithOptionalElse() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { computed<bool> present = true; "
            "if (present) { bits<1> value; } } entry Header;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        const auto& conditional = result.program.structs.front().items.at(1);
        QCOMPARE(conditional.kind, DslStructItemKind::Conditional);
        QVERIFY(conditional.condition.booleanShorthand);
        QVERIFY(conditional.elseItems.empty());
    }

    void parsesCheckedLazyByteRegions() {
        const auto result = DslParser::parse(QStringLiteral(R"(
            struct Packet {
                bits<8> payload_size;
                @lazy(payload_size + 1)
                bytes payload @description("Deferred payload") @spec("Example", "1");
            }
            struct ConditionalPacket {
                bits<8> selector;
                if (selector == 1) { @lazy(1) bytes selected; }
            }
            struct RepeatedPacket {
                bits<8> count;
                repeat (count, 1) { @lazy(1) bytes chunk; }
            }
            entry Packet;
        )"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty() ? "" : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.structs.size(), std::size_t(3));
        const auto& items = result.program.structs.front().items;
        QCOMPARE(items.size(), std::size_t(2));
        QCOMPARE(items.back().kind, DslStructItemKind::LazyRegion);
        QCOMPARE(items.back().lazyRegion.name, QStringLiteral("payload"));
        QCOMPARE(items.back().lazyRegion.byteCountExpression.kind, DslExpressionKind::Binary);
        QCOMPARE(items.back().lazyRegion.byteCountExpression.binaryOperator,
                 DslBinaryOperator::Add);
        QCOMPARE(items.back().lazyRegion.annotations.size(), std::size_t(2));
        QCOMPARE(items.back().lazyRegion.annotations.at(0).name, QStringLiteral("description"));
        QCOMPARE(items.back().lazyRegion.annotations.at(1).name, QStringLiteral("spec"));

        const auto& conditional = result.program.structs.at(1).items.at(1);
        QCOMPARE(conditional.thenItems.front().kind, DslStructItemKind::LazyRegion);
        const auto& repeated = result.program.structs.at(2).items.at(1);
        QCOMPARE(repeated.repeatItems.front().kind, DslStructItemKind::LazyRegion);
    }

    void rejectsInvalidLazyByteRegionDeclarations() {
        struct Case final {
            QString source;
            DslDiagnosticCode diagnostic;
        };
        const std::vector<Case> cases{
            {QStringLiteral("struct Header { bytes payload; } entry Header;"),
             DslDiagnosticCode::UnexpectedToken},
            {QStringLiteral("struct Header { @description(\"bad\") @lazy(1) bytes payload; } "
                            "entry Header;"),
             DslDiagnosticCode::InvalidAnnotation},
            {QStringLiteral("struct Header { @lazy(1) bytes payload[2]; } entry Header;"),
             DslDiagnosticCode::InvalidArrayLength},
            {QStringLiteral("struct Header { @lazy(1) bytes payload @equals(0); } entry Header;"),
             DslDiagnosticCode::InvalidAnnotation},
            {QStringLiteral("enum Type { value = 0; } struct Header { "
                            "@lazy(1) bytes payload @enum(Type); } entry Header;"),
             DslDiagnosticCode::InvalidAnnotation},
            {QStringLiteral("struct Header { @lazy(true) bytes payload; } entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { @lazy(size) bytes payload; bits<8> size; } entry Header;"),
             DslDiagnosticCode::UnknownReference},
            {QStringLiteral(
                 "struct Header { se delta; @lazy(delta) bytes payload; } entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral("struct Header { bits<8> sizes[2]; @lazy(sizes) bytes payload; } "
                            "entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral("struct Header { bits<8> selector; if (selector == 1) { "
                            "computed<u64> local = 1; } @lazy(local) bytes payload; } "
                            "entry Header;"),
             DslDiagnosticCode::InvalidCondition},
            {QStringLiteral(
                 "struct Header { bits<1> prefix; @lazy(1) bytes payload; } entry Header;"),
             DslDiagnosticCode::InvalidEndian},
            {QStringLiteral("struct Header { bits<8> selector; if (selector == 1) { bits<1> odd; } "
                            "@lazy(1) bytes payload; } entry Header;"),
             DslDiagnosticCode::InvalidEndian},
            {QStringLiteral("struct Header { @lazy(1) bytes first; @lazy(1) bytes second; } "
                            "entry Header;"),
             DslDiagnosticCode::InvalidEndian},
            {QStringLiteral(
                 "struct Header { @lazy(1) bytes payload; bits<1> payload; } entry Header;"),
             DslDiagnosticCode::DuplicateName},
            {QStringLiteral("struct Header { @lazy(1) bytes payload; "
                            "computed<u64> size = payload; } entry Header;"),
             DslDiagnosticCode::UnknownReference},
            {QStringLiteral("struct Header { @lazy(0) bytes payload; "
                            "repeat (payload, 1) { bits<1> value; } } entry Header;"),
             DslDiagnosticCode::UnknownReference},
        };

        for (const Case& testCase : cases) {
            const auto result = DslParser::parse(testCase.source);
            QVERIFY(!result.succeeded());
            QVERIFY(hasDiagnostic(result, testCase.diagnostic));
        }
    }

    void recoversAfterMalformedLazyByteRegions() {
        const auto missingBytes = DslParser::parse(
            QStringLiteral("struct Header { @lazy(1) payload; bits<1> tail; } entry Header;"));
        QVERIFY(!missingBytes.succeeded());
        QVERIFY(hasDiagnostic(missingBytes, DslDiagnosticCode::UnexpectedToken));
        QCOMPARE(missingBytes.program.structs.front().items.size(), std::size_t(1));
        QCOMPARE(missingBytes.program.structs.front().items.front().field.name,
                 QStringLiteral("tail"));

        const auto malformedExpression = DslParser::parse(
            QStringLiteral("pure u64 add(u64 left, u64 right) { return left + right; } "
                           "struct Header { @lazy(add(1, )) bytes payload; bits<1> tail; } "
                           "entry Header;"));
        QVERIFY(!malformedExpression.succeeded());
        QVERIFY(hasDiagnostic(malformedExpression, DslDiagnosticCode::UnexpectedToken));
        QCOMPARE(malformedExpression.program.structs.front().items.size(), std::size_t(2));
        QCOMPARE(malformedExpression.program.structs.front().items.back().field.name,
                 QStringLiteral("tail"));
    }

    void rejectsInvalidPureAndComputedDeclarations() {
        struct Case final {
            QString source;
            DslDiagnosticCode diagnostic;
        };
        const std::vector<Case> cases{
            {QStringLiteral(
                 "@description(\"bad\") pure bool flag() { return true; } "
                 "struct Header { bits<1> value; } entry Header;"),
             DslDiagnosticCode::InvalidAnnotation},
            {QStringLiteral(
                 "pure u64 duplicate(u64 value, u64 value) { return value; } "
                 "struct Header { bits<1> value; } entry Header;"),
             DslDiagnosticCode::DuplicateName},
            {QStringLiteral(
                 "pure bool first() { return later(); } "
                 "pure bool later() { return true; } "
                 "struct Header { bits<1> value; } entry Header;"),
             DslDiagnosticCode::UnknownReference},
            {QStringLiteral(
                 "struct Header { computed<bool> flags[2] = true; } entry Header;"),
             DslDiagnosticCode::InvalidArrayLength},
            {QStringLiteral(
                 "struct Header { computed<u64> value = 1 @equals(1); } entry Header;"),
             DslDiagnosticCode::InvalidAnnotation},
            {QStringLiteral(
                 "struct Header { computed<u64> value = missing + 1; } entry Header;"),
             DslDiagnosticCode::UnknownReference},
            {QStringLiteral(
                 "struct Header { se delta; computed<u64> value = delta; } entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { bits<1> flags[2]; computed<u64> value = flags; } "
                 "entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { computed<u64> value = true + 1; } entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { computed<bool> flag = true; "
                 "switch (flag) { case 1: { bits<1> value; } } } entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { computed<bool> flag = true; "
                 "repeat (flag, 1) { bits<1> value; } } entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { bits<1> selector; if (selector == 1) { "
                 "computed<u64> local = 1; } computed<u64> value = local; } "
                 "entry Header;"),
             DslDiagnosticCode::InvalidCondition},
        };

        for (const Case& testCase : cases) {
            const auto result = DslParser::parse(testCase.source);
            QVERIFY(!result.succeeded());
            QVERIFY(hasDiagnostic(result, testCase.diagnostic));
        }
    }

    void recoversAfterMalformedComputedCallArguments() {
        const auto result = DslParser::parse(QStringLiteral(
            "pure u64 add(u64 left, u64 right) { return left + right; } "
            "struct Header { computed<u64> bad = add(1, ); bits<1> tail; } "
            "entry Header;"));

        QVERIFY(!result.succeeded());
        QVERIFY(hasDiagnostic(result, DslDiagnosticCode::UnexpectedToken));
        QCOMPARE(result.program.structs.size(), std::size_t(1));
        QCOMPARE(result.program.structs.front().items.size(), std::size_t(2));
        QCOMPARE(result.program.structs.front().items.back().field.name,
                 QStringLiteral("tail"));
    }

    void recoversAfterInvalidBooleanConditionExpression() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { computed<bool> flag = true; "
            "if (flag != false) { bits<1> selected; } bits<1> tail; } "
            "entry Header;"));

        QVERIFY(!result.succeeded());
        QVERIFY(hasDiagnostic(result, DslDiagnosticCode::MissingToken));
        QCOMPARE(result.program.structs.size(), std::size_t(1));
        const auto& items = result.program.structs.front().items;
        QCOMPARE(items.size(), std::size_t(3));
        QCOMPARE(items.back().field.name, QStringLiteral("tail"));
    }

    void rejectsInvalidEndianAndUnknownEnumReferences() {
        const auto badWidth = DslParser::parse(
            QStringLiteral("struct Header { bits<5, little> value; } entry Header;"));
        const auto badName = DslParser::parse(
            QStringLiteral("struct Header { bits<8, network> value; } entry Header;"));
        const auto unknownEnum = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> value @enum(Missing); } entry Header;"));

        QVERIFY(hasDiagnostic(badWidth, DslDiagnosticCode::InvalidEndian));
        QVERIFY(hasDiagnostic(badName, DslDiagnosticCode::InvalidEndian));
        QVERIFY(hasDiagnostic(unknownEnum, DslDiagnosticCode::UnknownReference));
    }

    void rejectsOutOfRangeBitWidths() {
        const auto zero = DslParser::parse(QStringLiteral(
            "struct Header { bits<0> bad; } entry Header;"));
        const auto tooWide = DslParser::parse(QStringLiteral(
            "struct Header { bits<65> bad; } entry Header;"));

        QVERIFY(hasDiagnostic(zero, DslDiagnosticCode::InvalidBitWidth));
        QVERIFY(hasDiagnostic(tooWide, DslDiagnosticCode::InvalidBitWidth));
    }

    void rejectsMalformedPresentationAnnotations() {
        const auto badSpec = DslParser::parse(QStringLiteral(
            "@spec(1, \"clause\") struct Header { bits<1> value; } entry Header;"));
        const auto badDescription = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> value @description(1); } entry Header;"));

        QVERIFY(hasDiagnostic(badSpec, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(badDescription, DslDiagnosticCode::InvalidAnnotation));
    }

    void rejectsInvalidEntryDeclarations() {
        const auto missing =
            DslParser::parse(QStringLiteral("struct Header { bits<1> value; }"));
        const auto unknown = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> value; } entry Missing;"));
        const auto duplicate = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> value; }\n"
            "entry Header;\n"
            "entry Header;"));

        QVERIFY(hasDiagnostic(missing, DslDiagnosticCode::MissingEntry));
        QVERIFY(hasDiagnostic(unknown, DslDiagnosticCode::UnknownReference));
        QVERIFY(hasDiagnostic(duplicate, DslDiagnosticCode::DuplicateName));
    }

    void rejectsDuplicateNamesAndUnsupportedScans() {
        const auto duplicate = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> value; bits<2> value; } entry Header;"));
        const auto unsupported = DslParser::parse(QStringLiteral(R"(
            struct Header { bits<1> value; }
            sequence<Header> units = scan(other_scanner);
            entry units;
        )"));

        QVERIFY(hasDiagnostic(duplicate, DslDiagnosticCode::DuplicateName));
        QVERIFY(hasDiagnostic(unsupported, DslDiagnosticCode::UnsupportedScanner));
        QVERIFY(hasDiagnostic(unsupported, DslDiagnosticCode::InvalidProgressiveAnnotation));
    }

    void parsesPayloadDispatchDeclaration() {
        const auto result = DslParser::parse(kPayloadDispatchSource);

        QVERIFY(result.succeeded());
        QVERIFY(result.program.payloadDispatch.has_value());
        const auto& dispatch = *result.program.payloadDispatch;
        QCOMPARE(dispatch.viewKind, QStringLiteral("rbsp"));
        QCOMPARE(dispatch.sequenceName, QStringLiteral("nal_units"));
        QCOMPARE(dispatch.controllerFieldName, QStringLiteral("nal_unit_type"));
        QCOMPARE(dispatch.annotations.size(), std::size_t(1));
        QCOMPARE(dispatch.annotations.front().name, QStringLiteral("spec"));
        QCOMPARE(dispatch.cases.size(), std::size_t(3));
        QCOMPARE(dispatch.cases.at(0).value, quint64(9));
        QCOMPARE(dispatch.cases.at(0).kind, DslPayloadCaseKind::Structure);
        QCOMPARE(dispatch.cases.at(0).targetName,
                 QStringLiteral("AccessUnitDelimiterRbsp"));
        QCOMPARE(dispatch.cases.at(1).value, quint64(10));
        QCOMPARE(dispatch.cases.at(1).kind, DslPayloadCaseKind::Empty);
        QVERIFY(dispatch.cases.at(1).targetName.isEmpty());
        QCOMPARE(dispatch.cases.at(2).value, quint64(11));
        QCOMPARE(dispatch.cases.at(2).kind, DslPayloadCaseKind::Empty);
    }

    void keepsPayloadAndEmptyUsableAsOrdinaryIdentifiers() {
        const auto result = DslParser::parse(QStringLiteral(R"(
            enum Kind {
                payload = 1;
                empty = 2;
            }
            struct Header {
                bits<8> payload @enum(Kind);
                bits<8> empty;
            }
            entry Header;
        )"));

        QVERIFY(result.succeeded());
        QVERIFY(!result.program.payloadDispatch.has_value());
    }

    void rejectsUnsupportedPayloadViewKindAndDuplicateDispatch() {
        const auto badView = DslParser::parse(payloadSource(
            QStringLiteral("payload<ebsp> nal_units switch (nal_unit_type) {\n"
                           "    case 9: AccessUnitDelimiterRbsp;\n"
                           "}\n")));
        const auto duplicate = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "    case 9: AccessUnitDelimiterRbsp;\n"
                           "}\n"
                           "payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "    case 10: empty;\n"
                           "}\n")));

        QVERIFY(hasDiagnostic(badView, DslDiagnosticCode::InvalidPayloadDispatch));
        QVERIFY(hasDiagnostic(duplicate, DslDiagnosticCode::DuplicateName));
    }

    void rejectsPayloadDispatchWithUnknownReferences() {
        const auto unknownSequence = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> missing_units switch (nal_unit_type) {\n"
                           "    case 9: AccessUnitDelimiterRbsp;\n"
                           "}\n")));
        const auto unknownController = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (missing_field) {\n"
                           "    case 9: AccessUnitDelimiterRbsp;\n"
                           "}\n")));
        const auto unknownTarget = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "    case 9: MissingRbsp;\n"
                           "}\n")));
        const auto structureSequence = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> NalUnitHeader switch (nal_unit_type) {\n"
                           "    case 9: AccessUnitDelimiterRbsp;\n"
                           "}\n")));

        QVERIFY(hasDiagnostic(unknownSequence, DslDiagnosticCode::UnknownReference));
        QVERIFY(hasDiagnostic(unknownController, DslDiagnosticCode::UnknownReference));
        QVERIFY(hasDiagnostic(unknownTarget, DslDiagnosticCode::UnknownReference));
        QVERIFY(hasDiagnostic(structureSequence, DslDiagnosticCode::UnknownReference));
    }

    void rejectsInvalidPayloadDispatchControllers() {
        const auto expGolombController = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader { bits<5> nal_unit_type; ue code; }
            struct Body { bits<8> value; }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (code) {
                case 9: Body;
            }
            entry nal_units;
        )"));
        const auto computedController = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader {
                bits<5> nal_unit_type;
                computed<u64> shifted = nal_unit_type + 1;
            }
            struct Body { bits<8> value; }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (shifted) {
                case 9: Body;
            }
            entry nal_units;
        )"));
        const auto arrayController = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader { bits<5> nal_unit_type; bits<4> flags[2]; }
            struct Body { bits<8> value; }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (flags) {
                case 9: Body;
            }
            entry nal_units;
        )"));
        const auto guardedController = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader {
                bits<5> nal_unit_type;
                if (nal_unit_type == 1) {
                    bits<3> guarded_type;
                }
            }
            struct Body { bits<8> value; }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (guarded_type) {
                case 9: Body;
            }
            entry nal_units;
        )"));

        QVERIFY(hasDiagnostic(expGolombController,
                              DslDiagnosticCode::InvalidPayloadDispatch));
        QVERIFY(hasDiagnostic(computedController,
                              DslDiagnosticCode::InvalidPayloadDispatch));
        QVERIFY(hasDiagnostic(arrayController,
                              DslDiagnosticCode::InvalidPayloadDispatch));
        QVERIFY(hasDiagnostic(guardedController,
                              DslDiagnosticCode::InvalidPayloadDispatch));
    }

    void rejectsInvalidPayloadDispatchArms() {
        const auto duplicateCase = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "    case 9: AccessUnitDelimiterRbsp;\n"
                           "    case 9: empty;\n"
                           "}\n")));
        const auto noCase = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "}\n")));
        const auto defaultArm = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "    case 9: AccessUnitDelimiterRbsp;\n"
                           "    default: empty;\n"
                           "}\n")));
        const auto selfTarget = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "    case 9: NalUnitHeader;\n"
                           "}\n")));
        const auto outOfRange = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "    case 32: AccessUnitDelimiterRbsp;\n"
                           "}\n")));

        QVERIFY(hasDiagnostic(duplicateCase, DslDiagnosticCode::InvalidPayloadDispatch));
        QVERIFY(hasDiagnostic(noCase, DslDiagnosticCode::InvalidPayloadDispatch));
        QVERIFY(hasDiagnostic(defaultArm, DslDiagnosticCode::InvalidPayloadDispatch));
        QVERIFY(hasDiagnostic(selfTarget, DslDiagnosticCode::InvalidPayloadDispatch));
        QVERIFY(hasDiagnostic(outOfRange, DslDiagnosticCode::ConstraintOutOfRange));
    }

    void rejectsPayloadDispatchWithoutMatchingEntry() {
        const auto result = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader { bits<5> nal_unit_type; }
            struct AccessUnitDelimiterRbsp { bits<8> value; }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (nal_unit_type) {
                case 9: AccessUnitDelimiterRbsp;
            }
            entry NalUnitHeader;
        )"));

        QVERIFY(hasDiagnostic(result, DslDiagnosticCode::InvalidPayloadDispatch));
    }

    void recoversFromMalformedPayloadDispatchArms() {
        const auto missingColon = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "    case 9 AccessUnitDelimiterRbsp;\n"
                           "}\n")));
        const auto missingSemicolon = DslParser::parse(payloadSource(
            QStringLiteral("payload<rbsp> nal_units switch (nal_unit_type) {\n"
                           "    case 9: AccessUnitDelimiterRbsp\n"
                           "}\n")));

        QVERIFY(!missingColon.succeeded());
        QVERIFY(hasDiagnostic(missingColon, DslDiagnosticCode::MissingToken));
        QVERIFY(!missingSemicolon.succeeded());
        QVERIFY(hasDiagnostic(missingSemicolon, DslDiagnosticCode::MissingToken));
    }

    void reportsLexicalFailuresWithoutCrashingParser() {
        const auto result = DslParser::parse(QStringLiteral(
            "@spec(\"unterminated) struct Header { bits<1> value; } entry Header;"));

        QVERIFY(!result.succeeded());
        QVERIFY(std::any_of(result.diagnostics.begin(),
                            result.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code == DslDiagnosticCode::UnterminatedString;
                            }));
    }
};

QTEST_GUILESS_MAIN(DslTest)

#include "dsl_test.moc"
