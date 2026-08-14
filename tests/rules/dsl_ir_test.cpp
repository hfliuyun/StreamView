#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>

#include <QTest>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

using streamview::rules::DslCompileResult;
using streamview::rules::DslCompiler;
using streamview::rules::DslConditionOperator;
using streamview::rules::DslDiagnosticCode;
using streamview::rules::DslEndian;
using streamview::rules::DslEntryKind;
using streamview::rules::DslOpcode;
using streamview::rules::DslParser;
using streamview::rules::DslScalarType;
using streamview::rules::DslTypedExpressionKind;
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
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

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

    void lowersSourceAnchoredAssertionsAtTheirStatementPosition() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct Header {
                bits<2> reference;
                bits<5> type;
                assert(type != 5 || reference != 0) at reference;
                bits<1> tail;
            }
            entry Header;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);
        QVERIFY2(first.succeeded(),
                 first.diagnostics.empty()
                     ? ""
                     : qPrintable(first.diagnostics.front().message));
        QVERIFY(second.succeeded());

        const auto& structure = first.program->structs.front();
        QCOMPARE(structure.fields.size(), std::size_t(3));
        QCOMPARE(structure.assertions.size(), std::size_t(1));
        const auto& assertion = structure.assertions.front();
        QCOMPARE(assertion.anchorFieldIndex, quint32(0));
        QCOMPARE(assertion.assertionFieldIndex, quint32(2));
        QCOMPARE(assertion.condition.kind, DslTypedExpressionKind::Binary);
        QCOMPARE(assertion.condition.type, DslScalarType::Bool);
        QCOMPARE(assertion.condition.binaryOperator,
                 streamview::rules::DslBinaryOperator::LogicalOr);
        QCOMPARE(assertion.condition.operands.at(0).operands.at(0).fieldIndex,
                 quint32(1));
        QCOMPARE(assertion.condition.operands.at(1).operands.at(0).fieldIndex,
                 quint32(0));

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertExpression,
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
        QCOMPARE(first.program->bytecode.at(3).operand, quint32(0));
        QCOMPARE(first.program->bytecode.at(3).immediate, quint64(0));
    }

    void expandsRepeatLocalAssertionsWithProjectionConditions() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct Marking {
                ue maximum;
                repeat (2) {
                    ue operation;
                    if (operation == 1) {
                        ue operand;
                        assert(operand <= maximum) at operand;
                    }
                } until (operation == 0);
                bits<1> tail;
            }
            entry Marking;
        )"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));

        const auto& structure = compiled.program->structs.front();
        QCOMPARE(structure.fields.size(), std::size_t(6));
        QCOMPARE(structure.assertions.size(), std::size_t(2));

        const auto& first = structure.assertions.at(0);
        QCOMPARE(first.anchorFieldIndex, quint32(2));
        QCOMPARE(first.assertionFieldIndex, quint32(3));
        QCOMPARE(first.conditions.size(), std::size_t(1));
        QCOMPARE(first.conditions.front().fieldIndex, quint32(1));
        QCOMPARE(first.conditions.front().expectedValue, quint64(1));
        QCOMPARE(first.condition.operands.at(0).fieldIndex, quint32(2));
        QCOMPARE(first.condition.operands.at(1).fieldIndex, quint32(0));

        const auto& second = structure.assertions.at(1);
        QCOMPARE(second.anchorFieldIndex, quint32(4));
        QCOMPARE(second.assertionFieldIndex, quint32(5));
        QCOMPARE(second.conditions.size(), std::size_t(2));
        QCOMPARE(second.conditions.at(0).fieldIndex, quint32(1));
        QVERIFY(second.conditions.at(0).negated);
        QCOMPARE(second.conditions.at(1).fieldIndex, quint32(3));
        QCOMPARE(second.conditions.at(1).expectedValue, quint64(1));

        const auto assertionOpcodes = std::count_if(
            compiled.program->bytecode.begin(),
            compiled.program->bytecode.end(),
            [](const streamview::rules::DslInstruction& instruction) {
                return instruction.opcode == DslOpcode::AssertExpression;
            });
        QCOMPARE(assertionOpcodes, std::ptrdiff_t(2));
    }

    void lowersPowerOfTwoAsAValidatedUnsignedExpression() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct Header {
                bits<4> exponent;
                computed<u64> value = power_of_two(exponent);
            }
            entry Header;
        )"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& expression =
            compiled.program->structs.front().fields.at(1).computedExpression;
        QVERIFY(expression.has_value());
        QCOMPARE(expression->kind, DslTypedExpressionKind::PowerOfTwo);
        QCOMPARE(expression->type, DslScalarType::U64);
        QCOMPARE(expression->operands.size(), std::size_t(1));
        QCOMPARE(expression->operands.front().kind, DslTypedExpressionKind::FieldReference);
    }

    void lowersMoreRbspDataAsAValidatedSourceStateExpression() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct Header {
                computed<bool> has_extension = more_rbsp_data();
                if (has_extension) {
                    bits<1> extension_flag;
                }
            }
            entry Header;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& expression =
            compiled.program->structs.front().fields.front().computedExpression;
        QVERIFY(expression.has_value());
        QCOMPARE(expression->kind, DslTypedExpressionKind::MoreRbspData);
        QCOMPARE(expression->type, DslScalarType::Bool);
        QVERIFY(expression->operands.empty());
    }

    void lowersImportedContextValuesInSourceAnchoredAssertions() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            pure bool is_disabled(u64 value) {
                return value == 0;
            }
            @context("h264-pps", id)
            struct Pps {
                bits<8> id;
                bits<1> weighted_pred_flag @context_export;
                bits<7> reserved;
            }
            @context_import("h264-pps", pic_parameter_set_id)
            struct Slice {
                bits<8> pic_parameter_set_id;
                assert(is_disabled(
                           context_value(pic_parameter_set_id,
                                         h264_pps,
                                         weighted_pred_flag)))
                    at pic_parameter_set_id;
                bits<1> tail;
            }
            entry Slice;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);
        QVERIFY2(first.succeeded(),
                 first.diagnostics.empty()
                     ? ""
                     : qPrintable(first.diagnostics.front().message));
        QVERIFY(second.succeeded());

        const auto sliceIndex =
            *first.program->structureIndex(QStringLiteral("Slice"));
        const auto& structure = first.program->structs.at(sliceIndex);
        QCOMPARE(structure.fields.size(), std::size_t(2));
        QCOMPARE(structure.assertions.size(), std::size_t(1));
        const auto& assertion = structure.assertions.front();
        QCOMPARE(assertion.anchorFieldIndex, quint32(0));
        QCOMPARE(assertion.assertionFieldIndex, quint32(1));
        QCOMPARE(assertion.condition.kind, DslTypedExpressionKind::Binary);
        QCOMPARE(assertion.condition.type, DslScalarType::Bool);
        QCOMPARE(assertion.condition.binaryOperator,
                 streamview::rules::DslBinaryOperator::Equal);
        const auto& imported = assertion.condition.operands.at(0);
        QCOMPARE(imported.kind,
                 DslTypedExpressionKind::ImportedContextReference);
        QCOMPARE(imported.type, DslScalarType::U64);
        QCOMPARE(imported.contextImportIndex, quint32(0));
        QCOMPARE(imported.contextDefinitionKind,
                 streamview::core::ContextDefinitionKind::H264PictureParameterSet);
        QCOMPARE(imported.contextStructureIndex, quint32(0));
        QCOMPARE(imported.contextExportIndex, quint32(0));

        const std::size_t offset = structure.bytecodeOffset;
        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertExpression,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::EndStructure,
        };
        QCOMPARE(structure.bytecodeLength, expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(first.program->bytecode.at(offset + index).opcode,
                     expected.at(index));
            QCOMPARE(first.program->bytecode.at(offset + index).opcode,
                     second.program->bytecode.at(offset + index).opcode);
            QCOMPARE(first.program->bytecode.at(offset + index).operand,
                     second.program->bytecode.at(offset + index).operand);
            QCOMPARE(first.program->bytecode.at(offset + index).immediate,
                     second.program->bytecode.at(offset + index).immediate);
        }
        QCOMPARE(first.program->bytecode.at(offset + 2).operand, quint32(0));
        QCOMPARE(first.program->bytecode.at(offset + 2).immediate, quint64(0));
    }

    void rejectsInvalidImportedContextAssertionContracts() {
        const std::vector<QString> invalidSources{
            QStringLiteral(R"(
                @context("h264-pps", id)
                struct Pps {
                    bits<8> id;
                    bits<1> present @context_export;
                }
                struct Slice {
                    bits<8> id;
                    assert(context_value(id, h264_pps, present) == 1) at id;
                }
                entry Slice;
            )"),
            QStringLiteral(R"(
                @context("h264-pps", id)
                struct Pps { bits<8> id; bits<1> present; }
                @context_import("h264-pps", id)
                struct Slice {
                    bits<8> id;
                    assert(context_value(id, h264_pps, present) == 1) at id;
                }
                entry Slice;
            )"),
        };

        for (const QString& source : invalidSources) {
            const auto parsed = DslParser::parse(source);
            QVERIFY2(parsed.succeeded(),
                     parsed.diagnostics.empty()
                         ? ""
                         : qPrintable(parsed.diagnostics.front().message));
            const auto compiled = DslCompiler::compile(parsed.program);
            QVERIFY(!compiled.succeeded());
            QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidContext));
        }
    }

    void ordersAssertionsBetweenSentinelCompletionAndRepeatBounds() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct Header {
                bits<1> count;
                repeat (1) { bits<1> sentinel; } until (sentinel == 0);
                assert(count == 0) at count;
                repeat (count, 1) { bits<1> item; }
            }
            entry Header;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertSentinelTerminated,
            DslOpcode::AssertExpression,
            DslOpcode::AssertRepeatCount,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
        QCOMPARE(compiled.program->structs.front().assertions.front().assertionFieldIndex,
                 quint32(2));
    }

    void preservesSourceOrderForAssertionsAtTheSameFieldPosition() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag; assert(flag == 0) at flag; "
            "assert(flag != 1) at flag; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const auto& structure = compiled.program->structs.front();
        QCOMPARE(structure.assertions.size(), std::size_t(2));
        QCOMPARE(structure.assertions.at(0).assertionFieldIndex, quint32(1));
        QCOMPARE(structure.assertions.at(1).assertionFieldIndex, quint32(1));
        const auto first = std::find_if(
            compiled.program->bytecode.begin(),
            compiled.program->bytecode.end(),
            [](const auto& instruction) {
                return instruction.opcode == DslOpcode::AssertExpression;
            });
        QVERIFY(first != compiled.program->bytecode.end());
        QVERIFY(std::next(first) != compiled.program->bytecode.end());
        QCOMPARE(first->operand, quint32(0));
        QCOMPARE(std::next(first)->opcode, DslOpcode::AssertExpression);
        QCOMPARE(std::next(first)->operand, quint32(1));
    }

    void boundsAssertionsPerStructure() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag; assert(true) at flag; } entry Header;"));
        QVERIFY(parsed.succeeded());
        auto program = parsed.program;
        const auto assertion = program.structs.front().items.back();
        program.structs.front().items.resize(1);
        for (std::size_t index = 0;
             index < streamview::rules::DslTypedAssertion::maximumPerStructure();
             ++index) {
            program.structs.front().items.push_back(assertion);
        }

        const auto atLimit = DslCompiler::compile(program);
        QVERIFY2(atLimit.succeeded(),
                 atLimit.diagnostics.empty()
                     ? ""
                     : qPrintable(atLimit.diagnostics.front().message));
        QCOMPARE(atLimit.program->structs.front().assertions.size(),
                 streamview::rules::DslTypedAssertion::maximumPerStructure());

        program.structs.front().items.push_back(assertion);
        const auto aboveLimit = DslCompiler::compile(program);
        QVERIFY(!aboveLimit.succeeded());
        const auto diagnostic = std::find_if(
            aboveLimit.diagnostics.begin(),
            aboveLimit.diagnostics.end(),
            [](const auto& candidate) {
                return candidate.code == DslDiagnosticCode::InvalidCondition &&
                       candidate.message == QStringLiteral(
                           "A structure may contain at most 1024 assertions");
        });
        QVERIFY(diagnostic != aboveLimit.diagnostics.end());
        QCOMPARE(diagnostic->range.start.offset, assertion.range.start.offset);
        QCOMPARE(diagnostic->range.end.offset, assertion.range.end.offset);
    }

    void lowersUnsignedExpGolombRangeConstraintsToBoundAssertions() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { ue log2_max_frame_num_minus4 @range(0, 12); "
            "ue trailing; } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));

        const auto& structure = compiled.program->structs.front();
        QCOMPARE(structure.fields.size(), std::size_t(2));
        QVERIFY(structure.fields.at(0).rangeConstraint.has_value());
        QCOMPARE(structure.fields.at(0).rangeConstraint->minimum, quint64(0));
        QCOMPARE(structure.fields.at(0).rangeConstraint->maximum, quint64(12));
        QVERIFY(!structure.fields.at(1).rangeConstraint.has_value());

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::AssertRangeMinimum,
            DslOpcode::AssertRangeMaximum,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
        QCOMPARE(compiled.program->bytecode.at(2).operand, quint32(0));
        QCOMPARE(compiled.program->bytecode.at(2).immediate, quint64(0));
        QCOMPARE(compiled.program->bytecode.at(3).operand, quint32(0));
        QCOMPARE(compiled.program->bytecode.at(3).immediate, quint64(12));
    }

    void lowersFixedAndDynamicUnsignedBitRangesToBoundAssertions() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<4> fixed @range(0, 15); bits<4> width; "
            "bits<width> dynamic @range(0, 0); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));

        const auto& structure = compiled.program->structs.front();
        QCOMPARE(structure.fields.size(), std::size_t(3));
        QCOMPARE(structure.fields.at(0).type.kind, DslValueTypeKind::UnsignedBits);
        QCOMPARE(structure.fields.at(0).type.bitWidth, quint8(4));
        QCOMPARE(structure.fields.at(0).rangeConstraint->minimum, quint64(0));
        QCOMPARE(structure.fields.at(0).rangeConstraint->maximum, quint64(15));
        QVERIFY(structure.fields.at(2).bitWidthExpression.has_value());
        QCOMPARE(structure.fields.at(2).rangeConstraint->minimum, quint64(0));
        QCOMPARE(structure.fields.at(2).rangeConstraint->maximum, quint64(0));

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertRangeMinimum,
            DslOpcode::AssertRangeMaximum,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::AssertRangeMinimum,
            DslOpcode::AssertRangeMaximum,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
        QCOMPARE(compiled.program->bytecode.at(2).operand, quint32(0));
        QCOMPARE(compiled.program->bytecode.at(3).immediate, quint64(15));
        QCOMPARE(compiled.program->bytecode.at(6).operand, quint32(2));
        QCOMPARE(compiled.program->bytecode.at(7).immediate, quint64(0));
    }

    void lowersSignedExpGolombRangesToTwosComplementBoundAssertions() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { se offset @range(-6, 6); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));

        const auto& structure = compiled.program->structs.front();
        QCOMPARE(structure.fields.size(), std::size_t(1));
        QCOMPARE(structure.fields.at(0).type.kind, DslValueTypeKind::SignedExpGolomb);
        QVERIFY(!structure.fields.at(0).rangeConstraint.has_value());
        QVERIFY(structure.fields.at(0).signedRangeConstraint.has_value());
        QCOMPARE(structure.fields.at(0).signedRangeConstraint->minimum, qint64(-6));
        QCOMPARE(structure.fields.at(0).signedRangeConstraint->maximum, qint64(6));

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadSignedExpGolomb,
            DslOpcode::AssertRangeMinimum,
            DslOpcode::AssertRangeMaximum,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
        QCOMPARE(compiled.program->bytecode.at(2).operand, quint32(0));
        QCOMPARE(compiled.program->bytecode.at(2).immediate,
                 static_cast<quint64>(qint64(-6)));
        QCOMPARE(compiled.program->bytecode.at(3).operand, quint32(0));
        QCOMPARE(compiled.program->bytecode.at(3).immediate, quint64(6));
    }

    void rejectsSignedRangeBoundsOutsideTheEncodingDomainInCompiler() {
        const auto belowDomain = DslParser::parse(
            QStringLiteral("struct Header { se value "
                           "@range(-9223372036854775808, 0); } entry Header;"));
        const auto inverted = DslParser::parse(
            QStringLiteral("struct Header { se value @range(6, -6); } entry Header;"));
        const auto negativeOnUnsigned = DslParser::parse(
            QStringLiteral("struct Header { ue value @range(-1, 6); } entry Header;"));
        QVERIFY(belowDomain.program.structs.size() == std::size_t(1));
        QVERIFY(inverted.program.structs.size() == std::size_t(1));
        QVERIFY(negativeOnUnsigned.program.structs.size() == std::size_t(1));

        QVERIFY(!DslCompiler::compile(belowDomain.program).succeeded());
        QVERIFY(!DslCompiler::compile(inverted.program).succeeded());
        QVERIFY(!DslCompiler::compile(negativeOnUnsigned.program).succeeded());
    }

    void lowersCoincidentEqualsAndRangeConstraintsInDeclarationOrder() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { ue value @equals(4) @range(0, 12); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::AssertEquals,
            DslOpcode::AssertRangeMinimum,
            DslOpcode::AssertRangeMaximum,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
    }

    void expandsRangeConstraintsForEveryArrayElement() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { ue values[2] @range(1, 9); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        const auto& structure = compiled.program->structs.front();
        QCOMPARE(structure.fields.size(), std::size_t(2));
        for (std::size_t index = 0; index < structure.fields.size(); ++index) {
            QVERIFY(structure.fields.at(index).rangeConstraint.has_value());
            QCOMPARE(structure.fields.at(index).rangeConstraint->minimum, quint64(1));
            QCOMPARE(structure.fields.at(index).rangeConstraint->maximum, quint64(9));
        }

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::AssertRangeMinimum,
            DslOpcode::AssertRangeMaximum,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::AssertRangeMinimum,
            DslOpcode::AssertRangeMaximum,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
    }

    void reservesBoundedTypedFieldsForRbspTrailingBits() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Payload { bits<3> primary_pic_type; rbsp_trailing_bits; } "
            "entry Payload;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& fields = compiled.program->structs.front().fields;
        QCOMPARE(fields.size(), std::size_t(9));
        QCOMPARE(fields.at(1).name, QStringLiteral("rbsp_stop_one_bit"));
        QCOMPARE(fields.at(1).type.kind, DslValueTypeKind::UnsignedBits);
        QCOMPARE(fields.at(1).type.bitWidth, quint8(1));
        QCOMPARE(fields.at(1).equalsConstraint, std::optional<quint64>(1));
        for (std::size_t index = 0; index < 7; ++index) {
            QCOMPARE(fields.at(2 + index).name,
                     QStringLiteral("rbsp_alignment_zero_bit[%1]").arg(index));
            QCOMPARE(fields.at(2 + index).type.kind, DslValueTypeKind::UnsignedBits);
            QCOMPARE(fields.at(2 + index).type.bitWidth, quint8(1));
            QCOMPARE(fields.at(2 + index).equalsConstraint, std::optional<quint64>(0));
        }
        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::ReadRbspTrailingBits,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
        QCOMPARE(compiled.program->bytecode.at(2).operand, quint32(1));
    }

    void countsRbspTrailingBitsReservationAgainstTheStructureLimit() {
        const auto accepted = DslParser::parse(QStringLiteral(
            "struct Payload { bits<1> values[99991]; rbsp_trailing_bits; } "
            "entry Payload;"));
        const auto rejected = DslParser::parse(QStringLiteral(
            "struct Payload { bits<1> values[99992]; rbsp_trailing_bits; } "
            "entry Payload;"));
        QVERIFY(accepted.succeeded());
        QVERIFY(rejected.succeeded());

        const auto compiledAccepted = DslCompiler::compile(accepted.program);
        const auto compiledRejected = DslCompiler::compile(rejected.program);
        QVERIFY(compiledAccepted.succeeded());
        QCOMPARE(compiledAccepted.program->structs.front().fields.size(), std::size_t(99999));
        QVERIFY(!compiledRejected.succeeded());
        QVERIFY(hasDiagnostic(compiledRejected, DslDiagnosticCode::InvalidArrayLength));
    }

    void lowersCompressedPayloadToOneTypedFieldAndInstruction() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @spec("ITU-T H.264", "7.3.2")
            struct Slice {
                bits<3> prefix;
                compressed_payload slice_data
                    @description("Entropy-coded slice data.")
                    @spec("ITU-T H.264", "7.3.2.10");
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);
        QVERIFY2(first.succeeded(),
                 first.diagnostics.empty() ? "" : qPrintable(first.diagnostics.front().message));
        QVERIFY(second.succeeded());
        const auto& structure = first.program->structs.front();
        QCOMPARE(structure.fields.size(), std::size_t(2));
        const auto& payload = structure.fields.back();
        QCOMPARE(payload.name, QStringLiteral("slice_data"));
        QCOMPARE(payload.kind, streamview::rules::DslTypedFieldKind::Declared);
        QCOMPARE(payload.type.kind, DslValueTypeKind::CompressedPayload);
        QCOMPARE(payload.type.bitWidth, quint8(0));
        QCOMPARE(payload.type.endian, DslEndian::Big);
        QVERIFY(!payload.type.enumIndex.has_value());
        QVERIFY(!payload.contextEligible);
        QVERIFY(!payload.bitWidthExpression.has_value());
        QVERIFY(!payload.computedExpression.has_value());
        QVERIFY(!payload.lazyByteCountExpression.has_value());
        QVERIFY(payload.conditions.empty());
        QCOMPARE(payload.metadata.typeName, QStringLiteral("compressed_payload"));
        QCOMPARE(payload.metadata.description, QStringLiteral("Entropy-coded slice data."));
        QVERIFY(payload.metadata.specification.has_value());
        QCOMPARE(payload.metadata.specification->clause, QStringLiteral("7.3.2.10"));

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::RegisterCompressedPayload,
            DslOpcode::EndStructure,
        };
        QCOMPARE(first.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(first.program->bytecode.at(index).opcode, expected.at(index));
            QCOMPARE(first.program->bytecode.at(index).opcode,
                     second.program->bytecode.at(index).opcode);
            QCOMPARE(first.program->bytecode.at(index).operand,
                     second.program->bytecode.at(index).operand);
        }
        QCOMPARE(first.program->bytecode.at(2).operand, quint32(1));
        QCOMPARE(first.program->bytecode.at(2).immediate, quint64(0));
    }

    void countsCompressedPayloadAgainstTheStructureLimit() {
        const auto accepted = DslParser::parse(QStringLiteral(
            "struct P { bits<1> values[99998]; compressed_payload data; } entry P;"));
        const auto rejected = DslParser::parse(QStringLiteral(
            "struct P { bits<1> values[99999]; compressed_payload data; } entry P;"));
        QVERIFY(accepted.succeeded());
        QVERIFY(rejected.succeeded());

        const auto compiledAccepted = DslCompiler::compile(accepted.program);
        const auto compiledRejected = DslCompiler::compile(rejected.program);
        QVERIFY(compiledAccepted.succeeded());
        QCOMPARE(compiledAccepted.program->structs.front().fields.size(),
                 std::size_t(99999));
        QVERIFY(!compiledRejected.succeeded());
        QVERIFY(hasDiagnostic(compiledRejected, DslDiagnosticCode::InvalidArrayLength));
    }

    void rejectsMalformedCompressedPayloadAst() {
        const auto parse = []() {
            return DslParser::parse(QStringLiteral(
                "struct P { bits<1> prefix; compressed_payload data; } entry P;"));
        };
        std::vector<streamview::rules::DslProgram> malformed;

        auto nonTerminal = parse().program;
        nonTerminal.structs.front().items.push_back(
            nonTerminal.structs.front().items.front());
        malformed.push_back(std::move(nonTerminal));

        auto duplicate = parse().program;
        duplicate.structs.front().items.push_back(
            duplicate.structs.front().items.back());
        malformed.push_back(std::move(duplicate));

        auto nested = parse().program;
        streamview::rules::DslStructItem conditional;
        conditional.kind = streamview::rules::DslStructItemKind::Conditional;
        conditional.condition.fieldName = QStringLiteral("prefix");
        conditional.condition.expectedValue = 1;
        conditional.thenItems.push_back(nested.structs.front().items.back());
        nested.structs.front().items.back() = std::move(conditional);
        malformed.push_back(std::move(nested));

        auto withTrailingBits = parse().program;
        streamview::rules::DslStructItem trailing;
        trailing.kind = streamview::rules::DslStructItemKind::RbspTrailingBits;
        withTrailingBits.structs.front().items.push_back(std::move(trailing));
        malformed.push_back(std::move(withTrailingBits));

        auto badAnnotation = parse().program;
        streamview::rules::DslAnnotation annotation;
        annotation.name = QStringLiteral("equals");
        badAnnotation.structs.front().items.back().compressedPayload.annotations.push_back(
            std::move(annotation));
        malformed.push_back(std::move(badAnnotation));

        for (std::size_t index = 0; index < malformed.size(); ++index) {
            const auto compiled = DslCompiler::compile(malformed.at(index));
            QVERIFY(!compiled.succeeded());
            if (index + 1 == malformed.size()) {
                QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidAnnotation));
            } else {
                QVERIFY(hasDiagnostic(compiled,
                                      DslDiagnosticCode::InvalidCompressedPayload));
            }
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

    void compilesUnsignedExpGolombEnumDomainsIntoTypedIrAndBytecode() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum IdrAllISliceType { i = 2; all_i = 7; } "
            "struct Header { ue slice_type @enum(IdrAllISliceType) "
            "@equals(7) @range(2, 7); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& field = compiled.program->structs.front().fields.front();
        QCOMPARE(field.type.kind, DslValueTypeKind::UnsignedExpGolomb);
        QCOMPARE(field.type.enumIndex, std::optional<quint32>(0));
        QCOMPARE(field.metadata.typeName, QStringLiteral("IdrAllISliceType"));
        QVERIFY(field.contextEligible);
        QCOMPARE(field.equalsConstraint, std::optional<quint64>(7));
        QVERIFY(field.rangeConstraint.has_value());
        QCOMPARE(field.rangeConstraint->minimum, quint64(2));
        QCOMPARE(field.rangeConstraint->maximum, quint64(7));

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::AssertEquals,
            DslOpcode::AssertRangeMinimum,
            DslOpcode::AssertRangeMaximum,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
    }

    void rejectsUnsignedExpGolombEnumMembersOutsideTheEncodingRangeInCompiler() {
        auto parsed = DslParser::parse(QStringLiteral(
            "enum Type { valid = 0; } "
            "struct Header { ue value @enum(Type); } entry Header;"));
        QVERIFY(parsed.succeeded());
        parsed.program.enums.front().values.front().value =
            std::numeric_limits<quint64>::max();

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(!compiled.succeeded());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::EnumValueOutOfRange));
    }

    void compilesUnsignedExpGolombEqualsAndConditionIntoTypedIr() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Sps { ue pic_order_cnt_type @equals(0); "
            "ue log2_max_pic_order_cnt_lsb_minus4; } entry Sps;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& fields = compiled.program->structs.front().fields;
        QCOMPARE(fields.at(0).equalsConstraint, std::optional<quint64>(0));
        QVERIFY(fields.at(1).conditions.empty());
        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::AssertEquals,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::EndStructure,
        };
        QCOMPARE(compiled.program->bytecode.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            QCOMPARE(compiled.program->bytecode.at(index).opcode, expected.at(index));
        }
    }

    void rejectsAnUnsignedExpGolombEqualsValueOutsideItsEncodingRange() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { ue value @equals(18446744073709551615); } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(!compiled.succeeded());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::ConstraintOutOfRange));
    }

    void inlinesPureCallsIntoDeterministicComputedExpressions() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            pure u64 add(u64 left, u64 right) { return left + right; }
            pure bool between(u64 value, u64 low, u64 high) {
                return value >= low && value <= high;
            }
            struct Header {
                bits<8> value;
                computed<u64> adjusted = add(value, 1) @description("Adjusted.");
                computed<bool> selected = between(adjusted, 2, 5) @spec("Example", "1");
                bits<16, little> tail;
            }
            entry Header;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);
        QVERIFY2(first.succeeded(),
                 first.diagnostics.empty()
                     ? ""
                     : qPrintable(first.diagnostics.front().message));
        QVERIFY(second.succeeded());
        const auto& fields = first.program->structs.front().fields;
        QCOMPARE(fields.size(), std::size_t(4));
        QCOMPARE(fields.at(0).type.kind, DslValueTypeKind::UnsignedBits);
        QCOMPARE(fields.at(1).type.kind, DslValueTypeKind::ComputedUnsigned);
        QCOMPARE(fields.at(2).type.kind, DslValueTypeKind::ComputedBool);
        QCOMPARE(fields.at(3).type.endian, DslEndian::Little);
        QCOMPARE(fields.at(1).metadata.description, QStringLiteral("Adjusted."));
        QVERIFY(fields.at(2).metadata.specification.has_value());
        QVERIFY(fields.at(1).computedExpression.has_value());
        QVERIFY(fields.at(2).computedExpression.has_value());

        const auto& adjusted = *fields.at(1).computedExpression;
        QCOMPARE(adjusted.kind, DslTypedExpressionKind::Binary);
        QCOMPARE(adjusted.binaryOperator,
                 streamview::rules::DslBinaryOperator::Add);
        QCOMPARE(adjusted.operands.size(), std::size_t(2));
        QCOMPARE(adjusted.operands.at(0).kind,
                 DslTypedExpressionKind::FieldReference);
        QCOMPARE(adjusted.operands.at(0).fieldIndex, quint32(0));
        QCOMPARE(adjusted.operands.at(1).unsignedValue, quint64(1));

        const auto& selected = *fields.at(2).computedExpression;
        QCOMPARE(selected.kind, DslTypedExpressionKind::Binary);
        QCOMPARE(selected.binaryOperator,
                 streamview::rules::DslBinaryOperator::LogicalAnd);

        const std::vector<DslOpcode> expected{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::EvaluateComputed,
            DslOpcode::EvaluateComputed,
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
        }
    }

    void lowersComputedControllersIntoExistingGuardsAndRepeatBounds() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct Header {
                bits<2> raw;
                computed<u64> kind = raw;
                computed<bool> present = kind == 1;
                if (present) { bits<1> conditional_value; }
                switch (kind) { case 1: { bits<1> switched_value; } }
                computed<u64> count = kind;
                repeat (count, 2) {
                    computed<u64> local = count + 1;
                    bits<1> repeated_value;
                }
            }
            entry Header;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& structure = compiled.program->structs.front();
        const auto& fields = structure.fields;
        QCOMPARE(fields.size(), std::size_t(10));
        QCOMPARE(fields.at(3).name, QStringLiteral("conditional_value"));
        QCOMPARE(fields.at(3).conditions.front().fieldIndex, quint32(2));
        QCOMPARE(fields.at(3).conditions.front().expectedValue, quint64(1));
        QCOMPARE(fields.at(4).name, QStringLiteral("switched_value"));
        QCOMPARE(fields.at(4).conditions.front().fieldIndex, quint32(1));
        QCOMPARE(fields.at(6).name, QStringLiteral("local[0]"));
        QCOMPARE(fields.at(8).name, QStringLiteral("local[1]"));
        QCOMPARE(fields.at(6).conditions.front().fieldIndex, quint32(5));
        QCOMPARE(fields.at(6).conditions.front().op,
                 DslConditionOperator::GreaterThan);
        QCOMPARE(fields.at(8).conditions.front().expectedValue, quint64(1));
        QCOMPARE(structure.repeatBounds.size(), std::size_t(1));
        QCOMPARE(structure.repeatBounds.front().controllerFieldIndex, quint32(5));
        QCOMPARE(structure.repeatBounds.front().firstFieldIndex, quint32(6));

        const auto assertBound = std::find_if(
            compiled.program->bytecode.begin(),
            compiled.program->bytecode.end(),
            [](const auto& instruction) {
                return instruction.opcode == DslOpcode::AssertRepeatCount;
            });
        QVERIFY(assertBound != compiled.program->bytecode.end());
        QVERIFY(assertBound + 1 != compiled.program->bytecode.end());
        QCOMPARE((assertBound + 1)->opcode, DslOpcode::EvaluateComputed);
    }

    void compilesCheckedLazyByteRegions() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            pure u64 plus_one(u64 value) { return value + 1; }
            struct Packet {
                bits<8> payload_size;
                computed<u64> adjusted = payload_size;
                @lazy(plus_one(adjusted))
                bytes payload @description("Payload.") @spec("Example", "1");
            }
            struct ConditionalPacket {
                bits<8> selector;
                if (selector == 1) { @lazy(1) bytes selected; }
                else { @lazy(0) bytes fallback; }
            }
            struct SwitchPacket {
                bits<8> selector;
                switch (selector) {
                case 1: { @lazy(1) bytes selected; }
                case 2: { @lazy(2) bytes alternate; }
                default: { @lazy(0) bytes fallback; }
                }
            }
            struct RepeatedPacket {
                bits<8> count;
                repeat (count, 1) { @lazy(1) bytes chunk; }
            }
            struct EmptyPayload { @lazy(0) bytes payload; }
            struct TrailingField {
                @lazy(1) bytes payload;
                bits<16> tail;
            }
            entry Packet;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty() ? "" : qPrintable(parsed.diagnostics.front().message));

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);

        QVERIFY2(first.succeeded(),
                 first.diagnostics.empty() ? "" : qPrintable(first.diagnostics.front().message));
        QVERIFY(second.succeeded());
        QCOMPARE(first.program->structs.size(), std::size_t(6));

        const auto& packet = first.program->structs.at(0);
        QCOMPARE(packet.fields.size(), std::size_t(3));
        const auto& payload = packet.fields.at(2);
        QCOMPARE(payload.name, QStringLiteral("payload"));
        QCOMPARE(payload.type.kind, DslValueTypeKind::LazyBytes);
        QCOMPARE(payload.type.bitWidth, quint8(0));
        QVERIFY(!payload.computedExpression.has_value());
        QVERIFY(payload.lazyByteCountExpression.has_value());
        QCOMPARE(payload.metadata.typeName, QStringLiteral("bytes"));
        QCOMPARE(payload.metadata.description, QStringLiteral("Payload."));
        QVERIFY(payload.metadata.specification.has_value());
        QCOMPARE(payload.metadata.specification->standard, QStringLiteral("Example"));
        const auto& countExpression = *payload.lazyByteCountExpression;
        QCOMPARE(countExpression.kind, DslTypedExpressionKind::Binary);
        QCOMPARE(countExpression.binaryOperator, streamview::rules::DslBinaryOperator::Add);
        QCOMPARE(countExpression.type, streamview::rules::DslScalarType::U64);
        QCOMPARE(countExpression.operands.at(0).kind, DslTypedExpressionKind::FieldReference);
        QCOMPARE(countExpression.operands.at(0).fieldIndex, quint32(1));
        QCOMPARE(countExpression.operands.at(1).unsignedValue, quint64(1));

        const auto& conditional = first.program->structs.at(1).fields;
        QCOMPARE(conditional.size(), std::size_t(3));
        QCOMPARE(conditional.at(1).conditions.size(), std::size_t(1));
        QCOMPARE(conditional.at(1).conditions.front().fieldIndex, quint32(0));
        QCOMPARE(conditional.at(1).conditions.front().expectedValue, quint64(1));
        QVERIFY(!conditional.at(1).conditions.front().negated);
        QVERIFY(conditional.at(2).conditions.front().negated);

        const auto& switched = first.program->structs.at(2).fields;
        QCOMPARE(switched.size(), std::size_t(4));
        QVERIFY(!switched.at(1).conditions.front().negated);
        QVERIFY(!switched.at(2).conditions.front().negated);
        QCOMPARE(switched.at(3).conditions.size(), std::size_t(2));
        QCOMPARE(switched.at(3).conditions.at(0).expectedValue, quint64(1));
        QCOMPARE(switched.at(3).conditions.at(1).expectedValue, quint64(2));
        QVERIFY(switched.at(3).conditions.at(0).negated);
        QVERIFY(switched.at(3).conditions.at(1).negated);

        const auto& repeated = first.program->structs.at(3);
        QCOMPARE(repeated.fields.size(), std::size_t(2));
        QCOMPARE(repeated.fields.at(1).name, QStringLiteral("chunk[0]"));
        QCOMPARE(repeated.fields.at(1).conditions.front().fieldIndex, quint32(0));
        QCOMPARE(repeated.fields.at(1).conditions.front().op, DslConditionOperator::GreaterThan);
        QCOMPARE(repeated.fields.at(1).conditions.front().expectedValue, quint64(0));
        QCOMPARE(repeated.repeatBounds.size(), std::size_t(1));
        QCOMPARE(repeated.repeatBounds.front().firstFieldIndex, quint32(1));

        const auto& lazyOnly = first.program->structs.at(4);
        QCOMPARE(lazyOnly.fields.size(), std::size_t(1));
        QCOMPARE(lazyOnly.fields.front().type.kind, DslValueTypeKind::LazyBytes);

        const auto& trailingField = first.program->structs.at(5);
        QCOMPARE(trailingField.fields.size(), std::size_t(2));
        QCOMPARE(trailingField.fields.at(0).type.kind, DslValueTypeKind::LazyBytes);
        QCOMPARE(trailingField.fields.at(1).type.kind, DslValueTypeKind::UnsignedBits);

        const std::vector<std::vector<DslOpcode>> expectedOpcodes{
            {DslOpcode::BeginStructure, DslOpcode::ReadUnsignedBits, DslOpcode::EvaluateComputed,
             DslOpcode::RegisterLazyBytes, DslOpcode::EndStructure},
            {DslOpcode::BeginStructure, DslOpcode::ReadUnsignedBits, DslOpcode::RegisterLazyBytes,
             DslOpcode::RegisterLazyBytes, DslOpcode::EndStructure},
            {DslOpcode::BeginStructure, DslOpcode::ReadUnsignedBits, DslOpcode::RegisterLazyBytes,
             DslOpcode::RegisterLazyBytes, DslOpcode::RegisterLazyBytes,
             DslOpcode::EndStructure},
            {DslOpcode::BeginStructure, DslOpcode::ReadUnsignedBits, DslOpcode::AssertRepeatCount,
             DslOpcode::RegisterLazyBytes, DslOpcode::EndStructure},
            {DslOpcode::BeginStructure, DslOpcode::RegisterLazyBytes, DslOpcode::EndStructure},
            {DslOpcode::BeginStructure, DslOpcode::RegisterLazyBytes,
             DslOpcode::ReadUnsignedBits, DslOpcode::EndStructure},
        };
        for (std::size_t structIndex = 0; structIndex < expectedOpcodes.size(); ++structIndex) {
            const auto& structure = first.program->structs.at(structIndex);
            QCOMPARE(structure.bytecodeLength,
                     static_cast<quint32>(expectedOpcodes.at(structIndex).size()));
            for (std::size_t instructionIndex = 0;
                 instructionIndex < expectedOpcodes.at(structIndex).size(); ++instructionIndex) {
                QCOMPARE(
                    first.program->bytecode.at(structure.bytecodeOffset + instructionIndex).opcode,
                    expectedOpcodes.at(structIndex).at(instructionIndex));
            }
        }
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

    void rejectsInvalidLazyByteRegionsDuringCompilation() {
        struct Case final {
            QString source;
            DslDiagnosticCode diagnostic;
        };
        const std::vector<Case> cases{
            {QStringLiteral("struct Header { @lazy(true) bytes payload; } entry Header;"),
             DslDiagnosticCode::InvalidType},
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
            {QStringLiteral("struct Header { @lazy(1) bytes payload; bits<16, little> tail; } "
                            "entry Header;"),
             DslDiagnosticCode::InvalidEndian},
            {QStringLiteral(
                 "struct Header { @lazy(1) bytes payload; bits<1> payload; } entry Header;"),
             DslDiagnosticCode::DuplicateName},
            {QStringLiteral("struct Header { @lazy(1) bytes payload; "
                            "computed<u64> size = payload; } entry Header;"),
             DslDiagnosticCode::UnknownReference},
            {QStringLiteral("struct Header { bits<24> count; repeat (count, 99999) { "
                            "@lazy(0) bytes payload; } } entry Header;"),
             DslDiagnosticCode::InvalidArrayLength},
            {QStringLiteral("struct Header { bits<8> count; repeat (count, 2) { "
                            "@lazy(0) bytes payload; } } entry Header;"),
             DslDiagnosticCode::InvalidEndian},
        };

        for (const Case& testCase : cases) {
            const auto parsed = DslParser::parse(testCase.source);
            const auto compiled = DslCompiler::compile(parsed.program);
            QVERIFY(!compiled.succeeded());
            QVERIFY(!compiled.program.has_value());
            QVERIFY(hasDiagnostic(compiled, testCase.diagnostic));
        }

        auto malformed = DslParser::parse(
            QStringLiteral("struct Header { @lazy(1) bytes payload; } entry Header;"));
        QVERIFY(malformed.succeeded());
        malformed.program.structs.front().items.front().lazyRegion.byteCountExpression.kind =
            static_cast<streamview::rules::DslExpressionKind>(255);
        const auto malformedResult = DslCompiler::compile(malformed.program);
        QVERIFY(!malformedResult.succeeded());
        QVERIFY(hasDiagnostic(malformedResult, DslDiagnosticCode::InvalidExpression));
    }

    void rejectsExpandedPureExpressionsAndComputedProjectionOverflow() {
        const auto balanced = [](const auto& self, int leaves) -> QString {
            if (leaves == 1) {
                return QStringLiteral("value");
            }
            const int left = leaves / 2;
            return QStringLiteral("(%1 + %2)")
                .arg(self(self, left), self(self, leaves - left));
        };
        const QString oversizedSource =
            QStringLiteral("pure u64 large(u64 value) { return %1; } "
                           "struct Header { bits<1> value; "
                           "computed<u64> result = large(value) + large(value); } "
                           "entry Header;")
                .arg(balanced(balanced, 128));
        const QString oversizedLazySource =
            QStringLiteral("pure u64 large(u64 value) { return %1; } "
                           "struct Header { bits<8> value; "
                           "@lazy(large(value) + large(value)) bytes payload; } "
                           "entry Header;")
                .arg(balanced(balanced, 128));
        const auto oversized = DslParser::parse(oversizedSource);
        const auto oversizedLazy = DslParser::parse(oversizedLazySource);
        const auto projection = DslParser::parse(QStringLiteral(
            "struct Header { bits<17> count; repeat (count, 99999) { "
            "computed<u64> value = count; } } entry Header;"));
        QVERIFY2(oversized.succeeded(),
                 oversized.diagnostics.empty()
                     ? ""
                     : qPrintable(oversized.diagnostics.front().message));
        QVERIFY(oversizedLazy.succeeded());
        QVERIFY(projection.succeeded());

        const auto compiledOversized = DslCompiler::compile(oversized.program);
        const auto compiledOversizedLazy = DslCompiler::compile(oversizedLazy.program);
        const auto compiledProjection = DslCompiler::compile(projection.program);
        QVERIFY(!compiledOversized.succeeded());
        QVERIFY(hasDiagnostic(compiledOversized, DslDiagnosticCode::InvalidExpression));
        QVERIFY(!compiledOversizedLazy.succeeded());
        QVERIFY(hasDiagnostic(compiledOversizedLazy, DslDiagnosticCode::InvalidExpression));
        QVERIFY(!compiledProjection.succeeded());
        QVERIFY(hasDiagnostic(compiledProjection, DslDiagnosticCode::InvalidArrayLength));
    }

    void boundsInliningWorkForUnusedPureArguments() {
        QString source = QStringLiteral(
            "pure u64 first(u64 p0, u64 p1, u64 p2, u64 p3, "
            "u64 p4, u64 p5, u64 p6, u64 p7, u64 p8, u64 p9, "
            "u64 p10, u64 p11, u64 p12, u64 p13, u64 p14, u64 p15) { "
            "return p0; } ");
        QString previous = QStringLiteral("value");
        for (int level = 1; level <= 4; ++level) {
            QStringList arguments;
            for (int index = 0; index < 16; ++index) {
                arguments.push_back(previous);
            }
            const QString functionName = QStringLiteral("level%1").arg(level);
            source += QStringLiteral("pure u64 %1(u64 value) { return first(%2); } ")
                          .arg(functionName, arguments.join(QStringLiteral(", ")));
            previous = functionName + QStringLiteral("(value)");
        }
        source += QStringLiteral(
            "struct Header { bits<1> value; computed<u64> result = level4(value); } "
            "entry Header;");
        const auto parsed = DslParser::parse(source);
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(!compiled.succeeded());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidExpression));
    }

    void rejectsMalformedComputedExpressionAst() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> value; computed<u64> result = value + 1; } "
            "entry Header;"));
        QVERIFY(parsed.succeeded());

        auto invalidKind = parsed.program;
        invalidKind.structs.front().items.at(1).computed.expression.kind =
            static_cast<streamview::rules::DslExpressionKind>(255);
        const auto compiledKind = DslCompiler::compile(invalidKind);
        QVERIFY(!compiledKind.succeeded());
        QVERIFY(hasDiagnostic(compiledKind, DslDiagnosticCode::InvalidExpression));

        auto invalidOperator = parsed.program;
        invalidOperator.structs.front().items.at(1).computed.expression.binaryOperator =
            static_cast<streamview::rules::DslBinaryOperator>(255);
        const auto compiledOperator = DslCompiler::compile(invalidOperator);
        QVERIFY(!compiledOperator.succeeded());
        QVERIFY(hasDiagnostic(compiledOperator, DslDiagnosticCode::InvalidExpression));
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

    void lowersBoundedSentinelRepeatsToDeterministicGuardedProjections() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { repeat (3) { ue operation; "
            "if (operation == 1) { ue argument; } "
            "} until (operation == 0); bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto first = DslCompiler::compile(parsed.program);
        const auto second = DslCompiler::compile(parsed.program);
        QVERIFY2(first.succeeded(),
                 first.diagnostics.empty()
                     ? ""
                     : qPrintable(first.diagnostics.front().message));
        QVERIFY(second.succeeded());
        const auto& structure = first.program->structs.front();
        const std::vector<QString> expectedNames{
            QStringLiteral("operation[0]"),
            QStringLiteral("argument[0]"),
            QStringLiteral("operation[1]"),
            QStringLiteral("argument[1]"),
            QStringLiteral("operation[2]"),
            QStringLiteral("argument[2]"),
            QStringLiteral("tail"),
        };
        QCOMPARE(structure.fields.size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            QCOMPARE(structure.fields.at(index).name, expectedNames.at(index));
        }
        QVERIFY(structure.fields.at(0).conditions.empty());
        QCOMPARE(structure.fields.at(1).conditions.size(), std::size_t(1));
        QCOMPARE(structure.fields.at(1).conditions.front().fieldIndex, quint32(0));
        QCOMPARE(structure.fields.at(1).conditions.front().expectedValue, quint64(1));
        QVERIFY(!structure.fields.at(1).conditions.front().negated);
        QCOMPARE(structure.fields.at(2).conditions.size(), std::size_t(1));
        QCOMPARE(structure.fields.at(2).conditions.front().fieldIndex, quint32(0));
        QCOMPARE(structure.fields.at(2).conditions.front().expectedValue, quint64(0));
        QVERIFY(structure.fields.at(2).conditions.front().negated);
        QCOMPARE(structure.fields.at(3).conditions.size(), std::size_t(2));
        QCOMPARE(structure.fields.at(3).conditions.at(1).fieldIndex, quint32(2));
        QCOMPARE(structure.fields.at(4).conditions.size(), std::size_t(2));
        QVERIFY(structure.fields.at(4).conditions.at(0).negated);
        QVERIFY(structure.fields.at(4).conditions.at(1).negated);
        QVERIFY(structure.fields.at(6).conditions.empty());

        QCOMPARE(structure.sentinelRepeats.size(), std::size_t(1));
        const auto& repeat = structure.sentinelRepeats.front();
        QCOMPARE(repeat.firstFieldIndices,
                 std::vector<quint32>({0, 2, 4}));
        QCOMPARE(repeat.sentinelFieldIndices,
                 std::vector<quint32>({0, 2, 4}));
        QCOMPARE(repeat.assertionFieldIndex, quint32(6));
        QCOMPARE(repeat.terminatingValue, quint64(0));
        QVERIFY(repeat.conditions.empty());

        const std::vector<DslOpcode> expectedOpcodes{
            DslOpcode::BeginStructure,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::ReadUnsignedExpGolomb,
            DslOpcode::AssertSentinelTerminated,
            DslOpcode::ReadUnsignedBits,
            DslOpcode::EndStructure,
        };
        QCOMPARE(first.program->bytecode.size(), expectedOpcodes.size());
        QCOMPARE(first.program->bytecode.size(), second.program->bytecode.size());
        for (std::size_t index = 0; index < expectedOpcodes.size(); ++index) {
            QCOMPARE(first.program->bytecode.at(index).opcode,
                     expectedOpcodes.at(index));
            QCOMPARE(first.program->bytecode.at(index).opcode,
                     second.program->bytecode.at(index).opcode);
            QCOMPARE(first.program->bytecode.at(index).operand,
                     second.program->bytecode.at(index).operand);
            QCOMPARE(first.program->bytecode.at(index).immediate,
                     second.program->bytecode.at(index).immediate);
        }
    }

    void ordersNestedSentinelRepeatAssertionsAtSharedProjectionBoundaries() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { repeat (2) { bits<1> outer; repeat (2) { "
            "bits<1> inner; } until (inner == 0); } until (outer == 0); "
            "bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& structure = compiled.program->structs.front();
        QCOMPARE(structure.sentinelRepeats.size(), std::size_t(3));
        QCOMPARE(structure.sentinelRepeats.at(0).sentinelFieldIndices,
                 std::vector<quint32>({1, 2}));
        QCOMPARE(structure.sentinelRepeats.at(0).assertionFieldIndex, quint32(3));
        QCOMPARE(structure.sentinelRepeats.at(1).sentinelFieldIndices,
                 std::vector<quint32>({4, 5}));
        QCOMPARE(structure.sentinelRepeats.at(1).assertionFieldIndex, quint32(6));
        QCOMPARE(structure.sentinelRepeats.at(2).sentinelFieldIndices,
                 std::vector<quint32>({0, 3}));
        QCOMPARE(structure.sentinelRepeats.at(2).assertionFieldIndex, quint32(6));

        std::vector<std::size_t> assertionOffsets;
        std::vector<quint32> assertionOperands;
        for (std::size_t index = 0; index < compiled.program->bytecode.size(); ++index) {
            if (compiled.program->bytecode.at(index).opcode ==
                DslOpcode::AssertSentinelTerminated) {
                assertionOffsets.push_back(index);
                assertionOperands.push_back(compiled.program->bytecode.at(index).operand);
            }
        }
        QCOMPARE(assertionOffsets, std::vector<std::size_t>({4, 8, 9}));
        QCOMPARE(assertionOperands, std::vector<quint32>({0, 1, 2}));
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
        const auto validSentinel = DslParser::parse(QStringLiteral(
            "struct Header { repeat (2) { ue operation; } "
            "until (operation == 0); } entry Header;"));
        QVERIFY(repeatedUnaligned.succeeded());
        QVERIFY(hasDiagnostic(tailUnaligned, DslDiagnosticCode::InvalidEndian));
        QVERIFY(valid.succeeded());
        QVERIFY(validSentinel.succeeded());

        const auto badAlignment = DslCompiler::compile(repeatedUnaligned.program);
        QVERIFY(hasDiagnostic(badAlignment, DslDiagnosticCode::InvalidEndian));

        auto malformed = valid.program;
        malformed.structs.front().items.at(1).repeatMaximum = 0;
        const auto malformedResult = DslCompiler::compile(malformed);
        QVERIFY(hasDiagnostic(malformedResult, DslDiagnosticCode::InvalidArrayLength));

        auto malformedSentinel = validSentinel.program;
        malformedSentinel.structs.front().items.front().repeatMaximum = 65;
        const auto malformedSentinelResult = DslCompiler::compile(malformedSentinel);
        QVERIFY(hasDiagnostic(malformedSentinelResult,
                              DslDiagnosticCode::InvalidArrayLength));
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
            "struct Header { se prefix @equals(0); bits<16, little> value; } entry Header;"));
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

    void lowersPayloadDispatchToResolvedCasesWithoutNewOpcodes() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader { bits<3> pad; bits<5> nal_unit_type; }
            struct AccessUnitDelimiterRbsp { bits<8> value; }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            @description("Payload selection.")
            payload<rbsp> nal_units switch (nal_unit_type) {
                case 9: AccessUnitDelimiterRbsp;
                case 10: empty;
            }
            entry nal_units;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

        const auto compiled = DslCompiler::compile(parsed.program);

        QVERIFY(compiled.succeeded());
        const auto& program = *compiled.program;
        QVERIFY(program.payloadDispatch.has_value());
        const auto& dispatch = *program.payloadDispatch;
        QCOMPARE(dispatch.scanIndex, quint32(0));
        QCOMPARE(dispatch.controllerFieldIndex, quint32(1));
        QCOMPARE(dispatch.metadata.description, QStringLiteral("Payload selection."));
        QCOMPARE(dispatch.cases.size(), std::size_t(2));

        const auto* structureCase = dispatch.find(9);
        QVERIFY(structureCase != nullptr);
        QVERIFY(structureCase->structureIndex.has_value());
        QCOMPARE(program.structs.at(*structureCase->structureIndex).name,
                 QStringLiteral("AccessUnitDelimiterRbsp"));

        const auto* emptyCase = dispatch.find(10);
        QVERIFY(emptyCase != nullptr);
        QVERIFY(!emptyCase->structureIndex.has_value());
        QVERIFY(dispatch.find(11) == nullptr);

        QVERIFY(std::none_of(program.bytecode.begin(),
                             program.bytecode.end(),
                             [](const auto& instruction) {
                                 return instruction.opcode > DslOpcode::EndStructure;
                             }));
    }

    void rejectsPayloadDispatchThatSurvivesParsingWithAnInvalidController() {
        auto parsed = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader { bits<3> pad; bits<5> nal_unit_type; }
            struct AccessUnitDelimiterRbsp { bits<8> value; }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (nal_unit_type) {
                case 9: AccessUnitDelimiterRbsp;
            }
            entry nal_units;
        )"));
        QVERIFY(parsed.succeeded());
        parsed.program.payloadDispatch->controllerFieldName = QStringLiteral("pad_missing");

        const auto compiled = DslCompiler::compile(parsed.program);

        QVERIFY(!compiled.succeeded());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidPayloadDispatch));
    }

    void lowersRuleDeclaredContextDefinitionsToStableFieldIndexes() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", sps_id)
            struct Sps {
                ue sps_id;
                ue frame_width @context_export;
                computed<u64> frame_width_plus_one = frame_width + 1
                    @context_export;
            }
            @context("h264-pps", pps_id)
            @context_dependency("h264-sps", sps_id)
            struct Pps {
                ue pps_id;
                ue sps_id;
                bits<1> entropy_mode @context_export;
            }
            @context_import("h264-pps", pps_id)
            @context_import("h264-sps", sps_id)
            struct Slice {
                ue pps_id;
                ue sps_id;
            }
            entry Sps;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

        const auto compiled = DslCompiler::compile(parsed.program);

        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& sps = compiled.program->structs.at(0).contextDefinition;
        QVERIFY(sps.has_value());
        QCOMPARE(sps->kind,
                 streamview::core::ContextDefinitionKind::H264SequenceParameterSet);
        QCOMPARE(sps->keyFieldIndex, quint32(0));
        QVERIFY(sps->dependencies.empty());
        QCOMPARE(sps->exportFieldIndices, std::vector<quint32>({1, 2}));

        const auto& pps = compiled.program->structs.at(1).contextDefinition;
        QVERIFY(pps.has_value());
        QCOMPARE(pps->kind,
                 streamview::core::ContextDefinitionKind::H264PictureParameterSet);
        QCOMPARE(pps->keyFieldIndex, quint32(0));
        QCOMPARE(pps->dependencies.size(), std::size_t(1));
        QCOMPARE(pps->dependencies.front().kind,
                 streamview::core::ContextDefinitionKind::H264SequenceParameterSet);
        QCOMPARE(pps->dependencies.front().keyFieldIndex, quint32(1));
        QCOMPARE(pps->exportFieldIndices, std::vector<quint32>({2}));

        const auto& slice = compiled.program->structs.at(2).contextImports;
        QCOMPARE(slice.size(), std::size_t(2));
        QCOMPARE(slice.at(0).kind,
                 streamview::core::ContextDefinitionKind::H264PictureParameterSet);
        QCOMPARE(slice.at(0).keyFieldIndex, quint32(0));
        QCOMPARE(slice.at(1).kind,
                 streamview::core::ContextDefinitionKind::H264SequenceParameterSet);
        QCOMPARE(slice.at(1).keyFieldIndex, quint32(1));
    }

    void lowersImportedContextValuesIntoDynamicBitWidths() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-sps", id)
            struct Sps {
                bits<8> id;
                ue log2_max_frame_num_minus4 @context_export;
            }
            @context("h264-pps", id)
            @context_dependency("h264-sps", sps_id)
            struct Pps {
                bits<8> id;
                bits<8> sps_id;
            }
            @context_import("h264-pps", pic_parameter_set_id)
            struct SliceHeader {
                ue pic_parameter_set_id;
                bits<context_value(pic_parameter_set_id,
                                   h264_sps,
                                   log2_max_frame_num_minus4) + 4> frame_num;
            }
            entry SliceHeader;
        )"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& field = compiled.program->structs.at(2).fields.at(1);
        QCOMPARE(field.type.kind, DslValueTypeKind::UnsignedBits);
        QCOMPARE(field.type.bitWidth, quint8(0));
        QVERIFY(field.bitWidthExpression.has_value());
        const auto& expression = *field.bitWidthExpression;
        QCOMPARE(expression.kind, DslTypedExpressionKind::Binary);
        QCOMPARE(expression.operands.at(0).kind,
                 DslTypedExpressionKind::ImportedContextReference);
        const auto& imported = expression.operands.at(0);
        QCOMPARE(imported.contextImportIndex, quint32(0));
        QCOMPARE(imported.contextDefinitionKind,
                 streamview::core::ContextDefinitionKind::H264SequenceParameterSet);
        QCOMPARE(imported.contextStructureIndex, quint32(0));
        QCOMPARE(imported.contextExportIndex, quint32(0));
    }

    void lowersImportedContextEqualityConditionsIntoGuards() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-pps", id)
            struct Pps {
                bits<8> id;
                bits<1> present @context_export;
            }
            @context_import("h264-pps", id)
            struct Slice {
                bits<8> id;
                if (context_value(id, h264_pps, present) == 1) {
                    ue optional_value;
                } else {
                    bits<1> absent_value;
                }
                bits<1> tail;
            }
            entry Slice;
        )"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& fields = compiled.program->structs.at(1).fields;
        QCOMPARE(fields.size(), std::size_t(4));
        QCOMPARE(fields.at(1).conditions.size(), std::size_t(1));
        QCOMPARE(fields.at(2).conditions.size(), std::size_t(1));
        QVERIFY(fields.at(1).conditions.front().expression.has_value());
        QVERIFY(fields.at(2).conditions.front().expression.has_value());
        QVERIFY(!fields.at(1).conditions.front().negated);
        QVERIFY(fields.at(2).conditions.front().negated);
        const auto& imported = *fields.at(1).conditions.front().expression;
        QCOMPARE(imported.kind, DslTypedExpressionKind::ImportedContextReference);
        QCOMPARE(imported.contextImportIndex, quint32(0));
        QCOMPARE(imported.contextDefinitionKind,
                 streamview::core::ContextDefinitionKind::H264PictureParameterSet);
        QCOMPARE(imported.contextStructureIndex, quint32(0));
        QCOMPARE(imported.contextExportIndex, quint32(0));
        QCOMPARE(fields.at(1).conditions.front().expectedValue, quint64(1));
        QVERIFY(fields.at(3).conditions.empty());
    }

    void lowersHeaderValueToASequenceElementReferenceWithoutNewOpcodes() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            struct NalUnitHeader {
                bits<2> nal_ref_idc;
                bits<5> nal_unit_type;
            }
            struct SliceHeader {
                ue first_mb_in_slice;
                if (header_value(nal_ref_idc) == 0) {
                    ue non_reference_marker;
                }
            }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (nal_unit_type) {
                case 1: SliceHeader;
            }
            entry nal_units;
        )"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& slice = compiled.program->structs.at(1);
        const auto& guarded = slice.fields.at(1);
        QCOMPARE(guarded.name, QStringLiteral("non_reference_marker"));
        QCOMPARE(guarded.conditions.size(), std::size_t(1));
        const auto& condition = guarded.conditions.front();
        QVERIFY(condition.expression.has_value());
        QCOMPARE(condition.expression->kind,
                 DslTypedExpressionKind::SequenceElementReference);
        QCOMPARE(condition.expression->type, DslScalarType::U64);
        QVERIFY(condition.expression->operands.empty());
        QCOMPARE(condition.expression->elementFieldIndex, quint32(0));
        QCOMPARE(condition.expectedValue, quint64(0));

        const auto& element = compiled.program->structs.front();
        QCOMPARE(element.fields.at(condition.expression->elementFieldIndex).name,
                 QStringLiteral("nal_ref_idc"));
    }

    void lowersReservedExternalLeavesInComputedInitializers() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-pps", id)
            struct Pps {
                bits<8> id;
                ue num_ref_idx_l0_default_active_minus1 @context_export;
            }
            struct NalUnitHeader {
                bits<2> nal_ref_idc;
                bits<5> nal_unit_type;
            }
            @context_import("h264-pps", pic_parameter_set_id)
            struct SliceHeader {
                ue pic_parameter_set_id;
                computed<u64> effective_l0_count =
                    context_value(pic_parameter_set_id,
                                  h264_pps,
                                  num_ref_idx_l0_default_active_minus1) + 1;
                computed<bool> is_reference_picture = header_value(nal_ref_idc) != 0;
            }
            @index(progressive)
            sequence<NalUnitHeader> nal_units = scan(h264_start_code);
            payload<rbsp> nal_units switch (nal_unit_type) {
                case 1: SliceHeader;
            }
            entry nal_units;
        )"));
        QVERIFY(parsed.succeeded());

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& slice = compiled.program->structs.at(2);
        const auto& count = slice.fields.at(1);
        QCOMPARE(count.name, QStringLiteral("effective_l0_count"));
        QVERIFY(count.computedExpression.has_value());
        QCOMPARE(count.computedExpression->type, DslScalarType::U64);
        QCOMPARE(count.computedExpression->kind, DslTypedExpressionKind::Binary);
        QCOMPARE(count.computedExpression->binaryOperator,
                 streamview::rules::DslBinaryOperator::Add);
        const auto& imported = count.computedExpression->operands.at(0);
        QCOMPARE(imported.kind, DslTypedExpressionKind::ImportedContextReference);
        QCOMPARE(imported.type, DslScalarType::U64);

        const auto& flag = slice.fields.at(2);
        QCOMPARE(flag.name, QStringLiteral("is_reference_picture"));
        QVERIFY(flag.computedExpression.has_value());
        QCOMPARE(flag.computedExpression->type, DslScalarType::Bool);
        const auto& header = flag.computedExpression->operands.at(0);
        QCOMPARE(header.kind, DslTypedExpressionKind::SequenceElementReference);
        QVERIFY(header.operands.empty());
        QCOMPARE(compiled.program->structs.at(1)
                     .fields.at(header.elementFieldIndex)
                     .name,
                 QStringLiteral("nal_ref_idc"));

        const auto computedOpcodes = std::count_if(
            compiled.program->bytecode.begin(),
            compiled.program->bytecode.end(),
            [](const streamview::rules::DslInstruction& instruction) {
                return instruction.opcode == DslOpcode::EvaluateComputed;
            });
        QCOMPARE(computedOpcodes, 2);
    }

    void lowersOptionalFieldValuesWithDeclaredFallbacks() {
        const auto parsed = DslParser::parse(QStringLiteral(R"(
            @context("h264-pps", id)
            struct Pps {
                bits<8> id;
                ue num_ref_idx_l0_default_active_minus1 @context_export;
            }
            @context_import("h264-pps", pic_parameter_set_id)
            struct SliceHeader {
                ue pic_parameter_set_id;
                bits<1> override_flag;
                if (override_flag == 1) {
                    ue num_ref_idx_l0_active_minus1;
                }
                computed<u64> effective_l0_count =
                    optional_value(num_ref_idx_l0_active_minus1,
                                   context_value(pic_parameter_set_id,
                                                 h264_pps,
                                                 num_ref_idx_l0_default_active_minus1))
                    + 1;
            }
            entry SliceHeader;
        )"));
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? ""
                     : qPrintable(parsed.diagnostics.front().message));

        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));
        const auto& slice = compiled.program->structs.at(1);
        const auto& count = slice.fields.at(3);
        QCOMPARE(count.name, QStringLiteral("effective_l0_count"));
        QVERIFY(count.computedExpression.has_value());
        QCOMPARE(count.computedExpression->kind, DslTypedExpressionKind::Binary);
        const auto& optional = count.computedExpression->operands.at(0);
        QCOMPARE(optional.kind, DslTypedExpressionKind::OptionalFieldReference);
        QCOMPARE(optional.type, DslScalarType::U64);
        QCOMPARE(slice.fields.at(optional.fieldIndex).name,
                 QStringLiteral("num_ref_idx_l0_active_minus1"));
        // The fallback is the single operand, and it keeps its own contract.
        QCOMPARE(optional.operands.size(), std::size_t(1));
        QCOMPARE(optional.operands.front().kind,
                 DslTypedExpressionKind::ImportedContextReference);

        // No new opcode: the leaf rides the existing computed evaluation.
        const auto computedOpcodes = std::count_if(
            compiled.program->bytecode.begin(),
            compiled.program->bytecode.end(),
            [](const streamview::rules::DslInstruction& instruction) {
                return instruction.opcode == DslOpcode::EvaluateComputed;
            });
        QCOMPARE(computedOpcodes, 1);
    }

    void rejectsInvalidOptionalFieldValueContracts() {
        const std::vector<std::pair<QString, QString>> cases{
            {QStringLiteral("unknown field"),
             QStringLiteral("struct S { bits<1> flag; "
                            "computed<u64> derived = optional_value(missing, 0); } "
                            "entry S;")},
            {QStringLiteral("later field"),
             QStringLiteral("struct S { bits<1> flag; "
                            "computed<u64> derived = optional_value(tail, 0); "
                            "bits<1> tail; } entry S;")},
            {QStringLiteral("array field"),
             QStringLiteral("struct S { bits<1> values[2]; "
                            "computed<u64> derived = optional_value(values, 0); } "
                            "entry S;")},
            {QStringLiteral("signed field"),
             QStringLiteral("struct S { se value; "
                            "computed<u64> derived = optional_value(value, 0); } "
                            "entry S;")},
            {QStringLiteral("boolean field"),
             QStringLiteral("struct S { bits<1> flag; "
                            "computed<bool> derived = flag == 1; "
                            "computed<u64> widened = optional_value(derived, 0); } "
                            "entry S;")},
            {QStringLiteral("boolean fallback"),
             QStringLiteral("struct S { bits<1> flag; "
                            "computed<u64> derived = optional_value(flag, true); } "
                            "entry S;")},
            {QStringLiteral("branch-local fallback"),
             QStringLiteral("struct S { bits<1> flag; "
                            "if (flag == 1) { bits<4> guarded; } "
                            "computed<u64> derived = optional_value(flag, guarded); } "
                            "entry S;")},
            {QStringLiteral("repeat body field out of scope"),
             QStringLiteral("struct S { bits<2> count; "
                            "repeat (count, 4) { bits<1> inner; } "
                            "computed<u64> derived = optional_value(inner, 0); } "
                            "entry S;")},
            {QStringLiteral("switch controller position"),
             QStringLiteral("struct S { bits<1> flag; "
                            "if (flag == 1) { bits<4> guarded; } "
                            "switch (optional_value(guarded, 0)) { case 0: {} } } "
                            "entry S;")},
        };

        for (std::size_t index = 0; index < cases.size(); ++index) {
            const auto parsed = DslParser::parse(cases.at(index).second);
            if (!parsed.succeeded()) {
                continue;
            }
            const auto compiled = DslCompiler::compile(parsed.program);
            QVERIFY2(!compiled.succeeded(),
                     qPrintable(QStringLiteral("case %1 (%2) was accepted")
                                    .arg(index)
                                    .arg(cases.at(index).first)));
        }
    }

    void rejectsInvalidHeaderValueReferences() {
        const std::vector<std::pair<QString, QString>> cases{
            {QStringLiteral("unknown element field"),
             QStringLiteral(
                 "struct NalUnitHeader { bits<2> nal_ref_idc; bits<5> nal_unit_type; } "
                 "struct S { ue a; if (header_value(missing) == 0) { ue b; } } "
                 "@index(progressive) sequence<NalUnitHeader> u = scan(h264_start_code); "
                 "payload<rbsp> u switch (nal_unit_type) { case 1: S; } entry u;")},
            {QStringLiteral("guarded element field"),
             QStringLiteral(
                 "struct NalUnitHeader { bits<2> nal_ref_idc; bits<5> nal_unit_type; "
                 "if (nal_ref_idc == 0) { bits<1> guarded_flag; } } "
                 "struct S { ue a; if (header_value(guarded_flag) == 0) { ue b; } } "
                 "@index(progressive) sequence<NalUnitHeader> u = scan(h264_start_code); "
                 "payload<rbsp> u switch (nal_unit_type) { case 1: S; } entry u;")},
            {QStringLiteral("signed element field"),
             QStringLiteral(
                 "struct NalUnitHeader { bits<2> nal_ref_idc; bits<5> nal_unit_type; "
                 "se offset; } "
                 "struct S { ue a; if (header_value(offset) == 0) { ue b; } } "
                 "@index(progressive) sequence<NalUnitHeader> u = scan(h264_start_code); "
                 "payload<rbsp> u switch (nal_unit_type) { case 1: S; } entry u;")},
            {QStringLiteral("element structure self reference"),
             QStringLiteral(
                 "struct NalUnitHeader { bits<2> nal_ref_idc; bits<5> nal_unit_type; "
                 "if (header_value(nal_ref_idc) == 0) { bits<1> flag; } } "
                 "struct S { ue a; } "
                 "@index(progressive) sequence<NalUnitHeader> u = scan(h264_start_code); "
                 "payload<rbsp> u switch (nal_unit_type) { case 1: S; } entry u;")},
            {QStringLiteral("no declared sequence"),
             QStringLiteral("struct S { ue a; if (header_value(nal_ref_idc) == 0) "
                            "{ ue b; } } entry S;")},
        };
        for (const auto& [label, source] : cases) {
            const auto parsed = DslParser::parse(source);
            QVERIFY2(parsed.succeeded(), qPrintable(label));
            const auto compiled = DslCompiler::compile(parsed.program);
            QVERIFY2(!compiled.succeeded(), qPrintable(label));
            const bool reported = std::any_of(
                compiled.diagnostics.begin(),
                compiled.diagnostics.end(),
                [](const streamview::rules::DslDiagnostic& diagnostic) {
                    return diagnostic.message.contains(QStringLiteral("header_value"));
                });
            QVERIFY2(reported, qPrintable(label));
        }
    }

    void rejectsInvalidImportedContextEqualityConditionContracts() {
        const std::vector<QString> invalidSources{
            QStringLiteral(R"(
                @context("h264-pps", id)
                struct Pps { bits<8> id; bits<1> present @context_export; }
                struct Slice {
                    bits<8> id;
                    if (context_value(id, h264_pps, present) == 1) { bits<1> value; }
                }
                entry Slice;
            )"),
            QStringLiteral(R"(
                @context("h264-pps", id)
                struct Pps { bits<8> id; bits<1> present @context_export; }
                @context_import("h264-pps", id)
                struct Slice {
                    if (context_value(id, h264_pps, present) == 1) { bits<1> value; }
                    bits<8> id;
                }
                entry Slice;
            )"),
            QStringLiteral(R"(
                @context("h264-sps", id)
                struct Sps { bits<8> id; bits<1> present @context_export; }
                @context_import("aac-asc", id)
                struct Slice {
                    bits<8> id;
                    if (context_value(id, h264_sps, present) == 1) { bits<1> value; }
                }
                entry Slice;
            )"),
            QStringLiteral(R"(
                @context("h264-pps", id)
                struct Pps { bits<8> id; bits<1> present; }
                @context_import("h264-pps", id)
                struct Slice {
                    bits<8> id;
                    if (context_value(id, h264_pps, present) == 1) { bits<1> value; }
                }
                entry Slice;
            )"),
            QStringLiteral(R"(
                @context("h264-pps", id)
                struct First { bits<8> id; bits<1> present @context_export; }
                @context("h264-pps", id)
                struct Second { bits<8> id; bits<1> present @context_export; }
                @context_import("h264-pps", id)
                struct Slice {
                    bits<8> id;
                    if (context_value(id, h264_pps, present) == 1) { bits<1> value; }
                }
                entry Slice;
            )"),
            QStringLiteral(R"(
                @context("h264-pps", id)
                struct Pps { bits<8> id; bits<1> present @context_export; }
                @context("h264-sps", id)
                struct Sps { bits<8> id; }
                @context_import("h264-pps", id)
                @context_import("h264-sps", id)
                struct Slice {
                    bits<8> id;
                    if (context_value(id, h264_pps, present) == 1) { bits<1> value; }
                }
                entry Slice;
            )"),
        };

        for (const QString& source : invalidSources) {
            const auto parsed = DslParser::parse(source);
            QVERIFY2(parsed.succeeded(),
                     parsed.diagnostics.empty()
                         ? ""
                         : qPrintable(parsed.diagnostics.front().message));
            const auto compiled = DslCompiler::compile(parsed.program);
            QVERIFY(!compiled.succeeded());
            QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidContext));
        }
    }

    void rejectsInvalidImportedDynamicBitWidthContracts() {
        const std::vector<QString> invalidSources{
            QStringLiteral(R"(
                @context("h264-sps", id)
                struct Sps { bits<8> id; bits<8> width @context_export; }
                @context_import("aac-asc", id)
                struct Consumer {
                    bits<8> id;
                    bits<context_value(id, h264_sps, width)> value;
                }
                entry Consumer;
            )"),
            QStringLiteral(R"(
                @context("h264-sps", id)
                struct First { bits<8> id; bits<8> width @context_export; }
                @context("h264-sps", id)
                struct Second { bits<8> id; bits<8> other @context_export; }
                @context_import("h264-sps", id)
                struct Consumer {
                    bits<8> id;
                    bits<context_value(id, h264_sps, width)> value;
                }
                entry Consumer;
            )"),
            QStringLiteral(R"(
                @context("h264-sps", id)
                struct Sps { bits<8> id; bits<8> other @context_export; }
                @context_import("h264-sps", id)
                struct Consumer {
                    bits<8> id;
                    bits<context_value(id, h264_sps, width)> value;
                }
                entry Consumer;
            )"),
            QStringLiteral(R"(
                @context("h264-sps", id)
                struct Sps { bits<8> id; bits<8> width @context_export; }
                struct Consumer {
                    bits<8> id;
                    bits<context_value(id, h264_sps, width)> value;
                }
                entry Consumer;
            )"),
            QStringLiteral(R"(
                @context("h264-sps", id)
                struct Sps { bits<8> id; bits<8> width @context_export; }
                @context_import("h264-sps", id)
                struct Consumer {
                    bits<context_value(id, h264_sps, width)> value;
                    bits<8> id;
                }
                entry Consumer;
            )"),
            QStringLiteral(R"(
                @context("h264-sps", id)
                struct Sps { bits<8> id; bits<8> width @context_export; }
                @context_import("h264-sps", id)
                struct Consumer {
                    bits<8> id;
                    bits<context_value(id, h264_sps, width), little> value;
                }
                entry Consumer;
            )"),
            QStringLiteral(R"(
                @context("h264-sps", id)
                struct Sps { bits<8> id; bits<8> width @context_export; }
                @context_import("h264-sps", id)
                struct Consumer {
                    bits<8> id;
                    bits<context_value(id, h264_sps, width)> value[2];
                }
                entry Consumer;
            )"),
            QStringLiteral(R"(
                enum Mode { A = 0; }
                @context("h264-sps", id)
                struct Sps { bits<8> id; bits<8> width @context_export; }
                @context_import("h264-sps", id)
                struct Consumer {
                    bits<8> id;
                    bits<context_value(id, h264_sps, width)> value @enum(Mode);
                }
                entry Consumer;
            )"),
            QStringLiteral(R"(
                @context("h264-sps", id)
                struct Sps { bits<8> id; bits<8> width @context_export; }
                struct Consumer {
                    bits<8> id;
                    computed<u64> value = context_value(id, h264_sps, width);
                }
                entry Consumer;
            )"),
            QStringLiteral(R"(
                @context("h264-sps", id)
                @context_import("h264-sps", id)
                struct Sps {
                    bits<8> id;
                    bits<8> width @context_export;
                    bits<context_value(id, h264_sps, width)> value @context_export;
                }
                entry Sps;
            )"),
        };

        for (std::size_t index = 0; index < invalidSources.size(); ++index) {
            const QString& source = invalidSources.at(index);
            const auto parsed = DslParser::parse(source);
            QVERIFY2(parsed.succeeded(),
                     parsed.diagnostics.empty()
                         ? ""
                         : qPrintable(parsed.diagnostics.front().message));
            const auto compiled = DslCompiler::compile(parsed.program);
            QVERIFY(!compiled.succeeded());
            QVERIFY(!compiled.diagnostics.empty());
            if (index < 5 || index >= 8) {
                QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidContext));
            }
        }
    }

    void countsImportedReferencesAgainstTheExpressionNodeBudget() {
        const auto sourceWithLeaves = [](quint32 leafCount) {
            const QString leaf =
                QStringLiteral("context_value(id, h264_sps, width)");
            const std::function<QString(quint32)> build =
                [&](quint32 count) -> QString {
                if (count == 1) {
                    return leaf;
                }
                const quint32 leftCount = count / 2;
                return QStringLiteral("(%1 + %2)")
                    .arg(build(leftCount), build(count - leftCount));
            };
            return QStringLiteral(
                       "@context(\"h264-sps\", id) "
                       "struct Sps { bits<8> id; bits<8> width @context_export; } "
                       "@context_import(\"h264-sps\", id) "
                       "struct Consumer { bits<8> id; bits<%1> value; } "
                       "entry Consumer;")
                .arg(build(leafCount));
        };

        const auto acceptedParsed = DslParser::parse(sourceWithLeaves(128));
        QVERIFY(acceptedParsed.succeeded());
        const auto accepted = DslCompiler::compile(acceptedParsed.program);
        QVERIFY2(accepted.succeeded(),
                 accepted.diagnostics.empty()
                     ? ""
                     : qPrintable(accepted.diagnostics.front().message));

        const auto rejectedParsed = DslParser::parse(sourceWithLeaves(129));
        QVERIFY(rejectedParsed.succeeded());
        const auto rejected = DslCompiler::compile(rejectedParsed.program);
        QVERIFY(!rejected.succeeded());
        QVERIFY(hasDiagnostic(rejected, DslDiagnosticCode::InvalidExpression));
    }

    void rejectsInvalidRuleDeclaredContextContracts() {
        const std::vector<QString> invalidSources{
            QStringLiteral(
                "@context(\"h264-sps\", id) @context(\"h264-pps\", id) "
                "struct C { bits<1> id; } entry C;"),
            QStringLiteral(
                "@context(\"h264-pps\", id) "
                "@context_dependency(\"h264-sps\", dependency) "
                "@context_dependency(\"h264-sps\", dependency) "
                "struct C { bits<1> id; bits<1> dependency; } entry C;"),
            QStringLiteral(
                "@context(\"unknown\", id) struct C { bits<1> id; } entry C;"),
            QStringLiteral(
                "@context_dependency(\"h264-sps\", id) "
                "struct C { bits<1> id; } entry C;"),
            QStringLiteral(
                "@context(\"h264-sps\", id) struct C { bits<1> id; "
                "if (id == 1) { bits<1> value @context_export; } } entry C;"),
            QStringLiteral(
                "@context(\"h264-sps\", id) struct C { bits<1> id; "
                "se value @context_export; } entry C;"),
            QStringLiteral(
                "@context(\"h264-sps\", id) struct C { bits<1> id; "
                "bits<1> value[2] @context_export; } entry C;"),
            QStringLiteral(
                "@context(\"h264-sps\", id) struct C { bits<1> id; "
                "bits<1> value @context_export(1); } entry C;"),
            QStringLiteral(
                "@context(\"h264-sps\") struct C { bits<1> id; } entry C;"),
            QStringLiteral(
                "@context_import(\"h264-sps\", id) "
                "@context_import(\"h264-sps\", id) "
                "struct C { bits<1> id; } entry C;"),
            QStringLiteral(
                "@context_import(\"h264-sps\", missing) "
                "struct C { bits<1> id; } entry C;"),
            QStringLiteral(
                "@context_import(\"h264-sps\", value) "
                "struct C { bits<1> flag; if (flag == 1) { bits<1> value; } } "
                "entry C;"),
            QStringLiteral(
                "@context_import(\"h264-sps\", value) "
                "struct C { bits<1> value[2]; } entry C;"),
            QStringLiteral(
                "@context_import(\"h264-sps\", value) "
                "struct C { se value; } entry C;"),
            QStringLiteral(
                "@context_import(\"h264-sps\", value) "
                "struct C { bits<1> count; repeat (count, 1) { bits<1> value; } } "
                "entry C;"),
            QStringLiteral(
                "@context_import(\"h264-sps\", value) "
                "struct C { bits<8> count; @lazy(count) bytes value; } entry C;"),
            QStringLiteral(
                "@context_import(\"h264-sps\", rbsp_stop_one_bit) "
                "struct C { rbsp_trailing_bits; } entry C;"),
            QStringLiteral(
                "@context_import(\"h264-sps\", value) "
                "struct C { computed<bool> value = true; } entry C;"),
        };

        for (const QString& source : invalidSources) {
            const auto parsed = DslParser::parse(source);
            QVERIFY2(parsed.succeeded(), qPrintable(source));
            const auto compiled = DslCompiler::compile(parsed.program);
            QVERIFY2(!compiled.succeeded(), qPrintable(source));
            QVERIFY2(hasDiagnostic(compiled, DslDiagnosticCode::InvalidContext),
                     qPrintable(source));
        }

        QString tooManyExports =
            QStringLiteral("@context(\"h264-sps\", id) struct C { bits<8> id; ");
        for (quint32 index = 0; index < 65; ++index) {
            tooManyExports += QStringLiteral("bits<1> value%1 @context_export; ").arg(index);
        }
        tooManyExports += QStringLiteral("} entry C;");
        const auto parsed = DslParser::parse(tooManyExports);
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(!compiled.succeeded());
        QVERIFY(hasDiagnostic(compiled, DslDiagnosticCode::InvalidContext));

        QString maximumDependencies =
            QStringLiteral("@context(\"h264-pps\", id) " );
        for (quint32 index = 0; index < 16; ++index) {
            maximumDependencies += QStringLiteral(
                "@context_dependency(\"h264-sps\", dependency%1) " ).arg(index);
        }
        maximumDependencies += QStringLiteral("struct C { bits<8> id; " );
        for (quint32 index = 0; index < 16; ++index) {
            maximumDependencies +=
                QStringLiteral("bits<8> dependency%1; " ).arg(index);
        }
        maximumDependencies += QStringLiteral("} entry C;" );
        const auto maximumParsed = DslParser::parse(maximumDependencies);
        QVERIFY(maximumParsed.succeeded());
        const auto maximumCompiled = DslCompiler::compile(maximumParsed.program);
        QVERIFY2(maximumCompiled.succeeded(),
                 maximumCompiled.diagnostics.empty()
                     ? ""
                     : qPrintable(maximumCompiled.diagnostics.front().message));
        QCOMPARE(maximumCompiled.program->structs.front().contextDefinition->dependencies.size(),
                 std::size_t(16));

        QString tooManyDependencies = maximumDependencies;
        tooManyDependencies.replace(
            QStringLiteral("struct C {"),
            QStringLiteral(
                "@context_dependency(\"h264-sps\", dependency16) struct C {"));
        tooManyDependencies.replace(QStringLiteral("} entry C;"),
                                    QStringLiteral("bits<8> dependency16; } entry C;"));
        const auto tooManyParsed = DslParser::parse(tooManyDependencies);
        QVERIFY(tooManyParsed.succeeded());
        const auto tooManyCompiled = DslCompiler::compile(tooManyParsed.program);
        QVERIFY(!tooManyCompiled.succeeded());
        QVERIFY(hasDiagnostic(tooManyCompiled, DslDiagnosticCode::InvalidContext));

        QString maximumImports;
        for (quint32 index = 0; index < 16; ++index) {
            maximumImports += QStringLiteral(
                "@context_import(\"h264-sps\", import%1) ").arg(index);
        }
        maximumImports += QStringLiteral("struct I { ");
        for (quint32 index = 0; index < 16; ++index) {
            maximumImports += QStringLiteral("bits<8> import%1; ").arg(index);
        }
        maximumImports += QStringLiteral("} entry I;");
        const auto maximumImportsParsed = DslParser::parse(maximumImports);
        QVERIFY(maximumImportsParsed.succeeded());
        const auto maximumImportsCompiled =
            DslCompiler::compile(maximumImportsParsed.program);
        QVERIFY2(maximumImportsCompiled.succeeded(),
                 maximumImportsCompiled.diagnostics.empty()
                     ? ""
                     : qPrintable(maximumImportsCompiled.diagnostics.front().message));
        QCOMPARE(maximumImportsCompiled.program->structs.front().contextImports.size(),
                 std::size_t(16));

        QString tooManyImports = maximumImports;
        tooManyImports.replace(
            QStringLiteral("struct I {"),
            QStringLiteral("@context_import(\"h264-sps\", import16) struct I {"));
        tooManyImports.replace(QStringLiteral("} entry I;"),
                               QStringLiteral("bits<8> import16; } entry I;"));
        const auto tooManyImportsParsed = DslParser::parse(tooManyImports);
        QVERIFY(tooManyImportsParsed.succeeded());
        const auto tooManyImportsCompiled =
            DslCompiler::compile(tooManyImportsParsed.program);
        QVERIFY(!tooManyImportsCompiled.succeeded());
        QVERIFY(hasDiagnostic(tooManyImportsCompiled,
                              DslDiagnosticCode::InvalidContext));
    }
};

QTEST_GUILESS_MAIN(DslIrTest)

#include "dsl_ir_test.moc"
