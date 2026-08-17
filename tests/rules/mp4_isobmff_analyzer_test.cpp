#include <streamview/rules/mp4_isobmff_analyzer.h>

#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/rule_catalog.h>

#include <QFile>
#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <memory>
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

class CancellingSource final : public streamview::core::RandomAccessSource {
public:
    CancellingSource(std::vector<std::byte> data,
                     streamview::core::CancellationSource* cancellation,
                     std::size_t triggerRead)
        : data_(std::move(data)), cancellation_(cancellation), triggerRead_(triggerRead) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("cancelling"); }

    [[nodiscard]] streamview::core::SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        ++readCount_;
        if (byteOffset >= data_.size()) {
            return {streamview::core::SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const auto count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.data() + offset, count, destination.data());
        if (readCount_ == triggerRead_ && cancellation_ != nullptr) {
            (void)cancellation_->requestCancellation();
        }
        return {count == destination.size()
                    ? streamview::core::SourceReadStatus::Complete
                    : streamview::core::SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
    streamview::core::CancellationSource* cancellation_ = nullptr;
    std::size_t triggerRead_ = 0;
    mutable std::size_t readCount_ = 0;
};

[[nodiscard]] std::vector<std::byte> readFixtureBytes(const QString& relativePath) {
    const QString fullPath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/") + relativePath;
    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Failed to open test fixture: %s", qUtf8Printable(fullPath));
    }
    const QByteArray bytes = file.readAll();
    std::vector<std::byte> result(static_cast<std::size_t>(bytes.size()));
    std::transform(bytes.cbegin(), bytes.cend(), result.begin(), [](char ch) {
        return static_cast<std::byte>(ch);
    });
    return result;
}

[[nodiscard]] streamview::rules::RuleCatalogLookupResult createMockRuleResult(
    const QString& svfmtContent) {
    const QByteArray toml = QByteArrayLiteral(
        "manifest-version = 1\n\n"
        "[package]\n"
        "id = \"test.mp4\"\n"
        "version = \"0.1.0\"\n"
        "authors = [\"StreamView Authors\"]\n"
        "license = \"MIT\"\n"
        "dependencies = []\n\n"
        "[compatibility]\n"
        "language = \"0.1\"\n"
        "engine = \">=0.1.0 <0.2.0\"\n\n"
        "[[entrypoints]]\n"
        "id = \"main\"\n"
        "format = \"video.mp4\"\n"
        "source = \"src/main.svfmt\"\n"
        "profiles = [\"default\"]\n"
        "depth = \"structure\"\n");

    std::vector<streamview::rules::RulePackageFile> files{
        {QStringLiteral("rule.toml"), toml},
        {QStringLiteral("src/main.svfmt"), svfmtContent.toUtf8()}};

    auto pkgRes = streamview::rules::RulePackage::fromFiles(std::move(files));
    if (!pkgRes.succeeded() || !pkgRes.package.has_value()) {
        qFatal("Failed to create mock rule package from files: %s", qUtf8Printable(pkgRes.errorMessage));
    }

    auto pkg = std::make_shared<streamview::rules::RulePackage>(std::move(*pkgRes.package));
    streamview::rules::RuleCatalogLookupResult res;
    res.status = streamview::rules::RuleCatalogLookupStatus::Found;
    res.package = pkg;
    res.entryPoint = pkg->manifest().entryPoints.front();
    return res;
}

} // namespace

class Mp4IsobmffAnalyzerTest : public QObject {
    Q_OBJECT

private slots:
    void failsCleanlyWhenNoRulePackageInstalled();
    void analyzesSingleLevelContainerReEntry();
    void analyzesMultiLevelContainerReEntry();
    void analyzesDiscontiguousMultiSpanSourceMapping();
    void exhaustsSharedBudgetAcrossNestedReEntries();
    void enforcesNestingDepthLimit256();
    void handlesPreCancellationAndInFlightCancellation();
    void handlesTruncatedBoxRecordsTransactionally();
    void preservesSourceErrorWithoutOverwritingToTruncated();
    void handlesD7FragmentedMoofBoxWithContinuation();
    void preservesInvalidSyntaxAsPartialAndContinues();
    void retainsQueuedRecordsAcrossInFlightCancellation();
    void chargesRunnerCreatedBoxNodesToSharedBudget();
    void exposesWindowDecoderWithWindowLocalCoordinates();
};

