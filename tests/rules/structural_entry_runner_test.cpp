#include <streamview/rules/structural_entry_runner.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/bounded_source_view.h>
#include <streamview/core/cancellation.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_ir.h>

#include <QFile>
#include <QString>
#include <QStringList>
#include <QtTest>

#include <memory>
#include <vector>

using namespace streamview::core;
using namespace streamview::rules;

namespace {

[[nodiscard]] std::vector<std::byte> toBytes(std::initializer_list<quint8> list) {
    std::vector<std::byte> result;
    result.reserve(list.size());
    for (const quint8 b : list) {
        result.push_back(static_cast<std::byte>(b));
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> toBytes(const std::vector<quint8>& list) {
    std::vector<std::byte> result;
    result.reserve(list.size());
    for (const quint8 b : list) {
        result.push_back(static_cast<std::byte>(b));
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
        const auto count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.data() + offset, count, destination.data());
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
};

class FaultySource final : public RandomAccessSource {
public:
    FaultySource(std::vector<std::byte> data, quint64 failAtOffset)
        : data_(std::move(data)), failAtOffset_(failAtOffset) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return data_.size(); }
    [[nodiscard]] QString identity() const override { return QStringLiteral("faulty-source"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= sizeBytes()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const quint64 available = sizeBytes() - byteOffset;
        const std::size_t count = static_cast<std::size_t>(
            std::min(static_cast<quint64>(destination.size()), available));
        if (byteOffset + static_cast<quint64>(count) > failAtOffset_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("Injected I/O failure")};
        }
        for (std::size_t i = 0; i < count; ++i) {
            destination[i] = data_[static_cast<std::size_t>(byteOffset + i)];
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
    quint64 failAtOffset_ = 0;
};

enum class MalformedReadMode {
    IncompleteSuccess,
    OversizedSuccess,
};

class MalformedSource final : public RandomAccessSource {
public:
    explicit MalformedSource(MalformedReadMode mode) : mode_(mode) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return 2; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("malformed"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (mode_ == MalformedReadMode::OversizedSuccess) {
            return {SourceReadStatus::Complete, destination.size() + 1, {}};
        }
        return {SourceReadStatus::Complete, destination.size() - 1, {}};
    }

private:
    MalformedReadMode mode_;
};

[[nodiscard]] std::vector<std::byte> readFixtureBytes(const QString& relativePath) {
    const QString basePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/");
    QFile file(basePath + relativePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = file.readAll();
    const auto uSize = static_cast<std::size_t>(data.size());
    std::vector<std::byte> bytes(uSize);
    std::memcpy(bytes.data(), data.constData(), uSize);
    return bytes;
}

} // namespace

class StructuralEntryRunnerTest : public QObject {
    Q_OBJECT

private slots:
    void boundedSourceViewSingleSpan();
    void boundedSourceViewDisjointSpans();
    void boundedSourceViewOutOfBoundsAndEof();
    void boundedSourceViewPropagatesSourceError();
    void boundedSourceViewRejectsMalformedSuccessfulReads();
    void boundedSourceViewRejectsUnalignedSpans();

    void rejectsSequenceEntryKind();
    void rejectsOutOfRangeTargetIndex();
    void rejectsZeroOrUnalignedLogicalLength();
    void rejectsUnalignedSourceSpans();
    void executesLocalMinimalStructSuccess();
    void executesAcrossDisjointPhysicalSpansAndMapsFieldLocations();
    void handlesTruncatedSourceWithPartialTree();
    void handlesUnderlyingSourceError();
    void handlesCancellation();
    void handlesResourceLimits();
    void executesOfficialAacAscOnEsdsFixture();
};

void StructuralEntryRunnerTest::boundedSourceViewSingleSpan() {
    MemorySource source(toBytes({0x10, 0x20, 0x30, 0x40, 0x50}));
    const auto span = SourceSpan::create(SourceBitAddress(8), 24); // bytes [1, 4) -> 0x20, 0x30, 0x40
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    BoundedSourceView view(source, *mapping, 3);
    QCOMPARE(view.sizeBytes(), quint64{3});
    QCOMPARE(view.identity(), source.identity());

    std::array<std::byte, 3> dest{};
    const auto readRes = view.readAt(0, dest);
    QCOMPARE(readRes.status, SourceReadStatus::Complete);
    QCOMPARE(readRes.bytesRead, std::size_t{3});
    QCOMPARE(static_cast<quint8>(dest[0]), 0x20);
    QCOMPARE(static_cast<quint8>(dest[1]), 0x30);
    QCOMPARE(static_cast<quint8>(dest[2]), 0x40);
}

void StructuralEntryRunnerTest::boundedSourceViewDisjointSpans() {
    // baseSource: 100 bytes of data
    std::vector<quint8> rawData(100, 0);
    rawData[10] = 0xAA;
    rawData[11] = 0xBB;
    rawData[50] = 0xCC;
    rawData[51] = 0xDD;
    MemorySource source(toBytes(rawData));

    const auto span1 = SourceSpan::create(SourceBitAddress(80), 16);  // bytes [10, 12) -> 0xAA, 0xBB
    const auto span2 = SourceSpan::create(SourceBitAddress(400), 16); // bytes [50, 52) -> 0xCC, 0xDD
    QVERIFY(span1.has_value());
    QVERIFY(span2.has_value());

    const auto mapping = SourceMapping::create(LogicalViewId(2), {*span1, *span2});
    QVERIFY(mapping.has_value());
    QCOMPARE(mapping->logicalBitLength(), quint64{32}); // 4 bytes total

    BoundedSourceView view(source, *mapping, 4);
    QCOMPARE(view.sizeBytes(), quint64{4});

    // Read across disjoint span boundary (e.g. 3 bytes starting from logical offset 1)
    std::array<std::byte, 3> dest{};
    const auto readRes = view.readAt(1, dest);
    QCOMPARE(readRes.status, SourceReadStatus::Complete);
    QCOMPARE(readRes.bytesRead, std::size_t{3});
    QCOMPARE(static_cast<quint8>(dest[0]), 0xBB); // logical byte 1 (span1 byte 1)
    QCOMPARE(static_cast<quint8>(dest[1]), 0xCC); // logical byte 2 (span2 byte 0)
    QCOMPARE(static_cast<quint8>(dest[2]), 0xDD); // logical byte 3 (span2 byte 1)
}

void StructuralEntryRunnerTest::boundedSourceViewOutOfBoundsAndEof() {
    MemorySource source(toBytes({0x01, 0x02, 0x03, 0x04}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 16); // 2 bytes
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(3), {*span});
    QVERIFY(mapping.has_value());

    BoundedSourceView view(source, *mapping, 2);

    // Read past sizeBytes
    std::array<std::byte, 2> dest{};
    const auto eofRes = view.readAt(2, dest);
    QCOMPARE(eofRes.status, SourceReadStatus::EndOfSource);
    QCOMPARE(eofRes.bytesRead, std::size_t{0});

    // Partial read at boundary
    const auto partialRes = view.readAt(1, dest);
    QCOMPARE(partialRes.status, SourceReadStatus::EndOfSource);
    QCOMPARE(partialRes.bytesRead, std::size_t{1});
    QCOMPARE(static_cast<quint8>(dest[0]), 0x02);
}

void StructuralEntryRunnerTest::boundedSourceViewPropagatesSourceError() {
    std::vector<quint8> rawData(100, 0);
    rawData[10] = 0xAA;
    rawData[11] = 0xBB;
    FaultySource source(toBytes(rawData), 50); // Fails while reading the second mapped span.
    const auto firstSpan = SourceSpan::create(SourceBitAddress(80), 16);
    const auto secondSpan = SourceSpan::create(SourceBitAddress(400), 16);
    QVERIFY(firstSpan.has_value());
    QVERIFY(secondSpan.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(4), {*firstSpan, *secondSpan});
    QVERIFY(mapping.has_value());

    BoundedSourceView view(source, *mapping, 4);
    std::array<std::byte, 4> dest{};
    const auto readRes = view.readAt(0, dest);
    QCOMPARE(readRes.status, SourceReadStatus::Error);
    QCOMPARE(readRes.bytesRead, std::size_t{2});
    QCOMPARE(readRes.errorMessage, QStringLiteral("Injected I/O failure"));
}

void StructuralEntryRunnerTest::boundedSourceViewRejectsMalformedSuccessfulReads() {
    const auto span = SourceSpan::create(SourceBitAddress(0), 16);
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(5), {*span});
    QVERIFY(mapping.has_value());
    std::array<std::byte, 2> destination{};

    MalformedSource incompleteSource(MalformedReadMode::IncompleteSuccess);
    BoundedSourceView incompleteView(incompleteSource, *mapping, 2);
    const auto incompleteResult = incompleteView.readAt(0, destination);
    QCOMPARE(incompleteResult.status, SourceReadStatus::Error);
    QCOMPARE(incompleteResult.bytesRead, std::size_t{1});
    QCOMPARE(incompleteResult.errorMessage,
             QStringLiteral("Bounded source view received an incomplete successful read"));

    MalformedSource oversizedSource(MalformedReadMode::OversizedSuccess);
    BoundedSourceView oversizedView(oversizedSource, *mapping, 2);
    const auto oversizedResult = oversizedView.readAt(0, destination);
    QCOMPARE(oversizedResult.status, SourceReadStatus::Error);
    QCOMPARE(oversizedResult.bytesRead, std::size_t{0});
    QCOMPARE(oversizedResult.errorMessage,
             QStringLiteral("Bounded source view received an oversized read"));
}

void StructuralEntryRunnerTest::boundedSourceViewRejectsUnalignedSpans() {
    MemorySource source(toBytes({0xFF, 0xFF}));
    // Non-byte-aligned span (bit start 3)
    const auto span = SourceSpan::create(SourceBitAddress(3), 8);
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(5), {*span});
    QVERIFY(mapping.has_value());

    BoundedSourceView view(source, *mapping, 1);
    std::array<std::byte, 1> dest{};
    const auto readRes = view.readAt(0, dest);
    QCOMPARE(readRes.status, SourceReadStatus::Error);
    QCOMPARE(readRes.errorMessage, QStringLiteral("Bounded source view span is not byte-aligned"));
}

void StructuralEntryRunnerTest::rejectsSequenceEntryKind() {
    const auto parsed = DslParser::parse(
        QStringLiteral("struct Header { bits<8> a; } @index(progressive) sequence<Header> items = scan(h264_start_code); entry items;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    MemorySource source(toBytes({0x01, 0x02}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 16);
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(result.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(result.execution.errorMessage,
             QStringLiteral("Structural entry runner requires DslEntryKind::Structure"));
    QVERIFY(!result.succeeded());
    QVERIFY(result.tree == nullptr);
}

void StructuralEntryRunnerTest::rejectsOutOfRangeTargetIndex() {
    const auto parsed = DslParser::parse(QStringLiteral("struct Header { bits<8> a; } entry Header;"));
    QVERIFY(parsed.succeeded());
    auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    // Corrupt targetIndex
    compiled.program->entry.targetIndex = 999;

    MemorySource source(toBytes({0x01}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 8);
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(result.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(result.execution.errorMessage,
             QStringLiteral("Structural entry targetIndex is out of range"));
}

void StructuralEntryRunnerTest::rejectsZeroOrUnalignedLogicalLength() {
    const auto parsed = DslParser::parse(QStringLiteral("struct Header { bits<8> a; } entry Header;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    MemorySource source(toBytes({0x01}));

    const auto zeroMapping = SourceMapping::create(LogicalViewId(1), {});
    QVERIFY(zeroMapping.has_value());
    const auto zeroResult = StructuralEntryRunner::execute(
        source, *zeroMapping, *compiled.program);
    QCOMPARE(zeroResult.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(zeroResult.execution.errorMessage,
             QStringLiteral("Source mapping logical length is zero"));
    QVERIFY(zeroResult.tree == nullptr);

    // Non-byte-aligned logical length (7 bits)
    const auto unalignedSpan = SourceSpan::create(SourceBitAddress(0), 7);
    const auto unalignedMapping = SourceMapping::create(LogicalViewId(1), {*unalignedSpan});
    QVERIFY(unalignedMapping.has_value());

    const auto unalignedRes = StructuralEntryRunner::execute(source, *unalignedMapping, *compiled.program);
    QCOMPARE(unalignedRes.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(unalignedRes.execution.errorMessage,
             QStringLiteral("Source mapping logical length is not byte-aligned"));
}

void StructuralEntryRunnerTest::rejectsUnalignedSourceSpans() {
    const auto parsed = DslParser::parse(QStringLiteral("struct Header { bits<8> a; } entry Header;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    MemorySource source(toBytes({0xFF, 0xFF}));

    // Unaligned start bit (bit 3, 8 bits long)
    const auto span = SourceSpan::create(SourceBitAddress(3), 8);
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(1), {*span});
    QVERIFY(mapping.has_value());

    const auto res = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(res.execution.status, DslExecutionStatus::InvalidDefinition);
    QCOMPARE(res.execution.errorMessage,
             QStringLiteral("Source mapping spans must be byte-aligned"));
}

void StructuralEntryRunnerTest::executesLocalMinimalStructSuccess() {
    const auto parsed = DslParser::parse(
        QStringLiteral("struct Minimal { bits<8> a; bits<16> b; } entry Minimal;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    MemorySource source(toBytes({0x12, 0x34, 0x56}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 24); // 3 bytes
    QVERIFY(span.has_value());
    const auto mapping = SourceMapping::create(LogicalViewId(10), {*span});
    QVERIFY(mapping.has_value());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QVERIFY(result.succeeded());
    QCOMPARE(result.execution.status, DslExecutionStatus::Materialized);
    QVERIFY(result.tree != nullptr);

    // Root region contains the decoded structure node
    const auto rootNode = result.tree->node(result.tree->rootId());
    QVERIFY(rootNode.has_value());
    QCOMPARE(rootNode->children().size(), std::size_t{1});

    const auto structNode = result.tree->node(rootNode->children().front());
    QVERIFY(structNode.has_value());
    QCOMPARE(structNode->name(), QStringLiteral("Minimal"));
    QCOMPARE(structNode->children().size(), std::size_t{2});

    const auto fieldA = result.tree->node(structNode->children()[0]);
    QVERIFY(fieldA.has_value());
    QCOMPARE(fieldA->name(), QStringLiteral("a"));
    QCOMPARE(fieldA->value().toULongLong(), quint64{0x12});
    QVERIFY(fieldA->location().has_value());
    QCOMPARE(fieldA->location()->logicalRange().start().bitOffset(), quint64{0});
    QCOMPARE(fieldA->location()->logicalRange().bitLength(), quint64{8});
    QCOMPARE(fieldA->location()->sourceSpans().size(), std::size_t{1});
    QCOMPARE(fieldA->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{0});
    QCOMPARE(fieldA->location()->sourceSpans().front().bitLength(), quint64{8});

    const auto fieldB = result.tree->node(structNode->children()[1]);
    QVERIFY(fieldB.has_value());
    QCOMPARE(fieldB->name(), QStringLiteral("b"));
    QCOMPARE(fieldB->value().toULongLong(), quint64{0x3456});
    QVERIFY(fieldB->location().has_value());
    QCOMPARE(fieldB->location()->logicalRange().start().bitOffset(), quint64{8});
    QCOMPARE(fieldB->location()->logicalRange().bitLength(), quint64{16});
    QCOMPARE(fieldB->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{8});
    QCOMPARE(fieldB->location()->sourceSpans().front().bitLength(), quint64{16});
}

void StructuralEntryRunnerTest::executesAcrossDisjointPhysicalSpansAndMapsFieldLocations() {
    // Root source with data at disjoint offsets:
    // Offset 100 (bits 800..808): 0xAA (field 'first')
    // Offset 200 (bits 1600..1616): 0xBB, 0xCC (field 'second')
    std::vector<quint8> rootData(300, 0);
    rootData[100] = 0xAA;
    rootData[200] = 0xBB;
    rootData[201] = 0xCC;
    MemorySource source(toBytes(rootData));

    const auto span1 = SourceSpan::create(SourceBitAddress(800), 8);   // byte 100
    const auto span2 = SourceSpan::create(SourceBitAddress(1600), 16); // bytes 200..201
    QVERIFY(span1.has_value());
    QVERIFY(span2.has_value());

    const auto mapping = SourceMapping::create(LogicalViewId(20), {*span1, *span2});
    QVERIFY(mapping.has_value());
    QCOMPARE(mapping->logicalBitLength(), quint64{24});

    const auto parsed = DslParser::parse(
        QStringLiteral("struct Disjoint { bits<8> first; bits<16> second; } entry Disjoint;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QVERIFY(result.succeeded());
    QCOMPARE(result.execution.status, DslExecutionStatus::Materialized);

    const auto rootNode = result.tree->node(result.tree->rootId());
    const auto structNode = result.tree->node(rootNode->children().front());
    QCOMPARE(structNode->name(), QStringLiteral("Disjoint"));

    // field 'first' mapped to root source bits [800, 808)
    const auto field1 = result.tree->node(structNode->children()[0]);
    QCOMPARE(field1->name(), QStringLiteral("first"));
    QCOMPARE(field1->value().toULongLong(), quint64{0xAA});
    QVERIFY(field1->location().has_value());
    QCOMPARE(field1->location()->sourceSpans().size(), std::size_t{1});
    QCOMPARE(field1->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{800});
    QCOMPARE(field1->location()->sourceSpans().front().bitLength(), quint64{8});

    // field 'second' mapped to root source bits [1600, 1616)
    const auto field2 = result.tree->node(structNode->children()[1]);
    QCOMPARE(field2->name(), QStringLiteral("second"));
    QCOMPARE(field2->value().toULongLong(), quint64{0xBBCC});
    QVERIFY(field2->location().has_value());
    QCOMPARE(field2->location()->sourceSpans().size(), std::size_t{1});
    QCOMPARE(field2->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1600});
    QCOMPARE(field2->location()->sourceSpans().front().bitLength(), quint64{16});
}

void StructuralEntryRunnerTest::handlesTruncatedSourceWithPartialTree() {
    const auto parsed = DslParser::parse(
        QStringLiteral("struct Large { bits<16> first; bits<16> second; } entry Large;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    // Only 2 bytes provided (for 'first'), 'second' is truncated
    MemorySource source(toBytes({0x11, 0x22}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 16);
    const auto mapping = SourceMapping::create(LogicalViewId(30), {*span});
    QVERIFY(mapping.has_value());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(result.execution.status, DslExecutionStatus::TruncatedSource);
    QVERIFY(!result.succeeded());
    QVERIFY(result.tree != nullptr);

    // Partial nodes are preserved
    const auto rootNode = result.tree->node(result.tree->rootId());
    QVERIFY(rootNode.has_value());
    const auto structNode = result.tree->node(rootNode->children().front());
    QVERIFY(structNode.has_value());
    QCOMPARE(structNode->state(), MaterializationState::Invalid);
    QVERIFY(!structNode->diagnostics().empty());
    QCOMPARE(structNode->diagnostics().front().code, DiagnosticCode::TruncatedSource);
}

void StructuralEntryRunnerTest::handlesUnderlyingSourceError() {
    FaultySource source(toBytes({0x11, 0x22, 0x33, 0x44}), 2);
    const auto span = SourceSpan::create(SourceBitAddress(0), 32);
    const auto mapping = SourceMapping::create(LogicalViewId(40), {*span});
    QVERIFY(mapping.has_value());

    const auto parsed = DslParser::parse(
        QStringLiteral("struct FourBytes { bits<16> a; bits<16> b; } entry FourBytes;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program);
    QCOMPARE(result.execution.status, DslExecutionStatus::SourceError);
    QVERIFY(!result.succeeded());
}

void StructuralEntryRunnerTest::handlesCancellation() {
    MemorySource source(toBytes({0x11, 0x22, 0x33, 0x44}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 32);
    const auto mapping = SourceMapping::create(LogicalViewId(50), {*span});
    QVERIFY(mapping.has_value());

    const auto parsed = DslParser::parse(
        QStringLiteral("struct CancelTarget { bits<32> a; } entry CancelTarget;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    CancellationSource cancelSource;
    (void)cancelSource.requestCancellation();

    StructuralExecutionOptions options;
    options.cancellation = cancelSource.token();

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program, options);
    QCOMPARE(result.execution.status, DslExecutionStatus::Cancelled);
    QVERIFY(!result.succeeded());
}

void StructuralEntryRunnerTest::handlesResourceLimits() {
    MemorySource source(toBytes({0x01, 0x02, 0x03, 0x04}));
    const auto span = SourceSpan::create(SourceBitAddress(0), 32);
    const auto mapping = SourceMapping::create(LogicalViewId(60), {*span});
    QVERIFY(mapping.has_value());

    const auto parsed = DslParser::parse(
        QStringLiteral("struct MultiField { bits<8> a; bits<8> b; bits<8> c; bits<8> d; } entry MultiField;"));
    QVERIFY(parsed.succeeded());
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY(compiled.succeeded());

    StructuralExecutionOptions options;
    options.limits.maximumInstructions = 2; // Exceeded during execution

    const auto result = StructuralEntryRunner::execute(source, *mapping, *compiled.program, options);
    QCOMPARE(result.execution.status, DslExecutionStatus::ResourceLimit);
    QVERIFY(!result.succeeded());
}

void StructuralEntryRunnerTest::executesOfficialAacAscOnEsdsFixture() {
    // 1. Read real fixture mp4_p5h_mp4a_esds.mp4
    const auto fixtureBytes = readFixtureBytes(QStringLiteral("mp4_p5h_mp4a_esds.mp4"));
    QVERIFY(!fixtureBytes.empty());
    MemorySource rootSource(fixtureBytes);

    // In mp4_p5h_mp4a_esds.mp4, AudioSpecificConfig is 2 bytes at root byte offset 146 (0x12 0x10)
    // Bit address: 146 * 8 = 1168 bits. Bit length: 16 bits.
    const auto ascSpan = SourceSpan::create(SourceBitAddress(1168), 16);
    QVERIFY(ascSpan.has_value());
    const auto ascMapping = SourceMapping::create(LogicalViewId(100), {*ascSpan});
    QVERIFY(ascMapping.has_value());

    // 2. Read official org.streamview.aac aac_asc.svfmt rule
    const QString ascRulePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/src/rules/official/org.streamview.aac/src/aac_asc.svfmt");
    QFile ruleFile(ascRulePath);
    QVERIFY2(ruleFile.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(ruleFile.errorString()));
    const QString ruleSource = QString::fromUtf8(ruleFile.readAll());
    QVERIFY(!ruleSource.isEmpty());

    const auto parsed = DslParser::parse(ruleSource);
    QVERIFY2(parsed.succeeded(), "DSL parse failed");
    const auto compiled = DslCompiler::compile(parsed.program);
    QVERIFY2(compiled.succeeded(), "DSL compile failed");

    // 3. Execute StructuralEntryRunner on the official AudioSpecificConfig entrypoint
    const auto result = StructuralEntryRunner::execute(rootSource, *ascMapping, *compiled.program);
    QVERIFY2(result.succeeded(), qPrintable(result.execution.errorMessage));
    QCOMPARE(result.execution.status, DslExecutionStatus::Materialized);
    QVERIFY(result.tree != nullptr);

    // 4. Verify AST hierarchy and exact field values
    const auto rootNode = result.tree->node(result.tree->rootId());
    QVERIFY(rootNode.has_value());
    QCOMPARE(rootNode->children().size(), std::size_t{1});

    const auto ascNode = result.tree->node(rootNode->children().front());
    QVERIFY(ascNode.has_value());
    QCOMPARE(ascNode->name(), QStringLiteral("AudioSpecificConfig"));

    // Find and verify specific fields
    std::optional<AnalysisNode> audioObjectTypeNode;
    std::optional<AnalysisNode> samplingFreqIndexNode;
    std::optional<AnalysisNode> channelConfigNode;
    std::optional<AnalysisNode> frameLengthFlagNode;
    std::optional<AnalysisNode> dependsOnCoreCoderNode;
    std::optional<AnalysisNode> extensionFlagNode;
    QStringList childNames;

    for (const auto childId : ascNode->children()) {
        const auto child = result.tree->node(childId);
        if (!child) continue;
        childNames.push_back(child->name());
        if (child->name() == QStringLiteral("audio_object_type")) audioObjectTypeNode = child;
        else if (child->name() == QStringLiteral("sampling_frequency_index")) samplingFreqIndexNode = child;
        else if (child->name() == QStringLiteral("channel_configuration")) channelConfigNode = child;
        else if (child->name() == QStringLiteral("frame_length_flag")) frameLengthFlagNode = child;
        else if (child->name() == QStringLiteral("depends_on_core_coder")) dependsOnCoreCoderNode = child;
        else if (child->name() == QStringLiteral("extension_flag")) extensionFlagNode = child;
    }
    QCOMPARE(childNames,
             QStringList({QStringLiteral("audio_object_type"),
                          QStringLiteral("sampling_frequency_index"),
                          QStringLiteral("channel_configuration"),
                          QStringLiteral("frame_length_flag"),
                          QStringLiteral("depends_on_core_coder"),
                          QStringLiteral("extension_flag")}));

    // audio_object_type == 2 (AAC LC)
    QVERIFY(audioObjectTypeNode.has_value());
    QCOMPARE(audioObjectTypeNode->value().toULongLong(), quint64{2});
    QVERIFY(audioObjectTypeNode->location().has_value());
    QCOMPARE(audioObjectTypeNode->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1168});
    QCOMPARE(audioObjectTypeNode->location()->sourceSpans().front().bitLength(), quint64{5});

    // sampling_frequency_index == 4 (44100 Hz), starts at bit 1168 + 5 = 1173
    QVERIFY(samplingFreqIndexNode.has_value());
    QCOMPARE(samplingFreqIndexNode->value().toULongLong(), quint64{4});
    QVERIFY(samplingFreqIndexNode->location().has_value());
    QCOMPARE(samplingFreqIndexNode->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1173});
    QCOMPARE(samplingFreqIndexNode->location()->sourceSpans().front().bitLength(), quint64{4});

    // channel_configuration == 2 (stereo), starts at bit 1173 + 4 = 1177
    QVERIFY(channelConfigNode.has_value());
    QCOMPARE(channelConfigNode->value().toULongLong(), quint64{2});
    QVERIFY(channelConfigNode->location().has_value());
    QCOMPARE(channelConfigNode->location()->sourceSpans().front().start().absoluteBitOffset(), quint64{1177});
    QCOMPARE(channelConfigNode->location()->sourceSpans().front().bitLength(), quint64{4});

    // frame_length_flag == 0
    QVERIFY(frameLengthFlagNode.has_value());
    QCOMPARE(frameLengthFlagNode->value().toULongLong(), quint64{0});
    QVERIFY(frameLengthFlagNode->location().has_value());
    QCOMPARE(frameLengthFlagNode->location()->sourceSpans().front().start().absoluteBitOffset(),
             quint64{1181});
    QCOMPARE(frameLengthFlagNode->location()->sourceSpans().front().bitLength(), quint64{1});

    // depends_on_core_coder == 0
    QVERIFY(dependsOnCoreCoderNode.has_value());
    QCOMPARE(dependsOnCoreCoderNode->value().toULongLong(), quint64{0});
    QVERIFY(dependsOnCoreCoderNode->location().has_value());
    QCOMPARE(dependsOnCoreCoderNode->location()->sourceSpans().front().start().absoluteBitOffset(),
             quint64{1182});
    QCOMPARE(dependsOnCoreCoderNode->location()->sourceSpans().front().bitLength(), quint64{1});

    // extension_flag == 0
    QVERIFY(extensionFlagNode.has_value());
    QCOMPARE(extensionFlagNode->value().toULongLong(), quint64{0});
    QVERIFY(extensionFlagNode->location().has_value());
    QCOMPARE(extensionFlagNode->location()->sourceSpans().front().start().absoluteBitOffset(),
             quint64{1183});
    QCOMPARE(extensionFlagNode->location()->sourceSpans().front().bitLength(), quint64{1});
}

QTEST_MAIN(StructuralEntryRunnerTest)
#include "structural_entry_runner_test.moc"
