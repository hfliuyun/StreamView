#include <streamview/rules/window_decoder.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/dsl_ir.h>

#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace {

class MemorySource final : public streamview::core::RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> data) : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("memory"); }

    [[nodiscard]] streamview::core::SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {streamview::core::SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= data_.size()) {
            return {streamview::core::SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const auto count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.data() + offset, count, destination.data());
        return {count == destination.size()
                    ? streamview::core::SourceReadStatus::Complete
                    : streamview::core::SourceReadStatus::EndOfSource,
                count, {}};
    }

private:
    std::vector<std::byte> data_;
};

class FailingSource final : public streamview::core::RandomAccessSource {
public:
    explicit FailingSource(quint64 sizeBytes = 1024) : sizeBytes_(sizeBytes) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return sizeBytes_; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("failing"); }

    [[nodiscard]] streamview::core::SourceReadResult
    readAt(quint64 /*byteOffset*/, std::span<std::byte> /*destination*/) const override {
        return {streamview::core::SourceReadStatus::Error, 0, QStringLiteral("I/O device fault")};
    }

private:
    quint64 sizeBytes_ = 1024;
};

[[nodiscard]] streamview::rules::DslTypedProgram compileProgram(const QString& sourceText) {
    auto parseResult = streamview::rules::DslParser::parse(sourceText);
    if (!parseResult.succeeded()) {
        qFatal("DSL parse failed: %s", qUtf8Printable(parseResult.diagnostics.empty() ? QString() : parseResult.diagnostics.front().message));
    }
    auto compileResult = streamview::rules::DslCompiler::compile(parseResult.program);
    if (!compileResult.succeeded()) {
        qFatal("DSL compile failed: %s", qUtf8Printable(compileResult.diagnostics.empty() ? QString() : compileResult.diagnostics.front().message));
    }
    return std::move(*compileResult.program);
}

[[nodiscard]] std::shared_ptr<streamview::core::AnalysisTree> makeTestTree() {
    auto treeOpt = streamview::core::AnalysisTree::create(QStringLiteral("test"));
    if (!treeOpt.has_value()) {
        qFatal("Failed to create test analysis tree");
    }
    return std::make_shared<streamview::core::AnalysisTree>(std::move(*treeOpt));
}

} // namespace

class WindowDecoderTest : public QObject {
    Q_OBJECT

private slots:
    void decodesCompletePageSuccessfully();
    void decodesPartialPageWhenEntriesLessThanPageSize();
    void clampsEntryCountWhenLargerThanRegionCapacity();
    void handlesOutOfRangeOrOverflowPageIndexAndPageSize();
    void rejectsInvalidDefinition();
    void decodesDiscontiguousMultiSpanSourceMapping();
    void exhaustsSharedBudgetAndReturnsResourceLimit();
    void abortsAndReturnsCancelledOnCancellationToken();
    void returnsSourceErrorOnSourceReadFailure();
};

