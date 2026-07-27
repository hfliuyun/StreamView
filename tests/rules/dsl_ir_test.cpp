#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>

#include <QTest>

#include <algorithm>

using streamview::rules::DslCompileResult;
using streamview::rules::DslCompiler;
using streamview::rules::DslConditionOperator;
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

bool hasDiagnostic(const streamview::rules::DslParseResult& result,
                   DslDiagnosticCode code) {
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

    void lowersBoundedRepeatsToDeterministicGuardedFieldProjections() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 2) { "
            "bits<1> selected; if (selected == 1) { ue code; } "
            "bits<2> flags[2] @equals(0); } bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);
        QVERIFY2(first.succeeded(),
                 first.diagnostics.empty()
                     ? ""
                     : qPrintable(first.diagnostics.front().message));
        QVERIFY(second.succeeded());
        const auto& structure = first.program->structs.front();
        const auto& fields = structure.fields;
        const std::vector<QString> expectedNames{
            QStringLiteral("count"),       QStringLiteral("selected[0]"),
            QStringLiteral("code[0]"),     QStringLiteral("flags[0][0]"),
            QStringLiteral("flags[0][1]"), QStringLiteral("selected[1]"),
            QStringLiteral("code[1]"),     QStringLiteral("flags[1][0]"),
            QStringLiteral("flags[1][1]"), QStringLiteral("tail"),
        };
        QCOMPARE(fields.size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            QCOMPARE(fields.at(index).name, expectedNames.at(index));
        }

        for (const std::size_t index : {std::size_t(1), std::size_t(3), std::size_t(4)}) {
            QCOMPARE(fields.at(index).conditions.size(), std::size_t(1));
            QCOMPARE(fields.at(index).conditions.front().fieldIndex, quint32(0));
            QCOMPARE(fields.at(index).conditions.front().expectedValue, quint64(0));
            QCOMPARE(fields.at(index).conditions.front().op,
                     DslConditionOperator::GreaterThan);
            QVERIFY(!fields.at(index).conditions.front().negated);
        }
        QCOMPARE(fields.at(2).conditions.size(), std::size_t(2));
        QCOMPARE(fields.at(2).conditions.at(0).op, DslConditionOperator::GreaterThan);
        QCOMPARE(fields.at(2).conditions.at(1).op, DslConditionOperator::Equal);
        QCOMPARE(fields.at(2).conditions.at(1).fieldIndex, quint32(1));
        QCOMPARE(fields.at(6).conditions.size(), std::size_t(2));
        QCOMPARE(fields.at(6).conditions.at(0).expectedValue, quint64(1));
        QCOMPARE(fields.at(6).conditions.at(1).fieldIndex, quint32(5));
        QVERIFY(fields.at(0).conditions.empty());
        QVERIFY(fields.at(9).conditions.empty());

        QCOMPARE(structure.repeatBounds.size(), std::size_t(1));
        QCOMPARE(structure.repeatBounds.front().controllerFieldIndex, quint32(0));
        QCOMPARE(structure.repeatBounds.front().firstFieldIndex, quint32(1));
        QCOMPARE(structure.repeatBounds.front().maximumCount, quint64(2));
        QVERIFY(structure.repeatBounds.front().conditions.empty());

        const std::vector<DslOpcode> expectedOpcodes{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertRepeatCount,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertEquals,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertEquals,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertEquals,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertEquals,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::EndStructure,
        };
        QCOMPARE(first.program->bytecode.size(), expectedOpcodes.size());
        QCOMPARE(first.program->bytecode.size(), second.program->bytecode.size());
        for (std::size_t index = 0; index < expectedOpcodes.size(); ++index) {
            QCOMPARE(first.program->bytecode.at(index).opcode, expectedOpcodes.at(index));
            QCOMPARE(first.program->bytecode.at(index).opcode,
                     second.program->bytecode.at(index).opcode);
            QCOMPARE(first.program->bytecode.at(index).operand,
                     second.program->bytecode.at(index).operand);
            QCOMPARE(first.program->bytecode.at(index).immediate,
                     second.program->bytecode.at(index).immediate);
        }
    }

    void resolvesUnsignedExpGolombCountsAcrossNestedRepeatScopes() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> outer_count; repeat (outer_count, 2) { "
            "ue inner_count; repeat (inner_count, 2) { bits<1> value; } } } "
            "entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& structure = compiled.program->structs.front();
        const auto& fields = structure.fields;
        const std::vector<QString> expectedNames{
            QStringLiteral("outer_count"), QStringLiteral("inner_count[0]"),
            QStringLiteral("value[0][0]"), QStringLiteral("value[0][1]"),
            QStringLiteral("inner_count[1]"), QStringLiteral("value[1][0]"),
            QStringLiteral("value[1][1]"),
        };
        QCOMPARE(fields.size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            QCOMPARE(fields.at(index).name, expectedNames.at(index));
        }
        QCOMPARE(structure.repeatBounds.size(), std::size_t(3));
        QCOMPARE(structure.repeatBounds.at(0).controllerFieldIndex, quint32(0));
        QCOMPARE(structure.repeatBounds.at(0).firstFieldIndex, quint32(1));
        QCOMPARE(structure.repeatBounds.at(1).controllerFieldIndex, quint32(1));
        QCOMPARE(structure.repeatBounds.at(1).firstFieldIndex, quint32(2));
        QCOMPARE(structure.repeatBounds.at(2).controllerFieldIndex, quint32(4));
        QCOMPARE(structure.repeatBounds.at(2).firstFieldIndex, quint32(5));
        QCOMPARE(structure.repeatBounds.at(1).conditions.size(), std::size_t(1));
        QCOMPARE(structure.repeatBounds.at(1).conditions.front().fieldIndex, quint32(0));
        QCOMPARE(structure.repeatBounds.at(1).conditions.front().op,
                 DslConditionOperator::GreaterThan);
        QCOMPARE(structure.repeatBounds.at(2).conditions.front().expectedValue, quint64(1));
        QCOMPARE(fields.at(2).conditions.at(1).fieldIndex, quint32(1));
        QCOMPARE(fields.at(5).conditions.at(1).fieldIndex, quint32(4));
    }

    void countsBoundedRepeatProjectionsAgainstTheStructureLimit() {
        const auto atLimit = DslParser::parse(QStringLiteral(
            "struct Header { bits<16> count; repeat (count, 49999) { "
            "bits<1> first; bits<1> second; } } entry Header;"));
        const auto overLimit = DslParser::parse(QStringLiteral(
            "struct Header { bits<16> count; repeat (count, 49999) { "
            "bits<1> first; bits<1> second; } bits<1> tail; } entry Header;"));
        QVERIFY(atLimit.succeeded());
        QVERIFY(overLimit.succeeded());

        const auto accepted = DslCompiler::compile(atLimit.program);
        const auto rejected = DslCompiler::compile(overLimit.program);
        QVERIFY2(accepted.succeeded(),
                 accepted.diagnostics.empty()
                     ? ""
                     : qPrintable(accepted.diagnostics.front().message));
        QCOMPARE(accepted.program->structs.front().fields.size(), std::size_t(99999));
        QVERIFY(!rejected.succeeded());
        QVERIFY(hasDiagnostic(rejected, DslDiagnosticCode::InvalidArrayLength));
    }

    void rejectsInvalidRepeatAlignmentAndMalformedCompilerInput() {
        const auto repeatedUnaligned = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> count; repeat (count, 2) { "
            "bits<8, little> value; bits<4> padding; } } entry Header;"));
        const auto tailUnaligned = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 2) { bits<8> value; } "
            "bits<16, little> tail; } entry Header;"));
        const auto valid = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 2) { bits<1> value; } } "
            "entry Header;"));
        QVERIFY(repeatedUnaligned.succeeded());
        QVERIFY(hasDiagnostic(tailUnaligned, DslDiagnosticCode::InvalidEndian));
        QVERIFY(valid.succeeded());

        const auto badAlignment = DslCompiler::compile(repeatedUnaligned.program);
        QVERIFY(hasDiagnostic(badAlignment, DslDiagnosticCode::InvalidEndian));

        auto malformed = valid.program;
        malformed.structs.front().items.at(1).repeatMaximum = 0;
        const auto malformedResult = DslCompiler::compile(malformed);
        QVERIFY(hasDiagnostic(malformedResult, DslDiagnosticCode::InvalidArrayLength));
    }

    void lowersEqualityConditionalBlocksToDeterministicGuardedFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; "
            "if (kind == 1) { bits<3> then_value; bits<1> flags[2] @equals(0); } "
            "else { bits<5> else_value; } bits<4> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);
        QVERIFY2(first.succeeded(),
                 first.diagnostics.empty()
                     ? ""
                     : qPrintable(first.diagnostics.front().message));
        QVERIFY(second.succeeded());
        const auto& fields = first.program->structs.front().fields;
        QCOMPARE(fields.size(), std::size_t(6));
        const std::vector<QString> names{
            QStringLiteral("kind"),
            QStringLiteral("then_value"),
            QStringLiteral("flags[0]"),
            QStringLiteral("flags[1]"),
            QStringLiteral("else_value"),
            QStringLiteral("tail"),
        };
        for (std::size_t index = 0; index < names.size(); ++index) {
            QCOMPARE(fields.at(index).name, names.at(index));
        }
        for (std::size_t index = 1; index <= 3; ++index) {
            QCOMPARE(fields.at(index).conditions.size(), std::size_t(1));
            QCOMPARE(fields.at(index).conditions.front().fieldIndex, quint32(0));
            QCOMPARE(fields.at(index).conditions.front().expectedValue, quint64(1));
            QVERIFY(!fields.at(index).conditions.front().negated);
        }
        QCOMPARE(fields.at(4).conditions.size(), std::size_t(1));
        QCOMPARE(fields.at(4).conditions.front().fieldIndex, quint32(0));
        QCOMPARE(fields.at(4).conditions.front().expectedValue, quint64(1));
        QVERIFY(fields.at(4).conditions.front().negated);
        QVERIFY(fields.at(0).conditions.empty());
        QVERIFY(fields.at(5).conditions.empty());

        QCOMPARE(first.program->bytecode.size(), std::size_t(10));
        QCOMPARE(first.program->bytecode.size(), second.program->bytecode.size());
        for (std::size_t index = 0; index < first.program->bytecode.size(); ++index) {
            QCOMPARE(first.program->bytecode.at(index).opcode,
                     second.program->bytecode.at(index).opcode);
            QCOMPARE(first.program->bytecode.at(index).operand,
                     second.program->bytecode.at(index).operand);
            QCOMPARE(first.program->bytecode.at(index).immediate,
                     second.program->bytecode.at(index).immediate);
        }
    }

    void stacksNestedConditionalGuardsAcrossArraysAndAlternatives() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum Kind { zero = 0; one = 1; } "
            "struct Header { bits<1> outer @enum(Kind); "
            "if (outer == 1) { bits<1> inner; "
            "if (inner == 0) { bits<2> nested[2]; } else { ue other; } } "
            "else { bits<4> alternative; } bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& fields = compiled.program->structs.front().fields;
        QCOMPARE(fields.size(), std::size_t(7));
        QCOMPARE(fields.at(0).name, QStringLiteral("outer"));
        QCOMPARE(fields.at(1).name, QStringLiteral("inner"));
        QCOMPARE(fields.at(2).name, QStringLiteral("nested[0]"));
        QCOMPARE(fields.at(3).name, QStringLiteral("nested[1]"));
        QCOMPARE(fields.at(4).name, QStringLiteral("other"));
        QCOMPARE(fields.at(5).name, QStringLiteral("alternative"));
        QCOMPARE(fields.at(6).name, QStringLiteral("tail"));

        QCOMPARE(fields.at(1).conditions.size(), std::size_t(1));
        QCOMPARE(fields.at(1).conditions.front().fieldIndex, quint32(0));
        QVERIFY(!fields.at(1).conditions.front().negated);
        for (std::size_t index = 2; index <= 3; ++index) {
            QCOMPARE(fields.at(index).conditions.size(), std::size_t(2));
            QCOMPARE(fields.at(index).conditions.at(0).fieldIndex, quint32(0));
            QCOMPARE(fields.at(index).conditions.at(0).expectedValue, quint64(1));
            QVERIFY(!fields.at(index).conditions.at(0).negated);
            QCOMPARE(fields.at(index).conditions.at(1).fieldIndex, quint32(1));
            QCOMPARE(fields.at(index).conditions.at(1).expectedValue, quint64(0));
            QVERIFY(!fields.at(index).conditions.at(1).negated);
        }
        QCOMPARE(fields.at(4).conditions.size(), std::size_t(2));
        QVERIFY(fields.at(4).conditions.at(1).negated);
        QCOMPARE(fields.at(5).conditions.size(), std::size_t(1));
        QVERIFY(fields.at(5).conditions.front().negated);
        QVERIFY(fields.at(6).conditions.empty());
    }

    void rejectsInvalidConditionalControllersInCompiler() {
        struct Case final {
            QString source;
            DslDiagnosticCode diagnostic;
        };
        const std::vector<Case> cases{
            {QStringLiteral(
                 "struct Header { if (kind == 1) { bits<1> value; } bits<1> kind; } "
                 "entry Header;"),
             DslDiagnosticCode::UnknownReference},
            {QStringLiteral(
                 "struct Header { bits<1> flags[2]; "
                 "if (flags == 1) { bits<1> value; } } entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { ue code; if (code == 1) { bits<1> value; } } "
                 "entry Header;"),
             DslDiagnosticCode::InvalidType},
            {QStringLiteral(
                 "struct Header { bits<1> flag; "
                 "if (flag == 2) { bits<1> value; } } entry Header;"),
             DslDiagnosticCode::ConstraintOutOfRange},
            {QStringLiteral(
                 "struct Header { bits<1> flag; if (flag == 1) { bits<1> local; } "
                 "if (local == 1) { bits<1> value; } } entry Header;"),
             DslDiagnosticCode::InvalidCondition},
        };

        for (const Case& testCase : cases) {
            const auto parsed = DslParser::parse(testCase.source);
            QVERIFY(!parsed.succeeded());
            const auto compiled = DslCompiler::compile(parsed.program);
            QVERIFY(!compiled.succeeded());
            QVERIFY(!compiled.program.has_value());
            QVERIFY(hasDiagnostic(compiled, testCase.diagnostic));
        }
    }

    void preservesStaticAlignmentOnlyAcrossEqualConditionalWidths() {
        const auto aligned = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> kind; if (kind == 1) { bits<8> first; } "
            "else { bits<8> second; } bits<16, little> tail; } entry Header;"));
        const auto unaligned = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> kind; if (kind == 1) { bits<8> first; } "
            "else { bits<16> second; } bits<16, little> tail; } entry Header;"));
        QVERIFY(aligned.succeeded());
        QVERIFY(!unaligned.succeeded());

        const auto compiledAligned = DslCompiler::compile(aligned.program);
        const auto compiledUnaligned = DslCompiler::compile(unaligned.program);
        QVERIFY(compiledAligned.succeeded());
        QVERIFY(!compiledUnaligned.succeeded());
        QVERIFY(hasDiagnostic(compiledUnaligned, DslDiagnosticCode::InvalidEndian));
    }

    void countsAllConditionalBranchFieldsAgainstTheExpansionLimit() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> kind; "
            "if (kind == 1) { bits<1> first[49999]; } "
            "else { bits<1> second[50000]; } } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(!compiled.succeeded());
        QVERIFY(!compiled.program.has_value());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidArrayLength));
    }

    void lowersEqualitySwitchArmsToDeterministicGuardedFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 1: { bits<3> compact_value; } "
            "case 2: { bits<1> flags[2] @equals(0); } "
            "default: { bits<4> unknown_value; } } bits<2> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);
        QVERIFY2(first.succeeded(),
                 first.diagnostics.empty()
                     ? ""
                     : qPrintable(first.diagnostics.front().message));
        QVERIFY(second.succeeded());
        const auto& fields = first.program->structs.front().fields;
        const std::vector<QString> names{
            QStringLiteral("kind"),
            QStringLiteral("compact_value"),
            QStringLiteral("flags[0]"),
            QStringLiteral("flags[1]"),
            QStringLiteral("unknown_value"),
            QStringLiteral("tail"),
        };
        QCOMPARE(fields.size(), names.size());
        for (std::size_t index = 0; index < names.size(); ++index) {
            QCOMPARE(fields.at(index).name, names.at(index));
        }
        QCOMPARE(fields.at(1).conditions.size(), std::size_t(1));
        QCOMPARE(fields.at(1).conditions.front().fieldIndex, quint32(0));
        QCOMPARE(fields.at(1).conditions.front().expectedValue, quint64(1));
        QVERIFY(!fields.at(1).conditions.front().negated);
        for (std::size_t index = 2; index <= 3; ++index) {
            QCOMPARE(fields.at(index).conditions.size(), std::size_t(1));
            QCOMPARE(fields.at(index).conditions.front().fieldIndex, quint32(0));
            QCOMPARE(fields.at(index).conditions.front().expectedValue, quint64(2));
            QVERIFY(!fields.at(index).conditions.front().negated);
        }
        QCOMPARE(fields.at(4).conditions.size(), std::size_t(2));
        QCOMPARE(fields.at(4).conditions.at(0).fieldIndex, quint32(0));
        QCOMPARE(fields.at(4).conditions.at(0).expectedValue, quint64(1));
        QVERIFY(fields.at(4).conditions.at(0).negated);
        QCOMPARE(fields.at(4).conditions.at(1).fieldIndex, quint32(0));
        QCOMPARE(fields.at(4).conditions.at(1).expectedValue, quint64(2));
        QVERIFY(fields.at(4).conditions.at(1).negated);
        QVERIFY(fields.at(0).conditions.empty());
        QVERIFY(fields.at(5).conditions.empty());

        QCOMPARE(first.program->bytecode.size(), std::size_t(10));
        QCOMPARE(first.program->bytecode.size(), second.program->bytecode.size());
        for (std::size_t index = 0; index < first.program->bytecode.size(); ++index) {
            QCOMPARE(first.program->bytecode.at(index).opcode,
                     second.program->bytecode.at(index).opcode);
            QCOMPARE(first.program->bytecode.at(index).operand,
                     second.program->bytecode.at(index).operand);
            QCOMPARE(first.program->bytecode.at(index).immediate,
                     second.program->bytecode.at(index).immediate);
        }
    }

    void stacksNestedSwitchAndConditionalGuards() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 1: { bits<1> flag; if (flag == 1) { bits<2> nested; } } "
            "case 2: { bits<3> alternative; } "
            "default: { bits<4> fallback; } } } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& fields = compiled.program->structs.front().fields;
        QCOMPARE(fields.size(), std::size_t(5));
        QCOMPARE(fields.at(2).name, QStringLiteral("nested"));
        QCOMPARE(fields.at(2).conditions.size(), std::size_t(2));
        QCOMPARE(fields.at(2).conditions.at(0).fieldIndex, quint32(0));
        QCOMPARE(fields.at(2).conditions.at(0).expectedValue, quint64(1));
        QVERIFY(!fields.at(2).conditions.at(0).negated);
        QCOMPARE(fields.at(2).conditions.at(1).fieldIndex, quint32(1));
        QCOMPARE(fields.at(2).conditions.at(1).expectedValue, quint64(1));
        QVERIFY(!fields.at(2).conditions.at(1).negated);
        QCOMPARE(fields.at(4).name, QStringLiteral("fallback"));
        QCOMPARE(fields.at(4).conditions.size(), std::size_t(2));
        QVERIFY(fields.at(4).conditions.at(0).negated);
        QVERIFY(fields.at(4).conditions.at(1).negated);
    }

    void resolvesEnumControllersForEqualitySwitches() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum Kind { compact = 1; extended = 2; } "
            "struct Header { bits<2> kind @enum(Kind); switch (kind) { "
            "case 1: { bits<3> compact_value; } "
            "case 2: { bits<5> extended_value; } } } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& fields = compiled.program->structs.front().fields;
        QCOMPARE(fields.at(0).type.kind, DslValueTypeKind::Enum);
        QCOMPARE(fields.at(1).conditions.size(), std::size_t(1));
        QCOMPARE(fields.at(1).conditions.front().fieldIndex, quint32(0));
        QCOMPARE(fields.at(1).conditions.front().expectedValue, quint64(1));
        QCOMPARE(fields.at(2).conditions.size(), std::size_t(1));
        QCOMPARE(fields.at(2).conditions.front().fieldIndex, quint32(0));
        QCOMPARE(fields.at(2).conditions.front().expectedValue, quint64(2));
    }

    void preservesStaticAlignmentOnlyAcrossCompleteEqualSwitchWidths() {
        const auto aligned = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> kind; switch (kind) { "
            "case 1: { bits<8> first; } case 2: { bits<8> second; } "
            "default: { bits<8> fallback; } } bits<16, little> tail; } entry Header;"));
        const auto unequal = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> kind; switch (kind) { "
            "case 1: { bits<8> first; } case 2: { bits<16> second; } "
            "default: { bits<8> fallback; } } bits<16, little> tail; } entry Header;"));
        const auto missingDefault = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> kind; switch (kind) { "
            "case 1: { bits<8> first; } } bits<16, little> tail; } entry Header;"));
        QVERIFY(aligned.succeeded());
        QVERIFY(!unequal.succeeded());
        QVERIFY(!missingDefault.succeeded());

        QVERIFY(DslCompiler::compile(aligned.program).succeeded());
        const auto compiledUnequal = DslCompiler::compile(unequal.program);
        const auto compiledMissingDefault = DslCompiler::compile(missingDefault.program);
        QVERIFY(!compiledUnequal.succeeded());
        QVERIFY(!compiledMissingDefault.succeeded());
        QVERIFY(hasDiagnostic(compiledUnequal, DslDiagnosticCode::InvalidEndian));
        QVERIFY(hasDiagnostic(compiledMissingDefault, DslDiagnosticCode::InvalidEndian));
    }

    void rejectsMalformedSwitchesAndCountsEveryArmAgainstTheFieldLimit() {
        const auto duplicate = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 1: { bits<1> first; } case 1: { bits<1> second; } } } "
            "entry Header;"));
        const auto noCase = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "default: { bits<1> value; } } } entry Header;"));
        const auto expanded = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> kind; switch (kind) { "
            "case 0: { bits<1> first[49999]; } "
            "case 1: { bits<1> second[50000]; } } } entry Header;"));

        const auto compiledDuplicate = DslCompiler::compile(duplicate.program);
        const auto compiledNoCase = DslCompiler::compile(noCase.program);
        const auto compiledExpanded = DslCompiler::compile(expanded.program);
        QVERIFY(!compiledDuplicate.succeeded());
        QVERIFY(!compiledNoCase.succeeded());
        QVERIFY(!compiledExpanded.succeeded());
        QVERIFY(hasDiagnostic(compiledDuplicate, DslDiagnosticCode::InvalidCondition));
        QVERIFY(hasDiagnostic(compiledNoCase, DslDiagnosticCode::InvalidCondition));
        QVERIFY(hasDiagnostic(compiledExpanded, DslDiagnosticCode::InvalidArrayLength));
    }

    void keepsMalformedSwitchGuardSlotsAlignedDuringDefensiveCompilation() {
        auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> kind; switch (kind) { "
            "case 0: { bits<1> local; } "
            "case 1: { if (local == 0) { bits<1> value; } } } } entry Header;"));
        QVERIFY(!parsed.succeeded());
        parsed.program.structs.front().items.at(1).switchArms.at(0).caseValue = 2;

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(!compiled.succeeded());
        QVERIFY(!compiled.program.has_value());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::ConstraintOutOfRange));
        QVERIFY(!hasDiagnostic(compiled, DslDiagnosticCode::InvalidCondition));
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
