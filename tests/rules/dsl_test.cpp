#include <streamview/rules/dsl.h>

#include <QTest>

#include <algorithm>
#include <optional>

using streamview::rules::DslAnnotationValueKind;
using streamview::rules::DslDiagnosticCode;
using streamview::rules::DslEndian;
using streamview::rules::DslFieldEncoding;
using streamview::rules::DslLexer;
using streamview::rules::DslParser;
using streamview::rules::DslTokenKind;

namespace {

[[nodiscard]] bool hasDiagnostic(const streamview::rules::DslParseResult& result,
                                 DslDiagnosticCode code) {
    return std::any_of(result.diagnostics.begin(),
                       result.diagnostics.end(),
                       [code](const auto& diagnostic) { return diagnostic.code == code; });
}

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
        QCOMPARE(structure.fields.size(), std::size_t(3));
        QCOMPARE(structure.fields.at(0).width, quint8(1));
        QCOMPARE(structure.fields.at(1).width, quint8(2));
        QCOMPARE(structure.fields.at(2).width, quint8(5));
        QCOMPARE(structure.fields.front().annotations.front().name,
                 QStringLiteral("equals"));
        QCOMPARE(structure.fields.front().annotations.front().arguments.front().integerValue,
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
        QCOMPARE(result.program.structs.front().fields.at(0).endian, DslEndian::Little);
        QCOMPARE(result.program.structs.front().fields.at(1).endian, DslEndian::Big);
        QCOMPARE(result.program.structs.front().fields.at(1).annotations.back().name,
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
        QCOMPARE(result.program.structs.front().fields.size(), std::size_t(2));
        QCOMPARE(result.program.structs.front().fields.at(0).name,
                 QStringLiteral("first_mb_in_slice"));
        QCOMPARE(result.program.structs.front().fields.at(0).encoding,
                 DslFieldEncoding::UnsignedExpGolomb);
        QCOMPARE(result.program.structs.front().fields.at(1).name,
                 QStringLiteral("slice_qp_delta"));
        QCOMPARE(result.program.structs.front().fields.at(1).encoding,
                 DslFieldEncoding::SignedExpGolomb);
    }

    void rejectsFixedWidthAnnotationsAndAlignmentAfterExpGolombFields() {
        const auto annotations = DslParser::parse(QStringLiteral(
            "enum Type { value = 1; } struct Header { "
            "ue first @equals(0); se second @enum(Type); } entry Header;"));
        const auto alignment = DslParser::parse(QStringLiteral(
            "struct Header { ue prefix; bits<16, little> value; } entry Header;"));

        QVERIFY(hasDiagnostic(annotations, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(alignment, DslDiagnosticCode::InvalidEndian));
    }

    void parsesFixedLengthArraysForAllScalarFieldEncodings() {
        const auto result = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> flags[3] @description(\"Flags.\"); "
            "ue codes[2]; se deltas[2]; } entry Header;"));

        QVERIFY2(result.succeeded(),
                 result.diagnostics.empty()
                     ? ""
                     : qPrintable(result.diagnostics.front().message));
        QCOMPARE(result.program.structs.front().fields.size(), std::size_t(3));
        const auto& flags = result.program.structs.front().fields.at(0);
        QCOMPARE(flags.name, QStringLiteral("flags"));
        QCOMPARE(flags.arrayLength, std::optional<quint64>(3));
        QCOMPARE(flags.annotations.back().name, QStringLiteral("description"));
        QCOMPARE(result.program.structs.front().fields.at(1).arrayLength,
                 std::optional<quint64>(2));
        QCOMPARE(result.program.structs.front().fields.at(2).arrayLength,
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

    void rejectsMissingOrUnknownEntry() {
        const auto missing =
            DslParser::parse(QStringLiteral("struct Header { bits<1> value; }"));
        const auto unknown = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> value; } entry Missing;"));

        QVERIFY(hasDiagnostic(missing, DslDiagnosticCode::MissingEntry));
        QVERIFY(hasDiagnostic(unknown, DslDiagnosticCode::UnknownReference));
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
