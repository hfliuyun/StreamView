#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>

#include <QTest>

#include <algorithm>

using streamview::rules::DslCompileResult;
using streamview::rules::DslCompiler;
using streamview::rules::DslDiagnosticCode;
using streamview::rules::DslEndian;
using streamview::rules::DslEntryKind;
using streamview::rules::DslOpcode;
using streamview::rules::DslParser;
using streamview::rules::DslValueTypeKind;

namespace {

bool hasDiagnostic(const DslCompileResult& result, DslDiagnosticCode code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [code](const auto& diagnostic) { return diagnostic.code == code; });
}

} // namespace

class DslIrTest final : public QObject {
    Q_OBJECT

private slots:
    void compilesResolvedTypesMetadataAndDeterministicBytecode() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "@spec(\"Example\", \"1.2\") struct Header { "
            "bits<1> flag @equals(0) @description(\"Flag.\"); "
            "bits<7> value; } "
            "@index(progressive) sequence<Header> units = scan(h264_start_code); "
            "entry units;"));
        QVERIFY(parsed.succeeded());

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);
        QVERIFY(first.succeeded());
        QVERIFY(second.succeeded());
        QCOMPARE(first.program->entry.kind, DslEntryKind::Sequence);
        QCOMPARE(first.program->entry.targetIndex, quint32(0));
        QCOMPARE(first.program->scans.size(), std::size_t(1));
        QCOMPARE(first.program->scans.front().elementStructIndex, quint32(0));
        QCOMPARE(first.program->structs.size(), std::size_t(1));

        const auto& structure = first.program->structs.front();
        QCOMPARE(structure.fields.size(), std::size_t(2));
        QCOMPARE(structure.fields.at(0).type.kind, DslValueTypeKind::UnsignedBits);
        QCOMPARE(structure.fields.at(0).type.bitWidth, quint8(1));
        QCOMPARE(structure.fields.at(0).equalsConstraint, std::optional<quint64>(0));
        QCOMPARE(structure.fields.at(0).metadata.description, QStringLiteral("Flag."));
        QCOMPARE(structure.fields.at(1).type.bitWidth, quint8(7));
        QVERIFY(structure.fields.at(1).metadata.specification.has_value());
        QCOMPARE(structure.fields.at(1).metadata.specification->standard,
                 QStringLiteral("Example"));

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertEquals,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::EndStructure,
        };
        QCOMPARE(first.program->bytecode.size(), expected.size());
        QCOMPARE(first.program->bytecode.size(), second.program->bytecode.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(first.program->bytecode.at(index).opcode, expected.at(index));
            QCOMPARE(first.program->bytecode.at(index).opcode,
                     second.program->bytecode.at(index).opcode);
            QCOMPARE(first.program->bytecode.at(index).operand,
                     second.program->bytecode.at(index).operand);
            QCOMPARE(first.program->bytecode.at(index).immediate,
                     second.program->bytecode.at(index).immediate);
        }
    }

    void compilesEnumAndExplicitEndianIntoTypedIr() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum NalUnitType { non_idr = 1; idr = 5; } "
            "struct Header { bits<16, little> value; "
            "bits<3> type @enum(NalUnitType); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        QCOMPARE(compiled.program->enums.size(), std::size_t(1));
        QCOMPARE(compiled.program->enumIndex(QStringLiteral("NalUnitType")),
                 std::optional<quint32>(0));
        const auto& fields = compiled.program->structs.front().fields;
        QCOMPARE(fields.at(0).type.endian, DslEndian::Little);
        QCOMPARE(fields.at(0).type.kind, DslValueTypeKind::UnsignedBits);
        QCOMPARE(fields.at(1).type.kind, DslValueTypeKind::Enum);
        QCOMPARE(fields.at(1).type.enumIndex, std::optional<quint32>(0));
        QCOMPARE(fields.at(1).metadata.typeName, QStringLiteral("NalUnitType"));
    }

    void compilesExpGolombFieldsIntoTypedIrAndBytecode() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct SliceHeader { ue first_mb_in_slice; "
            "se slice_qp_delta @description(\"QP delta.\"); } entry SliceHeader;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& fields = compiled.program->structs.front().fields;
        QCOMPARE(fields.size(), std::size_t(2));
        QCOMPARE(fields.at(0).type.kind, DslValueTypeKind::UnsignedExpGolomb);
        QCOMPARE(fields.at(0).metadata.typeName, QStringLiteral("ue"));
        QCOMPARE(fields.at(1).type.kind, DslValueTypeKind::SignedExpGolomb);
        QCOMPARE(fields.at(1).metadata.typeName, QStringLiteral("se"));
        QCOMPARE(fields.at(1).metadata.description, QStringLiteral("QP delta."));

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadSignedExpGolomb,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
    }

    void expandsFixedLengthArraysIntoDeterministicTypedFieldsAndBytecode() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> flags[3] @description(\"Flags.\") @equals(0); "
            "ue codes[2]; se deltas[2]; } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& fields = compiled.program->structs.front().fields;
        QCOMPARE(fields.size(), std::size_t(7));
        QCOMPARE(fields.at(0).name, QStringLiteral("flags[0]"));
        QCOMPARE(fields.at(1).name, QStringLiteral("flags[1]"));
        QCOMPARE(fields.at(2).name, QStringLiteral("flags[2]"));
        QCOMPARE(fields.at(0).type.kind, DslValueTypeKind::UnsignedBits);
        QCOMPARE(fields.at(0).metadata.description, QStringLiteral("Flags."));
        QCOMPARE(fields.at(0).equalsConstraint, std::optional<quint64>(0));
        QCOMPARE(fields.at(3).name, QStringLiteral("codes[0]"));
        QCOMPARE(fields.at(4).name, QStringLiteral("codes[1]"));
        QCOMPARE(fields.at(3).type.kind, DslValueTypeKind::UnsignedExpGolomb);
        QCOMPARE(fields.at(5).name, QStringLiteral("deltas[0]"));
        QCOMPARE(fields.at(6).name, QStringLiteral("deltas[1]"));
        QCOMPARE(fields.at(5).type.kind, DslValueTypeKind::SignedExpGolomb);

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertEquals,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertEquals,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertEquals,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadSignedExpGolomb,
            DslOpcode::ReadSignedExpGolomb,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
    }

    void propagatesEnumTypesAcrossFixedArrayElements() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum Type { one = 1; two = 2; } "
            "struct Header { bits<2> values[2] @enum(Type); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& fields = compiled.program->structs.front().fields;
        QCOMPARE(fields.size(), std::size_t(2));
        QCOMPARE(fields.at(0).name, QStringLiteral("values[0]"));
        QCOMPARE(fields.at(1).name, QStringLiteral("values[1]"));
        for (const auto& field : fields) {
            QCOMPARE(field.type.kind, DslValueTypeKind::Enum);
            QCOMPARE(field.type.enumIndex, std::optional<quint32>(0));
            QCOMPARE(field.metadata.typeName, QStringLiteral("Type"));
        }
    }

    void rejectsEnumValuesThatDoNotFitAndUnalignedLittleEndianFields() {
        const auto tooWide = DslParser::parse(QStringLiteral(
            "enum Type { too_large = 8; } "
            "struct Header { bits<3> value @enum(Type); } entry Header;"));
        const auto parsedTooWide = DslCompiler::compile(tooWide.program);
        QVERIFY(!parsedTooWide.succeeded());
        QVERIFY(hasDiagnostic(parsedTooWide, DslDiagnosticCode::EnumValueOutOfRange));

        const auto unaligned = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> prefix; bits<16, little> value; } entry Header;"));
        const auto compiledUnaligned = DslCompiler::compile(unaligned.program);
        QVERIFY(!compiledUnaligned.succeeded());
        QVERIFY(hasDiagnostic(compiledUnaligned, DslDiagnosticCode::InvalidEndian));
    }

    void rejectsConstraintsOutsideTheStaticFieldType() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag @equals(2); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);

        QVERIFY(!compiled.succeeded());
        QVERIFY(!compiled.program.has_value());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::ConstraintOutOfRange));
    }

    void rejectsExpGolombAnnotationsAndUnknownLittleEndianAlignmentInCompiler() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { ue prefix @equals(0); bits<16, little> value; } entry Header;"));
        QVERIFY(!parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(!compiled.succeeded());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidAnnotation));
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidEndian));

        const auto variableArray = DslParser::parse(QStringLiteral(
            "struct Header { ue values[2]; bits<16, little> value; } entry Header;"));
        const auto compiledVariableArray = DslCompiler::compile(variableArray.program);
        QVERIFY(!compiledVariableArray.succeeded());
        QVERIFY(hasDiagnostic(compiledVariableArray, DslDiagnosticCode::InvalidEndian));
    }

    void rejectsInvalidOrSandboxExceedingFixedArrayLengthsInCompiler() {
        const auto zero = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flags[0]; } entry Header;"));
        const auto atLimit = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flags[99998]; bits<1> tail; } entry Header;"));
        const auto overLimit = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flags[99999]; bits<1> tail; } entry Header;"));
        QVERIFY(!zero.succeeded());
        QVERIFY(atLimit.succeeded());
        QVERIFY(overLimit.succeeded());

        const auto compiledZero = DslCompiler::compile(zero.program);
        const auto compiledAtLimit = DslCompiler::compile(atLimit.program);
        const auto compiledOverLimit = DslCompiler::compile(overLimit.program);
        QVERIFY(!compiledZero.succeeded());
        QCOMPARE(compiledZero.diagnostics.size(), std::size_t(1));
        QVERIFY(compiledAtLimit.succeeded());
        QCOMPARE(compiledAtLimit.program->structs.front().fields.size(), std::size_t(99'999));
        QVERIFY(!compiledOverLimit.succeeded());
        QCOMPARE(compiledOverLimit.diagnostics.size(), std::size_t(1));
        QVERIFY(hasDiagnostic(compiledZero, DslDiagnosticCode::InvalidArrayLength));
        QVERIFY(hasDiagnostic(compiledOverLimit, DslDiagnosticCode::InvalidArrayLength));
    }

    void computesCompilerAlignmentAcrossExpandedFixedArrays() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<4> prefix[2]; bits<16, little> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        QCOMPARE(compiled.program->structs.front().fields.size(), std::size_t(3));
        QCOMPARE(compiled.program->structs.front().fields.at(2).type.endian,
                 DslEndian::Little);
    }

    void rejectsDuplicateEqualsConstraints() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag @equals(0) @equals(1); } entry Header;"));

        QVERIFY(!parsed.succeeded());
        QVERIFY(std::any_of(parsed.diagnostics.begin(),
                            parsed.diagnostics.end(),
                            [](const auto& diagnostic) {
                                return diagnostic.code == DslDiagnosticCode::InvalidAnnotation &&
                                       diagnostic.message.contains(QStringLiteral("at most once"));
                            }));

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(!compiled.succeeded());
        QVERIFY(!compiled.program.has_value());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidAnnotation));
    }

    void refusesInvalidParserOutputAsExecutableIr() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<0> flag; } entry Header;"));
        QVERIFY(!parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);

        QVERIFY(!compiled.succeeded());
        QVERIFY(!compiled.program.has_value());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidBitWidth));
    }
};

QTEST_GUILESS_MAIN(DslIrTest)

#include "dsl_ir_test.moc"
