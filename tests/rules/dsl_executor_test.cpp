#include <streamview/core/bit_reader.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/dsl_ir.h>

#include <QTest>
#include <QMetaType>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

using streamview::core::AnalysisNodeKind;
using streamview::core::AnalysisTree;
using streamview::core::BitReader;
using streamview::core::CancellationSource;
using streamview::core::DiagnosticCode;
using streamview::core::MaterializationState;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourceSpan;
using streamview::rules::DslExecutionStatus;
using streamview::rules::DslExecutionLimits;
using streamview::rules::DslExecutionOptions;
using streamview::rules::DslExecutor;
using streamview::rules::DslCompiler;
using streamview::rules::DslOpcode;
using streamview::rules::DslParser;
using streamview::rules::DslValueTypeKind;

namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

class MemorySource final : public RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> data) : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= data_.size()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const std::size_t count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(count),
                    destination.begin());
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
};

class FailingAfterFirstReadSource final : public RandomAccessSource {
public:
    explicit FailingAfterFirstReadSource(std::vector<std::byte> data)
        : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("failing-after-first");
    }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (readCount_++ > 0) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected source failure")};
        }
        if (byteOffset >= data_.size() || destination.size() > data_.size() - byteOffset) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(byteOffset),
                    static_cast<std::ptrdiff_t>(destination.size()),
                    destination.begin());
        return {SourceReadStatus::Complete, destination.size(), {}};
    }

private:
    std::vector<std::byte> data_;
    mutable quint64 readCount_ = 0;
};

[[nodiscard]] std::optional<streamview::core::SourceMapping>
mappingForBytes(quint64 byteCount) {
    const auto span = SourceSpan::create(streamview::core::SourceBitAddress(0), byteCount * 8U);
    if (!span) {
        return std::nullopt;
    }
    return streamview::core::SourceMapping::create(
        streamview::core::LogicalViewId(1), {*span});
}

} // namespace