void WindowDecoderTest::decodesCompletePageSuccessfully() {
    const QString dsl = QStringLiteral(
        "struct SampleEntry {\n"
        "    bits<32> entry_size;\n"
        "}\n"
        "struct SampleTable {\n"
        "    bits<32> entry_count;\n"
        "    computed<u64> table_bytes = entry_count * 4;\n"
        "    @lazy(table_bytes) bytes entries @window(SampleEntry, entry_count);\n"
        "}\n"
        "entry SampleTable;\n");
    const auto program = compileProgram(dsl);

    // Create 256 entries of 4 bytes each = 1024 bytes payload + 4 bytes header = 1028 bytes
    std::vector<std::byte> raw(1028);
    // Write entry_count = 256
    raw[0] = std::byte{0}; raw[1] = std::byte{0}; raw[2] = std::byte{1}; raw[3] = std::byte{0};
    for (quint32 i = 0; i < 256; ++i) {
        const quint32 val = (i + 1) * 10;
        raw[4 + i * 4 + 0] = static_cast<std::byte>((val >> 24) & 0xFF);
        raw[4 + i * 4 + 1] = static_cast<std::byte>((val >> 16) & 0xFF);
        raw[4 + i * 4 + 2] = static_cast<std::byte>((val >> 8) & 0xFF);
        raw[4 + i * 4 + 3] = static_cast<std::byte>(val & 0xFF);
    }

    MemorySource source(raw);
    const auto span = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(0), 1028 * 8U);
    const auto mapping = *streamview::core::SourceMapping::create(streamview::core::LogicalViewId(1), {span});
    streamview::core::BitReader reader(source, mapping);

    auto tree = makeTestTree();
    auto execResult = streamview::rules::DslExecutor::decodeStruct(
        program, 1 /* SampleTable struct index */, reader, mapping, 0, *tree, tree->rootId());
    QVERIFY2(execResult.structureNode.has_value(), qUtf8Printable(execResult.errorMessage));
    QCOMPARE(execResult.status, streamview::rules::DslExecutionStatus::Materialized);

    // Find the lazy window node
    auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto rootChildren = rootNode->children();
    QCOMPARE(rootChildren.size(), 1);

    auto tableNode = tree->node(rootChildren.front());
    QVERIFY(tableNode.has_value());
    const auto tableChildren = tableNode->children();
    QCOMPARE(tableChildren.size(), 3);

    const auto windowNodeId = tableChildren.back();
    const auto node = tree->node(windowNodeId);
    QVERIFY(node.has_value());
    QVERIFY(node->metadata().window.has_value());
    QCOMPARE(node->metadata().window->entryCount, 256ULL);

    auto budget = std::make_shared<streamview::rules::RunnerExecutionBudget>();
    streamview::rules::WindowDecoder decoder(program, source, mapping, tree, windowNodeId, budget);

    streamview::rules::WindowDecodeRequest req;
    req.pageIndex = 0;
    req.pageSize = 256;
    auto res = decoder.decodeWindow(req);

    QCOMPARE(res.status, streamview::rules::DslExecutionStatus::Materialized);
    QCOMPARE(res.decodedEntryCount, 256ULL);
    QCOMPARE(res.entryNodes.size(), 256);
    QVERIFY(budget->remainingNodes < 100'000);
}

void WindowDecoderTest::decodesPartialPageWhenEntriesLessThanPageSize() {
    const QString dsl = QStringLiteral(
        "struct SampleEntry {\n"
        "    bits<32> entry_size;\n"
        "}\n"
        "struct SampleTable {\n"
        "    bits<32> entry_count;\n"
        "    computed<u64> table_bytes = entry_count * 4;\n"
        "    @lazy(table_bytes) bytes entries @window(SampleEntry, entry_count);\n"
        "}\n"
        "entry SampleTable;\n");
    const auto program = compileProgram(dsl);

    // 10 entries = 40 bytes payload + 4 bytes header = 44 bytes
    std::vector<std::byte> raw(44);
    raw[0] = std::byte{0}; raw[1] = std::byte{0}; raw[2] = std::byte{0}; raw[3] = std::byte{10};
    for (quint32 i = 0; i < 10; ++i) {
        raw[4 + i * 4 + 3] = static_cast<std::byte>(i + 1);
    }

    MemorySource source(raw);
    const auto span = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(0), 44 * 8U);
    const auto mapping = *streamview::core::SourceMapping::create(streamview::core::LogicalViewId(1), {span});
    streamview::core::BitReader reader(source, mapping);

    auto tree = makeTestTree();
    auto execResult = streamview::rules::DslExecutor::decodeStruct(
        program, 1, reader, mapping, 0, *tree, tree->rootId());
    QVERIFY2(execResult.structureNode.has_value(), qUtf8Printable(execResult.errorMessage));
    QCOMPARE(execResult.status, streamview::rules::DslExecutionStatus::Materialized);

    auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto rootChildren = rootNode->children();
    auto tableNode = tree->node(rootChildren.front());
    QVERIFY(tableNode.has_value());
    const auto tableChildren = tableNode->children();
    const auto windowNodeId = tableChildren.back();

    auto budget = std::make_shared<streamview::rules::RunnerExecutionBudget>();
    streamview::rules::WindowDecoder decoder(program, source, mapping, tree, windowNodeId, budget);

    streamview::rules::WindowDecodeRequest req;
    req.pageIndex = 0;
    req.pageSize = 256;
    auto res = decoder.decodeWindow(req);

    QCOMPARE(res.status, streamview::rules::DslExecutionStatus::Materialized);
    QCOMPARE(res.decodedEntryCount, 10ULL);
    QCOMPARE(res.entryNodes.size(), 10);
}

