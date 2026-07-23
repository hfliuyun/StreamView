#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>

#include <QTest>

#include <algorithm>

using streamview::rules::DslCompileResult;
using streamview::rules::DslCompiler;
using streamview::rules::DslDiagnosticCode;
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

    void rejectsConstraintsOutsideTheStaticFieldType() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag @equals(2); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);

        QVERIFY(!compiled.succeeded());
        QVERIFY(!compiled.program.has_value());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::ConstraintOutOfRange));
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