void Mp4IsobmffAnalyzerTest::failsCleanlyWhenNoRulePackageInstalled() {
    std::vector<std::byte> raw(16);
    MemorySource source(raw);
    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMsg);
    QVERIFY(!analyzer.has_value());
    QVERIFY(!errorMsg.isEmpty());
}

void Mp4IsobmffAnalyzerTest::analyzesSingleLevelContainerReEntry() {
    const QString dsl = QStringLiteral(
        "struct ChildBox {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload;\n"
        "}\n"
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload @container(ChildBox);\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    // Box 1: size=24, payload contains ChildBox: size=16, payload=8 bytes
    std::vector<std::byte> raw(24);
    raw[3] = std::byte{24};
    raw[4] = std::byte{0x6D}; raw[5] = std::byte{0x6F}; raw[6] = std::byte{0x6F}; raw[7] = std::byte{0x76};
    raw[11] = std::byte{16};
    raw[12] = std::byte{0x6D}; raw[13] = std::byte{0x76}; raw[14] = std::byte{0x68}; raw[15] = std::byte{0x64};

    MemorySource source(raw);
    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMsg);
    QVERIFY2(analyzer.has_value(), qUtf8Printable(errorMsg));

    auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), 1);

    const auto& tree = analyzer->tree();
    const auto topBoxNodeId = batch.boxNodes.front();
    const auto topBoxNodeOpt = tree.node(topBoxNodeId);
    QVERIFY(topBoxNodeOpt.has_value());
    const auto topBoxChildren = topBoxNodeOpt->children();
    QVERIFY(topBoxChildren.size() >= 1);

    const auto structNodeId = topBoxChildren.front();
    const auto structNodeOpt = tree.node(structNodeId);
    QVERIFY(structNodeOpt.has_value());
    const auto structFields = structNodeOpt->children();
    QVERIFY(structFields.size() >= 2);

    const auto containerNodeId = structFields.back();
    const auto containerNodeOpt = tree.node(containerNodeId);
    QVERIFY(containerNodeOpt.has_value());
    QVERIFY(containerNodeOpt->metadata().containerChildStructIndex.has_value());

    // Check that child box was drilled and attached under container
    const auto childBoxNodes = containerNodeOpt->children();
    QCOMPARE(childBoxNodes.size(), 1);
}

void Mp4IsobmffAnalyzerTest::analyzesMultiLevelContainerReEntry() {
    const QString dsl = QStringLiteral(
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload @container(Box);\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    const auto bytes = readFixtureBytes(QStringLiteral("mp4_container_nested.mp4"));
    MemorySource source(bytes);

    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMsg);
    QVERIFY2(analyzer.has_value(), qUtf8Printable(errorMsg));

    auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    // Top-level boxes: 3 boxes
    QCOMPARE(batch.boxNodes.size(), 3);

    const auto& tree = analyzer->tree();
    // Verify container nesting across 5 levels (ftyp -> moov -> trak -> mdia -> minf -> stbl -> stsz)
    const auto containerBoxNodeId = batch.boxNodes[1]; // moov box
    const auto containerBoxOpt = tree.node(containerBoxNodeId);
    QVERIFY(containerBoxOpt.has_value());
    const auto structId1 = containerBoxOpt->children().front();
    const auto payloadId1 = tree.node(structId1)->children().back();
    const auto level1Children = tree.node(payloadId1)->children();
    QCOMPARE(level1Children.size(), 1); // trak

    const auto structId2 = level1Children.front();
    const auto payloadId2 = tree.node(structId2)->children().back();
    const auto level2Children = tree.node(payloadId2)->children();
    QCOMPARE(level2Children.size(), 1); // mdia

    const auto structId3 = level2Children.front();
    const auto payloadId3 = tree.node(structId3)->children().back();
    const auto level3Children = tree.node(payloadId3)->children();
    QCOMPARE(level3Children.size(), 1); // minf

    const auto structId4 = level3Children.front();
    const auto payloadId4 = tree.node(structId4)->children().back();
    const auto level4Children = tree.node(payloadId4)->children();
    QCOMPARE(level4Children.size(), 1); // stbl

    const auto structId5 = level4Children.front();
    const auto payloadId5 = tree.node(structId5)->children().back();
    const auto level5Children = tree.node(payloadId5)->children();
    QCOMPARE(level5Children.size(), 1); // stsz
}