void WindowDecoderTest::clampsEntryCountWhenLargerThanRegionCapacity() {
    const QString dsl = QStringLiteral(
        "struct SampleEntry {\n"
        "    bits<32> entry_size;\n"
        "}\n"
        "struct SampleTable {\n"
        "    bits<32> entry_count;\n"
        "    computed<u64> table_bytes = 20;\n" // Only 20 bytes available (5 entries)
        "    @lazy(table_bytes) bytes entries @window(SampleEntry, entry_count);\n"
        "}\n"
        "entry SampleTable;\n");
    const auto program = compileProgram(dsl);

    // Header 4 bytes + 20 bytes = 24 bytes, but entry_count declares 100
    std::vector<std::byte> raw(24);
    raw[0] = std::byte{0}; raw[1] = std::byte{0}; raw[2] = std::byte{0}; raw[3] = std::byte{100};

    MemorySource source(raw);
    const auto span = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(0), 24 * 8U);
    const auto mapping = *streamview::core::SourceMapping::create(streamview::core::LogicalViewId(1), {span});
    streamview::core::BitReader reader(source, mapping);

    auto tree = makeTestTree();
    auto execResult = streamview::rules::DslExecutor::decodeStruct(
        program, 1, reader, mapping, 0, *tree, tree->rootId());
    QVERIFY2(execResult.structureNode.has_value(), qUtf8Printable(execResult.errorMessage));
    QCOMPARE(execResult.status, streamview::rules::DslExecutionStatus::Materialized);

    auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto rootChildren = rootNode->children();
    auto tableNode = tree->node(rootChildren.front());
    QVERIFY(tableNode.has_value());
    const auto tableChildren = tableNode->children();
    const auto windowNodeId = tableChildren.back();

    auto budget = std::make_shared<streamview::rules::RunnerExecutionBudget>();
    streamview::rules::WindowDecoder decoder(program, source, mapping, tree, windowNodeId, budget);

    streamview::rules::WindowDecodeRequest req;
    req.pageIndex = 0;
    req.pageSize = 256;
    auto res = decoder.decodeWindow(req);

    // Clamped to 5 entries, returns TruncatedSource because requested < entryCount
    QCOMPARE(res.status, streamview::rules::DslExecutionStatus::TruncatedSource);
    QCOMPARE(res.decodedEntryCount, 5ULL);
}

