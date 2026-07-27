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
#include <utility>
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
using streamview::rules::DslConditionOperator;
using streamview::rules::DslBinaryOperator;
using streamview::rules::DslOpcode;
using streamview::rules::DslParser;
using streamview::rules::DslScalarType;
using streamview::rules::DslTypedExpression;
using streamview::rules::DslTypedExpressionKind;
using streamview::rules::DslTypedProgram;
using streamview::rules::DslUnaryOperator;
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
        ++readCount_;
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

    [[nodiscard]] quint64 readCount() const noexcept { return readCount_; }

private:
    std::vector<std::byte> data_;
    mutable quint64 readCount_ = 0;
};

class CancellingMemorySource final : public RandomAccessSource {
public:
    CancellingMemorySource(std::vector<std::byte> data,
                           CancellationSource& cancellation)
        : data_(std::move(data)), cancellation_(&cancellation) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("cancelling-memory");
    }

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
        if (!cancellationRequested_) {
            cancellationRequested_ = true;
            (void)cancellation_->requestCancellation();
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
    CancellationSource* cancellation_ = nullptr;
    mutable bool cancellationRequested_ = false;
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

[[nodiscard]] std::optional<streamview::core::SourceMapping> mappingForSpans(
    std::initializer_list<std::pair<quint64, quint64>> ranges) {
    std::vector<SourceSpan> spans;
    spans.reserve(ranges.size());
    for (const auto& [start, bitLength] : ranges) {
        const auto span =
            SourceSpan::create(streamview::core::SourceBitAddress(start), bitLength);
        if (!span) {
            return std::nullopt;
        }
        spans.push_back(*span);
    }
    return streamview::core::SourceMapping::create(
        streamview::core::LogicalViewId(1), std::move(spans));
}

} // namespace