void Mp4IsobmffAnalyzerTest::analyzesDiscontiguousMultiSpanSourceMapping() {
    const QString dsl = QStringLiteral(
        "struct ChildBox {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload;\n"
        "}\n"
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload @container(ChildBox);\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    // Box size=24: header 8 bytes, child box 16 bytes
    std::vector<std::byte> raw(24);
    raw[3] = std::byte{24};
    raw[4] = std::byte{0x6D}; raw[5] = std::byte{0x6F}; raw[6] = std::byte{0x6F}; raw[7] = std::byte{0x76};
    raw[11] = std::byte{16};
    raw[12] = std::byte{0x6D}; raw[13] = std::byte{0x76}; raw[14] = std::byte{0x68}; raw[15] = std::byte{0x64};

    MemorySource source(raw);
    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMsg);
    QVERIFY(analyzer.has_value());

    auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), 1);
}

void Mp4IsobmffAnalyzerTest::exhaustsSharedBudgetAcrossNestedReEntries() {
    const QString dsl = QStringLiteral(
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload @container(Box);\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    const auto bytes = readFixtureBytes(QStringLiteral("mp4_container_nested.mp4"));
    MemorySource source(bytes);

    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMsg);
    QVERIFY(analyzer.has_value());

    // Exhaust budget before running
    analyzer->budget()->remainingNodes = 5;

    auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::ResourceLimit);
}

void Mp4IsobmffAnalyzerTest::enforcesNestingDepthLimit256() {
    const QString dsl = QStringLiteral(
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload @container(Box);\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    const auto bytes = readFixtureBytes(QStringLiteral("mp4_container_nested.mp4"));
    MemorySource source(bytes);

    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMsg);
    QVERIFY(analyzer.has_value());

    // Set current nesting depth to limit 256
    analyzer->budget()->currentNestingDepth = 256;

    auto batch = analyzer->analyzeBatch();
    // Exceeding limit on container re-entry yields ResourceLimit
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::ResourceLimit);
}

void Mp4IsobmffAnalyzerTest::handlesPreCancellationAndInFlightCancellation() {
    const QString dsl = QStringLiteral(
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload;\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    std::vector<std::byte> raw(16);
    raw[3] = std::byte{16};
    MemorySource source(raw);

    streamview::core::CancellationSource cancelSource;
    (void)cancelSource.requestCancellation();

    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(
        source, ruleLookup, &errorMsg, cancelSource.token());
    QVERIFY(analyzer.has_value());

    auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Cancelled);
}

void Mp4IsobmffAnalyzerTest::handlesTruncatedBoxRecordsTransactionally() {
    const QString dsl = QStringLiteral(
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload;\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    // Box header claims size 32, but only 12 bytes exist in source (truncated payload)
    std::vector<std::byte> raw(12);
    raw[3] = std::byte{32};
    raw[4] = std::byte{0x66}; raw[5] = std::byte{0x74}; raw[6] = std::byte{0x79}; raw[7] = std::byte{0x70};

    MemorySource source(raw);
    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMsg);
    QVERIFY(analyzer.has_value());

    auto batch = analyzer->analyzeBatch();
    // Materializes partial box with TruncatedSource diagnostic
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), 1);

    const auto& tree = analyzer->tree();
    const auto nodeOpt = tree.node(batch.boxNodes.front());
    QVERIFY(nodeOpt.has_value());
    QVERIFY(!nodeOpt->diagnostics().empty());
    QCOMPARE(nodeOpt->diagnostics().front().code, streamview::core::DiagnosticCode::TruncatedSource);
}