class DslExecutorTest final : public QObject {
    Q_OBJECT

private slots:
    void materializesFieldsWithValuesAndLocations() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<5> second; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(8));
        QVERIFY(result.structureNode.has_value());
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->kind(), AnalysisNodeKind::Structure);
        QCOMPARE(structure->children().size(), std::size_t(2));
        const auto first = tree->node(structure->children().at(0));
        const auto second = tree->node(structure->children().at(1));
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QCOMPARE(first->value().toULongLong(), quint64(5));
        QCOMPARE(second->value().toULongLong(), quint64(18));
        QVERIFY(first->location().has_value());
        QCOMPARE(first->location()->sourceSpans().size(), std::size_t(1));
        QCOMPARE(first->location()->sourceSpans().front().start().absoluteBitOffset(), quint64(0));
        QCOMPARE(first->location()->sourceSpans().front().bitLength(), quint64(3));
        QCOMPARE(structure->state(), streamview::core::MaterializationState::Materialized);
    }

    void decodesUnsignedAndSignedExpGolombCodewordsWithExactLocations() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Codes { ue unsigned_zero; ue unsigned_one; ue unsigned_two; "
            "se signed_zero; se positive_one; se negative_one; "
            "se positive_two; se negative_two; } entry Codes;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0xa7, 0x4c, 0x85}));
        const auto mapping = mappingForBytes(3);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 24);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("exp-golomb"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Codes"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(24));
        QCOMPARE(result.instructionsExecuted, quint64(10));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(8));

        const std::vector<quint64> unsignedValues{0, 1, 2};
        const std::vector<quint64> starts{0, 1, 4, 7, 8, 11, 14, 19};
        const std::vector<quint64> lengths{1, 3, 3, 1, 3, 3, 5, 5};
        for (std::size_t index = 0; index < unsignedValues.size(); ++index) {
            const auto field = tree->node(structure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->value().metaType().id(), QMetaType::ULongLong);
            QCOMPARE(field->value().toULongLong(), unsignedValues.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     starts.at(index));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(), lengths.at(index));
        }

        const std::vector<qlonglong> signedValues{0, 1, -1, 2, -2};
        for (std::size_t index = 0; index < signedValues.size(); ++index) {
            const std::size_t childIndex = index + unsignedValues.size();
            const auto field = tree->node(structure->children().at(childIndex));
            QVERIFY(field.has_value());
            QCOMPARE(field->value().metaType().id(), QMetaType::LongLong);
            QCOMPARE(field->value().toLongLong(), signedValues.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     starts.at(childIndex));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(),
                     lengths.at(childIndex));
        }
    }

    void rollsBackAComponentExpGolombReadWhenTheCodewordIsTruncated() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> prefix; ue value; bits<1> suffix; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0b10100000}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 3);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("exp-golomb-truncated"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
        QCOMPARE(result.bitsConsumed, quint64(1));
        QCOMPARE(result.instructionsExecuted, quint64(3));
        QCOMPARE(result.nodesCreated, quint64(2));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(1));
        const auto prefix = tree->node(structure->children().front());
        QVERIFY(prefix.has_value());
        QCOMPARE(prefix->value().toULongLong(), quint64(1));
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::TruncatedSource);
        QCOMPARE(structure->diagnostics().front().fieldPath, QStringLiteral("Header.value"));
        QVERIFY(structure->diagnostics().front().location.has_value());
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(1));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(2));
        QVERIFY(tree->hasPartialResults());
    }

    void rollsBackAComponentExpGolombReadOnSourceError() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<1> prefix; ue value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        FailingAfterFirstReadSource source(bytes({0x80}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("exp-golomb-source-error"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::SourceError);
        QCOMPARE(result.bitsConsumed, quint64(1));
        QCOMPARE(reader.position(), quint64(1));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::SourceError);
        QCOMPARE(structure->diagnostics().front().fieldPath, QStringLiteral("Header.value"));
        QVERIFY(structure->diagnostics().front().location.has_value());
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(1));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(1));
    }

    void acceptsTheLongestRepresentableUnsignedExpGolombCodeword() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { ue value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes(
            {0, 0, 0, 0, 0, 0, 0, 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe}));
        const auto mapping = mappingForBytes(16);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 127);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("exp-golomb-maximum"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(127));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const auto value = tree->node(structure->children().front());
        QVERIFY(value.has_value());
        QCOMPARE(value->value().toULongLong(), std::numeric_limits<quint64>::max() - 1U);
        QCOMPARE(value->location()->sourceSpans().front().bitLength(), quint64(127));
    }

    void rejectsSixtyFourLeadingZeroBitsWithoutConsumingTheField() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { ue value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0, 0, 0, 0, 0, 0, 0, 0}));
        const auto mapping = mappingForBytes(8);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 64);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("exp-golomb-overflow"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
        QCOMPARE(result.bitsConsumed, quint64(0));
        QCOMPARE(reader.position(), quint64(0));
        QCOMPARE(result.instructionsExecuted, quint64(2));
        QCOMPARE(result.nodesCreated, quint64(1));
        QVERIFY(result.errorMessage.contains(QStringLiteral("64-bit")));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QVERIFY(structure->children().empty());
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::InvalidSyntax);
        QCOMPARE(structure->diagnostics().front().fieldPath, QStringLiteral("Header.value"));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(64));
    }

    void materializesFixedLengthBitArrayElementsWithLocationsAndMetadata() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "@spec(\"Example\", \"A.1\") struct Header { "
            "bits<2> flags[3] @description(\"Flags.\"); bits<2> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0b01101100}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("fixed-array-bits"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(8));
        QCOMPARE(result.instructionsExecuted, quint64(6));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(4));

        const std::vector<QString> names{
            QStringLiteral("flags[0]"),
            QStringLiteral("flags[1]"),
            QStringLiteral("flags[2]"),
            QStringLiteral("tail"),
        };
        const std::vector<quint64> values{1, 2, 3, 0};
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto field = tree->node(structure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), names.at(index));
            QCOMPARE(field->value().toULongLong(), values.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     static_cast<quint64>(index * 2));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(), quint64(2));
            QVERIFY(field->metadata().specification.has_value());
            QCOMPARE(field->metadata().specification->standard, QStringLiteral("Example"));
            if (index < 3) {
                QCOMPARE(field->metadata().description, QStringLiteral("Flags."));
            }
        }
    }

    void decodesLittleEndianArrayElementsWithoutChangingTheirSourceLocations() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<16, little> values[2]; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x34, 0x12, 0x78, 0x56}));
        const auto mapping = mappingForBytes(4);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 32);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("fixed-array-little-endian"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(2));
        const std::vector<quint64> values{0x1234, 0x5678};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto field = tree->node(structure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(),
                     QStringLiteral("values[%1]").arg(static_cast<qulonglong>(index)));
            QCOMPARE(field->value().toULongLong(), values.at(index));
            QCOMPARE(field->location()->logicalRange().bitLength(), quint64(16));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     static_cast<quint64>(index * 16));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(), quint64(16));
        }
    }

    void materializesFixedLengthExpGolombArraysWithExactLocations() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Codes { ue unsigned_values[3]; se signed_values[3]; } entry Codes;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0xa7, 0x4c}));
        const auto mapping = mappingForBytes(2);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 16);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("fixed-array-exp-golomb"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Codes"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(14));
        QCOMPARE(result.instructionsExecuted, quint64(8));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(6));

        const std::vector<quint64> starts{0, 1, 4, 7, 8, 11};
        const std::vector<quint64> lengths{1, 3, 3, 1, 3, 3};
        const std::vector<quint64> unsignedValues{0, 1, 2};
        for (std::size_t index = 0; index < unsignedValues.size(); ++index) {
            const auto field = tree->node(structure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(),
                     QStringLiteral("unsigned_values[%1]")
                         .arg(static_cast<qulonglong>(index)));
            QCOMPARE(field->value().metaType().id(), QMetaType::ULongLong);
            QCOMPARE(field->value().toULongLong(), unsignedValues.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     starts.at(index));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(), lengths.at(index));
        }
        const std::vector<qlonglong> signedValues{0, 1, -1};
        for (std::size_t index = 0; index < signedValues.size(); ++index) {
            const std::size_t childIndex = index + unsignedValues.size();
            const auto field = tree->node(structure->children().at(childIndex));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(),
                     QStringLiteral("signed_values[%1]")
                         .arg(static_cast<qulonglong>(index)));
            QCOMPARE(field->value().metaType().id(), QMetaType::LongLong);
            QCOMPARE(field->value().toLongLong(), signedValues.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     starts.at(childIndex));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(),
                     lengths.at(childIndex));
        }
    }

    void retainsCompletedFixedArrayElementsWhenTheNextElementIsTruncated() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<3> values[3]; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0b10111001}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("fixed-array-truncated"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
        QCOMPARE(result.bitsConsumed, quint64(6));
        QCOMPARE(reader.position(), quint64(6));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Invalid);
        QCOMPARE(structure->children().size(), std::size_t(2));
        QCOMPARE(tree->node(structure->children().at(0))->name(), QStringLiteral("values[0]"));
        QCOMPARE(tree->node(structure->children().at(1))->name(), QStringLiteral("values[1]"));
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::TruncatedSource);
        QCOMPARE(structure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.values[2]"));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(6));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(2));
        QVERIFY(tree->hasPartialResults());
    }

    void appliesConstraintsAndBudgetsPerFixedArrayElement() {
        const auto constrained = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> reserved[2] @equals(0); } entry Header;"));
        const auto budgeted = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flags[3]; } entry Header;"));
        QVERIFY(constrained.succeeded());
        QVERIFY(budgeted.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource constraintSource(bytes({0x40}));
        BitReader constraintReader(constraintSource, *range);
        auto constraintTree = AnalysisTree::create(QStringLiteral("fixed-array-constraint"));
        QVERIFY(constraintTree.has_value());
        const auto constraintResult = DslExecutor::decodeStruct(constrained.program,
                                                                 QStringLiteral("Header"),
                                                                 constraintReader,
                                                                 *mapping,
                                                                 0,
                                                                 *constraintTree,
                                                                 constraintTree->rootId());
        QCOMPARE(constraintResult.status, DslExecutionStatus::InvalidSyntax);
        const auto constraintStructure = constraintTree->node(*constraintResult.structureNode);
        QVERIFY(constraintStructure.has_value());
        QCOMPARE(constraintStructure->state(), MaterializationState::Invalid);
        QCOMPARE(constraintStructure->children().size(), std::size_t(2));
        QCOMPARE(constraintStructure->diagnostics().size(), std::size_t(1));
        QCOMPARE(constraintStructure->diagnostics().front().code,
                 DiagnosticCode::InvalidSyntax);
        QCOMPARE(constraintStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.reserved[1]"));
        QVERIFY(constraintTree->hasPartialResults());

        MemorySource budgetSource(bytes({0xe0}));
        BitReader budgetReader(budgetSource, *range);
        auto budgetTree = AnalysisTree::create(QStringLiteral("fixed-array-budget"));
        QVERIFY(budgetTree.has_value());
        DslExecutionOptions options;
        options.limits.maximumInstructions = 2;
        const auto budgetResult = DslExecutor::decodeStruct(budgeted.program,
                                                             QStringLiteral("Header"),
                                                             budgetReader,
                                                             *mapping,
                                                             0,
                                                             *budgetTree,
                                                             budgetTree->rootId(),
                                                             options);
        QCOMPARE(budgetResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(budgetResult.instructionsExecuted, quint64(2));
        QCOMPARE(budgetResult.bitsConsumed, quint64(1));
        const auto budgetStructure = budgetTree->node(*budgetResult.structureNode);
        QVERIFY(budgetStructure.has_value());
        QCOMPARE(budgetStructure->state(), MaterializationState::Invalid);
        QCOMPARE(budgetStructure->children().size(), std::size_t(1));
        QCOMPARE(budgetTree->node(budgetStructure->children().front())->name(),
                 QStringLiteral("flags[0]"));
        QCOMPARE(budgetStructure->diagnostics().size(), std::size_t(1));
        QCOMPARE(budgetStructure->diagnostics().front().code, DiagnosticCode::ResourceLimit);
        QVERIFY(budgetTree->hasPartialResults());

        MemorySource nodeBudgetSource(bytes({0xe0}));
        BitReader nodeBudgetReader(nodeBudgetSource, *range);
        auto nodeBudgetTree = AnalysisTree::create(QStringLiteral("fixed-array-node-budget"));
        QVERIFY(nodeBudgetTree.has_value());
        DslExecutionOptions nodeBudgetOptions;
        nodeBudgetOptions.limits.maximumMaterializedNodes = 2;
        const auto nodeBudgetResult = DslExecutor::decodeStruct(budgeted.program,
                                                                 QStringLiteral("Header"),
                                                                 nodeBudgetReader,
                                                                 *mapping,
                                                                 0,
                                                                 *nodeBudgetTree,
                                                                 nodeBudgetTree->rootId(),
                                                                 nodeBudgetOptions);
        QCOMPARE(nodeBudgetResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(nodeBudgetResult.nodesCreated, quint64(2));
        QCOMPARE(nodeBudgetResult.bitsConsumed, quint64(1));
        const auto nodeBudgetStructure =
            nodeBudgetTree->node(*nodeBudgetResult.structureNode);
        QVERIFY(nodeBudgetStructure.has_value());
        QCOMPARE(nodeBudgetStructure->state(), MaterializationState::Invalid);
        QCOMPARE(nodeBudgetStructure->children().size(), std::size_t(1));
        QCOMPARE(nodeBudgetTree->node(nodeBudgetStructure->children().front())->name(),
                 QStringLiteral("flags[0]"));
        QCOMPARE(nodeBudgetStructure->diagnostics().size(), std::size_t(1));
        QCOMPARE(nodeBudgetStructure->diagnostics().front().code,
                 DiagnosticCode::ResourceLimit);
        QCOMPARE(nodeBudgetStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.flags[1]"));
        QVERIFY(nodeBudgetTree->hasPartialResults());
    }

    void validatesEachFixedArrayEnumElementAndRetainsUnknownValues() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum Type { one = 1; two = 2; } "
            "struct Header { bits<2> values[2] @enum(Type); } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource validSource(bytes({0x60}));
        BitReader validReader(validSource, *range);
        auto validTree = AnalysisTree::create(QStringLiteral("fixed-array-enum-valid"));
        QVERIFY(validTree.has_value());
        const auto valid = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      validReader,
                                                      *mapping,
                                                      0,
                                                      *validTree,
                                                      validTree->rootId());
        QCOMPARE(valid.status, DslExecutionStatus::Materialized);
        const auto validStructure = validTree->node(*valid.structureNode);
        QVERIFY(validStructure.has_value());
        QCOMPARE(validStructure->children().size(), std::size_t(2));
        QCOMPARE(validTree->node(validStructure->children().at(0))->value().toULongLong(),
                 quint64(1));
        QCOMPARE(validTree->node(validStructure->children().at(1))->value().toULongLong(),
                 quint64(2));

        MemorySource invalidSource(bytes({0x70}));
        BitReader invalidReader(invalidSource, *range);
        auto invalidTree = AnalysisTree::create(QStringLiteral("fixed-array-enum-invalid"));
        QVERIFY(invalidTree.has_value());
        const auto invalid = DslExecutor::decodeStruct(*compiled.program,
                                                        quint32(0),
                                                        invalidReader,
                                                        *mapping,
                                                        0,
                                                        *invalidTree,
                                                        invalidTree->rootId());
        QCOMPARE(invalid.status, DslExecutionStatus::InvalidSyntax);
        const auto invalidStructure = invalidTree->node(*invalid.structureNode);
        QVERIFY(invalidStructure.has_value());
        QCOMPARE(invalidStructure->state(), MaterializationState::Invalid);
        QCOMPARE(invalidStructure->children().size(), std::size_t(2));
        const auto unknown = invalidTree->node(invalidStructure->children().at(1));
        QVERIFY(unknown.has_value());
        QCOMPARE(unknown->name(), QStringLiteral("values[1]"));
        QCOMPARE(unknown->value().toULongLong(), quint64(3));
        QCOMPARE(invalidStructure->diagnostics().size(), std::size_t(1));
        QCOMPARE(invalidStructure->diagnostics().front().code, DiagnosticCode::InvalidSyntax);
        QCOMPARE(invalidStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.values[1]"));
        QCOMPARE(invalidStructure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(2));
        QCOMPARE(invalidStructure->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(2));
        QVERIFY(invalidTree->hasPartialResults());
    }

    void decodesExplicitLittleEndianWithoutChangingSourceLocation() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<16, little> value; bits<3> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x34, 0x12, 0xa0}));
        const auto mapping = mappingForBytes(3);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 24);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("little-endian"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(19));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const auto value = tree->node(structure->children().at(0));
        const auto tail = tree->node(structure->children().at(1));
        QVERIFY(value.has_value());
        QVERIFY(tail.has_value());
        QCOMPARE(value->value().toULongLong(), quint64(0x1234));
        QCOMPARE(tail->value().toULongLong(), quint64(5));
        QCOMPARE(value->location()->logicalRange().bitLength(), quint64(16));
        QCOMPARE(value->location()->sourceSpans().front().start().absoluteBitOffset(), quint64(0));
        QCOMPARE(value->location()->sourceSpans().front().bitLength(), quint64(16));
    }

    void decodesAFullWidthLittleEndianValue() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<64, little> value; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({1, 2, 3, 4, 5, 6, 7, 8}));
        const auto mapping = mappingForBytes(8);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 64);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("little-endian-64"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       reader,
                                                       *mapping,
                                                       0,
                                                       *tree,
                                                       tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const auto value = tree->node(structure->children().front());
        QVERIFY(value.has_value());
        QCOMPARE(value->value().toULongLong(), quint64(0x0807060504030201ULL));
    }

    void rejectsLittleEndianAtAnUnalignedSourceAddress() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<16, little> value; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x03, 0x41, 0x20}));
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(4), 16);
        QVERIFY(range.has_value());
        const auto mapping = streamview::core::SourceMapping::create(
            streamview::core::LogicalViewId(1), {*range});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("unaligned-little-endian"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       reader,
                                                       *mapping,
                                                       0,
                                                       *tree,
                                                       tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(result.bitsConsumed, quint64(0));
        QCOMPARE(result.nodesCreated, quint64(1));
    }

    void validatesEnumValuesAndRetainsUnknownValueLocation() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum Type { one = 1; five = 5; } "
            "struct Header { bits<3> value @enum(Type); } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource validSource(bytes({0xa0}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader validReader(validSource, *range);
        auto validTree = AnalysisTree::create(QStringLiteral("enum-valid"));
        QVERIFY(validTree.has_value());
        const auto valid = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      validReader,
                                                      *mapping,
                                                      0,
                                                      *validTree,
                                                      validTree->rootId());
        QCOMPARE(valid.status, DslExecutionStatus::Materialized);
        const auto validStructure = validTree->node(*valid.structureNode);
        QVERIFY(validStructure.has_value());
        QCOMPARE(validTree->node(validStructure->children().front())->value().toULongLong(),
                 quint64(5));

        MemorySource invalidSource(bytes({0xe0}));
        BitReader invalidReader(invalidSource, *range);
        auto invalidTree = AnalysisTree::create(QStringLiteral("enum-invalid"));
        QVERIFY(invalidTree.has_value());
        const auto invalid = DslExecutor::decodeStruct(*compiled.program,
                                                        quint32(0),
                                                        invalidReader,
                                                        *mapping,
                                                        0,
                                                        *invalidTree,
                                                        invalidTree->rootId());
        QCOMPARE(invalid.status, DslExecutionStatus::InvalidSyntax);
        const auto invalidStructure = invalidTree->node(*invalid.structureNode);
        QVERIFY(invalidStructure.has_value());
        QCOMPARE(invalidStructure->children().size(), std::size_t(1));
        QCOMPARE(invalidStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.value"));
        QVERIFY(invalidStructure->diagnostics().front().location.has_value());
        QCOMPARE(
            invalidStructure->diagnostics().front().location->sourceSpans().front().bitLength(),
            quint64(3));
    }

    void rejectsMalformedEnumAndEndianTypedIr() {
        const auto enumParsed = DslParser::parse(QStringLiteral(
            "enum Type { one = 1; } struct Header { bits<3> value @enum(Type); } entry Header;"));
        const auto enumCompiled = DslCompiler::compile(enumParsed.program);
        QVERIFY(enumCompiled.succeeded());
        auto malformedEnum = *enumCompiled.program;
        malformedEnum.structs.front().fields.front().type.enumIndex = quint32(99);

        MemorySource source(bytes({0x20}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader enumReader(source, *range);
        auto enumTree = AnalysisTree::create(QStringLiteral("malformed-enum"));
        QVERIFY(enumTree.has_value());
        const auto enumResult = DslExecutor::decodeStruct(malformedEnum,
                                                           quint32(0),
                                                           enumReader,
                                                           *mapping,
                                                           0,
                                                           *enumTree,
                                                           enumTree->rootId());
        QCOMPARE(enumResult.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(enumResult.nodesCreated, quint64(1));

        const auto endianParsed = DslParser::parse(
            QStringLiteral("struct Header { bits<8> value; } entry Header;"));
        const auto endianCompiled = DslCompiler::compile(endianParsed.program);
        QVERIFY(endianCompiled.succeeded());
        auto malformedEndian = *endianCompiled.program;
        malformedEndian.structs.front().fields.front().type.endian =
            streamview::rules::DslEndian::Little;
        malformedEndian.structs.front().fields.front().type.bitWidth = 3;
        BitReader endianReader(source, *range);
        auto endianTree = AnalysisTree::create(QStringLiteral("malformed-endian"));
        QVERIFY(endianTree.has_value());
        const auto endianResult = DslExecutor::decodeStruct(malformedEndian,
                                                             quint32(0),
                                                             endianReader,
                                                             *mapping,
                                                             0,
                                                             *endianTree,
                                                             endianTree->rootId());
        QCOMPARE(endianResult.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(endianResult.nodesCreated, quint64(1));
    }

    void rejectsMalformedExpGolombOpcodeAndTypeWithoutConsumingInput() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { ue value; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x80}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        auto malformedOpcode = *compiled.program;
        malformedOpcode.bytecode.at(1).opcode = DslOpcode::ReadUnsignedBits;
        BitReader opcodeReader(source, *range);
        auto opcodeTree = AnalysisTree::create(QStringLiteral("malformed-exp-opcode"));
        QVERIFY(opcodeTree.has_value());
        const auto opcodeResult = DslExecutor::decodeStruct(malformedOpcode,
                                                             quint32(0),
                                                             opcodeReader,
                                                             *mapping,
                                                             0,
                                                             *opcodeTree,
                                                             opcodeTree->rootId());
        QCOMPARE(opcodeResult.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(opcodeResult.bitsConsumed, quint64(0));
        QCOMPARE(opcodeResult.nodesCreated, quint64(1));

        auto malformedType = *compiled.program;
        malformedType.structs.front().fields.front().type.kind = DslValueTypeKind::UnsignedBits;
        BitReader typeReader(source, *range);
        auto typeTree = AnalysisTree::create(QStringLiteral("malformed-exp-type"));
        QVERIFY(typeTree.has_value());
        const auto typeResult = DslExecutor::decodeStruct(malformedType,
                                                           quint32(0),
                                                           typeReader,
                                                           *mapping,
                                                           0,
                                                           *typeTree,
                                                           typeTree->rootId());
        QCOMPARE(typeResult.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(typeResult.bitsConsumed, quint64(0));
        QCOMPARE(typeResult.nodesCreated, quint64(1));
    }

    void carriesPresentationMetadataIntoAnalysisNodes() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "@spec(\"Example Standard\", \"4.2\") "
            "@description(\"A compact header.\") "
            "struct Header { "
            "bits<3> first @description(\"First field.\"); "
            "bits<5> second; "
            "} entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QVERIFY(result.structureNode.has_value());

        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->metadata().typeName, QStringLiteral("struct"));
        QCOMPARE(structure->metadata().description, QStringLiteral("A compact header."));
        QVERIFY(structure->metadata().specification.has_value());
        QCOMPARE(structure->metadata().specification->standard,
                 QStringLiteral("Example Standard"));
        QCOMPARE(structure->metadata().specification->clause, QStringLiteral("4.2"));

        const auto first = tree->node(structure->children().at(0));
        const auto second = tree->node(structure->children().at(1));
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QCOMPARE(first->metadata().typeName, QStringLiteral("bits"));
        QCOMPARE(first->metadata().description, QStringLiteral("First field."));
        QVERIFY(first->metadata().specification.has_value());
        QCOMPARE(first->metadata().specification->standard,
                 QStringLiteral("Example Standard"));
        QCOMPARE(first->location()->logicalRange().bitLength(), quint64{3});
        QCOMPARE(second->metadata().typeName, QStringLiteral("bits"));
        QVERIFY(second->metadata().description.isEmpty());
        QVERIFY(second->metadata().specification.has_value());
    }

    void retainsCompleteFieldsWhenTheNextFieldIsTruncated() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<8> second; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
        QVERIFY(result.structureNode.has_value());
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code,
                 streamview::core::DiagnosticCode::TruncatedSource);
        QVERIFY(structure->diagnostics().front().location.has_value());
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().size(),
                 std::size_t(1));
        QCOMPARE(structure->diagnostics()
                     .front()
                     .location->sourceSpans()
                     .front()
                     .start()
                     .absoluteBitOffset(),
                 quint64(3));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(5));
        QVERIFY(tree->hasPartialResults());
    }

    void enforcesEqualsConstraintAndKeepsFieldLocation() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> zero @equals(0); } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x80}));
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(1));
        QVERIFY(structure->children().front().value() != 0);
        QCOMPARE(structure->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(structure->diagnostics().front().fieldPath, QStringLiteral("Header.zero"));
    }

    void executesTypedIrByResolvedStructureIndex() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<5> second; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.instructionsExecuted, quint64(4));
        QCOMPARE(result.nodesCreated, quint64(3));
    }

    void enforcesInstructionBudgetAndRetainsCompletedFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<5> second; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        DslExecutionOptions options;
        options.limits.maximumInstructions = 2;

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId(), options);

        QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(result.instructionsExecuted, quint64(2));
        QCOMPARE(result.nodesCreated, quint64(2));
        QCOMPARE(result.bitsConsumed, quint64(3));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Invalid);
        QCOMPARE(structure->children().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::ResourceLimit);
    }

    void enforcesNodeBudgetAndRetainsCompletedFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> first; bits<5> second; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0b10110010}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        DslExecutionOptions options;
        options.limits.maximumMaterializedNodes = 2;

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId(), options);

        QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(result.nodesCreated, quint64(2));
        QCOMPARE(result.bitsConsumed, quint64(3));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::ResourceLimit);
    }

    void enforcesInclusiveNodeDepthBudget() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flag; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource successSource(bytes({0x00}));
        BitReader successReader(successSource, *range);
        auto successTree = AnalysisTree::create(QStringLiteral("success"));
        QVERIFY(successTree.has_value());
        DslExecutionOptions successOptions;
        successOptions.limits.maximumNodeDepth = 3;
        const auto success = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       successReader,
                                                       *mapping,
                                                       0,
                                                       *successTree,
                                                       successTree->rootId(),
                                                       successOptions);
        QCOMPARE(success.status, DslExecutionStatus::Materialized);

        MemorySource limitedSource(bytes({0x00}));
        BitReader limitedReader(limitedSource, *range);
        auto limitedTree = AnalysisTree::create(QStringLiteral("limited"));
        QVERIFY(limitedTree.has_value());
        DslExecutionOptions limitedOptions;
        limitedOptions.limits.maximumNodeDepth = 2;
        const auto limited = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       limitedReader,
                                                       *mapping,
                                                       0,
                                                       *limitedTree,
                                                       limitedTree->rootId(),
                                                       limitedOptions);
        QCOMPARE(limited.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(limited.nodesCreated, quint64(1));
        QVERIFY(limited.structureNode.has_value());
        const auto structure = limitedTree->node(*limited.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Invalid);
        QVERIFY(structure->children().empty());
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::ResourceLimit);
    }

    void observesPreCancelledTokenBeforeTheFirstInstruction() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flag; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x00}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        CancellationSource cancellation;
        QVERIFY(cancellation.requestCancellation());
        DslExecutionOptions options;
        options.cancellation = cancellation.token();

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId(), options);

        QCOMPARE(result.status, DslExecutionStatus::Cancelled);
        QCOMPARE(result.instructionsExecuted, quint64(0));
        QCOMPARE(result.nodesCreated, quint64(0));
        QVERIFY(!result.structureNode.has_value());
        const auto root = tree->node(tree->rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Cancelled);
        QCOMPARE(root->diagnostics().front().code, DiagnosticCode::Cancelled);
    }

    void rejectsMalformedEqualityBytecodeAfterRetainingTheField() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag @equals(0); } entry Header;"));
        auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        QCOMPARE(compiled.program->bytecode.size(), std::size_t(4));
        compiled.program->bytecode.at(2).immediate = 1;

        MemorySource source(bytes({0x00}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(result.nodesCreated, quint64(2));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Invalid);
        QCOMPARE(structure->children().size(), std::size_t(1));
    }

    void exposesTheDocumentedDefaultLimits() {
        const DslExecutionLimits limits;
        QCOMPARE(limits.maximumInstructions, quint64(1'000'000));
        QCOMPARE(limits.maximumCallDepth, quint32(64));
        QCOMPARE(limits.maximumViewDepth, quint32(64));
        QCOMPARE(limits.maximumNodeDepth, quint32(256));
        QCOMPARE(limits.maximumMaterializedNodes, quint64(100'000));
        QCOMPARE(limits.cancellationCheckInterval, quint64(1'024));
    }

    void rejectsLimitsAboveTheSandboxContract() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<1> flag; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x00}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("test"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        DslExecutionOptions options;
        options.limits.maximumInstructions =
            DslExecutionLimits::defaultMaximumInstructions() + 1U;

        const auto result = DslExecutor::decodeStruct(
            *compiled.program, quint32(0), reader, *mapping, 0, *tree, tree->rootId(), options);

        QCOMPARE(result.status, DslExecutionStatus::ResourceLimit);
        const auto root = tree->node(tree->rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->diagnostics().front().code, DiagnosticCode::ResourceLimit);
    }
};

QTEST_GUILESS_MAIN(DslExecutorTest)

#include "dsl_executor_test.moc"