class DslExecutorTest final : public QObject {
    Q_OBJECT

private slots:
    void rejectsMismatchedReaderBackingBeforeExecution() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<8> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0xff, 0x00}));
        const auto readerSpan =
            SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        const auto mappingSpan =
            SourceSpan::create(streamview::core::SourceBitAddress(8), 8);
        QVERIFY(readerSpan.has_value());
        QVERIFY(mappingSpan.has_value());
        const auto mapping = streamview::core::SourceMapping::create(
            streamview::core::LogicalViewId(1), {*mappingSpan});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *readerSpan);
        auto tree = AnalysisTree::create(QStringLiteral("mismatched-reader-backing"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(result.errorMessage,
                 QStringLiteral("DSL reader backing does not match the supplied source mapping"));
        QCOMPARE(result.instructionsExecuted, quint64{0});
        QCOMPARE(result.bitsConsumed, quint64{0});
        QCOMPARE(result.nodesCreated, quint64{0});
        QVERIFY(!result.structureNode.has_value());
        QCOMPARE(reader.position(), quint64{0});
        QCOMPARE(source.readCount(), quint64{0});
        const auto root = tree->node(tree->rootId());
        QVERIFY(root.has_value());
        QVERIFY(root->children().empty());
    }

    void rejectsDifferentMappedSpanTopologyBeforeExecution() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<16> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0xab, 0xcd, 0xef}));
        const auto readerMapping = mappingForSpans({{0, 8}, {16, 8}});
        const auto publishedMapping = mappingForSpans({{0, 16}});
        QVERIFY(readerMapping.has_value());
        QVERIFY(publishedMapping.has_value());
        BitReader reader(source, *readerMapping);
        auto tree = AnalysisTree::create(QStringLiteral("mismatched-span-topology"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(parsed.program,
                                                       QStringLiteral("Header"),
                                                       reader,
                                                       *publishedMapping,
                                                       0,
                                                       *tree,
                                                       tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(result.instructionsExecuted, quint64{0});
        QCOMPARE(result.nodesCreated, quint64{0});
        QVERIFY(!result.structureNode.has_value());
        QCOMPARE(reader.position(), quint64{0});
        QCOMPARE(source.readCount(), quint64{0});
    }

    void acceptsAnEmptyMappedReaderAndReportsFieldTruncation() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<1> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({}));
        const auto mapping = streamview::core::SourceMapping::create(
            streamview::core::LogicalViewId(1), {});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);
        auto tree = AnalysisTree::create(QStringLiteral("empty-mapped-reader"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
        QCOMPARE(result.bitsConsumed, quint64{0});
        QCOMPARE(result.nodesCreated, quint64{1});
        QVERIFY(result.structureNode.has_value());
        QCOMPARE(reader.position(), quint64{0});
        QCOMPARE(source.readCount(), quint64{0});
    }

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

    void materializesComputedValuesWithoutSourceLocations() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "pure u64 twice(u64 value) { return value * 2; } "
            "pure bool at_least(u64 value, u64 minimum) { return value >= minimum; } "
            "struct Header { bits<4> source; "
            "computed<u64> doubled = twice(source) @description(\"Derived value.\"); "
            "computed<bool> large = at_least(doubled, 10); bits<4> tail; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x6a}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        auto tree = AnalysisTree::create(QStringLiteral("computed-values"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       reader,
                                                       *mapping,
                                                       0,
                                                       *tree,
                                                       tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(8));
        QCOMPARE(result.instructionsExecuted, quint64(6));
        QCOMPARE(result.nodesCreated, quint64(5));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(4));
        const auto sourceField = tree->node(structure->children().at(0));
        const auto doubled = tree->node(structure->children().at(1));
        const auto large = tree->node(structure->children().at(2));
        const auto tail = tree->node(structure->children().at(3));
        QVERIFY(sourceField.has_value());
        QVERIFY(doubled.has_value());
        QVERIFY(large.has_value());
        QVERIFY(tail.has_value());
        QCOMPARE(sourceField->kind(), AnalysisNodeKind::SyntaxField);
        QCOMPARE(doubled->kind(), AnalysisNodeKind::ComputedField);
        QCOMPARE(large->kind(), AnalysisNodeKind::ComputedField);
        QCOMPARE(tail->kind(), AnalysisNodeKind::SyntaxField);
        QCOMPARE(sourceField->value().toULongLong(), quint64(6));
        QCOMPARE(doubled->value().toULongLong(), quint64(12));
        QCOMPARE(large->value().toBool(), true);
        QCOMPARE(tail->value().toULongLong(), quint64(10));
        QVERIFY(sourceField->location().has_value());
        QVERIFY(!doubled->location().has_value());
        QVERIFY(!large->location().has_value());
        QVERIFY(tail->location().has_value());
        QCOMPARE(doubled->metadata().typeName, QStringLiteral("computed<u64>"));
        QCOMPARE(doubled->metadata().description, QStringLiteral("Derived value."));
        QCOMPARE(large->metadata().typeName, QStringLiteral("computed<bool>"));
    }

    void registersMappedLazyByteRegionsWithoutReadingPayloads() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { @lazy(2) bytes payload @description(\"Payload.\"); } "
            "entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0xaa, 0xff, 0xbb}));
        const auto mapping = mappingForSpans({{0, 8}, {16, 8}});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);
        auto tree = AnalysisTree::create(QStringLiteral("mapped-lazy"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(16));
        QCOMPARE(result.instructionsExecuted, quint64(3));
        QCOMPARE(result.nodesCreated, quint64(2));
        QCOMPARE(reader.position(), quint64(16));
        QCOMPARE(source.readCount(), quint64(0));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Materialized);
        QCOMPARE(structure->children().size(), std::size_t(1));
        const auto payload = tree->node(structure->children().front());
        QVERIFY(payload.has_value());
        QCOMPARE(payload->kind(), AnalysisNodeKind::Region);
        QCOMPARE(payload->state(), MaterializationState::Lazy);
        QCOMPARE(payload->name(), QStringLiteral("payload"));
        QCOMPARE(payload->metadata().typeName, QStringLiteral("bytes"));
        QCOMPARE(payload->metadata().description, QStringLiteral("Payload."));
        QVERIFY(payload->location().has_value());
        QCOMPARE(payload->location()->logicalRange().start().bitOffset(), quint64(0));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(16));
        QCOMPARE(payload->location()->sourceSpans().size(), std::size_t(2));
        QCOMPARE(payload->location()->sourceSpans().at(0).start().absoluteBitOffset(), quint64(0));
        QCOMPARE(payload->location()->sourceSpans().at(0).bitLength(), quint64(8));
        QCOMPARE(payload->location()->sourceSpans().at(1).start().absoluteBitOffset(), quint64(16));
        QCOMPARE(payload->location()->sourceSpans().at(1).bitLength(), quint64(8));
    }

    void registersLogicallyAlignedLazyRegionsFromUnalignedSourceSpans() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { @lazy(1) bytes payload; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x0a, 0xa0}));
        const auto mapping = mappingForSpans({{4, 8}});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);
        auto tree = AnalysisTree::create(QStringLiteral("unaligned-source-lazy"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(8));
        QCOMPARE(reader.position(), quint64(8));
        QCOMPARE(source.readCount(), quint64(0));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(1));
        const auto payload = tree->node(structure->children().front());
        QVERIFY(payload.has_value());
        QCOMPARE(payload->state(), MaterializationState::Lazy);
        QVERIFY(payload->location().has_value());
        QCOMPARE(payload->location()->logicalRange().start().bitOffset(), quint64(0));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(8));
        QCOMPARE(payload->location()->sourceSpans().size(), std::size_t(1));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(4));
        QCOMPARE(payload->location()->sourceSpans().front().bitLength(), quint64(8));
    }

    void skipsDynamicLazyPayloadsAndContinuesWithTrailingFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> size; @lazy(size) bytes payload; bits<8> tail; } "
            "entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x02, 0xaa, 0xbb, 0xcc}));
        const auto mapping = mappingForBytes(4);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 32);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("dynamic-lazy"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       reader,
                                                       *mapping,
                                                       0,
                                                       *tree,
                                                       tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(32));
        QCOMPARE(result.instructionsExecuted, quint64(5));
        QCOMPARE(result.nodesCreated, quint64(4));
        QCOMPARE(source.readCount(), quint64(2));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(3));
        const auto payload = tree->node(structure->children().at(1));
        const auto tail = tree->node(structure->children().at(2));
        QVERIFY(payload.has_value());
        QVERIFY(tail.has_value());
        QCOMPARE(payload->kind(), AnalysisNodeKind::Region);
        QCOMPARE(payload->state(), MaterializationState::Lazy);
        QCOMPARE(payload->location()->logicalRange().start().bitOffset(), quint64(8));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(16));
        QCOMPARE(payload->location()->sourceSpans().size(), std::size_t(1));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(8));
        QCOMPARE(payload->location()->sourceSpans().front().bitLength(), quint64(16));
        QCOMPARE(tail->value().toULongLong(), quint64(0xcc));
        QCOMPARE(tail->location()->logicalRange().start().bitOffset(), quint64(24));
    }

    void materializesZeroLengthLazyRegionsWithoutAdvancingTheReader() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { @lazy(0) bytes empty; bits<8> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x5a}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("empty-lazy"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(8));
        QCOMPARE(reader.position(), quint64(8));
        QCOMPARE(source.readCount(), quint64(1));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(2));
        const auto empty = tree->node(structure->children().at(0));
        const auto tail = tree->node(structure->children().at(1));
        QVERIFY(empty.has_value());
        QVERIFY(tail.has_value());
        QCOMPARE(empty->kind(), AnalysisNodeKind::Region);
        QCOMPARE(empty->state(), MaterializationState::Materialized);
        QVERIFY(empty->location().has_value());
        QCOMPARE(empty->location()->logicalRange().start().bitOffset(), quint64(0));
        QCOMPARE(empty->location()->logicalRange().bitLength(), quint64(0));
        QVERIFY(empty->location()->sourceSpans().empty());
        QCOMPARE(tail->value().toULongLong(), quint64(0x5a));
        QVERIFY(tail->location().has_value());
        QCOMPARE(tail->location()->logicalRange().start().bitOffset(), quint64(0));
    }

    void skipsGuardedLazyRegionsWithoutEvaluatingTheirExpressions() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> present; if (present == 1) { "
            "@lazy(1 / 0) bytes payload; } bits<8> tail; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x00, 0x5a}));
        const auto mapping = mappingForBytes(2);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 16);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("skipped-lazy"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       reader,
                                                       *mapping,
                                                       0,
                                                       *tree,
                                                       tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(16));
        QCOMPARE(result.instructionsExecuted, quint64(5));
        QCOMPARE(result.nodesCreated, quint64(3));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(2));
        QCOMPARE(tree->node(structure->children().at(0))->name(), QStringLiteral("present"));
        QCOMPARE(tree->node(structure->children().at(1))->name(), QStringLiteral("tail"));
        QCOMPARE(tree->node(structure->children().at(1))->value().toULongLong(), quint64(0x5a));
    }

    void rejectsLazyRangeFailuresBeforeAppendingOrAdvancing() {
        const auto truncatedParsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> size; @lazy(size) bytes payload; } entry Header;"));
        const auto truncatedCompiled = DslCompiler::compile(truncatedParsed.program);
        QVERIFY(truncatedParsed.succeeded());
        QVERIFY(truncatedCompiled.succeeded());

        MemorySource truncatedSource(bytes({0x03, 0x00, 0xaa, 0x00, 0xbb}));
        const auto truncatedMapping = mappingForSpans({{0, 8}, {16, 8}, {32, 8}});
        QVERIFY(truncatedMapping.has_value());
        BitReader truncatedReader(truncatedSource, *truncatedMapping);
        auto truncatedTree = AnalysisTree::create(QStringLiteral("truncated-lazy"));
        QVERIFY(truncatedTree.has_value());

        const auto truncated = DslExecutor::decodeStruct(*truncatedCompiled.program,
                                                         quint32(0),
                                                         truncatedReader,
                                                         *truncatedMapping,
                                                         0,
                                                         *truncatedTree,
                                                         truncatedTree->rootId());
        QCOMPARE(truncated.status, DslExecutionStatus::TruncatedSource);
        QCOMPARE(truncated.instructionsExecuted, quint64(3));
        QCOMPARE(truncated.bitsConsumed, quint64(8));
        QCOMPARE(truncated.nodesCreated, quint64(2));
        QCOMPARE(truncatedReader.position(), quint64(8));
        QCOMPARE(truncatedSource.readCount(), quint64(1));
        const auto truncatedStructure = truncatedTree->node(*truncated.structureNode);
        QVERIFY(truncatedStructure.has_value());
        QCOMPARE(truncatedStructure->children().size(), std::size_t(1));
        QCOMPARE(truncatedStructure->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = truncatedStructure->diagnostics().front();
        QCOMPARE(diagnostic.code, DiagnosticCode::TruncatedSource);
        QCOMPARE(diagnostic.fieldPath, QStringLiteral("Header.payload"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->logicalRange().start().bitOffset(), quint64(8));
        QCOMPARE(diagnostic.location->logicalRange().bitLength(), quint64(16));
        QCOMPARE(diagnostic.location->sourceSpans().size(), std::size_t(2));
        QCOMPARE(diagnostic.location->sourceSpans().at(0).start().absoluteBitOffset(),
                 quint64(16));
        QCOMPARE(diagnostic.location->sourceSpans().at(0).bitLength(), quint64(8));
        QCOMPARE(diagnostic.location->sourceSpans().at(1).start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(diagnostic.location->sourceSpans().at(1).bitLength(), quint64(8));

        const std::vector<QString> invalidCounts{
            QStringLiteral("18446744073709551615 + 1"),
            QStringLiteral("2305843009213693952"),
        };
        for (std::size_t index = 0; index < invalidCounts.size(); ++index) {
            const auto parsed = DslParser::parse(
                QStringLiteral("struct Header { @lazy(%1) bytes payload; } entry Header;")
                    .arg(invalidCounts.at(index)));
            const auto compiled = DslCompiler::compile(parsed.program);
            QVERIFY(parsed.succeeded());
            QVERIFY(compiled.succeeded());

            MemorySource source(bytes({0xaa}));
            const auto mapping = mappingForBytes(1);
            const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
            QVERIFY(mapping.has_value());
            QVERIFY(range.has_value());
            BitReader reader(source, *range);
            auto tree =
                AnalysisTree::create(QStringLiteral("invalid-lazy-count-%1").arg(index));
            QVERIFY(tree.has_value());
            const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
            QCOMPARE(result.instructionsExecuted, quint64(2));
            QCOMPARE(result.bitsConsumed, quint64(0));
            QCOMPARE(result.nodesCreated, quint64(1));
            QCOMPARE(reader.position(), quint64(0));
            QCOMPARE(source.readCount(), quint64(0));
            const auto structure = tree->node(*result.structureNode);
            QVERIFY(structure.has_value());
            QVERIFY(structure->children().empty());
            QCOMPARE(structure->diagnostics().front().fieldPath,
                     QStringLiteral("Header.payload"));
            QVERIFY(!structure->diagnostics().front().location.has_value());
        }
    }

    void rejectsLazyRegionsAtUnalignedAbsoluteLogicalStarts() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { @lazy(1) bytes payload; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x03, 0x41, 0x20}));
        const auto mapping = mappingForSpans({{4, 20}});
        QVERIFY(mapping.has_value());
        auto reader = BitReader::fromMappingSlice(source, *mapping, 4, 8);
        QVERIFY(reader.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("unaligned-logical-lazy"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(parsed.program,
                                                       QStringLiteral("Header"),
                                                       *reader,
                                                       *mapping,
                                                       4,
                                                       *tree,
                                                       tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(result.instructionsExecuted, quint64(2));
        QCOMPARE(result.bitsConsumed, quint64(0));
        QCOMPARE(result.nodesCreated, quint64(1));
        QCOMPARE(reader->position(), quint64(0));
        QCOMPARE(source.readCount(), quint64(0));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QVERIFY(structure->children().empty());
        QVERIFY(!structure->diagnostics().front().location.has_value());
    }

    void accountsForLazyNodeInstructionAndCancellationBudgets() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> size; @lazy(size) bytes payload; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(2);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 16);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource instructionSource(bytes({0x01, 0xaa}));
        BitReader instructionReader(instructionSource, *range);
        auto instructionTree = AnalysisTree::create(QStringLiteral("lazy-instruction-budget"));
        QVERIFY(instructionTree.has_value());
        DslExecutionOptions instructionOptions;
        instructionOptions.limits.maximumInstructions = 2;
        const auto instructionResult = DslExecutor::decodeStruct(*compiled.program,
                                                                  quint32(0),
                                                                  instructionReader,
                                                                  *mapping,
                                                                  0,
                                                                  *instructionTree,
                                                                  instructionTree->rootId(),
                                                                  instructionOptions);
        QCOMPARE(instructionResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(instructionResult.instructionsExecuted, quint64(2));
        QCOMPARE(instructionResult.bitsConsumed, quint64(8));
        QCOMPARE(instructionResult.nodesCreated, quint64(2));
        QCOMPARE(instructionReader.position(), quint64(8));
        const auto instructionStructure =
            instructionTree->node(*instructionResult.structureNode);
        QVERIFY(instructionStructure.has_value());
        QCOMPARE(instructionStructure->children().size(), std::size_t(1));

        MemorySource nodeSource(bytes({0x01, 0xaa}));
        BitReader nodeReader(nodeSource, *range);
        auto nodeTree = AnalysisTree::create(QStringLiteral("lazy-node-budget"));
        QVERIFY(nodeTree.has_value());
        DslExecutionOptions nodeOptions;
        nodeOptions.limits.maximumMaterializedNodes = 2;
        const auto nodeResult = DslExecutor::decodeStruct(*compiled.program,
                                                         quint32(0),
                                                         nodeReader,
                                                         *mapping,
                                                         0,
                                                         *nodeTree,
                                                         nodeTree->rootId(),
                                                         nodeOptions);
        QCOMPARE(nodeResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(nodeResult.instructionsExecuted, quint64(3));
        QCOMPARE(nodeResult.bitsConsumed, quint64(8));
        QCOMPARE(nodeResult.nodesCreated, quint64(2));
        QCOMPARE(nodeReader.position(), quint64(8));
        const auto nodeStructure = nodeTree->node(*nodeResult.structureNode);
        QVERIFY(nodeStructure.has_value());
        QCOMPARE(nodeStructure->children().size(), std::size_t(1));
        QCOMPARE(nodeStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.payload"));

        CancellationSource cancellation;
        CancellingMemorySource cancellingSource(bytes({0x01, 0xaa}), cancellation);
        BitReader cancellingReader(cancellingSource, *range);
        auto cancellingTree = AnalysisTree::create(QStringLiteral("lazy-cancelled"));
        QVERIFY(cancellingTree.has_value());
        DslExecutionOptions cancellationOptions;
        cancellationOptions.cancellation = cancellation.token();
        cancellationOptions.limits.cancellationCheckInterval = 1;
        const auto cancelled = DslExecutor::decodeStruct(*compiled.program,
                                                        quint32(0),
                                                        cancellingReader,
                                                        *mapping,
                                                        0,
                                                        *cancellingTree,
                                                        cancellingTree->rootId(),
                                                        cancellationOptions);
        QCOMPARE(cancelled.status, DslExecutionStatus::Cancelled);
        QCOMPARE(cancelled.instructionsExecuted, quint64(2));
        QCOMPARE(cancelled.bitsConsumed, quint64(8));
        QCOMPARE(cancelled.nodesCreated, quint64(2));
        QCOMPARE(cancellingReader.position(), quint64(8));
        const auto cancelledStructure = cancellingTree->node(*cancelled.structureNode);
        QVERIFY(cancelledStructure.has_value());
        QCOMPARE(cancelledStructure->state(), MaterializationState::Cancelled);
        QCOMPARE(cancelledStructure->children().size(), std::size_t(1));
    }

    void rejectsMalformedLazyDefinitionsBeforeExecution() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> size; @lazy(size) bytes payload; bits<8> tail; } "
            "entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());

        std::vector<DslTypedProgram> malformed;
        auto missingExpression = *compiled.program;
        missingExpression.structs.front().fields.at(1).lazyByteCountExpression.reset();
        malformed.push_back(std::move(missingExpression));

        auto invalidWidth = *compiled.program;
        invalidWidth.structs.front().fields.at(1).type.bitWidth = 8;
        malformed.push_back(std::move(invalidWidth));

        auto invalidEndian = *compiled.program;
        invalidEndian.structs.front().fields.at(1).type.endian =
            streamview::rules::DslEndian::Little;
        malformed.push_back(std::move(invalidEndian));

        auto invalidEnum = *compiled.program;
        invalidEnum.structs.front().fields.at(1).type.enumIndex = 0;
        malformed.push_back(std::move(invalidEnum));

        auto invalidConstraint = *compiled.program;
        invalidConstraint.structs.front().fields.at(1).equalsConstraint = 1;
        malformed.push_back(std::move(invalidConstraint));

        auto invalidScalarType = *compiled.program;
        invalidScalarType.structs.front().fields.at(1).lazyByteCountExpression->type =
            DslScalarType::Bool;
        malformed.push_back(std::move(invalidScalarType));

        auto invalidKind = *compiled.program;
        invalidKind.structs.front().fields.at(1).lazyByteCountExpression->kind =
            static_cast<DslTypedExpressionKind>(255);
        malformed.push_back(std::move(invalidKind));

        auto skippedInvalidKind = *compiled.program;
        skippedInvalidKind.structs.front().fields.at(1).conditions.push_back(
            {0, 0, false, DslConditionOperator::Equal});
        skippedInvalidKind.structs.front().fields.at(1).lazyByteCountExpression->kind =
            static_cast<DslTypedExpressionKind>(255);
        malformed.push_back(std::move(skippedInvalidKind));

        auto futureReference = *compiled.program;
        futureReference.structs.front().fields.at(1).lazyByteCountExpression->fieldIndex = 2;
        malformed.push_back(std::move(futureReference));

        auto lazyDependency = *compiled.program;
        DslTypedExpression literal;
        literal.kind = DslTypedExpressionKind::UnsignedLiteral;
        literal.type = DslScalarType::U64;
        literal.unsignedValue = 1;
        auto& firstField = lazyDependency.structs.front().fields.at(0);
        firstField.type.kind = DslValueTypeKind::LazyBytes;
        firstField.type.bitWidth = 0;
        firstField.lazyByteCountExpression = literal;
        malformed.push_back(std::move(lazyDependency));

        auto sourceWithLazyExpression = *compiled.program;
        sourceWithLazyExpression.structs.front().fields.front().lazyByteCountExpression =
            sourceWithLazyExpression.structs.front().fields.at(1).lazyByteCountExpression;
        malformed.push_back(std::move(sourceWithLazyExpression));

        auto lazyWithComputedExpression = *compiled.program;
        lazyWithComputedExpression.structs.front().fields.at(1).computedExpression = literal;
        malformed.push_back(std::move(lazyWithComputedExpression));

        const auto mapping = mappingForBytes(3);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 24);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        for (std::size_t index = 0; index < malformed.size(); ++index) {
            MemorySource source(bytes({0x01, 0xaa, 0xbb}));
            BitReader reader(source, *range);
            auto tree =
                AnalysisTree::create(QStringLiteral("malformed-lazy-definition-%1").arg(index));
            QVERIFY(tree.has_value());
            const auto result = DslExecutor::decodeStruct(malformed.at(index),
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
            QCOMPARE(result.instructionsExecuted, quint64(0));
            QCOMPARE(result.bitsConsumed, quint64(0));
            QCOMPARE(result.nodesCreated, quint64(0));
            QCOMPARE(reader.position(), quint64(0));
            QCOMPARE(source.readCount(), quint64(0));
            QVERIFY(!result.structureNode.has_value());
        }
    }

    void rejectsMalformedLazyOpcodesAfterRetainingEarlierFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> size; @lazy(size) bytes payload; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());
        const auto lazyInstruction =
            std::find_if(compiled.program->bytecode.cbegin(),
                         compiled.program->bytecode.cend(),
                         [](const auto& instruction) {
                             return instruction.opcode == DslOpcode::RegisterLazyBytes;
                         });
        QVERIFY(lazyInstruction != compiled.program->bytecode.cend());
        const auto lazyInstructionIndex = static_cast<std::size_t>(
            std::distance(compiled.program->bytecode.cbegin(), lazyInstruction));

        std::vector<DslTypedProgram> malformed;
        auto wrongOperand = *compiled.program;
        wrongOperand.bytecode.at(lazyInstructionIndex).operand = 0;
        malformed.push_back(std::move(wrongOperand));
        auto wrongReadOpcode = *compiled.program;
        wrongReadOpcode.bytecode.at(lazyInstructionIndex).opcode = DslOpcode::ReadUnsignedBits;
        malformed.push_back(std::move(wrongReadOpcode));
        auto wrongComputedOpcode = *compiled.program;
        wrongComputedOpcode.bytecode.at(lazyInstructionIndex).opcode = DslOpcode::EvaluateComputed;
        malformed.push_back(std::move(wrongComputedOpcode));
        auto invalidOpcode = *compiled.program;
        invalidOpcode.bytecode.at(lazyInstructionIndex).opcode = static_cast<DslOpcode>(255);
        malformed.push_back(std::move(invalidOpcode));

        const auto mapping = mappingForBytes(2);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 16);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        for (std::size_t index = 0; index < malformed.size(); ++index) {
            MemorySource source(bytes({0x01, 0xaa}));
            BitReader reader(source, *range);
            auto tree =
                AnalysisTree::create(QStringLiteral("malformed-lazy-opcode-%1").arg(index));
            QVERIFY(tree.has_value());
            const auto result = DslExecutor::decodeStruct(malformed.at(index),
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
            QCOMPARE(result.instructionsExecuted, quint64(3));
            QCOMPARE(result.bitsConsumed, quint64(8));
            QCOMPARE(result.nodesCreated, quint64(2));
            QCOMPARE(reader.position(), quint64(8));
            QCOMPARE(source.readCount(), quint64(1));
            const auto structure = tree->node(*result.structureNode);
            QVERIFY(structure.has_value());
            QCOMPARE(structure->state(), MaterializationState::Invalid);
            QCOMPARE(structure->children().size(), std::size_t(1));
            QCOMPARE(tree->node(structure->children().front())->name(), QStringLiteral("size"));
        }
    }

    void shortCircuitsBooleanExpressionsAndRejectsInvalidArithmetic() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> pad; "
            "computed<u64> math = ((8 * 3) / 2) % 5 + 7 - 3; "
            "computed<bool> comparisons = math == 6 && math != 7 && math < 7 && "
            "math <= 6 && math > 5 && math >= 6; "
            "computed<bool> negated = !false; "
            "computed<bool> safe_and = false && (1 / 0 == 0); "
            "computed<bool> safe_or = true || (1 % 0 == 0); } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x00}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 1);
        auto tree = AnalysisTree::create(QStringLiteral("computed-operators"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       reader,
                                                       *mapping,
                                                       0,
                                                       *tree,
                                                       tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(1));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(6));
        QCOMPARE(tree->node(structure->children().at(1))->value().toULongLong(), quint64(6));
        QCOMPARE(tree->node(structure->children().at(2))->value().toBool(), true);
        QCOMPARE(tree->node(structure->children().at(3))->value().toBool(), true);
        QCOMPARE(tree->node(structure->children().at(4))->value().toBool(), false);
        QCOMPARE(tree->node(structure->children().at(5))->value().toBool(), true);

        const std::vector<QString> invalidExpressions = {
            QStringLiteral("18446744073709551615 + 1"),
            QStringLiteral("18446744073709551615 * 2"),
            QStringLiteral("0 - 1"),
            QStringLiteral("1 / 0"),
            QStringLiteral("1 % 0"),
        };
        for (std::size_t index = 0; index < invalidExpressions.size(); ++index) {
            const auto invalidParsed = DslParser::parse(
                QStringLiteral("struct Header { bits<1> pad; computed<u64> failure = %1; } "
                               "entry Header;")
                    .arg(invalidExpressions.at(index)));
            const auto invalidCompiled = DslCompiler::compile(invalidParsed.program);
            QVERIFY(invalidParsed.succeeded());
            QVERIFY(invalidCompiled.succeeded());

            MemorySource invalidSource(bytes({0x00}));
            BitReader invalidReader(invalidSource, *range);
            auto invalidTree = AnalysisTree::create(
                QStringLiteral("computed-arithmetic-%1").arg(index));
            QVERIFY(invalidTree.has_value());
            const auto invalid = DslExecutor::decodeStruct(*invalidCompiled.program,
                                                           quint32(0),
                                                           invalidReader,
                                                           *mapping,
                                                           0,
                                                           *invalidTree,
                                                           invalidTree->rootId());
            QCOMPARE(invalid.status, DslExecutionStatus::InvalidSyntax);
            QCOMPARE(invalid.instructionsExecuted, quint64(3));
            QCOMPARE(invalid.bitsConsumed, quint64(1));
            QCOMPARE(invalid.nodesCreated, quint64(2));
            const auto invalidStructure = invalidTree->node(*invalid.structureNode);
            QVERIFY(invalidStructure.has_value());
            QCOMPARE(invalidStructure->state(), MaterializationState::Invalid);
            QCOMPARE(invalidStructure->children().size(), std::size_t(1));
            QCOMPARE(invalidStructure->diagnostics().size(), std::size_t(1));
            QCOMPARE(invalidStructure->diagnostics().front().code,
                     DiagnosticCode::InvalidSyntax);
            QCOMPARE(invalidStructure->diagnostics().front().fieldPath,
                     QStringLiteral("Header.failure"));
            QVERIFY(!invalidStructure->diagnostics().front().location.has_value());
        }
    }

    void usesComputedValuesAsGuardAndRepeatControllers() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> base; computed<u64> count = base - 1; "
            "computed<bool> enabled = count == 2; "
            "if (enabled) { computed<u64> selected = count + 10; } "
            "else { computed<u64> skipped_if = 1 / 0; } "
            "switch (count) { case 2: { computed<u64> selected_case = count * 2; } "
            "default: { computed<u64> skipped_default = 1 / 0; } } "
            "repeat (count, 2) { computed<u64> repeated = count + 1; } "
            "bits<1> tail; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(parsed.succeeded(),
                 parsed.diagnostics.empty()
                     ? "Parser failed without a diagnostic"
                     : qPrintable(parsed.diagnostics.front().message));
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? "Compiler failed without a diagnostic"
                     : qPrintable(compiled.diagnostics.front().message));

        MemorySource source(bytes({0x70}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 4);
        auto tree = AnalysisTree::create(QStringLiteral("computed-controllers"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       reader,
                                                       *mapping,
                                                       0,
                                                       *tree,
                                                       tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(4));
        QCOMPARE(result.instructionsExecuted, quint64(13));
        QCOMPARE(result.nodesCreated, quint64(9));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const std::vector<QString> expectedNames = {
            QStringLiteral("base"),
            QStringLiteral("count"),
            QStringLiteral("enabled"),
            QStringLiteral("selected"),
            QStringLiteral("selected_case"),
            QStringLiteral("repeated[0]"),
            QStringLiteral("repeated[1]"),
            QStringLiteral("tail"),
        };
        const std::vector<quint64> expectedValues = {3, 2, 1, 12, 4, 3, 3, 1};
        QCOMPARE(structure->children().size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto field = tree->node(structure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), expectedNames.at(index));
            QCOMPARE(field->value().toULongLong(), expectedValues.at(index));
        }
        QVERIFY(!tree->node(structure->children().at(1))->location().has_value());
        QVERIFY(!tree->node(structure->children().at(2))->location().has_value());
        QVERIFY(!tree->node(structure->children().at(3))->location().has_value());
        QVERIFY(!tree->node(structure->children().at(4))->location().has_value());
        QVERIFY(!tree->node(structure->children().at(5))->location().has_value());
        QVERIFY(!tree->node(structure->children().at(6))->location().has_value());
    }

    void reportsComputedRepeatBoundsWithoutSourceLocations() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> base; computed<u64> count = base + 1; "
            "repeat (count, 3) { computed<u64> item = count; } } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x60}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 3);
        auto tree = AnalysisTree::create(QStringLiteral("computed-repeat-over-maximum"));
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        QVERIFY(tree.has_value());
        BitReader reader(source, *range);
        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       reader,
                                                       *mapping,
                                                       0,
                                                       *tree,
                                                       tree->rootId());

        QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
        QCOMPARE(result.instructionsExecuted, quint64(4));
        QCOMPARE(result.bitsConsumed, quint64(3));
        QCOMPARE(result.nodesCreated, quint64(3));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Invalid);
        QCOMPARE(structure->children().size(), std::size_t(2));
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().code, DiagnosticCode::InvalidSyntax);
        QCOMPARE(structure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.count"));
        QVERIFY(!structure->diagnostics().front().location.has_value());
    }

    void accountsForComputedInstructionNodeAndCancellationBudgets() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> source; computed<u64> first = source + 1; "
            "computed<u64> second = first + 1; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 1);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource instructionSource(bytes({0x80}));
        BitReader instructionReader(instructionSource, *range);
        auto instructionTree = AnalysisTree::create(QStringLiteral("computed-instruction-budget"));
        QVERIFY(instructionTree.has_value());
        DslExecutionOptions instructionOptions;
        instructionOptions.limits.maximumInstructions = 3;
        const auto instructionResult = DslExecutor::decodeStruct(*compiled.program,
                                                                  quint32(0),
                                                                  instructionReader,
                                                                  *mapping,
                                                                  0,
                                                                  *instructionTree,
                                                                  instructionTree->rootId(),
                                                                  instructionOptions);
        QCOMPARE(instructionResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(instructionResult.instructionsExecuted, quint64(3));
        QCOMPARE(instructionResult.bitsConsumed, quint64(1));
        QCOMPARE(instructionResult.nodesCreated, quint64(3));
        const auto instructionStructure =
            instructionTree->node(*instructionResult.structureNode);
        QVERIFY(instructionStructure.has_value());
        QCOMPARE(instructionStructure->children().size(), std::size_t(2));

        MemorySource nodeSource(bytes({0x80}));
        BitReader nodeReader(nodeSource, *range);
        auto nodeTree = AnalysisTree::create(QStringLiteral("computed-node-budget"));
        QVERIFY(nodeTree.has_value());
        DslExecutionOptions nodeOptions;
        nodeOptions.limits.maximumMaterializedNodes = 3;
        const auto nodeResult = DslExecutor::decodeStruct(*compiled.program,
                                                         quint32(0),
                                                         nodeReader,
                                                         *mapping,
                                                         0,
                                                         *nodeTree,
                                                         nodeTree->rootId(),
                                                         nodeOptions);
        QCOMPARE(nodeResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(nodeResult.instructionsExecuted, quint64(4));
        QCOMPARE(nodeResult.bitsConsumed, quint64(1));
        QCOMPARE(nodeResult.nodesCreated, quint64(3));
        const auto nodeStructure = nodeTree->node(*nodeResult.structureNode);
        QVERIFY(nodeStructure.has_value());
        QCOMPARE(nodeStructure->children().size(), std::size_t(2));
        QCOMPARE(nodeStructure->diagnostics().size(), std::size_t(1));
        QCOMPARE(nodeStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.second"));
        QVERIFY(!nodeStructure->diagnostics().front().location.has_value());

        CancellationSource cancellation;
        CancellingMemorySource cancellingSource(bytes({0x80}), cancellation);
        BitReader cancellingReader(cancellingSource, *range);
        auto cancellingTree = AnalysisTree::create(QStringLiteral("computed-cancelled"));
        QVERIFY(cancellingTree.has_value());
        DslExecutionOptions cancellationOptions;
        cancellationOptions.cancellation = cancellation.token();
        cancellationOptions.limits.cancellationCheckInterval = 1;
        const auto cancelled = DslExecutor::decodeStruct(*compiled.program,
                                                        quint32(0),
                                                        cancellingReader,
                                                        *mapping,
                                                        0,
                                                        *cancellingTree,
                                                        cancellingTree->rootId(),
                                                        cancellationOptions);
        QCOMPARE(cancelled.status, DslExecutionStatus::Cancelled);
        QCOMPARE(cancelled.instructionsExecuted, quint64(2));
        QCOMPARE(cancelled.bitsConsumed, quint64(1));
        QCOMPARE(cancelled.nodesCreated, quint64(2));
        const auto cancelledStructure = cancellingTree->node(*cancelled.structureNode);
        QVERIFY(cancelledStructure.has_value());
        QCOMPARE(cancelledStructure->state(), MaterializationState::Cancelled);
        QCOMPARE(cancelledStructure->children().size(), std::size_t(1));
    }

    void rejectsMalformedComputedExpressionsBeforeExecution() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> source; computed<u64> first = source + 1; "
            "computed<bool> flag = first > 1; } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());

        std::vector<DslTypedProgram> malformed;
        auto missingExpression = *compiled.program;
        missingExpression.structs.front().fields.at(1).computedExpression.reset();
        malformed.push_back(std::move(missingExpression));

        auto invalidStorage = *compiled.program;
        invalidStorage.structs.front().fields.at(1).type.bitWidth = 1;
        malformed.push_back(std::move(invalidStorage));

        auto invalidScalarType = *compiled.program;
        invalidScalarType.structs.front().fields.at(1).computedExpression->type =
            static_cast<DslScalarType>(255);
        malformed.push_back(std::move(invalidScalarType));

        auto invalidKind = *compiled.program;
        invalidKind.structs.front().fields.at(1).computedExpression->kind =
            static_cast<DslTypedExpressionKind>(255);
        malformed.push_back(std::move(invalidKind));

        auto skippedInvalidKind = *compiled.program;
        skippedInvalidKind.structs.front().fields.at(2).conditions.push_back(
            {0, 0, false, DslConditionOperator::Equal});
        skippedInvalidKind.structs.front().fields.at(2).computedExpression->kind =
            static_cast<DslTypedExpressionKind>(255);
        malformed.push_back(std::move(skippedInvalidKind));

        auto invalidOperator = *compiled.program;
        invalidOperator.structs.front().fields.at(1).computedExpression->binaryOperator =
            static_cast<DslBinaryOperator>(255);
        malformed.push_back(std::move(invalidOperator));

        auto invalidArity = *compiled.program;
        invalidArity.structs.front().fields.at(1).computedExpression->operands.pop_back();
        malformed.push_back(std::move(invalidArity));

        auto futureReference = *compiled.program;
        futureReference.structs.front()
            .fields.at(1)
            .computedExpression->operands.front()
            .fieldIndex = 2;
        malformed.push_back(std::move(futureReference));

        auto unavailableDependency = *compiled.program;
        unavailableDependency.structs.front().fields.at(1).conditions.push_back(
            {0, 1, false, DslConditionOperator::Equal});
        malformed.push_back(std::move(unavailableDependency));

        auto excessiveDepth = *compiled.program;
        DslTypedExpression deepExpression;
        deepExpression.kind = DslTypedExpressionKind::BooleanLiteral;
        deepExpression.type = DslScalarType::Bool;
        for (std::size_t depth = 0; depth < 64; ++depth) {
            DslTypedExpression wrapper;
            wrapper.kind = DslTypedExpressionKind::Unary;
            wrapper.type = DslScalarType::Bool;
            wrapper.unaryOperator = DslUnaryOperator::LogicalNot;
            wrapper.operands.push_back(std::move(deepExpression));
            deepExpression = std::move(wrapper);
        }
        excessiveDepth.structs.front().fields.at(2).computedExpression =
            std::move(deepExpression);
        malformed.push_back(std::move(excessiveDepth));

        auto excessiveNodes = *compiled.program;
        std::vector<DslTypedExpression> expressionLevel(256);
        while (expressionLevel.size() > 1) {
            std::vector<DslTypedExpression> nextLevel;
            nextLevel.reserve(expressionLevel.size() / 2);
            for (std::size_t index = 0; index < expressionLevel.size(); index += 2) {
                DslTypedExpression parent;
                parent.kind = DslTypedExpressionKind::Binary;
                parent.type = DslScalarType::U64;
                parent.binaryOperator = DslBinaryOperator::Add;
                parent.operands.push_back(std::move(expressionLevel.at(index)));
                parent.operands.push_back(std::move(expressionLevel.at(index + 1)));
                nextLevel.push_back(std::move(parent));
            }
            expressionLevel = std::move(nextLevel);
        }
        excessiveNodes.structs.front().fields.at(1).computedExpression =
            std::move(expressionLevel.front());
        malformed.push_back(std::move(excessiveNodes));

        auto sourceWithExpression = *compiled.program;
        sourceWithExpression.structs.front().fields.front().computedExpression =
            sourceWithExpression.structs.front().fields.at(1).computedExpression;
        malformed.push_back(std::move(sourceWithExpression));

        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 1);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        for (std::size_t index = 0; index < malformed.size(); ++index) {
            MemorySource source(bytes({0x80}));
            BitReader reader(source, *range);
            auto tree = AnalysisTree::create(
                QStringLiteral("malformed-computed-expression-%1").arg(index));
            QVERIFY(tree.has_value());
            const auto result = DslExecutor::decodeStruct(malformed.at(index),
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
            QCOMPARE(result.instructionsExecuted, quint64(0));
            QCOMPARE(result.bitsConsumed, quint64(0));
            QCOMPARE(result.nodesCreated, quint64(0));
            QVERIFY(!result.structureNode.has_value());
        }
    }

    void rejectsMalformedComputedControllersBeforeExecution() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> source; computed<bool> flag = source == 1; "
            "if (flag) { bits<1> guarded; } computed<u64> count = 1; "
            "repeat (count, 1) { bits<1> item; } } entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());
        QCOMPARE(compiled.program->structs.front().fields.at(2).conditions.size(),
                 std::size_t(1));
        QCOMPARE(compiled.program->structs.front().repeatBounds.size(), std::size_t(1));

        std::vector<DslTypedProgram> malformed;
        auto falseBooleanLiteral = *compiled.program;
        falseBooleanLiteral.structs.front().fields.at(2).conditions.front().expectedValue = 0;
        malformed.push_back(std::move(falseBooleanLiteral));
        auto greaterThanBoolean = *compiled.program;
        greaterThanBoolean.structs.front().fields.at(2).conditions.front().op =
            DslConditionOperator::GreaterThan;
        malformed.push_back(std::move(greaterThanBoolean));
        auto booleanRepeatController = *compiled.program;
        booleanRepeatController.structs.front().repeatBounds.front().controllerFieldIndex = 1;
        malformed.push_back(std::move(booleanRepeatController));

        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 3);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        for (std::size_t index = 0; index < malformed.size(); ++index) {
            MemorySource source(bytes({0xe0}));
            BitReader reader(source, *range);
            auto tree = AnalysisTree::create(
                QStringLiteral("malformed-computed-controller-%1").arg(index));
            QVERIFY(tree.has_value());
            const auto result = DslExecutor::decodeStruct(malformed.at(index),
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
            QCOMPARE(result.instructionsExecuted, quint64(0));
            QCOMPARE(result.bitsConsumed, quint64(0));
            QCOMPARE(result.nodesCreated, quint64(0));
            QVERIFY(!result.structureNode.has_value());
        }
    }

    void rejectsMalformedComputedOpcodesAfterRetainingEarlierFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> source; computed<u64> value = source + 1; } "
            "entry Header;"));
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(parsed.succeeded());
        QVERIFY(compiled.succeeded());
        QCOMPARE(compiled.program->bytecode.at(2).opcode, DslOpcode::EvaluateComputed);

        std::vector<DslTypedProgram> malformed;
        auto wrongOperand = *compiled.program;
        wrongOperand.bytecode.at(2).operand = 0;
        malformed.push_back(std::move(wrongOperand));
        auto wrongOpcode = *compiled.program;
        wrongOpcode.bytecode.at(2).opcode = DslOpcode::ReadUnsignedBits;
        malformed.push_back(std::move(wrongOpcode));
        auto invalidOpcode = *compiled.program;
        invalidOpcode.bytecode.at(2).opcode = static_cast<DslOpcode>(255);
        malformed.push_back(std::move(invalidOpcode));

        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 1);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        for (std::size_t index = 0; index < malformed.size(); ++index) {
            MemorySource source(bytes({0x80}));
            BitReader reader(source, *range);
            auto tree = AnalysisTree::create(
                QStringLiteral("malformed-computed-opcode-%1").arg(index));
            QVERIFY(tree.has_value());
            const auto result = DslExecutor::decodeStruct(malformed.at(index),
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
            QCOMPARE(result.instructionsExecuted, quint64(3));
            QCOMPARE(result.bitsConsumed, quint64(1));
            QCOMPARE(result.nodesCreated, quint64(2));
            const auto structure = tree->node(*result.structureNode);
            QVERIFY(structure.has_value());
            QCOMPARE(structure->state(), MaterializationState::Invalid);
            QCOMPARE(structure->children().size(), std::size_t(1));
            QCOMPARE(tree->node(structure->children().front())->name(),
                     QStringLiteral("source"));
        }
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
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
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

    void materializesOnlyTheSelectedConditionalBranchAndTail() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; "
            "if (kind == 1) { bits<3> then_value; } "
            "else { bits<5> else_value; } bits<3> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource trueSource(bytes({0x6e}));
        const auto trueMapping = mappingForBytes(1);
        const auto trueRange = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(trueMapping.has_value());
        QVERIFY(trueRange.has_value());
        BitReader trueReader(trueSource, *trueRange);
        auto trueTree = AnalysisTree::create(QStringLiteral("conditional-true"));
        QVERIFY(trueTree.has_value());
        const auto trueResult = DslExecutor::decodeStruct(*compiled.program,
                                                         quint32(0),
                                                         trueReader,
                                                         *trueMapping,
                                                         0,
                                                         *trueTree,
                                                         trueTree->rootId());
        QCOMPARE(trueResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(trueResult.bitsConsumed, quint64(8));
        QCOMPARE(trueResult.instructionsExecuted, quint64(6));
        QCOMPARE(trueResult.nodesCreated, quint64(4));
        const auto trueStructure = trueTree->node(*trueResult.structureNode);
        QVERIFY(trueStructure.has_value());
        QCOMPARE(trueStructure->children().size(), std::size_t(3));
        const std::vector<QString> trueNames{QStringLiteral("kind"),
                                             QStringLiteral("then_value"),
                                             QStringLiteral("tail")};
        const std::vector<quint64> trueValues{1, 5, 6};
        const std::vector<quint64> trueStarts{0, 2, 5};
        for (std::size_t index = 0; index < trueNames.size(); ++index) {
            const auto field = trueTree->node(trueStructure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), trueNames.at(index));
            QCOMPARE(field->value().toULongLong(), trueValues.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     trueStarts.at(index));
        }

        MemorySource falseSource(bytes({0xaa, 0xc0}));
        const auto falseMapping = mappingForBytes(2);
        const auto falseRange = SourceSpan::create(streamview::core::SourceBitAddress(0), 10);
        QVERIFY(falseMapping.has_value());
        QVERIFY(falseRange.has_value());
        BitReader falseReader(falseSource, *falseRange);
        auto falseTree = AnalysisTree::create(QStringLiteral("conditional-false"));
        QVERIFY(falseTree.has_value());
        const auto falseResult = DslExecutor::decodeStruct(*compiled.program,
                                                          quint32(0),
                                                          falseReader,
                                                          *falseMapping,
                                                          0,
                                                          *falseTree,
                                                          falseTree->rootId());
        QCOMPARE(falseResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(falseResult.bitsConsumed, quint64(10));
        QCOMPARE(falseResult.instructionsExecuted, quint64(6));
        QCOMPARE(falseResult.nodesCreated, quint64(4));
        const auto falseStructure = falseTree->node(*falseResult.structureNode);
        QVERIFY(falseStructure.has_value());
        QCOMPARE(falseStructure->children().size(), std::size_t(3));
        const std::vector<QString> falseNames{QStringLiteral("kind"),
                                              QStringLiteral("else_value"),
                                              QStringLiteral("tail")};
        const std::vector<quint64> falseValues{2, 21, 3};
        const std::vector<quint64> falseStarts{0, 2, 7};
        for (std::size_t index = 0; index < falseNames.size(); ++index) {
            const auto field = falseTree->node(falseStructure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), falseNames.at(index));
            QCOMPARE(field->value().toULongLong(), falseValues.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     falseStarts.at(index));
        }
    }

    void executesBoundedRepeatCountsWithoutClamping() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> count; repeat (count, 3) { bits<2> value; } "
            "bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY2(compiled.succeeded(),
                 compiled.diagnostics.empty()
                     ? ""
                     : qPrintable(compiled.diagnostics.front().message));

        struct Case final {
            QString name;
            std::vector<std::byte> sourceBytes;
            quint64 bitLength = 0;
            std::vector<QString> fieldNames;
            std::vector<quint64> fieldValues;
            std::vector<quint64> fieldStarts;
        };
        const std::vector<Case> cases{
            {QStringLiteral("repeat-zero"),
             bytes({0x10}),
             4,
             {QStringLiteral("count"), QStringLiteral("tail")},
             {0, 1},
             {0, 3}},
            {QStringLiteral("repeat-two"),
             bytes({0x4d}),
             8,
             {QStringLiteral("count"),
              QStringLiteral("value[0]"),
              QStringLiteral("value[1]"),
              QStringLiteral("tail")},
             {2, 1, 2, 1},
             {0, 3, 5, 7}},
            {QStringLiteral("repeat-maximum"),
             bytes({0x6d, 0x80}),
             10,
             {QStringLiteral("count"),
              QStringLiteral("value[0]"),
              QStringLiteral("value[1]"),
              QStringLiteral("value[2]"),
              QStringLiteral("tail")},
             {3, 1, 2, 3, 0},
             {0, 3, 5, 7, 9}},
        };

        for (const Case& testCase : cases) {
            MemorySource source(testCase.sourceBytes);
            const auto mapping = mappingForBytes(testCase.sourceBytes.size());
            const auto range = SourceSpan::create(
                streamview::core::SourceBitAddress(0), testCase.bitLength);
            QVERIFY(mapping.has_value());
            QVERIFY(range.has_value());
            BitReader reader(source, *range);
            auto tree = AnalysisTree::create(testCase.name);
            QVERIFY(tree.has_value());

            const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::Materialized);
            QCOMPARE(result.bitsConsumed, testCase.bitLength);
            QCOMPARE(result.instructionsExecuted, quint64(8));
            QCOMPARE(result.nodesCreated, testCase.fieldNames.size() + std::size_t(1));
            const auto structure = tree->node(*result.structureNode);
            QVERIFY(structure.has_value());
            QCOMPARE(structure->children().size(), testCase.fieldNames.size());
            for (std::size_t index = 0; index < testCase.fieldNames.size(); ++index) {
                const auto field = tree->node(structure->children().at(index));
                QVERIFY(field.has_value());
                QCOMPARE(field->name(), testCase.fieldNames.at(index));
                QCOMPARE(field->value().toULongLong(), testCase.fieldValues.at(index));
                QCOMPARE(field->location()->sourceSpans().front().start()
                             .absoluteBitOffset(),
                         testCase.fieldStarts.at(index));
            }
        }

        MemorySource invalidSource(bytes({0x80}));
        const auto invalidMapping = mappingForBytes(1);
        const auto invalidRange =
            SourceSpan::create(streamview::core::SourceBitAddress(0), 3);
        QVERIFY(invalidMapping.has_value());
        QVERIFY(invalidRange.has_value());
        BitReader invalidReader(invalidSource, *invalidRange);
        auto invalidTree = AnalysisTree::create(QStringLiteral("repeat-over-maximum"));
        QVERIFY(invalidTree.has_value());
        const auto invalid = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       invalidReader,
                                                       *invalidMapping,
                                                       0,
                                                       *invalidTree,
                                                       invalidTree->rootId());
        QCOMPARE(invalid.status, DslExecutionStatus::InvalidSyntax);
        QCOMPARE(invalid.bitsConsumed, quint64(3));
        QCOMPARE(invalid.instructionsExecuted, quint64(3));
        QCOMPARE(invalid.nodesCreated, quint64(2));
        const auto invalidStructure = invalidTree->node(*invalid.structureNode);
        QVERIFY(invalidStructure.has_value());
        QCOMPARE(invalidStructure->children().size(), std::size_t(1));
        QCOMPARE(invalidStructure->diagnostics().size(), std::size_t(1));
        QCOMPARE(invalidStructure->diagnostics().front().code,
                 DiagnosticCode::InvalidSyntax);
        QCOMPARE(invalidStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.count"));
        QCOMPARE(invalidStructure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(0));
        QCOMPARE(invalidStructure->diagnostics().front().location->sourceSpans().front()
                     .bitLength(),
                 quint64(3));
    }

    void skipsGuardedRepeatBoundsWhenTheirEnclosingBranchIsAbsent() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag; if (flag == 1) { bits<2> count; "
            "repeat (count, 2) { bits<1> value; } } else { bits<1> fallback; } "
            "bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x60}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 3);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("guarded-repeat-absent"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      reader,
                                                      *mapping,
                                                      0,
                                                      *tree,
                                                      tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(3));
        QCOMPARE(result.instructionsExecuted, quint64(9));
        QCOMPARE(result.nodesCreated, quint64(4));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(3));
        QCOMPARE(tree->node(structure->children().at(0))->name(), QStringLiteral("flag"));
        QCOMPARE(tree->node(structure->children().at(1))->name(),
                 QStringLiteral("fallback"));
        QCOMPARE(tree->node(structure->children().at(2))->name(), QStringLiteral("tail"));
    }

    void executesNestedRepeatsWithUnsignedExpGolombLocalCounts() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> outer_count; repeat (outer_count, 2) { "
            "ue inner_count; repeat (inner_count, 2) { bits<2> value; } } "
            "bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0xad, 0xa0}));
        const auto mapping = mappingForBytes(2);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 11);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("nested-repeat"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      reader,
                                                      *mapping,
                                                      0,
                                                      *tree,
                                                      tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(11));
        QCOMPARE(result.instructionsExecuted, quint64(13));
        QCOMPARE(result.nodesCreated, quint64(7));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const std::vector<QString> names{
            QStringLiteral("outer_count"),
            QStringLiteral("inner_count[0]"),
            QStringLiteral("inner_count[1]"),
            QStringLiteral("value[1][0]"),
            QStringLiteral("value[1][1]"),
            QStringLiteral("tail"),
        };
        const std::vector<quint64> values{2, 0, 2, 1, 2, 1};
        const std::vector<quint64> starts{0, 2, 3, 6, 8, 10};
        const std::vector<quint64> lengths{2, 1, 3, 2, 2, 1};
        QCOMPARE(structure->children().size(), names.size());
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto field = tree->node(structure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), names.at(index));
            QCOMPARE(field->value().toULongLong(), values.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     starts.at(index));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(),
                     lengths.at(index));
        }
    }

    void materializesRepeatedLittleEndianArraysWithMetadata() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<8> count; repeat (count, 2) { "
            "bits<16, little> value @description(\"Repeated value.\"); "
            "bits<4> tags[2]; } } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x02, 0x34, 0x12, 0xab, 0x78, 0x56, 0xcd}));
        const auto mapping = mappingForBytes(7);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 56);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("repeat-little-array"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      reader,
                                                      *mapping,
                                                      0,
                                                      *tree,
                                                      tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(56));
        QCOMPARE(result.instructionsExecuted, quint64(10));
        QCOMPARE(result.nodesCreated, quint64(8));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const std::vector<QString> names{
            QStringLiteral("count"),
            QStringLiteral("value[0]"),
            QStringLiteral("tags[0][0]"),
            QStringLiteral("tags[0][1]"),
            QStringLiteral("value[1]"),
            QStringLiteral("tags[1][0]"),
            QStringLiteral("tags[1][1]"),
        };
        const std::vector<quint64> values{2, 0x1234, 0xa, 0xb, 0x5678, 0xc, 0xd};
        const std::vector<quint64> starts{0, 8, 24, 28, 32, 48, 52};
        const std::vector<quint64> lengths{8, 16, 4, 4, 16, 4, 4};
        QCOMPARE(structure->children().size(), names.size());
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto field = tree->node(structure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), names.at(index));
            QCOMPARE(field->value().toULongLong(), values.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     starts.at(index));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(), lengths.at(index));
            if (field->name().startsWith(QStringLiteral("value"))) {
                QCOMPARE(field->metadata().description, QStringLiteral("Repeated value."));
            }
        }
    }

    void materializesRepeatedExpGolombFieldsWithExactLocations() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 2) { ue code; se delta; } "
            "bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0xa9, 0xb8}));
        const auto mapping = mappingForBytes(2);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 13);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("repeat-exp-golomb"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      reader,
                                                      *mapping,
                                                      0,
                                                      *tree,
                                                      tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64(13));
        QCOMPARE(result.instructionsExecuted, quint64(9));
        QCOMPARE(result.nodesCreated, quint64(7));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const std::vector<QString> names{
            QStringLiteral("count"),
            QStringLiteral("code[0]"),
            QStringLiteral("delta[0]"),
            QStringLiteral("code[1]"),
            QStringLiteral("delta[1]"),
            QStringLiteral("tail"),
        };
        const std::vector<qint64> values{2, 0, 1, 2, -1, 1};
        const std::vector<quint64> starts{0, 2, 3, 6, 9, 12};
        const std::vector<quint64> lengths{2, 1, 3, 3, 3, 1};
        QCOMPARE(structure->children().size(), names.size());
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto field = tree->node(structure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), names.at(index));
            QCOMPARE(field->value().toLongLong(), values.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     starts.at(index));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(), lengths.at(index));
        }
    }

    void retainsUnknownRepeatedEnumValuesWithExactLocations() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum Type { zero = 0; one = 1; } struct Header { bits<2> count; "
            "repeat (count, 2) { bits<2> kind @enum(Type); } } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0x8c}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 6);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("repeat-enum-invalid"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      reader,
                                                      *mapping,
                                                      0,
                                                      *tree,
                                                      tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidSyntax);
        QCOMPARE(result.bitsConsumed, quint64(6));
        QCOMPARE(result.instructionsExecuted, quint64(5));
        QCOMPARE(result.nodesCreated, quint64(4));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(3));
        const auto unknown = tree->node(structure->children().at(2));
        QVERIFY(unknown.has_value());
        QCOMPARE(unknown->name(), QStringLiteral("kind[1]"));
        QCOMPARE(unknown->value().toULongLong(), quint64(3));
        QCOMPARE(unknown->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(4));
        QCOMPARE(unknown->location()->sourceSpans().front().bitLength(), quint64(2));
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.kind[1]"));
        QVERIFY(structure->diagnostics().front().location.has_value());
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(4));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(2));
    }

    void retainsCompletedRepeatFieldsWhenTheNextFieldIsTruncated() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 3) { "
            "bits<3> first; bits<2> second; } } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        MemorySource source(bytes({0xac, 0xe0}));
        const auto mapping = mappingForBytes(2);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 11);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("repeat-truncated"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      reader,
                                                      *mapping,
                                                      0,
                                                      *tree,
                                                      tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
        QCOMPARE(result.bitsConsumed, quint64(10));
        QCOMPARE(reader.position(), quint64(10));
        QCOMPARE(result.instructionsExecuted, quint64(7));
        QCOMPARE(result.nodesCreated, quint64(5));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t(4));
        const std::vector<QString> names{QStringLiteral("count"),
                                         QStringLiteral("first[0]"),
                                         QStringLiteral("second[0]"),
                                         QStringLiteral("first[1]")};
        const std::vector<quint64> values{2, 5, 2, 3};
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto field = tree->node(structure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), names.at(index));
            QCOMPARE(field->value().toULongLong(), values.at(index));
        }
        QCOMPARE(structure->diagnostics().size(), std::size_t(1));
        QCOMPARE(structure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.second[1]"));
        QVERIFY(structure->diagnostics().front().location.has_value());
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(10));
        QCOMPARE(structure->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(1));
        QVERIFY(tree->hasPartialResults());
    }

    void appliesEqualsConstraintsToPresentRepeatIterationsOnly() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 2) { "
            "bits<1> reserved @equals(0); } } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());

        MemorySource validSource(bytes({0x40}));
        const auto validRange = SourceSpan::create(streamview::core::SourceBitAddress(0), 3);
        QVERIFY(validRange.has_value());
        BitReader validReader(validSource, *validRange);
        auto validTree = AnalysisTree::create(QStringLiteral("repeat-equals-valid"));
        QVERIFY(validTree.has_value());
        const auto valid = DslExecutor::decodeStruct(*compiled.program,
                                                    quint32(0),
                                                    validReader,
                                                    *mapping,
                                                    0,
                                                    *validTree,
                                                    validTree->rootId());
        QCOMPARE(valid.status, DslExecutionStatus::Materialized);
        QCOMPARE(valid.bitsConsumed, quint64(3));
        QCOMPARE(valid.instructionsExecuted, quint64(8));
        QCOMPARE(valid.nodesCreated, quint64(3));
        const auto validStructure = validTree->node(*valid.structureNode);
        QVERIFY(validStructure.has_value());
        QCOMPARE(validStructure->children().size(), std::size_t(2));
        const auto reserved = validTree->node(validStructure->children().at(1));
        QVERIFY(reserved.has_value());
        QCOMPARE(reserved->name(), QStringLiteral("reserved[0]"));

        MemorySource invalidSource(bytes({0x90}));
        const auto invalidRange = SourceSpan::create(streamview::core::SourceBitAddress(0), 4);
        QVERIFY(invalidRange.has_value());
        BitReader invalidReader(invalidSource, *invalidRange);
        auto invalidTree = AnalysisTree::create(QStringLiteral("repeat-equals-invalid"));
        QVERIFY(invalidTree.has_value());
        const auto invalid = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      invalidReader,
                                                      *mapping,
                                                      0,
                                                      *invalidTree,
                                                      invalidTree->rootId());
        QCOMPARE(invalid.status, DslExecutionStatus::InvalidSyntax);
        QCOMPARE(invalid.bitsConsumed, quint64(4));
        QCOMPARE(invalid.instructionsExecuted, quint64(7));
        QCOMPARE(invalid.nodesCreated, quint64(4));
        const auto invalidStructure = invalidTree->node(*invalid.structureNode);
        QVERIFY(invalidStructure.has_value());
        QCOMPARE(invalidStructure->children().size(), std::size_t(3));
        QCOMPARE(invalidStructure->diagnostics().size(), std::size_t(1));
        QCOMPARE(invalidStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.reserved[1]"));
        QVERIFY(invalidStructure->diagnostics().front().location.has_value());
        QCOMPARE(invalidStructure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(3));
    }

    void enforcesRepeatBudgetsAndObservesMidExecutionCancellation() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 3) { bits<1> flag; } "
            "bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());

        MemorySource instructionSource(bytes({0x70}));
        const auto instructionRange =
            SourceSpan::create(streamview::core::SourceBitAddress(0), 4);
        QVERIFY(instructionRange.has_value());
        BitReader instructionReader(instructionSource, *instructionRange);
        auto instructionTree = AnalysisTree::create(QStringLiteral("repeat-instruction-budget"));
        QVERIFY(instructionTree.has_value());
        DslExecutionOptions instructionOptions;
        instructionOptions.limits.maximumInstructions = 6;
        const auto instructionResult = DslExecutor::decodeStruct(*compiled.program,
                                                                quint32(0),
                                                                instructionReader,
                                                                *mapping,
                                                                0,
                                                                *instructionTree,
                                                                instructionTree->rootId(),
                                                                instructionOptions);
        QCOMPARE(instructionResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(instructionResult.instructionsExecuted, quint64(6));
        QCOMPARE(instructionResult.bitsConsumed, quint64(3));
        QCOMPARE(instructionResult.nodesCreated, quint64(3));
        const auto instructionStructure =
            instructionTree->node(*instructionResult.structureNode);
        QVERIFY(instructionStructure.has_value());
        QCOMPARE(instructionStructure->children().size(), std::size_t(2));

        MemorySource nodeSource(bytes({0xe8}));
        const auto nodeRange = SourceSpan::create(streamview::core::SourceBitAddress(0), 6);
        QVERIFY(nodeRange.has_value());
        BitReader nodeReader(nodeSource, *nodeRange);
        auto nodeTree = AnalysisTree::create(QStringLiteral("repeat-node-budget"));
        QVERIFY(nodeTree.has_value());
        DslExecutionOptions nodeOptions;
        nodeOptions.limits.maximumMaterializedNodes = 4;
        const auto nodeResult = DslExecutor::decodeStruct(*compiled.program,
                                                         quint32(0),
                                                         nodeReader,
                                                         *mapping,
                                                         0,
                                                         *nodeTree,
                                                         nodeTree->rootId(),
                                                         nodeOptions);
        QCOMPARE(nodeResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(nodeResult.instructionsExecuted, quint64(6));
        QCOMPARE(nodeResult.bitsConsumed, quint64(4));
        QCOMPARE(nodeResult.nodesCreated, quint64(4));
        const auto nodeStructure = nodeTree->node(*nodeResult.structureNode);
        QVERIFY(nodeStructure.has_value());
        QCOMPARE(nodeStructure->children().size(), std::size_t(3));
        QCOMPARE(nodeStructure->diagnostics().size(), std::size_t(1));
        QCOMPARE(nodeStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.flag[2]"));

        CancellationSource cancellation;
        CancellingMemorySource cancellingSource(bytes({0x70}), cancellation);
        const auto cancellationRange =
            SourceSpan::create(streamview::core::SourceBitAddress(0), 4);
        QVERIFY(cancellationRange.has_value());
        BitReader cancellingReader(cancellingSource, *cancellationRange);
        auto cancellingTree = AnalysisTree::create(QStringLiteral("repeat-cancelled"));
        QVERIFY(cancellingTree.has_value());
        DslExecutionOptions cancellationOptions;
        cancellationOptions.cancellation = cancellation.token();
        cancellationOptions.limits.cancellationCheckInterval = 1;
        const auto cancelled = DslExecutor::decodeStruct(*compiled.program,
                                                        quint32(0),
                                                        cancellingReader,
                                                        *mapping,
                                                        0,
                                                        *cancellingTree,
                                                        cancellingTree->rootId(),
                                                        cancellationOptions);
        QCOMPARE(cancelled.status, DslExecutionStatus::Cancelled);
        QCOMPARE(cancelled.instructionsExecuted, quint64(2));
        QCOMPARE(cancelled.bitsConsumed, quint64(2));
        QCOMPARE(cancelled.nodesCreated, quint64(2));
        const auto cancelledStructure = cancellingTree->node(*cancelled.structureNode);
        QVERIFY(cancelledStructure.has_value());
        QCOMPARE(cancelledStructure->state(), MaterializationState::Cancelled);
        QCOMPARE(cancelledStructure->children().size(), std::size_t(1));
        QCOMPARE(cancelledStructure->diagnostics().size(), std::size_t(1));
        QCOMPARE(cancelledStructure->diagnostics().front().code, DiagnosticCode::Cancelled);
    }

    void rejectsMalformedRepeatBoundsBeforeExecutingBytecode() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> count; repeat (count, 2) { bits<1> value; } } "
            "entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        std::vector<DslTypedProgram> malformed;
        auto emptyBound = *compiled.program;
        emptyBound.structs.front().repeatBounds.front().firstFieldIndex =
            static_cast<quint32>(emptyBound.structs.front().fields.size());
        malformed.push_back(std::move(emptyBound));
        auto bodyController = *compiled.program;
        bodyController.structs.front().repeatBounds.front().controllerFieldIndex =
            bodyController.structs.front().repeatBounds.front().firstFieldIndex;
        malformed.push_back(std::move(bodyController));

        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 2);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        for (std::size_t index = 0; index < malformed.size(); ++index) {
            MemorySource source(bytes({0x00}));
            BitReader reader(source, *range);
            auto tree = AnalysisTree::create(
                QStringLiteral("malformed-repeat-bound-%1").arg(index));
            QVERIFY(tree.has_value());

            const auto result = DslExecutor::decodeStruct(malformed.at(index),
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
            QCOMPARE(result.instructionsExecuted, quint64(0));
            QCOMPARE(result.bitsConsumed, quint64(0));
            QCOMPARE(result.nodesCreated, quint64(0));
            QVERIFY(!result.structureNode.has_value());
        }
    }

    void rejectsMalformedRepeatAssertionAfterRetainingTheController() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<3> count; repeat (count, 3) { bits<2> value; } "
            "bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        DslTypedProgram malformed = *compiled.program;
        const auto assertion = std::find_if(
            malformed.bytecode.begin(), malformed.bytecode.end(), [](const auto& instruction) {
                return instruction.opcode == DslOpcode::AssertRepeatCount;
            });
        QVERIFY(assertion != malformed.bytecode.end());
        assertion->operand =
            static_cast<quint32>(malformed.structs.front().repeatBounds.size());

        MemorySource source(bytes({0x80}));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 3);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        BitReader reader(source, *range);
        auto tree = AnalysisTree::create(QStringLiteral("malformed-repeat-assertion"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(malformed,
                                                      quint32(0),
                                                      reader,
                                                      *mapping,
                                                      0,
                                                      *tree,
                                                      tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(result.instructionsExecuted, quint64(3));
        QCOMPARE(result.bitsConsumed, quint64(3));
        QCOMPARE(result.nodesCreated, quint64(2));
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->state(), MaterializationState::Invalid);
        QCOMPARE(structure->children().size(), std::size_t(1));
        const auto count = tree->node(structure->children().front());
        QVERIFY(count.has_value());
        QCOMPARE(count->name(), QStringLiteral("count"));
    }

    void materializesNestedConditionalArraysAndExpGolombFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> outer; if (outer == 1) { bits<1> inner; "
            "if (inner == 0) { bits<2> values[2] @equals(1); } "
            "else { ue code; se delta; } } else { bits<3> alternative; } "
            "bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 7);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource arraySource(bytes({0x96}));
        BitReader arrayReader(arraySource, *range);
        auto arrayTree = AnalysisTree::create(QStringLiteral("conditional-array"));
        QVERIFY(arrayTree.has_value());
        const auto arrayResult = DslExecutor::decodeStruct(*compiled.program,
                                                          quint32(0),
                                                          arrayReader,
                                                          *mapping,
                                                          0,
                                                          *arrayTree,
                                                          arrayTree->rootId());
        QCOMPARE(arrayResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(arrayResult.bitsConsumed, quint64(7));
        QCOMPARE(arrayResult.instructionsExecuted, quint64(12));
        QCOMPARE(arrayResult.nodesCreated, quint64(6));
        const auto arrayStructure = arrayTree->node(*arrayResult.structureNode);
        QVERIFY(arrayStructure.has_value());
        QCOMPARE(arrayStructure->children().size(), std::size_t(5));
        const std::vector<QString> arrayNames{QStringLiteral("outer"),
                                              QStringLiteral("inner"),
                                              QStringLiteral("values[0]"),
                                              QStringLiteral("values[1]"),
                                              QStringLiteral("tail")};
        const std::vector<quint64> arrayValues{1, 0, 1, 1, 1};
        const std::vector<quint64> arrayStarts{0, 1, 2, 4, 6};
        const std::vector<quint64> arrayLengths{1, 1, 2, 2, 1};
        for (std::size_t index = 0; index < arrayNames.size(); ++index) {
            const auto field = arrayTree->node(arrayStructure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), arrayNames.at(index));
            QCOMPARE(field->value().toULongLong(), arrayValues.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     arrayStarts.at(index));
            QCOMPARE(field->location()->sourceSpans().front().bitLength(),
                     arrayLengths.at(index));
        }

        MemorySource expSource(bytes({0xea}));
        BitReader expReader(expSource, *range);
        auto expTree = AnalysisTree::create(QStringLiteral("conditional-exp-golomb"));
        QVERIFY(expTree.has_value());
        const auto expResult = DslExecutor::decodeStruct(*compiled.program,
                                                        quint32(0),
                                                        expReader,
                                                        *mapping,
                                                        0,
                                                        *expTree,
                                                        expTree->rootId());
        QCOMPARE(expResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(expResult.bitsConsumed, quint64(7));
        QCOMPARE(expResult.instructionsExecuted, quint64(12));
        QCOMPARE(expResult.nodesCreated, quint64(6));
        const auto expStructure = expTree->node(*expResult.structureNode);
        QVERIFY(expStructure.has_value());
        QCOMPARE(expStructure->children().size(), std::size_t(5));
        QCOMPARE(expTree->node(expStructure->children().at(2))->name(),
                 QStringLiteral("code"));
        QCOMPARE(expTree->node(expStructure->children().at(2))->value().toULongLong(),
                 quint64(0));
        QCOMPARE(expTree->node(expStructure->children().at(3))->name(),
                 QStringLiteral("delta"));
        QCOMPARE(expTree->node(expStructure->children().at(3))->value().toLongLong(),
                 qlonglong(1));
        QCOMPARE(expTree->node(expStructure->children().at(2))->location()->sourceSpans().front()
                     .start().absoluteBitOffset(),
                 quint64(2));
        QCOMPARE(expTree->node(expStructure->children().at(2))->location()->sourceSpans().front()
                     .bitLength(),
                 quint64(1));
        QCOMPARE(expTree->node(expStructure->children().at(3))->location()->sourceSpans().front()
                     .start().absoluteBitOffset(),
                 quint64(3));
        QCOMPARE(expTree->node(expStructure->children().at(3))->location()->sourceSpans().front()
                     .bitLength(),
                 quint64(3));
        QCOMPARE(expTree->node(expStructure->children().at(4))->location()->sourceSpans().front()
                     .start().absoluteBitOffset(),
                 quint64(6));
    }

    void usesEnumControllersAndGuardsLittleEndianFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum Kind { first = 1; second = 2; } "
            "struct Header { bits<8> kind @enum(Kind); "
            "if (kind == 1) { bits<16, little> value; } "
            "else { bits<16> alternative; } bits<8> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(4);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 32);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource trueSource(bytes({1, 0x34, 0x12, 0xab}));
        BitReader trueReader(trueSource, *range);
        auto trueTree = AnalysisTree::create(QStringLiteral("conditional-enum-little"));
        QVERIFY(trueTree.has_value());
        const auto trueResult = DslExecutor::decodeStruct(*compiled.program,
                                                         quint32(0),
                                                         trueReader,
                                                         *mapping,
                                                         0,
                                                         *trueTree,
                                                         trueTree->rootId());
        QCOMPARE(trueResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(trueResult.bitsConsumed, quint64(32));
        QCOMPARE(trueResult.instructionsExecuted, quint64(6));
        QCOMPARE(trueResult.nodesCreated, quint64(4));
        const auto trueStructure = trueTree->node(*trueResult.structureNode);
        QVERIFY(trueStructure.has_value());
        QCOMPARE(trueStructure->children().size(), std::size_t(3));
        QCOMPARE(trueTree->node(trueStructure->children().at(1))->name(),
                 QStringLiteral("value"));
        QCOMPARE(trueTree->node(trueStructure->children().at(1))->value().toULongLong(),
                 quint64(0x1234));

        MemorySource falseSource(bytes({2, 0x12, 0x34, 0xcd}));
        BitReader falseReader(falseSource, *range);
        auto falseTree = AnalysisTree::create(QStringLiteral("conditional-enum-alternative"));
        QVERIFY(falseTree.has_value());
        const auto falseResult = DslExecutor::decodeStruct(*compiled.program,
                                                          quint32(0),
                                                          falseReader,
                                                          *mapping,
                                                          0,
                                                          *falseTree,
                                                          falseTree->rootId());
        QCOMPARE(falseResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(falseResult.bitsConsumed, quint64(32));
        QCOMPARE(falseResult.instructionsExecuted, quint64(6));
        QCOMPARE(falseResult.nodesCreated, quint64(4));
        const auto falseStructure = falseTree->node(*falseResult.structureNode);
        QVERIFY(falseStructure.has_value());
        QCOMPARE(falseStructure->children().size(), std::size_t(3));
        QCOMPARE(falseTree->node(falseStructure->children().at(1))->name(),
                 QStringLiteral("alternative"));
        QCOMPARE(falseTree->node(falseStructure->children().at(1))->value().toULongLong(),
                 quint64(0x1234));
    }

    void reportsTruncationOnlyForSelectedConditionalFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag; if (flag == 1) { bits<7> payload; } "
            "bits<7> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());

        MemorySource selectedSource(bytes({0xf0}));
        const auto shortRange = SourceSpan::create(streamview::core::SourceBitAddress(0), 4);
        QVERIFY(shortRange.has_value());
        BitReader selectedReader(selectedSource, *shortRange);
        auto selectedTree = AnalysisTree::create(QStringLiteral("conditional-truncated"));
        QVERIFY(selectedTree.has_value());
        const auto selected = DslExecutor::decodeStruct(*compiled.program,
                                                        quint32(0),
                                                        selectedReader,
                                                        *mapping,
                                                        0,
                                                        *selectedTree,
                                                        selectedTree->rootId());
        QCOMPARE(selected.status, DslExecutionStatus::TruncatedSource);
        QCOMPARE(selected.bitsConsumed, quint64(1));
        const auto selectedStructure = selectedTree->node(*selected.structureNode);
        QVERIFY(selectedStructure.has_value());
        QCOMPARE(selectedStructure->state(), MaterializationState::Invalid);
        QCOMPARE(selectedStructure->children().size(), std::size_t(1));
        QCOMPARE(selectedStructure->diagnostics().front().code,
                 DiagnosticCode::TruncatedSource);
        QCOMPARE(selectedStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.payload"));
        QCOMPARE(selectedStructure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(1));
        QCOMPARE(selectedStructure->diagnostics().front().location->sourceSpans().front()
                     .bitLength(),
                 quint64(3));

        MemorySource skippedSource(bytes({0x55}));
        const auto fullRange = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(fullRange.has_value());
        BitReader skippedReader(skippedSource, *fullRange);
        auto skippedTree = AnalysisTree::create(QStringLiteral("conditional-skipped-source"));
        QVERIFY(skippedTree.has_value());
        const auto skipped = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       skippedReader,
                                                       *mapping,
                                                       0,
                                                       *skippedTree,
                                                       skippedTree->rootId());
        QCOMPARE(skipped.status, DslExecutionStatus::Materialized);
        QCOMPARE(skipped.bitsConsumed, quint64(8));
        const auto skippedStructure = skippedTree->node(*skipped.structureNode);
        QVERIFY(skippedStructure.has_value());
        QCOMPARE(skippedStructure->children().size(), std::size_t(2));
        QCOMPARE(skippedTree->node(skippedStructure->children().at(1))->name(),
                 QStringLiteral("tail"));
    }

    void appliesEqualsOnlyToSelectedConditionalFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag; "
            "if (flag == 1) { bits<1> reserved @equals(0); } "
            "else { bits<1> value; } bits<6> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource selectedSource(bytes({0xc0}));
        BitReader selectedReader(selectedSource, *range);
        auto selectedTree = AnalysisTree::create(QStringLiteral("conditional-equals-selected"));
        QVERIFY(selectedTree.has_value());
        const auto selected = DslExecutor::decodeStruct(*compiled.program,
                                                        quint32(0),
                                                        selectedReader,
                                                        *mapping,
                                                        0,
                                                        *selectedTree,
                                                        selectedTree->rootId());
        QCOMPARE(selected.status, DslExecutionStatus::InvalidSyntax);
        QCOMPARE(selected.bitsConsumed, quint64(2));
        QCOMPARE(selected.nodesCreated, quint64(3));
        const auto selectedStructure = selectedTree->node(*selected.structureNode);
        QVERIFY(selectedStructure.has_value());
        QCOMPARE(selectedStructure->children().size(), std::size_t(2));
        QCOMPARE(selectedStructure->diagnostics().front().code,
                 DiagnosticCode::InvalidSyntax);
        QCOMPARE(selectedStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.reserved"));

        MemorySource skippedSource(bytes({0x40}));
        BitReader skippedReader(skippedSource, *range);
        auto skippedTree = AnalysisTree::create(QStringLiteral("conditional-equals-skipped"));
        QVERIFY(skippedTree.has_value());
        const auto skipped = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       skippedReader,
                                                       *mapping,
                                                       0,
                                                       *skippedTree,
                                                       skippedTree->rootId());
        QCOMPARE(skipped.status, DslExecutionStatus::Materialized);
        QCOMPARE(skipped.bitsConsumed, quint64(8));
        QCOMPARE(skipped.instructionsExecuted, quint64(7));
        QCOMPARE(skipped.nodesCreated, quint64(4));
        const auto skippedStructure = skippedTree->node(*skipped.structureNode);
        QVERIFY(skippedStructure.has_value());
        QCOMPARE(skippedStructure->children().size(), std::size_t(3));
        QCOMPARE(skippedTree->node(skippedStructure->children().at(1))->name(),
                 QStringLiteral("value"));
    }

    void countsSkippedConditionalInstructionsButNotNodes() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag; "
            "if (flag == 1) { bits<1> reserved @equals(0); } "
            "else { bits<1> value; } bits<6> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource instructionSource(bytes({0x40}));
        BitReader instructionReader(instructionSource, *range);
        auto instructionTree = AnalysisTree::create(QStringLiteral("conditional-instructions"));
        QVERIFY(instructionTree.has_value());
        DslExecutionOptions instructionOptions;
        instructionOptions.limits.maximumInstructions = 4;
        const auto instructionResult = DslExecutor::decodeStruct(*compiled.program,
                                                                quint32(0),
                                                                instructionReader,
                                                                *mapping,
                                                                0,
                                                                *instructionTree,
                                                                instructionTree->rootId(),
                                                                instructionOptions);
        QCOMPARE(instructionResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(instructionResult.instructionsExecuted, quint64(4));
        QCOMPARE(instructionResult.bitsConsumed, quint64(1));
        QCOMPARE(instructionResult.nodesCreated, quint64(2));

        MemorySource nodeSource(bytes({0x40}));
        BitReader nodeReader(nodeSource, *range);
        auto nodeTree = AnalysisTree::create(QStringLiteral("conditional-nodes"));
        QVERIFY(nodeTree.has_value());
        DslExecutionOptions nodeOptions;
        nodeOptions.limits.maximumMaterializedNodes = 4;
        const auto nodeResult = DslExecutor::decodeStruct(*compiled.program,
                                                         quint32(0),
                                                         nodeReader,
                                                         *mapping,
                                                         0,
                                                         *nodeTree,
                                                         nodeTree->rootId(),
                                                         nodeOptions);
        QCOMPARE(nodeResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(nodeResult.nodesCreated, quint64(4));
        QCOMPARE(nodeResult.bitsConsumed, quint64(8));
    }

    void rejectsMalformedConditionalGuardsBeforeExecutingBytecode() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> outer; if (outer == 1) { bits<1> local; "
            "if (local == 1) { bits<1> value; } } } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        QCOMPARE(compiled.program->structs.front().fields.size(), std::size_t(3));

        std::vector<DslTypedProgram> malformed;
        auto outOfRange = *compiled.program;
        outOfRange.structs.front().fields.at(2).conditions.back().fieldIndex = 99;
        malformed.push_back(std::move(outOfRange));
        auto future = *compiled.program;
        future.structs.front().fields.at(2).conditions.back().fieldIndex = 2;
        malformed.push_back(std::move(future));
        auto expectedTooWide = *compiled.program;
        expectedTooWide.structs.front().fields.at(2).conditions.front().expectedValue = 2;
        malformed.push_back(std::move(expectedTooWide));
        auto unavailable = *compiled.program;
        unavailable.structs.front().fields.at(2).conditions.erase(
            unavailable.structs.front().fields.at(2).conditions.begin());
        malformed.push_back(std::move(unavailable));
        auto invalidController = *compiled.program;
        invalidController.structs.front().fields.front().type.kind =
            DslValueTypeKind::UnsignedExpGolomb;
        invalidController.structs.front().fields.front().type.bitWidth = 0;
        malformed.push_back(std::move(invalidController));
        auto invalidOperator = *compiled.program;
        invalidOperator.structs.front().fields.at(2).conditions.back().op =
            static_cast<DslConditionOperator>(255);
        malformed.push_back(std::move(invalidOperator));

        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());
        for (std::size_t index = 0; index < malformed.size(); ++index) {
            MemorySource source(bytes({0xe0}));
            BitReader reader(source, *range);
            auto tree = AnalysisTree::create(
                QStringLiteral("malformed-conditional-%1").arg(index));
            QVERIFY(tree.has_value());
            const auto result = DslExecutor::decodeStruct(malformed.at(index),
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
            QCOMPARE(result.instructionsExecuted, quint64(0));
            QCOMPARE(result.bitsConsumed, quint64(0));
            QCOMPARE(result.nodesCreated, quint64(0));
            QVERIFY(!result.structureNode.has_value());
        }
    }

    void rejectsMalformedFieldsEvenWhenTheirConditionalBranchIsSkipped() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<1> flag; if (flag == 1) { bits<1> fixed; } "
            "else { ue code; } } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        auto malformedFixed = *compiled.program;
        malformedFixed.structs.front().fields.at(1).type.bitWidth = 0;
        MemorySource fixedSource(bytes({0x00}));
        BitReader fixedReader(fixedSource, *range);
        auto fixedTree = AnalysisTree::create(QStringLiteral("malformed-skipped-fixed"));
        QVERIFY(fixedTree.has_value());
        const auto fixedResult = DslExecutor::decodeStruct(malformedFixed,
                                                           quint32(0),
                                                           fixedReader,
                                                           *mapping,
                                                           0,
                                                           *fixedTree,
                                                           fixedTree->rootId());
        QCOMPARE(fixedResult.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(fixedResult.instructionsExecuted, quint64(3));
        QCOMPARE(fixedResult.bitsConsumed, quint64(1));
        QCOMPARE(fixedResult.nodesCreated, quint64(2));

        auto malformedExpGolomb = *compiled.program;
        malformedExpGolomb.structs.front().fields.at(2).equalsConstraint = 0;
        MemorySource expSource(bytes({0x80}));
        BitReader expReader(expSource, *range);
        auto expTree = AnalysisTree::create(QStringLiteral("malformed-skipped-exp-golomb"));
        QVERIFY(expTree.has_value());
        const auto expResult = DslExecutor::decodeStruct(malformedExpGolomb,
                                                         quint32(0),
                                                         expReader,
                                                         *mapping,
                                                         0,
                                                         *expTree,
                                                         expTree->rootId());
        QCOMPARE(expResult.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(expResult.instructionsExecuted, quint64(4));
        QCOMPARE(expResult.bitsConsumed, quint64(2));
        QCOMPARE(expResult.nodesCreated, quint64(3));
    }

    void materializesSelectedSwitchCasesDefaultAndNoMatch() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 1: { bits<3> compact_value; } "
            "case 2: { bits<5> extended_value; } "
            "default: { bits<4> unknown_value; } } bits<2> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());

        struct Case final {
            QString name;
            std::vector<std::byte> sourceBytes;
            quint64 bitLength = 0;
            std::vector<QString> fieldNames;
            std::vector<quint64> fieldValues;
            std::vector<quint64> fieldStarts;
        };
        const std::vector<Case> cases{
            {QStringLiteral("switch-case-one"),
             bytes({0x6c}),
             7,
             {QStringLiteral("kind"),
              QStringLiteral("compact_value"),
              QStringLiteral("tail")},
             {1, 5, 2},
             {0, 2, 5}},
            {QStringLiteral("switch-case-two"),
             bytes({0xab, 0x80}),
             9,
             {QStringLiteral("kind"),
              QStringLiteral("extended_value"),
              QStringLiteral("tail")},
             {2, 21, 3},
             {0, 2, 7}},
            {QStringLiteral("switch-default"),
             bytes({0xe5}),
             8,
             {QStringLiteral("kind"),
              QStringLiteral("unknown_value"),
              QStringLiteral("tail")},
             {3, 9, 1},
             {0, 2, 6}},
        };

        for (const Case& testCase : cases) {
            MemorySource source(testCase.sourceBytes);
            const auto mapping = mappingForBytes(testCase.sourceBytes.size());
            const auto range = SourceSpan::create(
                streamview::core::SourceBitAddress(0), testCase.bitLength);
            QVERIFY(mapping.has_value());
            QVERIFY(range.has_value());
            BitReader reader(source, *range);
            auto tree = AnalysisTree::create(testCase.name);
            QVERIFY(tree.has_value());
            const auto result = DslExecutor::decodeStruct(*compiled.program,
                                                          quint32(0),
                                                          reader,
                                                          *mapping,
                                                          0,
                                                          *tree,
                                                          tree->rootId());
            QCOMPARE(result.status, DslExecutionStatus::Materialized);
            QCOMPARE(result.bitsConsumed, testCase.bitLength);
            QCOMPARE(result.instructionsExecuted, quint64(7));
            QCOMPARE(result.nodesCreated, quint64(4));
            const auto structure = tree->node(*result.structureNode);
            QVERIFY(structure.has_value());
            QCOMPARE(structure->children().size(), testCase.fieldNames.size());
            for (std::size_t index = 0; index < testCase.fieldNames.size(); ++index) {
                const auto field = tree->node(structure->children().at(index));
                QVERIFY(field.has_value());
                QCOMPARE(field->name(), testCase.fieldNames.at(index));
                QCOMPARE(field->value().toULongLong(), testCase.fieldValues.at(index));
                QCOMPARE(field->location()->sourceSpans().front().start()
                             .absoluteBitOffset(),
                         testCase.fieldStarts.at(index));
            }
        }

        const auto withoutDefault = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 1: { bits<3> compact_value; } } bits<4> tail; } entry Header;"));
        QVERIFY(withoutDefault.succeeded());
        const auto compiledWithoutDefault = DslCompiler::compile(withoutDefault.program);
        QVERIFY(compiledWithoutDefault.succeeded());
        MemorySource unmatchedSource(bytes({0xa8}));
        const auto unmatchedMapping = mappingForBytes(1);
        const auto unmatchedRange =
            SourceSpan::create(streamview::core::SourceBitAddress(0), 6);
        QVERIFY(unmatchedMapping.has_value());
        QVERIFY(unmatchedRange.has_value());
        BitReader unmatchedReader(unmatchedSource, *unmatchedRange);
        auto unmatchedTree = AnalysisTree::create(QStringLiteral("switch-no-match"));
        QVERIFY(unmatchedTree.has_value());
        const auto unmatched = DslExecutor::decodeStruct(*compiledWithoutDefault.program,
                                                         quint32(0),
                                                         unmatchedReader,
                                                         *unmatchedMapping,
                                                         0,
                                                         *unmatchedTree,
                                                         unmatchedTree->rootId());
        QCOMPARE(unmatched.status, DslExecutionStatus::Materialized);
        QCOMPARE(unmatched.bitsConsumed, quint64(6));
        QCOMPARE(unmatched.instructionsExecuted, quint64(5));
        QCOMPARE(unmatched.nodesCreated, quint64(3));
        const auto unmatchedStructure = unmatchedTree->node(*unmatched.structureNode);
        QVERIFY(unmatchedStructure.has_value());
        QCOMPARE(unmatchedStructure->children().size(), std::size_t(2));
        const auto unmatchedTail =
            unmatchedTree->node(unmatchedStructure->children().at(1));
        QVERIFY(unmatchedTail.has_value());
        QCOMPARE(unmatchedTail->name(), QStringLiteral("tail"));
        QCOMPARE(unmatchedTail->value().toULongLong(), quint64(10));
        QCOMPARE(unmatchedTail->location()->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(2));
    }

    void materializesSwitchNestedConditionalArraysAndExpGolombFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 1: { bits<1> inner; if (inner == 1) { "
            "bits<2> values[2] @equals(1); } else { ue code; se delta; } } "
            "case 2: { bits<3> alternative; } "
            "default: { bits<4> fallback; } } bits<1> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource arraySource(bytes({0x6b}));
        BitReader arrayReader(arraySource, *range);
        auto arrayTree = AnalysisTree::create(QStringLiteral("switch-array"));
        QVERIFY(arrayTree.has_value());
        const auto arrayResult = DslExecutor::decodeStruct(*compiled.program,
                                                          quint32(0),
                                                          arrayReader,
                                                          *mapping,
                                                          0,
                                                          *arrayTree,
                                                          arrayTree->rootId());
        QCOMPARE(arrayResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(arrayResult.bitsConsumed, quint64(8));
        QCOMPARE(arrayResult.instructionsExecuted, quint64(13));
        QCOMPARE(arrayResult.nodesCreated, quint64(6));
        const auto arrayStructure = arrayTree->node(*arrayResult.structureNode);
        QVERIFY(arrayStructure.has_value());
        const std::vector<QString> arrayNames{QStringLiteral("kind"),
                                              QStringLiteral("inner"),
                                              QStringLiteral("values[0]"),
                                              QStringLiteral("values[1]"),
                                              QStringLiteral("tail")};
        const std::vector<quint64> arrayValues{1, 1, 1, 1, 1};
        const std::vector<quint64> arrayStarts{0, 2, 3, 5, 7};
        QCOMPARE(arrayStructure->children().size(), arrayNames.size());
        for (std::size_t index = 0; index < arrayNames.size(); ++index) {
            const auto field = arrayTree->node(arrayStructure->children().at(index));
            QVERIFY(field.has_value());
            QCOMPARE(field->name(), arrayNames.at(index));
            QCOMPARE(field->value().toULongLong(), arrayValues.at(index));
            QCOMPARE(field->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     arrayStarts.at(index));
        }

        MemorySource expSource(bytes({0x55}));
        BitReader expReader(expSource, *range);
        auto expTree = AnalysisTree::create(QStringLiteral("switch-exp-golomb"));
        QVERIFY(expTree.has_value());
        const auto expResult = DslExecutor::decodeStruct(*compiled.program,
                                                        quint32(0),
                                                        expReader,
                                                        *mapping,
                                                        0,
                                                        *expTree,
                                                        expTree->rootId());
        QCOMPARE(expResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(expResult.bitsConsumed, quint64(8));
        QCOMPARE(expResult.instructionsExecuted, quint64(13));
        QCOMPARE(expResult.nodesCreated, quint64(6));
        const auto expStructure = expTree->node(*expResult.structureNode);
        QVERIFY(expStructure.has_value());
        QCOMPARE(expStructure->children().size(), std::size_t(5));
        const auto code = expTree->node(expStructure->children().at(2));
        const auto delta = expTree->node(expStructure->children().at(3));
        const auto tail = expTree->node(expStructure->children().at(4));
        QVERIFY(code.has_value());
        QVERIFY(delta.has_value());
        QVERIFY(tail.has_value());
        QCOMPARE(code->name(), QStringLiteral("code"));
        QCOMPARE(code->value().toULongLong(), quint64(0));
        QCOMPARE(code->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(3));
        QCOMPARE(code->location()->sourceSpans().front().bitLength(), quint64(1));
        QCOMPARE(delta->name(), QStringLiteral("delta"));
        QCOMPARE(delta->value().toLongLong(), qlonglong(1));
        QCOMPARE(delta->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(4));
        QCOMPARE(delta->location()->sourceSpans().front().bitLength(), quint64(3));
        QCOMPARE(tail->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(7));
    }

    void usesEnumSwitchControllersAndGuardsLittleEndianFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "enum Kind { little = 1; big = 2; other = 3; } "
            "struct Header { bits<8> kind @enum(Kind); switch (kind) { "
            "case 1: { bits<16, little> value; } "
            "case 2: { bits<16> alternative; } "
            "default: { bits<16> fallback; } } bits<8> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(4);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 32);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource littleSource(bytes({1, 0x34, 0x12, 0xab}));
        BitReader littleReader(littleSource, *range);
        auto littleTree = AnalysisTree::create(QStringLiteral("switch-enum-little"));
        QVERIFY(littleTree.has_value());
        const auto little = DslExecutor::decodeStruct(*compiled.program,
                                                      quint32(0),
                                                      littleReader,
                                                      *mapping,
                                                      0,
                                                      *littleTree,
                                                      littleTree->rootId());
        QCOMPARE(little.status, DslExecutionStatus::Materialized);
        QCOMPARE(little.bitsConsumed, quint64(32));
        QCOMPARE(little.instructionsExecuted, quint64(7));
        QCOMPARE(little.nodesCreated, quint64(4));
        const auto littleStructure = littleTree->node(*little.structureNode);
        QVERIFY(littleStructure.has_value());
        const auto value = littleTree->node(littleStructure->children().at(1));
        QVERIFY(value.has_value());
        QCOMPARE(value->name(), QStringLiteral("value"));
        QCOMPARE(value->value().toULongLong(), quint64(0x1234));

        MemorySource defaultSource(bytes({3, 0x12, 0x34, 0xcd}));
        BitReader defaultReader(defaultSource, *range);
        auto defaultTree = AnalysisTree::create(QStringLiteral("switch-enum-default"));
        QVERIFY(defaultTree.has_value());
        const auto defaultResult = DslExecutor::decodeStruct(*compiled.program,
                                                            quint32(0),
                                                            defaultReader,
                                                            *mapping,
                                                            0,
                                                            *defaultTree,
                                                            defaultTree->rootId());
        QCOMPARE(defaultResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(defaultResult.bitsConsumed, quint64(32));
        QCOMPARE(defaultResult.instructionsExecuted, quint64(7));
        QCOMPARE(defaultResult.nodesCreated, quint64(4));
        const auto defaultStructure = defaultTree->node(*defaultResult.structureNode);
        QVERIFY(defaultStructure.has_value());
        const auto fallback = defaultTree->node(defaultStructure->children().at(1));
        QVERIFY(fallback.has_value());
        QCOMPARE(fallback->name(), QStringLiteral("fallback"));
        QCOMPARE(fallback->value().toULongLong(), quint64(0x1234));
    }

    void reportsTruncationAndEqualsOnlyForSelectedSwitchArms() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 1: { bits<6> payload; } "
            "case 2: { bits<1> reserved @equals(0); } "
            "default: { bits<1> fallback; } } bits<5> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        QVERIFY(mapping.has_value());

        MemorySource truncatedSource(bytes({0x70}));
        const auto shortRange = SourceSpan::create(streamview::core::SourceBitAddress(0), 4);
        QVERIFY(shortRange.has_value());
        BitReader truncatedReader(truncatedSource, *shortRange);
        auto truncatedTree = AnalysisTree::create(QStringLiteral("switch-truncated"));
        QVERIFY(truncatedTree.has_value());
        const auto truncated = DslExecutor::decodeStruct(*compiled.program,
                                                         quint32(0),
                                                         truncatedReader,
                                                         *mapping,
                                                         0,
                                                         *truncatedTree,
                                                         truncatedTree->rootId());
        QCOMPARE(truncated.status, DslExecutionStatus::TruncatedSource);
        QCOMPARE(truncated.bitsConsumed, quint64(2));
        QCOMPARE(truncated.nodesCreated, quint64(2));
        const auto truncatedStructure = truncatedTree->node(*truncated.structureNode);
        QVERIFY(truncatedStructure.has_value());
        QCOMPARE(truncatedStructure->children().size(), std::size_t(1));
        QCOMPARE(truncatedStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.payload"));
        QCOMPARE(truncatedStructure->diagnostics().front().location->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(2));
        QCOMPARE(truncatedStructure->diagnostics().front().location->sourceSpans().front()
                     .bitLength(),
                 quint64(2));

        MemorySource invalidSource(bytes({0xa0}));
        const auto fullRange = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(fullRange.has_value());
        BitReader invalidReader(invalidSource, *fullRange);
        auto invalidTree = AnalysisTree::create(QStringLiteral("switch-equals-selected"));
        QVERIFY(invalidTree.has_value());
        const auto invalid = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       invalidReader,
                                                       *mapping,
                                                       0,
                                                       *invalidTree,
                                                       invalidTree->rootId());
        QCOMPARE(invalid.status, DslExecutionStatus::InvalidSyntax);
        QCOMPARE(invalid.bitsConsumed, quint64(3));
        QCOMPARE(invalid.nodesCreated, quint64(3));
        const auto invalidStructure = invalidTree->node(*invalid.structureNode);
        QVERIFY(invalidStructure.has_value());
        QCOMPARE(invalidStructure->diagnostics().front().fieldPath,
                 QStringLiteral("Header.reserved"));

        MemorySource skippedSource(bytes({0xf5}));
        BitReader skippedReader(skippedSource, *fullRange);
        auto skippedTree = AnalysisTree::create(QStringLiteral("switch-equals-skipped"));
        QVERIFY(skippedTree.has_value());
        const auto skipped = DslExecutor::decodeStruct(*compiled.program,
                                                       quint32(0),
                                                       skippedReader,
                                                       *mapping,
                                                       0,
                                                       *skippedTree,
                                                       skippedTree->rootId());
        QCOMPARE(skipped.status, DslExecutionStatus::Materialized);
        QCOMPARE(skipped.bitsConsumed, quint64(8));
        QCOMPARE(skipped.instructionsExecuted, quint64(8));
        QCOMPARE(skipped.nodesCreated, quint64(4));
        const auto skippedStructure = skippedTree->node(*skipped.structureNode);
        QVERIFY(skippedStructure.has_value());
        QCOMPARE(skippedStructure->children().size(), std::size_t(3));
        const auto fallback = skippedTree->node(skippedStructure->children().at(1));
        const auto tail = skippedTree->node(skippedStructure->children().at(2));
        QVERIFY(fallback.has_value());
        QVERIFY(tail.has_value());
        QCOMPARE(fallback->name(), QStringLiteral("fallback"));
        QCOMPARE(fallback->value().toULongLong(), quint64(1));
        QCOMPARE(tail->value().toULongLong(), quint64(21));
    }

    void countsSkippedSwitchInstructionsButNotNodes() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 0: { bits<1> first @equals(0); } "
            "case 1: { bits<1> second; } "
            "default: { bits<1> fallback; } } bits<5> tail; } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        MemorySource instructionSource(bytes({0x60}));
        BitReader instructionReader(instructionSource, *range);
        auto instructionTree = AnalysisTree::create(QStringLiteral("switch-instructions"));
        QVERIFY(instructionTree.has_value());
        DslExecutionOptions instructionOptions;
        instructionOptions.limits.maximumInstructions = 4;
        const auto instructionResult = DslExecutor::decodeStruct(*compiled.program,
                                                                quint32(0),
                                                                instructionReader,
                                                                *mapping,
                                                                0,
                                                                *instructionTree,
                                                                instructionTree->rootId(),
                                                                instructionOptions);
        QCOMPARE(instructionResult.status, DslExecutionStatus::ResourceLimit);
        QCOMPARE(instructionResult.instructionsExecuted, quint64(4));
        QCOMPARE(instructionResult.bitsConsumed, quint64(2));
        QCOMPARE(instructionResult.nodesCreated, quint64(2));

        MemorySource nodeSource(bytes({0x60}));
        BitReader nodeReader(nodeSource, *range);
        auto nodeTree = AnalysisTree::create(QStringLiteral("switch-nodes"));
        QVERIFY(nodeTree.has_value());
        DslExecutionOptions nodeOptions;
        nodeOptions.limits.maximumMaterializedNodes = 4;
        const auto nodeResult = DslExecutor::decodeStruct(*compiled.program,
                                                         quint32(0),
                                                         nodeReader,
                                                         *mapping,
                                                         0,
                                                         *nodeTree,
                                                         nodeTree->rootId(),
                                                         nodeOptions);
        QCOMPARE(nodeResult.status, DslExecutionStatus::Materialized);
        QCOMPARE(nodeResult.instructionsExecuted, quint64(8));
        QCOMPARE(nodeResult.bitsConsumed, quint64(8));
        QCOMPARE(nodeResult.nodesCreated, quint64(4));
    }

    void rejectsMalformedSwitchGuardsAndSkippedFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<2> kind; switch (kind) { "
            "case 1: { bits<1> first; } case 2: { bits<1> second; } "
            "default: { bits<1> fallback; } } } entry Header;"));
        QVERIFY(parsed.succeeded());
        const auto compiled = DslCompiler::compile(parsed.program);
        QVERIFY(compiled.succeeded());
        QCOMPARE(compiled.program->structs.front().fields.at(3).conditions.size(),
                 std::size_t(2));
        const auto mapping = mappingForBytes(1);
        const auto range = SourceSpan::create(streamview::core::SourceBitAddress(0), 8);
        QVERIFY(mapping.has_value());
        QVERIFY(range.has_value());

        auto malformedGuard = *compiled.program;
        malformedGuard.structs.front().fields.at(3).conditions.at(1).fieldIndex = 3;
        MemorySource guardSource(bytes({0xc0}));
        BitReader guardReader(guardSource, *range);
        auto guardTree = AnalysisTree::create(QStringLiteral("malformed-switch-guard"));
        QVERIFY(guardTree.has_value());
        const auto guardResult = DslExecutor::decodeStruct(malformedGuard,
                                                           quint32(0),
                                                           guardReader,
                                                           *mapping,
                                                           0,
                                                           *guardTree,
                                                           guardTree->rootId());
        QCOMPARE(guardResult.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(guardResult.instructionsExecuted, quint64(0));
        QCOMPARE(guardResult.bitsConsumed, quint64(0));
        QCOMPARE(guardResult.nodesCreated, quint64(0));
        QVERIFY(!guardResult.structureNode.has_value());

        auto malformedSkippedField = *compiled.program;
        malformedSkippedField.structs.front().fields.at(1).type.bitWidth = 0;
        MemorySource fieldSource(bytes({0x80}));
        BitReader fieldReader(fieldSource, *range);
        auto fieldTree = AnalysisTree::create(QStringLiteral("malformed-skipped-switch-field"));
        QVERIFY(fieldTree.has_value());
        const auto fieldResult = DslExecutor::decodeStruct(malformedSkippedField,
                                                           quint32(0),
                                                           fieldReader,
                                                           *mapping,
                                                           0,
                                                           *fieldTree,
                                                           fieldTree->rootId());
        QCOMPARE(fieldResult.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(fieldResult.instructionsExecuted, quint64(3));
        QCOMPARE(fieldResult.bitsConsumed, quint64(2));
        QCOMPARE(fieldResult.nodesCreated, quint64(2));
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

    void materializesMappedFieldAcrossASourceGap() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<16> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0xab, 0xff, 0xcd}));
        const auto mapping = mappingForSpans({{0, 8}, {16, 8}});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);
        auto tree = AnalysisTree::create(QStringLiteral("mapped-fixed-field"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64{16});
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QCOMPARE(structure->children().size(), std::size_t{1});
        const auto value = tree->node(structure->children().front());
        QVERIFY(value.has_value());
        QCOMPARE(value->value().toULongLong(), quint64{0xabcd});
        QVERIFY(value->location().has_value());
        QCOMPARE(value->location()->logicalRange().bitLength(), quint64{16});
        QCOMPARE(value->location()->sourceSpans().size(), std::size_t{2});
        QCOMPARE(value->location()->sourceSpans().at(0).start().absoluteBitOffset(), quint64{0});
        QCOMPARE(value->location()->sourceSpans().at(0).bitLength(), quint64{8});
        QCOMPARE(value->location()->sourceSpans().at(1).start().absoluteBitOffset(), quint64{16});
        QCOMPARE(value->location()->sourceSpans().at(1).bitLength(), quint64{8});
    }

    void executesAMappedReaderSliceAtANonzeroLogicalStart() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<16> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0xaa, 0xff, 0xbc, 0xff, 0xde}));
        const auto mapping = mappingForSpans({{0, 8}, {16, 8}, {32, 8}});
        QVERIFY(mapping.has_value());
        auto reader = BitReader::fromMappingSlice(source, *mapping, 8, 16);
        QVERIFY(reader.has_value());
        auto tree = AnalysisTree::create(QStringLiteral("mapped-reader-slice"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(parsed.program,
                                                       QStringLiteral("Header"),
                                                       *reader,
                                                       *mapping,
                                                       8,
                                                       *tree,
                                                       tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64{16});
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const auto value = tree->node(structure->children().front());
        QVERIFY(value.has_value());
        QCOMPARE(value->value().toULongLong(), quint64{0xbcde});
        QCOMPARE(value->location()->logicalRange().start().bitOffset(), quint64{8});
        QCOMPARE(value->location()->sourceSpans().size(), std::size_t{2});
        QCOMPARE(value->location()->sourceSpans().at(0).start().absoluteBitOffset(), quint64{16});
        QCOMPARE(value->location()->sourceSpans().at(1).start().absoluteBitOffset(), quint64{32});
    }

    void locatesMappedTruncationAcrossAvailableSourceSpans() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<16> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0xab, 0xff, 0xc0}));
        const auto mapping = mappingForSpans({{0, 8}, {16, 4}});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);
        auto tree = AnalysisTree::create(QStringLiteral("mapped-truncation"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::TruncatedSource);
        QCOMPARE(result.bitsConsumed, quint64{0});
        QCOMPARE(reader.position(), quint64{0});
        QCOMPARE(source.readCount(), quint64{0});
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QVERIFY(structure->children().empty());
        QCOMPARE(structure->diagnostics().size(), std::size_t{1});
        const auto& diagnostic = structure->diagnostics().front();
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->logicalRange().bitLength(), quint64{12});
        QCOMPARE(diagnostic.location->sourceSpans().size(), std::size_t{2});
        QCOMPARE(diagnostic.location->sourceSpans().at(0).start().absoluteBitOffset(), quint64{0});
        QCOMPARE(diagnostic.location->sourceSpans().at(0).bitLength(), quint64{8});
        QCOMPARE(diagnostic.location->sourceSpans().at(1).start().absoluteBitOffset(), quint64{16});
        QCOMPARE(diagnostic.location->sourceSpans().at(1).bitLength(), quint64{4});
    }

    void decodesMappedExpGolombFieldAcrossASourceGap() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { ue value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x00, 0xa0}));
        const auto mapping = mappingForSpans({{0, 2}, {8, 3}});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);
        auto tree = AnalysisTree::create(QStringLiteral("mapped-exp-golomb"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64{5});
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const auto value = tree->node(structure->children().front());
        QVERIFY(value.has_value());
        QCOMPARE(value->value().toULongLong(), quint64{4});
        QVERIFY(value->location().has_value());
        QCOMPARE(value->location()->logicalRange().bitLength(), quint64{5});
        QCOMPARE(value->location()->sourceSpans().size(), std::size_t{2});
        QCOMPARE(value->location()->sourceSpans().at(0).start().absoluteBitOffset(), quint64{0});
        QCOMPARE(value->location()->sourceSpans().at(0).bitLength(), quint64{2});
        QCOMPARE(value->location()->sourceSpans().at(1).start().absoluteBitOffset(), quint64{8});
        QCOMPARE(value->location()->sourceSpans().at(1).bitLength(), quint64{3});
    }

    void keepsMappedFieldTransactionalWhenALaterSourceSpanFails() {
        const auto parsed = DslParser::parse(
            QStringLiteral("struct Header { bits<16> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        FailingAfterFirstReadSource source(bytes({0xab, 0xff, 0xcd}));
        const auto mapping = mappingForSpans({{0, 8}, {16, 8}});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);
        auto tree = AnalysisTree::create(QStringLiteral("mapped-source-error"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::SourceError);
        QCOMPARE(result.bitsConsumed, quint64{0});
        QCOMPARE(reader.position(), quint64{0});
        QCOMPARE(result.nodesCreated, quint64{1});
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        QVERIFY(structure->children().empty());
        QCOMPARE(structure->diagnostics().size(), std::size_t{1});
        const auto& diagnostic = structure->diagnostics().front();
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().size(), std::size_t{2});
        QCOMPARE(diagnostic.location->sourceSpans().at(0).start().absoluteBitOffset(), quint64{0});
        QCOMPARE(diagnostic.location->sourceSpans().at(1).start().absoluteBitOffset(), quint64{16});
    }

    void decodesMappedLittleEndianFieldAcrossASourceGap() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<16, little> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x34, 0xff, 0x12}));
        const auto mapping = mappingForSpans({{0, 8}, {16, 8}});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);
        auto tree = AnalysisTree::create(QStringLiteral("mapped-little-endian"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        QCOMPARE(result.bitsConsumed, quint64{16});
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const auto value = tree->node(structure->children().front());
        QVERIFY(value.has_value());
        QCOMPARE(value->value().toULongLong(), quint64{0x1234});
        QVERIFY(value->location().has_value());
        QCOMPARE(value->location()->sourceSpans().size(), std::size_t{2});
        QCOMPARE(value->location()->sourceSpans().at(0).start().absoluteBitOffset(), quint64{0});
        QCOMPARE(value->location()->sourceSpans().at(0).bitLength(), quint64{8});
        QCOMPARE(value->location()->sourceSpans().at(1).start().absoluteBitOffset(), quint64{16});
        QCOMPARE(value->location()->sourceSpans().at(1).bitLength(), quint64{8});
    }

    void allowsUnalignedLaterSpansInMappedLittleEndianFields() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<16, little> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x34, 0x10, 0x02}));
        const auto mapping = mappingForSpans({{0, 12}, {20, 4}});
        QVERIFY(mapping.has_value());
        BitReader reader(source, *mapping);
        auto tree = AnalysisTree::create(QStringLiteral("mapped-unaligned-later-span"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(
            parsed.program, QStringLiteral("Header"), reader, *mapping, 0, *tree, tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::Materialized);
        const auto structure = tree->node(*result.structureNode);
        QVERIFY(structure.has_value());
        const auto value = tree->node(structure->children().front());
        QVERIFY(value.has_value());
        QCOMPARE(value->value().toULongLong(), quint64{0x1234});
        QVERIFY(value->location().has_value());
        QCOMPARE(value->location()->sourceSpans().size(), std::size_t{2});
        QCOMPARE(value->location()->sourceSpans().at(0).bitLength(), quint64{12});
        QCOMPARE(value->location()->sourceSpans().at(1).start().absoluteBitOffset(), quint64{20});
        QCOMPARE(value->location()->sourceSpans().at(1).bitLength(), quint64{4});
    }

    void rejectsMappedLittleEndianAtAnUnalignedLogicalStart() {
        const auto parsed = DslParser::parse(QStringLiteral(
            "struct Header { bits<16, little> value; } entry Header;"));
        QVERIFY(parsed.succeeded());

        MemorySource source(bytes({0x03, 0x41, 0x20}));
        const auto mapping = mappingForSpans({{4, 20}});
        QVERIFY(mapping.has_value());
        auto reader = BitReader::fromMappingSlice(source, *mapping, 4, 16);
        QVERIFY(reader.has_value());
        QCOMPARE(reader->backingSpans().front().start().absoluteBitOffset(), quint64{8});
        auto tree = AnalysisTree::create(QStringLiteral("unaligned-logical-little-endian"));
        QVERIFY(tree.has_value());

        const auto result = DslExecutor::decodeStruct(parsed.program,
                                                       QStringLiteral("Header"),
                                                       *reader,
                                                       *mapping,
                                                       4,
                                                       *tree,
                                                       tree->rootId());
        QCOMPARE(result.status, DslExecutionStatus::InvalidDefinition);
        QCOMPARE(result.bitsConsumed, quint64{0});
        QCOMPARE(result.nodesCreated, quint64{1});
        QCOMPARE(reader->position(), quint64{0});
        QCOMPARE(source.readCount(), quint64{0});
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