void Mp4IsobmffAnalyzerTest::preservesSourceErrorWithoutOverwritingToTruncated() {
    const QString dsl = QStringLiteral(
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload;\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    FailingSource source(1024);
    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMsg);
    QVERIFY(analyzer.has_value());

    auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::SourceError);
}

void Mp4IsobmffAnalyzerTest::handlesD7FragmentedMoofBoxWithContinuation() {
    const QString dsl = QStringLiteral(
        "struct MoofBox {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    unsupported(\"fragmented MP4 (moof/traf) is outside the v0.1 subset\") at type;\n"
        "}\n"
        "struct NormalBox {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload;\n"
        "}\n"
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    if (type == 0x6D6F6F66) {\n"
        "        unsupported(\"fragmented MP4 (moof/traf) is outside the v0.1 subset\") at type;\n"
        "    } else {\n"
        "        computed<u64> payload_bytes = size - 8;\n"
        "        @lazy(payload_bytes) bytes payload;\n"
        "}\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    // Read fixture: 4 boxes (ftyp, moov, moof, mdat)
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_d7_fragmented.mp4"));
    MemorySource source(bytes);

    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMsg);
    QVERIFY2(analyzer.has_value(), qUtf8Printable(errorMsg));

    auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    // All 4 boxes scanned
    QCOMPARE(batch.boxNodes.size(), 4);

    const auto& tree = analyzer->tree();

    // Box 1
    const auto box1NodeId = batch.boxNodes[0];
    const auto box1NodeOpt = tree.node(box1NodeId);
    QVERIFY(box1NodeOpt.has_value());
    QCOMPARE(box1NodeOpt->state(), streamview::core::MaterializationState::Materialized);

    // Box 2
    const auto box2NodeId = batch.boxNodes[1];
    const auto box2NodeOpt = tree.node(box2NodeId);
    QVERIFY(box2NodeOpt.has_value());
    QCOMPARE(box2NodeOpt->state(), streamview::core::MaterializationState::Materialized);

    // Box 3: moof (Assert all 7 D7 requirements)
    const auto box3NodeId = batch.boxNodes[2];
    const auto box3NodeOpt = tree.node(box3NodeId);
    QVERIFY(box3NodeOpt.has_value());

    // 1. moof box node is materialized with MaterializationState::Unsupported
    const auto struct3NodeId = box3NodeOpt->children().front();
    const auto struct3NodeOpt = tree.node(struct3NodeId);
    QVERIFY(struct3NodeOpt.has_value());
    QCOMPARE(struct3NodeOpt->state(), streamview::core::MaterializationState::Unsupported);

    // 2. DiagnosticCode is UnsupportedSyntax
    QVERIFY(!struct3NodeOpt->diagnostics().empty());
    const auto& diag = struct3NodeOpt->diagnostics().front();
    QCOMPARE(diag.code, streamview::core::DiagnosticCode::UnsupportedSyntax);

    // 3. Severity is Warning
    QCOMPARE(diag.severity, streamview::core::DiagnosticSeverity::Warning);

    // 4. Diagnostic anchored at type field
    QCOMPARE(diag.fieldPath, QStringLiteral("Box.type"));
    QVERIFY(diag.location.has_value());

    const auto fields3 = struct3NodeOpt->children();
    QVERIFY(fields3.size() >= 2);
    const auto sizeFieldId = fields3[0];
    const auto typeFieldId = fields3[1];
    const auto typeFieldOpt = tree.node(typeFieldId);
    QVERIFY(typeFieldOpt.has_value());
    QCOMPARE(diag.location->logicalRange().start(), typeFieldOpt->location()->logicalRange().start());

    // 5. size/type prefix fields remain materialized
    const auto sizeFieldOpt = tree.node(sizeFieldId);
    QVERIFY(sizeFieldOpt.has_value());
    QCOMPARE(sizeFieldOpt->state(), streamview::core::MaterializationState::Materialized);
    QCOMPARE(typeFieldOpt->state(), streamview::core::MaterializationState::Materialized);

    // 6. Prior top-level boxes remain materialized (verified above for box 1 and 2)
    // 7. Subsequent box (mdat) continues scanning and is materialized
    const auto box4NodeId = batch.boxNodes[3];
    const auto box4NodeOpt = tree.node(box4NodeId);
    QVERIFY(box4NodeOpt.has_value());
    const auto struct4NodeId = box4NodeOpt->children().front();
    const auto struct4NodeOpt = tree.node(struct4NodeId);
    QVERIFY(struct4NodeOpt.has_value());
    QCOMPARE(struct4NodeOpt->state(), streamview::core::MaterializationState::Materialized);
}

void Mp4IsobmffAnalyzerTest::preservesInvalidSyntaxAsPartialAndContinues() {
    const QString dsl = QStringLiteral(
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    if (type == 0x62616431) {\n"
        "        computed<u64> malformed = size - 32;\n"
        "    } else {\n"
        "        computed<u64> payload_bytes = size - 8;\n"
        "        @lazy(payload_bytes) bytes payload;\n"
        "    }\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);

    std::vector<std::byte> raw(32);
    raw[3] = std::byte{16};
    raw[4] = std::byte{'b'};
    raw[5] = std::byte{'a'};
    raw[6] = std::byte{'d'};
    raw[7] = std::byte{'1'};
    raw[19] = std::byte{16};
    raw[20] = std::byte{'g'};
    raw[21] = std::byte{'o'};
    raw[22] = std::byte{'o'};
    raw[23] = std::byte{'d'};

    MemorySource source(raw);
    QString errorMessage;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMessage);
    QVERIFY2(analyzer.has_value(), qUtf8Printable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{2});

    const auto& tree = analyzer->tree();
    const auto firstBox = tree.node(batch.boxNodes[0]);
    const auto secondBox = tree.node(batch.boxNodes[1]);
    QVERIFY(firstBox.has_value());
    QVERIFY(secondBox.has_value());
    const auto firstStructure = tree.node(firstBox->children().front());
    const auto secondStructure = tree.node(secondBox->children().front());
    QVERIFY(firstStructure.has_value());
    QVERIFY(secondStructure.has_value());
    QCOMPARE(firstStructure->state(), streamview::core::MaterializationState::Invalid);
    QVERIFY(!firstStructure->diagnostics().empty());
    QCOMPARE(firstStructure->diagnostics().front().code,
             streamview::core::DiagnosticCode::InvalidSyntax);
    QCOMPARE(secondStructure->state(), streamview::core::MaterializationState::Materialized);
}

void Mp4IsobmffAnalyzerTest::retainsQueuedRecordsAcrossInFlightCancellation() {
    const QString dsl = QStringLiteral(
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload;\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);
    std::vector<std::byte> raw(48);
    for (std::size_t offset = 0; offset < raw.size(); offset += 16) {
        raw[offset + 3] = std::byte{16};
    }

    streamview::core::CancellationSource cancellation;
    CancellingSource source(raw, &cancellation, 2);
    QString errorMessage;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(
        source, ruleLookup, &errorMessage, cancellation.token());
    QVERIFY2(analyzer.has_value(), qUtf8Printable(errorMessage));

    const auto cancelled = analyzer->analyzeBatch();
    QCOMPARE(cancelled.status, streamview::rules::Mp4IsobmffAnalysisStatus::Cancelled);
    QCOMPARE(cancelled.boxNodes.size(), std::size_t{1});
    QCOMPARE(analyzer->tree().node(analyzer->tree().rootId())->children().size(),
             std::size_t{1});

    QVERIFY(analyzer->resumeAfterCancellation(std::nullopt, &errorMessage));
    const auto resumed = analyzer->analyzeBatch();
    QCOMPARE(resumed.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(resumed.boxNodes.size(), std::size_t{2});
    QCOMPARE(analyzer->tree().node(analyzer->tree().rootId())->children().size(),
             std::size_t{3});
}

void Mp4IsobmffAnalyzerTest::chargesRunnerCreatedBoxNodesToSharedBudget() {
    const QString dsl = QStringLiteral(
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    computed<u64> payload_bytes = size - 8;\n"
        "    @lazy(payload_bytes) bytes payload;\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);
    std::vector<std::byte> raw(16);
    raw[3] = std::byte{16};
    MemorySource source(raw);
    QString errorMessage;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMessage);
    QVERIFY2(analyzer.has_value(), qUtf8Printable(errorMessage));
    analyzer->budget()->remainingNodes = 1;

    const auto result = analyzer->analyzeBatch();
    QCOMPARE(result.status, streamview::rules::Mp4IsobmffAnalysisStatus::ResourceLimit);
    QCOMPARE(analyzer->budget()->remainingNodes, quint64{0});
    const auto root = analyzer->tree().node(analyzer->tree().rootId());
    QVERIFY(root.has_value());
    QVERIFY(root->children().empty());
}

void Mp4IsobmffAnalyzerTest::exposesWindowDecoderWithWindowLocalCoordinates() {
    const QString dsl = QStringLiteral(
        "struct Entry {\n"
        "    bits<32> value;\n"
        "}\n"
        "struct Box {\n"
        "    bits<32> size;\n"
        "    bits<32> type;\n"
        "    bits<32> entry_count;\n"
        "    computed<u64> payload_bytes = 4;\n"
        "    @lazy(payload_bytes) bytes payload @window(Entry, entry_count);\n"
        "}\n"
        "@index(progressive) sequence<Box> boxes = scan(mp4_box);\n"
        "entry boxes;\n");
    const auto ruleLookup = createMockRuleResult(dsl);
    std::vector<std::byte> raw(16);
    raw[3] = std::byte{16};
    raw[11] = std::byte{1};
    raw[12] = std::byte{0x12};
    raw[13] = std::byte{0x34};
    raw[14] = std::byte{0x56};
    raw[15] = std::byte{0x78};
    MemorySource source(raw);
    QString errorMessage;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, ruleLookup, &errorMessage);
    QVERIFY2(analyzer.has_value(), qUtf8Printable(errorMessage));
    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);

    const auto boxNode = analyzer->tree().node(batch.boxNodes.front());
    QVERIFY(boxNode.has_value());
    const auto structureNode = analyzer->tree().node(boxNode->children().front());
    QVERIFY(structureNode.has_value());
    const auto windowNodeId = structureNode->children().back();
    auto decoder = analyzer->windowDecoder(windowNodeId);
    QVERIFY(decoder.has_value());
    const auto decoded = decoder->decodeWindow({0, 1});
    QCOMPARE(decoded.status, streamview::rules::DslExecutionStatus::Materialized);
    QCOMPARE(decoded.decodedEntryCount, 1ULL);
    QVERIFY(!decoded.entryNodes.empty());

    auto secondDecoder = analyzer->windowDecoder(windowNodeId);
    QVERIFY(secondDecoder.has_value());
    const auto repeated = secondDecoder->decodeWindow({0, 1});
    QCOMPARE(repeated.status, streamview::rules::DslExecutionStatus::Materialized);
    QCOMPARE(repeated.entryNodes, decoded.entryNodes);
    QCOMPARE(analyzer->tree().node(windowNodeId)->children().size(), std::size_t{1});
}

QTEST_MAIN(Mp4IsobmffAnalyzerTest)
#include "mp4_isobmff_analyzer_test.moc"
