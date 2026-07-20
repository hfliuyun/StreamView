#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <vector>

using streamview::core::AnalysisNodeKind;
using streamview::core::AnalysisTree;
using streamview::core::BitReader;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourceSpan;
using streamview::rules::DslExecutionStatus;
using streamview::rules::DslExecutor;
using streamview::rules::DslParser;

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
};

QTEST_GUILESS_MAIN(DslExecutorTest)

#include "dsl_executor_test.moc"