void WindowDecoderTest::handlesOutOfRangeOrOverflowPageIndexAndPageSize() {
    const QString dsl = QStringLiteral(
        "struct SampleEntry {\n"
        "    bits<32> entry_size;\n"
        "}\n"
        "struct SampleTable {\n"
        "    bits<32> entry_count;\n"
        "    computed<u64> table_bytes = entry_count * 4;\n"
        "    @lazy(table_bytes) bytes entries @window(SampleEntry, entry_count);\n"
        "}\n"
        "entry SampleTable;\n");
    const auto program = compileProgram(dsl);

    std::vector<std::byte> raw(24);
    raw[0] = std::byte{0}; raw[1] = std::byte{0}; raw[2] = std::byte{0}; raw[3] = std::byte{5};

    MemorySource source(raw);
    const auto span = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(0), 24 * 8U);
    const auto mapping = *streamview::core::SourceMapping::create(streamview::core::LogicalViewId(1), {span});
    streamview::core::BitReader reader(source, mapping);

    auto tree = makeTestTree();
    auto execResult = streamview::rules::DslExecutor::decodeStruct(
        program, 1, reader, mapping, 0, *tree, tree->rootId());
    QVERIFY2(execResult.structureNode.has_value(), qUtf8Printable(execResult.errorMessage));
    QCOMPARE(execResult.status, streamview::rules::DslExecutionStatus::Materialized);

    auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto rootChildren = rootNode->children();
    auto tableNode = tree->node(rootChildren.front());
    QVERIFY(tableNode.has_value());
    const auto tableChildren = tableNode->children();
    const auto windowNodeId = tableChildren.back();

    auto budget = std::make_shared<streamview::rules::RunnerExecutionBudget>();
    streamview::rules::WindowDecoder decoder(program, source, mapping, tree, windowNodeId, budget);

    // Overflow pageIndex * pageSize
    streamview::rules::WindowDecodeRequest req1;
    req1.pageIndex = std::numeric_limits<quint64>::max();
    req1.pageSize = std::numeric_limits<quint64>::max();
    auto res1 = decoder.decodeWindow(req1);
    QCOMPARE(res1.status, streamview::rules::DslExecutionStatus::TruncatedSource);
    QCOMPARE(res1.decodedEntryCount, 0ULL);

    // Zero pageSize
    streamview::rules::WindowDecodeRequest req2;
    req2.pageIndex = 0;
    req2.pageSize = 0;
    auto res2 = decoder.decodeWindow(req2);
    QCOMPARE(res2.status, streamview::rules::DslExecutionStatus::InvalidDefinition);

    // pageIndex beyond entryCount
    streamview::rules::WindowDecodeRequest req3;
    req3.pageIndex = 100;
    req3.pageSize = 256;
    auto res3 = decoder.decodeWindow(req3);
    QCOMPARE(res3.status, streamview::rules::DslExecutionStatus::TruncatedSource);
    QCOMPARE(res3.decodedEntryCount, 0ULL);
}

void WindowDecoderTest::rejectsInvalidDefinition() {
    const QString dsl = QStringLiteral(
        "struct Simple {\n"
        "    bits<32> val;\n"
        "}\n"
        "entry Simple;\n");
    const auto program = compileProgram(dsl);

    std::vector<std::byte> raw(4);
    MemorySource source(raw);
    const auto span = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(0), 4 * 8U);
    const auto mapping = *streamview::core::SourceMapping::create(streamview::core::LogicalViewId(1), {span});
    streamview::core::BitReader reader(source, mapping);

    auto tree = makeTestTree();
    auto execResult = streamview::rules::DslExecutor::decodeStruct(program, 0, reader, mapping, 0, *tree, tree->rootId());
    QVERIFY(execResult.structureNode.has_value());

    auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto rootChildren = rootNode->children();
    const auto simpleNodeId = rootChildren.front();

    auto budget = std::make_shared<streamview::rules::RunnerExecutionBudget>();
    // simpleNodeId has no window metadata
    streamview::rules::WindowDecoder decoder(program, source, mapping, tree, simpleNodeId, budget);

    streamview::rules::WindowDecodeRequest req;
    req.pageIndex = 0;
    req.pageSize = 256;
    auto res = decoder.decodeWindow(req);
    QCOMPARE(res.status, streamview::rules::DslExecutionStatus::InvalidDefinition);
}

void WindowDecoderTest::decodesDiscontiguousMultiSpanSourceMapping() {
    const QString dsl = QStringLiteral(
        "struct SampleEntry {\n"
        "    bits<32> entry_size;\n"
        "}\n"
        "struct SampleTable {\n"
        "    bits<32> entry_count;\n"
        "    computed<u64> table_bytes = entry_count * 4;\n"
        "    @lazy(table_bytes) bytes entries @window(SampleEntry, entry_count);\n"
        "}\n"
        "entry SampleTable;\n");
    const auto program = compileProgram(dsl);

    std::vector<std::byte> raw(100);
    raw[10] = std::byte{0}; raw[11] = std::byte{0}; raw[12] = std::byte{0}; raw[13] = std::byte{2};
    raw[20] = std::byte{0}; raw[21] = std::byte{0}; raw[22] = std::byte{0}; raw[23] = std::byte{111};
    raw[40] = std::byte{0}; raw[41] = std::byte{0}; raw[42] = std::byte{0}; raw[43] = std::byte{222};

    MemorySource source(raw);
    const auto span1 = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(10 * 8U), 4 * 8U);
    const auto span2 = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(20 * 8U), 4 * 8U);
    const auto span3 = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(40 * 8U), 4 * 8U);
    const auto mapping = *streamview::core::SourceMapping::create(
        streamview::core::LogicalViewId(1), {span1, span2, span3});
    streamview::core::BitReader reader(source, mapping);

    auto tree = makeTestTree();
    auto execResult = streamview::rules::DslExecutor::decodeStruct(
        program, 1, reader, mapping, 0, *tree, tree->rootId());
    QVERIFY2(execResult.structureNode.has_value(), qUtf8Printable(execResult.errorMessage));
    QCOMPARE(execResult.status, streamview::rules::DslExecutionStatus::Materialized);

    auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto rootChildren = rootNode->children();
    auto tableNode = tree->node(rootChildren.front());
    QVERIFY(tableNode.has_value());
    const auto tableChildren = tableNode->children();
    const auto windowNodeId = tableChildren.back();

    auto budget = std::make_shared<streamview::rules::RunnerExecutionBudget>();
    streamview::rules::WindowDecoder decoder(program, source, mapping, tree, windowNodeId, budget);

    streamview::rules::WindowDecodeRequest req;
    req.pageIndex = 0;
    req.pageSize = 256;
    auto res = decoder.decodeWindow(req);

    QCOMPARE(res.status, streamview::rules::DslExecutionStatus::Materialized);
    QCOMPARE(res.decodedEntryCount, 2ULL);
}

void WindowDecoderTest::exhaustsSharedBudgetAndReturnsResourceLimit() {
    const QString dsl = QStringLiteral(
        "struct SampleEntry {\n"
        "    bits<32> entry_size;\n"
        "}\n"
        "struct SampleTable {\n"
        "    bits<32> entry_count;\n"
        "    computed<u64> table_bytes = entry_count * 4;\n"
        "    @lazy(table_bytes) bytes entries @window(SampleEntry, entry_count);\n"
        "}\n"
        "entry SampleTable;\n");
    const auto program = compileProgram(dsl);

    std::vector<std::byte> raw(44);
    raw[0] = std::byte{0}; raw[1] = std::byte{0}; raw[2] = std::byte{0}; raw[3] = std::byte{10};

    MemorySource source(raw);
    const auto span = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(0), 44 * 8U);
    const auto mapping = *streamview::core::SourceMapping::create(streamview::core::LogicalViewId(1), {span});
    streamview::core::BitReader reader(source, mapping);

    auto tree = makeTestTree();
    auto execResult = streamview::rules::DslExecutor::decodeStruct(program, 1, reader, mapping, 0, *tree, tree->rootId());
    QVERIFY2(execResult.structureNode.has_value(), qUtf8Printable(execResult.errorMessage));

    auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto rootChildren = rootNode->children();
    auto tableNode = tree->node(rootChildren.front());
    QVERIFY(tableNode.has_value());
    const auto tableChildren = tableNode->children();
    const auto windowNodeId = tableChildren.back();

    auto budget = std::make_shared<streamview::rules::RunnerExecutionBudget>();
    budget->remainingNodes = 3;

    streamview::rules::WindowDecoder decoder(program, source, mapping, tree, windowNodeId, budget);

    streamview::rules::WindowDecodeRequest req;
    req.pageIndex = 0;
    req.pageSize = 256;
    auto res = decoder.decodeWindow(req);

    QCOMPARE(res.status, streamview::rules::DslExecutionStatus::ResourceLimit);
}

void WindowDecoderTest::abortsAndReturnsCancelledOnCancellationToken() {
    const QString dsl = QStringLiteral(
        "struct SampleEntry {\n"
        "    bits<32> entry_size;\n"
        "}\n"
        "struct SampleTable {\n"
        "    bits<32> entry_count;\n"
        "    computed<u64> table_bytes = entry_count * 4;\n"
        "    @lazy(table_bytes) bytes entries @window(SampleEntry, entry_count);\n"
        "}\n"
        "entry SampleTable;\n");
    const auto program = compileProgram(dsl);

    std::vector<std::byte> raw(44);
    raw[0] = std::byte{0}; raw[1] = std::byte{0}; raw[2] = std::byte{0}; raw[3] = std::byte{10};

    MemorySource source(raw);
    const auto span = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(0), 44 * 8U);
    const auto mapping = *streamview::core::SourceMapping::create(streamview::core::LogicalViewId(1), {span});
    streamview::core::BitReader reader(source, mapping);

    auto tree = makeTestTree();
    auto execResult = streamview::rules::DslExecutor::decodeStruct(program, 1, reader, mapping, 0, *tree, tree->rootId());
    QVERIFY2(execResult.structureNode.has_value(), qUtf8Printable(execResult.errorMessage));

    auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto rootChildren = rootNode->children();
    auto tableNode = tree->node(rootChildren.front());
    QVERIFY(tableNode.has_value());
    const auto tableChildren = tableNode->children();
    const auto windowNodeId = tableChildren.back();

    streamview::core::CancellationSource cancelSource;
    (void)cancelSource.requestCancellation();

    auto budget = std::make_shared<streamview::rules::RunnerExecutionBudget>();
    streamview::rules::WindowDecoder decoder(
        program, source, mapping, tree, windowNodeId, budget, cancelSource.token());

    streamview::rules::WindowDecodeRequest req;
    req.pageIndex = 0;
    req.pageSize = 256;
    auto res = decoder.decodeWindow(req);

    QCOMPARE(res.status, streamview::rules::DslExecutionStatus::Cancelled);
}

void WindowDecoderTest::returnsSourceErrorOnSourceReadFailure() {
    const QString dsl = QStringLiteral(
        "struct SampleEntry {\n"
        "    bits<32> entry_size;\n"
        "}\n"
        "struct SampleTable {\n"
        "    bits<32> entry_count;\n"
        "    computed<u64> table_bytes = entry_count * 4;\n"
        "    @lazy(table_bytes) bytes entries @window(SampleEntry, entry_count);\n"
        "}\n"
        "entry SampleTable;\n");
    const auto program = compileProgram(dsl);

    std::vector<std::byte> raw(44);
    raw[0] = std::byte{0}; raw[1] = std::byte{0}; raw[2] = std::byte{0}; raw[3] = std::byte{10};
    MemorySource initialSource(raw);
    const auto span = *streamview::core::SourceSpan::create(streamview::core::SourceBitAddress(0), 44 * 8U);
    const auto mapping = *streamview::core::SourceMapping::create(streamview::core::LogicalViewId(1), {span});
    streamview::core::BitReader reader(initialSource, mapping);

    auto tree = makeTestTree();
    auto execResult = streamview::rules::DslExecutor::decodeStruct(program, 1, reader, mapping, 0, *tree, tree->rootId());
    QVERIFY2(execResult.structureNode.has_value(), qUtf8Printable(execResult.errorMessage));

    auto rootNode = tree->node(tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto rootChildren = rootNode->children();
    auto tableNode = tree->node(rootChildren.front());
    QVERIFY(tableNode.has_value());
    const auto tableChildren = tableNode->children();
    const auto windowNodeId = tableChildren.back();

    FailingSource failingSource(44);
    auto budget = std::make_shared<streamview::rules::RunnerExecutionBudget>();
    streamview::rules::WindowDecoder decoder(program, failingSource, mapping, tree, windowNodeId, budget);

    streamview::rules::WindowDecodeRequest req;
    req.pageIndex = 0;
    req.pageSize = 256;
    auto res = decoder.decodeWindow(req);

    QCOMPARE(res.status, streamview::rules::DslExecutionStatus::SourceError);
}

QTEST_MAIN(WindowDecoderTest)
#include "window_decoder_test.moc"
