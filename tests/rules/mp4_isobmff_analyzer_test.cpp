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
#include <optional>
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

class ToggleFailureSource final : public streamview::core::RandomAccessSource {
public:
    explicit ToggleFailureSource(std::vector<std::byte> data)
        : data_(std::move(data)), visibleSize_(data_.size()) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return visibleSize_; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("toggle"); }

    void failReads(bool fail) const noexcept { failReads_ = fail; }
    void setVisibleSize(quint64 size) const noexcept {
        visibleSize_ = std::min(size, static_cast<quint64>(data_.size()));
    }

    [[nodiscard]] streamview::core::SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (failReads_) {
            return {streamview::core::SourceReadStatus::Error, 0, QStringLiteral("I/O device fault")};
        }
        if (destination.empty()) {
            return {streamview::core::SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= visibleSize_) {
            return {streamview::core::SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const auto count = std::min({destination.size(), data_.size() - offset,
                                     static_cast<std::size_t>(visibleSize_ - byteOffset)});
        std::copy_n(data_.data() + offset, count, destination.data());
        return {count == destination.size()
                    ? streamview::core::SourceReadStatus::Complete
                    : streamview::core::SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
    mutable quint64 visibleSize_ = 0;
    mutable bool failReads_ = false;
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

[[nodiscard]] std::optional<streamview::core::AnalysisNodeId> findSampleTableWindow(
    const streamview::core::AnalysisTree& tree,
    const streamview::rules::Mp4IsobmffAnalysisBatch& batch,
    quint64 boxType) {
    if (batch.boxNodes.size() < 2) return std::nullopt;
    auto moov = tree.node(batch.boxNodes[1]);
    if (!moov || moov->children().empty()) return std::nullopt;
    auto moovStruct = tree.node(moov->children().front());
    if (!moovStruct || moovStruct->children().empty()) return std::nullopt;
    auto moovPayload = tree.node(moovStruct->children().back());
    if (!moovPayload || moovPayload->children().empty()) return std::nullopt;
    auto trakStruct = tree.node(moovPayload->children().front());
    if (!trakStruct || trakStruct->children().empty()) return std::nullopt;
    auto trakPayload = tree.node(trakStruct->children().back());
    if (!trakPayload || trakPayload->children().empty()) return std::nullopt;
    auto mdiaStruct = tree.node(trakPayload->children().front());
    if (!mdiaStruct || mdiaStruct->children().empty()) return std::nullopt;
    auto mdiaPayload = tree.node(mdiaStruct->children().back());
    if (!mdiaPayload || mdiaPayload->children().empty()) return std::nullopt;
    auto minfStruct = tree.node(mdiaPayload->children().front());
    if (!minfStruct || minfStruct->children().empty()) return std::nullopt;
    auto minfPayload = tree.node(minfStruct->children().back());
    if (!minfPayload || minfPayload->children().empty()) return std::nullopt;
    auto stblStruct = tree.node(minfPayload->children().front());
    if (!stblStruct || stblStruct->children().empty()) return std::nullopt;
    auto stblPayload = tree.node(stblStruct->children().back());
    if (!stblPayload) return std::nullopt;

    for (const auto boxId : stblPayload->children()) {
        auto boxStruct = tree.node(boxId);
        if (!boxStruct || boxStruct->children().size() < 2) continue;
        auto typeNode = tree.node(boxStruct->children()[1]);
        if (!typeNode || typeNode->value().toULongLong() != boxType) continue;
        auto payload = tree.node(boxStruct->children().back());
        if (!payload || payload->children().empty()) continue;
        auto payloadStruct = tree.node(payload->children().front());
        if (!payloadStruct) continue;
        for (const auto childId : payloadStruct->children()) {
            auto child = tree.node(childId);
            if (child && child->metadata().window.has_value()) return childId;
        }
    }
    return std::nullopt;
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
    void loadsBundledMp4PackageSuccessfully();
    void analyzesStandardFtypBoxAndBrands();
    void analyzesLargesize64BitBox();
    void analyzesSizeZeroEofBox();
    void analyzesUnknownOpaqueBox();
    void analyzesFullMoovContainerHierarchyV0();
    void analyzesFullBoxVersion1TimeHeadersAndEditList();
    void handlesLargeSizeHandlerReferenceBoxWireOrder();
    void rejectsUnsupportedFullBoxVersionWithoutV0Fallback();
    void analyzesSampleTableBoxesV0();
    void decodesAllSampleTableWindowsWithSourceSpans();
    void analyzesLargeSizeAndEofSampleTableBoxes();
    void analyzesSampleSizeUniformWithoutTable();
    void decodesSampleTableWindowsWithPagingAndBudget();
    void handlesSampleTableWindowFailures();
    void handlesUnsupportedSampleTableVersions();
};

void Mp4IsobmffAnalyzerTest::failsCleanlyWhenNoRulePackageInstalled() {
    std::vector<std::byte> raw(16);
    MemorySource source(raw);
    streamview::rules::RuleCatalogLookupResult invalidRule;
    invalidRule.status = streamview::rules::RuleCatalogLookupStatus::MissingContent;
    invalidRule.errorMessage = QStringLiteral("Missing rule package");
    QString errorMsg;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, invalidRule, &errorMsg);
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

void Mp4IsobmffAnalyzerTest::loadsBundledMp4PackageSuccessfully() {
    const auto loaded = streamview::rules::loadMp4IsobmffRulePackage();
    QVERIFY(loaded.succeeded());
    QVERIFY(loaded.package.has_value());
    QCOMPARE(loaded.package->identity().packageId(), QStringLiteral("org.streamview.mp4"));
    QCOMPARE(loaded.package->identity().packageVersion(), QStringLiteral("0.1.2"));
    const auto* mp4Source = loaded.package->fileContents(QStringLiteral("src/mp4_isobmff.svfmt"));
    QVERIFY(mp4Source != nullptr);
    QVERIFY(!mp4Source->isEmpty());
}

void Mp4IsobmffAnalyzerTest::analyzesStandardFtypBoxAndBrands() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5e_basic_ftyp_mdat.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto& tree = analyzer->tree();

    // Box 0: ftyp (32 bytes)
    const auto box0 = tree.node(batch.boxNodes[0]);
    QVERIFY(box0.has_value());
    QCOMPARE(box0->children().size(), std::size_t{1});
    const auto struct0 = tree.node(box0->children().front());
    QVERIFY(struct0.has_value());

    std::vector<QString> box0FieldNames;
    for (const auto childId : struct0->children()) {
        const auto child = tree.node(childId);
        if (child) {
            box0FieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedFtypFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("major_brand"),
        QStringLiteral("minor_version"),
        QStringLiteral("brand_count"),
        QStringLiteral("compatible_brands[0]"),
        QStringLiteral("compatible_brands[1]"),
        QStringLiteral("compatible_brands[2]"),
        QStringLiteral("compatible_brands[3]")
    };
    QCOMPARE(box0FieldNames, expectedFtypFields);

    const auto sizeNode = tree.node(struct0->children()[0]);
    QCOMPARE(sizeNode->value().toULongLong(), quint64{32});
    QVERIFY(sizeNode->location().has_value());
    QCOMPARE(sizeNode->location()->logicalRange().start().bitOffset(), quint64{0});
    QCOMPARE(sizeNode->location()->logicalRange().bitLength(), quint64{32});

    const auto typeNode = tree.node(struct0->children()[1]);
    QCOMPARE(typeNode->value().toULongLong(), quint64{0x66747970});
    QVERIFY(typeNode->location().has_value());
    QCOMPARE(typeNode->location()->logicalRange().start().bitOffset(), quint64{32});
    QCOMPARE(typeNode->location()->logicalRange().bitLength(), quint64{32});

    const auto majorBrandNode = tree.node(struct0->children()[2]);
    QCOMPARE(majorBrandNode->value().toULongLong(), quint64{0x69736F6D});
    QVERIFY(majorBrandNode->location().has_value());
    QCOMPARE(majorBrandNode->location()->logicalRange().start().bitOffset(), quint64{64});
    QCOMPARE(majorBrandNode->location()->logicalRange().bitLength(), quint64{32});

    const auto minorVerNode = tree.node(struct0->children()[3]);
    QCOMPARE(minorVerNode->value().toULongLong(), quint64{0x00000200});
    QVERIFY(minorVerNode->location().has_value());
    QCOMPARE(minorVerNode->location()->logicalRange().start().bitOffset(), quint64{96});
    QCOMPARE(minorVerNode->location()->logicalRange().bitLength(), quint64{32});

    const auto brand0Node = tree.node(struct0->children()[5]);
    QCOMPARE(brand0Node->value().toULongLong(), quint64{0x69736F6D});
    QVERIFY(brand0Node->location().has_value());
    QCOMPARE(brand0Node->location()->logicalRange().start().bitOffset(), quint64{128});
    QCOMPARE(brand0Node->location()->logicalRange().bitLength(), quint64{32});

    const auto brand1Node = tree.node(struct0->children()[6]);
    QCOMPARE(brand1Node->value().toULongLong(), quint64{0x69736F32});
    QVERIFY(brand1Node->location().has_value());
    QCOMPARE(brand1Node->location()->logicalRange().start().bitOffset(), quint64{160});
    QCOMPARE(brand1Node->location()->logicalRange().bitLength(), quint64{32});

    const auto brand2Node = tree.node(struct0->children()[7]);
    QCOMPARE(brand2Node->value().toULongLong(), quint64{0x61766331});
    QVERIFY(brand2Node->location().has_value());
    QCOMPARE(brand2Node->location()->logicalRange().start().bitOffset(), quint64{192});
    QCOMPARE(brand2Node->location()->logicalRange().bitLength(), quint64{32});

    const auto brand3Node = tree.node(struct0->children()[8]);
    QCOMPARE(brand3Node->value().toULongLong(), quint64{0x6D703431});
    QVERIFY(brand3Node->location().has_value());
    QCOMPARE(brand3Node->location()->logicalRange().start().bitOffset(), quint64{224});
    QCOMPARE(brand3Node->location()->logicalRange().bitLength(), quint64{32});

    // Box 1: free (8 bytes)
    const auto box1 = tree.node(batch.boxNodes[1]);
    QVERIFY(box1.has_value());
    const auto struct1 = tree.node(box1->children().front());
    QVERIFY(struct1.has_value());
    std::vector<QString> box1FieldNames;
    for (const auto childId : struct1->children()) {
        const auto child = tree.node(childId);
        if (child) {
            box1FieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedFreeFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("payload_bytes"),
        QStringLiteral("payload")
    };
    QCOMPARE(box1FieldNames, expectedFreeFields);

    // Box 2: mdat (24 bytes, 16 payload bytes)
    const auto box2 = tree.node(batch.boxNodes[2]);
    QVERIFY(box2.has_value());
    const auto struct2 = tree.node(box2->children().front());
    QVERIFY(struct2.has_value());
    std::vector<QString> box2FieldNames;
    for (const auto childId : struct2->children()) {
        const auto child = tree.node(childId);
        if (child) {
            box2FieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedMdatFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("payload_bytes"),
        QStringLiteral("payload")
    };
    QCOMPARE(box2FieldNames, expectedMdatFields);
    const auto mdatPayload = tree.node(struct2->children()[3]);
    QVERIFY(mdatPayload->location().has_value());
    QCOMPARE(mdatPayload->location()->logicalRange().start().bitOffset(), quint64{64});
    QCOMPARE(mdatPayload->location()->logicalRange().bitLength(), quint64{128});
}

void Mp4IsobmffAnalyzerTest::analyzesLargesize64BitBox() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5e_largesize_box.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto& tree = analyzer->tree();
    const auto box2 = tree.node(batch.boxNodes[2]);
    QVERIFY(box2.has_value());
    const auto struct2 = tree.node(box2->children().front());
    QVERIFY(struct2.has_value());

    std::vector<QString> box2FieldNames;
    for (const auto childId : struct2->children()) {
        const auto child = tree.node(childId);
        if (child) {
            box2FieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedLargeFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("largesize"),
        QStringLiteral("large_payload_bytes"),
        QStringLiteral("large_payload")
    };
    QCOMPARE(box2FieldNames, expectedLargeFields);

    const auto sizeNode = tree.node(struct2->children()[0]);
    QCOMPARE(sizeNode->value().toULongLong(), quint64{1});
    const auto typeNode = tree.node(struct2->children()[1]);
    QCOMPARE(typeNode->value().toULongLong(), quint64{0x6D646174});
    const auto largeSizeNode = tree.node(struct2->children()[2]);
    QCOMPARE(largeSizeNode->value().toULongLong(), quint64{32});
    QVERIFY(largeSizeNode->location().has_value());
    QCOMPARE(largeSizeNode->location()->logicalRange().start().bitOffset(), quint64{64});
    QCOMPARE(largeSizeNode->location()->logicalRange().bitLength(), quint64{64});

    const auto payloadNode = tree.node(struct2->children()[4]);
    QVERIFY(payloadNode->location().has_value());
    QCOMPARE(payloadNode->location()->logicalRange().start().bitOffset(), quint64{128});
    QCOMPARE(payloadNode->location()->logicalRange().bitLength(), quint64{128});
}

void Mp4IsobmffAnalyzerTest::analyzesSizeZeroEofBox() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5e_size0_eof_box.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto& tree = analyzer->tree();
    const auto box2 = tree.node(batch.boxNodes[2]);
    QVERIFY(box2.has_value());
    const auto struct2 = tree.node(box2->children().front());
    QVERIFY(struct2.has_value());

    std::vector<QString> box2FieldNames;
    for (const auto childId : struct2->children()) {
        const auto child = tree.node(childId);
        if (child) {
            box2FieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedSize0Fields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("eof_payload_bytes"),
        QStringLiteral("eof_payload")
    };
    QCOMPARE(box2FieldNames, expectedSize0Fields);

    const auto sizeNode = tree.node(struct2->children()[0]);
    QCOMPARE(sizeNode->value().toULongLong(), quint64{0});
    const auto typeNode = tree.node(struct2->children()[1]);
    QCOMPARE(typeNode->value().toULongLong(), quint64{0x6D646174});

    const auto payloadNode = tree.node(struct2->children()[3]);
    QVERIFY(payloadNode->location().has_value());
    QCOMPARE(payloadNode->location()->logicalRange().start().bitOffset(), quint64{64});
    QCOMPARE(payloadNode->location()->logicalRange().bitLength(), quint64{96});
}

void Mp4IsobmffAnalyzerTest::analyzesUnknownOpaqueBox() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5e_unknown_opaque_box.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto& tree = analyzer->tree();
    const auto box1 = tree.node(batch.boxNodes[1]);
    QVERIFY(box1.has_value());
    const auto struct1 = tree.node(box1->children().front());
    QVERIFY(struct1.has_value());

    std::vector<QString> box1FieldNames;
    for (const auto childId : struct1->children()) {
        const auto child = tree.node(childId);
        if (child) {
            box1FieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedSkipFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("payload_bytes"),
        QStringLiteral("payload")
    };
    QCOMPARE(box1FieldNames, expectedSkipFields);

    const auto sizeNode = tree.node(struct1->children()[0]);
    QCOMPARE(sizeNode->value().toULongLong(), quint64{24});
    const auto typeNode = tree.node(struct1->children()[1]);
    QCOMPARE(typeNode->value().toULongLong(), quint64{0x736B6970});

    const auto payloadNode = tree.node(struct1->children()[3]);
    QVERIFY(payloadNode->location().has_value());
    QCOMPARE(payloadNode->location()->logicalRange().start().bitOffset(), quint64{64});
    QCOMPARE(payloadNode->location()->logicalRange().bitLength(), quint64{128});

    const auto rootNode = analyzer->tree().node(analyzer->tree().rootId());
    QVERIFY(rootNode.has_value());
    QCOMPARE(rootNode->diagnostics().size(), std::size_t{0});
}

void Mp4IsobmffAnalyzerTest::analyzesFullMoovContainerHierarchyV0() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5f_full_hierarchy_v0.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto& tree = analyzer->tree();

    // Box 0: ftyp (32 bytes)
    const auto box0 = tree.node(batch.boxNodes[0]);
    QVERIFY(box0.has_value());

    // Box 1: moov (385 bytes)
    const auto box1 = tree.node(batch.boxNodes[1]);
    QVERIFY(box1.has_value());
    const auto struct1 = tree.node(box1->children().front());
    QVERIFY(struct1.has_value());
    std::vector<QString> moovFieldNames;
    for (const auto childId : struct1->children()) {
        const auto child = tree.node(childId);
        if (child) {
            moovFieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedMoovFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("moov_payload_bytes"),
        QStringLiteral("moov_payload")
    };
    QCOMPARE(moovFieldNames, expectedMoovFields);

    const auto moovPayloadNode = tree.node(struct1->children()[3]);
    QVERIFY(moovPayloadNode.has_value());
    // moov contains 2 child boxes: mvhd and trak
    QCOMPARE(moovPayloadNode->children().size(), std::size_t{2});

    // 1. mvhd (108 bytes)
    const auto mvhdStruct = tree.node(moovPayloadNode->children()[0]);
    QVERIFY(mvhdStruct.has_value());

    std::vector<QString> mvhdFieldNames;
    for (const auto childId : mvhdStruct->children()) {
        const auto child = tree.node(childId);
        if (child) {
            mvhdFieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedMvhdFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("mvhd_version"),
        QStringLiteral("mvhd_flags"),
        QStringLiteral("mvhd_v0_creation_time"),
        QStringLiteral("mvhd_v0_modification_time"),
        QStringLiteral("mvhd_v0_timescale"),
        QStringLiteral("mvhd_v0_duration"),
        QStringLiteral("mvhd_rate"),
        QStringLiteral("mvhd_volume"),
        QStringLiteral("mvhd_reserved"),
        QStringLiteral("mvhd_reserved_2"),
        QStringLiteral("mvhd_matrix_count"),
        QStringLiteral("mvhd_matrix[0]"),
        QStringLiteral("mvhd_matrix[1]"),
        QStringLiteral("mvhd_matrix[2]"),
        QStringLiteral("mvhd_matrix[3]"),
        QStringLiteral("mvhd_matrix[4]"),
        QStringLiteral("mvhd_matrix[5]"),
        QStringLiteral("mvhd_matrix[6]"),
        QStringLiteral("mvhd_matrix[7]"),
        QStringLiteral("mvhd_matrix[8]"),
        QStringLiteral("mvhd_pre_defined_count"),
        QStringLiteral("mvhd_pre_defined[0]"),
        QStringLiteral("mvhd_pre_defined[1]"),
        QStringLiteral("mvhd_pre_defined[2]"),
        QStringLiteral("mvhd_pre_defined[3]"),
        QStringLiteral("mvhd_pre_defined[4]"),
        QStringLiteral("mvhd_pre_defined[5]"),
        QStringLiteral("mvhd_next_track_id")
    };
    QCOMPARE(mvhdFieldNames, expectedMvhdFields);

    const auto mvhdVersion = tree.node(mvhdStruct->children()[2]);
    QCOMPARE(mvhdVersion->value().toULongLong(), quint64{0});
    const auto mvhdTimescale = tree.node(mvhdStruct->children()[6]);
    QCOMPARE(mvhdTimescale->value().toULongLong(), quint64{1000});
    const auto mvhdDuration = tree.node(mvhdStruct->children()[7]);
    QCOMPARE(mvhdDuration->value().toULongLong(), quint64{5000});
    const auto mvhdNextTrackId = tree.node(mvhdStruct->children()[29]);
    QCOMPARE(mvhdNextTrackId->value().toULongLong(), quint64{2});

    // 2. trak (269 bytes)
    const auto trakStruct = tree.node(moovPayloadNode->children()[1]);
    QVERIFY(trakStruct.has_value());
    const auto trakPayloadNode = tree.node(trakStruct->children()[3]);
    QVERIFY(trakPayloadNode.has_value());
    // trak contains 3 child boxes: tkhd, edts, mdia
    QCOMPARE(trakPayloadNode->children().size(), std::size_t{3});

    // 2.1 tkhd (92 bytes)
    const auto tkhdStruct = tree.node(trakPayloadNode->children()[0]);
    QVERIFY(tkhdStruct.has_value());
    std::vector<QString> tkhdFieldNames;
    for (const auto childId : tkhdStruct->children()) {
        const auto child = tree.node(childId);
        if (child) {
            tkhdFieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedTkhdFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("tkhd_version"),
        QStringLiteral("tkhd_flags"),
        QStringLiteral("tkhd_v0_creation_time"),
        QStringLiteral("tkhd_v0_modification_time"),
        QStringLiteral("tkhd_v0_track_id"),
        QStringLiteral("tkhd_v0_reserved"),
        QStringLiteral("tkhd_v0_duration"),
        QStringLiteral("tkhd_reserved_2"),
        QStringLiteral("tkhd_layer"),
        QStringLiteral("tkhd_alternate_group"),
        QStringLiteral("tkhd_volume"),
        QStringLiteral("tkhd_reserved_3"),
        QStringLiteral("tkhd_matrix_count"),
        QStringLiteral("tkhd_matrix[0]"),
        QStringLiteral("tkhd_matrix[1]"),
        QStringLiteral("tkhd_matrix[2]"),
        QStringLiteral("tkhd_matrix[3]"),
        QStringLiteral("tkhd_matrix[4]"),
        QStringLiteral("tkhd_matrix[5]"),
        QStringLiteral("tkhd_matrix[6]"),
        QStringLiteral("tkhd_matrix[7]"),
        QStringLiteral("tkhd_matrix[8]"),
        QStringLiteral("tkhd_width"),
        QStringLiteral("tkhd_height")
    };
    QCOMPARE(tkhdFieldNames, expectedTkhdFields);
    const auto tkhdTrackId = tree.node(tkhdStruct->children()[6]);
    QCOMPARE(tkhdTrackId->value().toULongLong(), quint64{1});
    const auto tkhdDuration = tree.node(tkhdStruct->children()[8]);
    QCOMPARE(tkhdDuration->value().toULongLong(), quint64{5000});
    const auto tkhdWidth = tree.node(tkhdStruct->children()[24]);
    QCOMPARE(tkhdWidth->value().toULongLong(), quint64{0x07800000});

    // 2.2 edts (36 bytes) -> contains elst (28 bytes)
    const auto edtsStruct = tree.node(trakPayloadNode->children()[1]);
    QVERIFY(edtsStruct.has_value());
    const auto edtsPayloadNode = tree.node(edtsStruct->children()[3]);
    QVERIFY(edtsPayloadNode.has_value());
    QCOMPARE(edtsPayloadNode->children().size(), std::size_t{1});

    const auto elstStruct = tree.node(edtsPayloadNode->children()[0]);
    QVERIFY(elstStruct.has_value());
    std::vector<QString> elstFieldNames;
    for (const auto childId : elstStruct->children()) {
        const auto child = tree.node(childId);
        if (child) {
            elstFieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedElstFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("elst_version"),
        QStringLiteral("elst_flags"),
        QStringLiteral("elst_v0_entry_count"),
        QStringLiteral("elst_v0_segment_duration[0]"),
        QStringLiteral("elst_v0_media_time[0]"),
        QStringLiteral("elst_v0_media_rate_integer[0]"),
        QStringLiteral("elst_v0_media_rate_fraction[0]")
    };
    QCOMPARE(elstFieldNames, expectedElstFields);
    const auto elstSegDur = tree.node(elstStruct->children()[5]);
    QCOMPARE(elstSegDur->value().toULongLong(), quint64{5000});
    const auto elstMediaTime = tree.node(elstStruct->children()[6]);
    QCOMPARE(elstMediaTime->value().toULongLong(), quint64{0});

    // 2.3 mdia (133 bytes) -> contains mdhd, hdlr, minf
    const auto mdiaStruct = tree.node(trakPayloadNode->children()[2]);
    QVERIFY(mdiaStruct.has_value());
    const auto mdiaPayloadNode = tree.node(mdiaStruct->children()[3]);
    QVERIFY(mdiaPayloadNode.has_value());
    QCOMPARE(mdiaPayloadNode->children().size(), std::size_t{3});

    // 2.3.1 mdhd (32 bytes)
    const auto mdhdStruct = tree.node(mdiaPayloadNode->children()[0]);
    QVERIFY(mdhdStruct.has_value());
    std::vector<QString> mdhdFieldNames;
    for (const auto childId : mdhdStruct->children()) {
        const auto child = tree.node(childId);
        if (child) {
            mdhdFieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedMdhdFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("mdhd_version"),
        QStringLiteral("mdhd_flags"),
        QStringLiteral("mdhd_v0_creation_time"),
        QStringLiteral("mdhd_v0_modification_time"),
        QStringLiteral("mdhd_v0_timescale"),
        QStringLiteral("mdhd_v0_duration"),
        QStringLiteral("mdhd_pad"),
        QStringLiteral("mdhd_language"),
        QStringLiteral("mdhd_pre_defined")
    };
    QCOMPARE(mdhdFieldNames, expectedMdhdFields);
    const auto mdhdTimescale = tree.node(mdhdStruct->children()[6]);
    QCOMPARE(mdhdTimescale->value().toULongLong(), quint64{30000});
    const auto mdhdDuration = tree.node(mdhdStruct->children()[7]);
    QCOMPARE(mdhdDuration->value().toULongLong(), quint64{150000});

    // 2.3.2 hdlr (45 bytes)
    const auto hdlrStruct = tree.node(mdiaPayloadNode->children()[1]);
    QVERIFY(hdlrStruct.has_value());
    std::vector<QString> hdlrFieldNames;
    for (const auto childId : hdlrStruct->children()) {
        const auto child = tree.node(childId);
        if (child) {
            hdlrFieldNames.push_back(child->name());
        }
    }
    const std::vector<QString> expectedHdlrFields = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("hdlr_version"),
        QStringLiteral("hdlr_flags"),
        QStringLiteral("hdlr_pre_defined"),
        QStringLiteral("hdlr_handler_type"),
        QStringLiteral("hdlr_reserved_0"),
        QStringLiteral("hdlr_reserved_1"),
        QStringLiteral("hdlr_reserved_2"),
        QStringLiteral("hdlr_name_bytes"),
        QStringLiteral("hdlr_name")
    };
    QCOMPARE(hdlrFieldNames, expectedHdlrFields);
    const auto handlerTypeNode = tree.node(hdlrStruct->children()[5]);
    QCOMPARE(handlerTypeNode->value().toULongLong(), quint64{0x76696465});

    // 2.3.3 minf (48 bytes) -> contains stbl (40 bytes) -> contains stsd, stts
    const auto minfStruct = tree.node(mdiaPayloadNode->children()[2]);
    QVERIFY(minfStruct.has_value());
    const auto minfPayloadNode = tree.node(minfStruct->children()[3]);
    QVERIFY(minfPayloadNode.has_value());
    QCOMPARE(minfPayloadNode->children().size(), std::size_t{1});

    const auto stblStruct = tree.node(minfPayloadNode->children()[0]);
    QVERIFY(stblStruct.has_value());
    const auto stblPayloadNode = tree.node(stblStruct->children()[3]);
    QVERIFY(stblPayloadNode.has_value());
    // stbl contains 2 opaque child boxes: stsd and stts
    QCOMPARE(stblPayloadNode->children().size(), std::size_t{2});

    const auto stsdStruct = tree.node(stblPayloadNode->children()[0]);
    QVERIFY(stsdStruct.has_value());
    const auto stsdType = tree.node(stsdStruct->children()[1]);
    QCOMPARE(stsdType->value().toULongLong(), quint64{0x73747364});

    const auto sttsStruct = tree.node(stblPayloadNode->children()[1]);
    QVERIFY(sttsStruct.has_value());
    const auto sttsType = tree.node(sttsStruct->children()[1]);
    QCOMPARE(sttsType->value().toULongLong(), quint64{0x73747473});

    // Box 2: mdat (24 bytes)
    const auto box2 = tree.node(batch.boxNodes[2]);
    QVERIFY(box2.has_value());
    const auto struct2 = tree.node(box2->children().front());
    QVERIFY(struct2.has_value());
    const auto mdatType = tree.node(struct2->children()[1]);
    QCOMPARE(mdatType->value().toULongLong(), quint64{0x6D646174});

    QCOMPARE(analyzer->tree().node(analyzer->tree().rootId())->diagnostics().size(), std::size_t{0});
}

void Mp4IsobmffAnalyzerTest::analyzesFullBoxVersion1TimeHeadersAndEditList() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5f_time_headers_v1.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto& tree = analyzer->tree();
    const auto box1 = tree.node(batch.boxNodes[1]);
    QVERIFY(box1.has_value());
    const auto struct1 = tree.node(box1->children().front());
    QVERIFY(struct1.has_value());
    const auto moovPayloadNode = tree.node(struct1->children()[3]);
    QVERIFY(moovPayloadNode.has_value());

    // 1. mvhd (v1)
    const auto mvhdStruct = tree.node(moovPayloadNode->children()[0]);
    QVERIFY(mvhdStruct.has_value());

    const auto mvhdVersion = tree.node(mvhdStruct->children()[2]);
    QCOMPARE(mvhdVersion->value().toULongLong(), quint64{1});
    const auto mvhdCreationTime = tree.node(mvhdStruct->children()[4]);
    QCOMPARE(mvhdCreationTime->value().toULongLong(), quint64{0x100000000});
    const auto mvhdModTime = tree.node(mvhdStruct->children()[5]);
    QCOMPARE(mvhdModTime->value().toULongLong(), quint64{0x100000001});
    const auto mvhdTimescale = tree.node(mvhdStruct->children()[6]);
    QCOMPARE(mvhdTimescale->value().toULongLong(), quint64{48000});
    const auto mvhdDuration = tree.node(mvhdStruct->children()[7]);
    QCOMPARE(mvhdDuration->value().toULongLong(), quint64{0x200000000});
    const auto mvhdNextTrackId = tree.node(mvhdStruct->children()[29]);
    QCOMPARE(mvhdNextTrackId->value().toULongLong(), quint64{3});

    // 2. trak -> tkhd (v1), edts -> elst (v1), mdia -> mdhd (v1), hdlr
    const auto trakStruct = tree.node(moovPayloadNode->children()[1]);
    QVERIFY(trakStruct.has_value());
    const auto trakPayloadNode = tree.node(trakStruct->children()[3]);
    QVERIFY(trakPayloadNode.has_value());

    // 2.1 tkhd (v1)
    const auto tkhdStruct = tree.node(trakPayloadNode->children()[0]);
    QVERIFY(tkhdStruct.has_value());
    const auto tkhdVersion = tree.node(tkhdStruct->children()[2]);
    QCOMPARE(tkhdVersion->value().toULongLong(), quint64{1});
    const auto tkhdCreationTime = tree.node(tkhdStruct->children()[4]);
    QCOMPARE(tkhdCreationTime->value().toULongLong(), quint64{0x100000000});
    const auto tkhdTrackId = tree.node(tkhdStruct->children()[6]);
    QCOMPARE(tkhdTrackId->value().toULongLong(), quint64{2});
    const auto tkhdDuration = tree.node(tkhdStruct->children()[8]);
    QCOMPARE(tkhdDuration->value().toULongLong(), quint64{0x200000000});

    // 2.2 edts -> elst (v1 with 2 entries)
    const auto edtsStruct = tree.node(trakPayloadNode->children()[1]);
    QVERIFY(edtsStruct.has_value());
    const auto edtsPayloadNode = tree.node(edtsStruct->children()[3]);
    QVERIFY(edtsPayloadNode.has_value());
    QCOMPARE(edtsPayloadNode->children().size(), std::size_t{1});

    const auto elstStruct = tree.node(edtsPayloadNode->children()[0]);
    QVERIFY(elstStruct.has_value());

    const auto elstVersion = tree.node(elstStruct->children()[2]);
    QCOMPARE(elstVersion->value().toULongLong(), quint64{1});
    const auto elstEntryCount = tree.node(elstStruct->children()[4]);
    QCOMPARE(elstEntryCount->value().toULongLong(), quint64{2});

    const auto elstSegDur0 = tree.node(elstStruct->children()[5]);
    QCOMPARE(elstSegDur0->value().toULongLong(), quint64{0x100000000});
    const auto elstMediaTime0 = tree.node(elstStruct->children()[6]);
    QCOMPARE(elstMediaTime0->value().toULongLong(), quint64{0xFFFFFFFFFFFFFFFF});

    const auto elstSegDur1 = tree.node(elstStruct->children()[9]);
    QCOMPARE(elstSegDur1->value().toULongLong(), quint64{0x100000000});
    const auto elstMediaTime1 = tree.node(elstStruct->children()[10]);
    QCOMPARE(elstMediaTime1->value().toULongLong(), quint64{0});

    // 2.3 mdia -> mdhd (v1)
    const auto mdiaStruct = tree.node(trakPayloadNode->children()[2]);
    QVERIFY(mdiaStruct.has_value());
    const auto mdiaPayloadNode = tree.node(mdiaStruct->children()[3]);
    QVERIFY(mdiaPayloadNode.has_value());

    const auto mdhdStruct = tree.node(mdiaPayloadNode->children()[0]);
    QVERIFY(mdhdStruct.has_value());
    const auto mdhdVersion = tree.node(mdhdStruct->children()[2]);
    QCOMPARE(mdhdVersion->value().toULongLong(), quint64{1});
    const auto mdhdCreationTime = tree.node(mdhdStruct->children()[4]);
    QCOMPARE(mdhdCreationTime->value().toULongLong(), quint64{0x100000000});
    const auto mdhdTimescale = tree.node(mdhdStruct->children()[6]);
    QCOMPARE(mdhdTimescale->value().toULongLong(), quint64{48000});
    const auto mdhdDuration = tree.node(mdhdStruct->children()[7]);
    QCOMPARE(mdhdDuration->value().toULongLong(), quint64{0x200000000});

    // 2.4 hdlr ('soun')
    const auto hdlrStruct = tree.node(mdiaPayloadNode->children()[1]);
    QVERIFY(hdlrStruct.has_value());
    const auto hdlrType = tree.node(hdlrStruct->children()[5]);
    QCOMPARE(hdlrType->value().toULongLong(), quint64{0x736F756E});

    QCOMPARE(analyzer->tree().node(analyzer->tree().rootId())->diagnostics().size(), std::size_t{0});
}

void Mp4IsobmffAnalyzerTest::handlesLargeSizeHandlerReferenceBoxWireOrder() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5f_large_hdlr_v0.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto& tree = analyzer->tree();
    const auto moov = tree.node(batch.boxNodes[1]);
    QVERIFY(moov.has_value());
    const auto moovStruct = tree.node(moov->children().front());
    QVERIFY(moovStruct.has_value());
    const auto moovPayload = tree.node(moovStruct->children().back());
    QVERIFY(moovPayload.has_value());

    const auto mdiaStruct = tree.node(moovPayload->children().front());
    QVERIFY(mdiaStruct.has_value());
    const auto mdiaPayload = tree.node(mdiaStruct->children().back());
    QVERIFY(mdiaPayload.has_value());

    const auto hdlrStruct = tree.node(mdiaPayload->children().front());
    QVERIFY(hdlrStruct.has_value());
    std::vector<QString> fieldNames;
    for (const auto childId : hdlrStruct->children()) {
        const auto child = tree.node(childId);
        QVERIFY(child.has_value());
        fieldNames.push_back(child->name());
    }
    const std::vector<QString> expectedFieldNames = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("hdlr_largesize"),
        QStringLiteral("hdlr_version"),
        QStringLiteral("hdlr_flags"),
        QStringLiteral("hdlr_pre_defined"),
        QStringLiteral("hdlr_handler_type"),
        QStringLiteral("hdlr_reserved_0"),
        QStringLiteral("hdlr_reserved_1"),
        QStringLiteral("hdlr_reserved_2"),
        QStringLiteral("hdlr_large_name_bytes"),
        QStringLiteral("hdlr_large_name"),
    };
    QCOMPARE(fieldNames, expectedFieldNames);

    const auto largeSize = tree.node(hdlrStruct->children()[2]);
    QVERIFY(largeSize->location().has_value());
    QCOMPARE(largeSize->value().toULongLong(), quint64{58});
    QCOMPARE(largeSize->location()->logicalRange().start().bitOffset(), quint64{64});
    QCOMPARE(largeSize->location()->logicalRange().bitLength(), quint64{64});

    const auto version = tree.node(hdlrStruct->children()[3]);
    QCOMPARE(version->value().toULongLong(), quint64{0});
    QCOMPARE(version->location()->logicalRange().start().bitOffset(), quint64{128});

    const auto handlerType = tree.node(hdlrStruct->children()[6]);
    QCOMPARE(handlerType->value().toULongLong(), quint64{0x76696465});
    QCOMPARE(handlerType->location()->logicalRange().start().bitOffset(), quint64{192});

    const auto nameBytes = tree.node(hdlrStruct->children()[10]);
    QCOMPARE(nameBytes->value().toULongLong(), quint64{18});
    const auto name = tree.node(hdlrStruct->children()[11]);
    QVERIFY(name->location().has_value());
    QCOMPARE(name->location()->logicalRange().start().bitOffset(), quint64{320});
    QCOMPARE(name->location()->logicalRange().bitLength(), quint64{144});
    QCOMPARE(hdlrStruct->state(), streamview::core::MaterializationState::Materialized);
    QVERIFY(hdlrStruct->diagnostics().empty());
}

void Mp4IsobmffAnalyzerTest::rejectsUnsupportedFullBoxVersionWithoutV0Fallback() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5f_unsupported_version.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto& tree = analyzer->tree();
    const auto mvhd = tree.node(batch.boxNodes[1]);
    QVERIFY(mvhd.has_value());
    const auto mvhdStruct = tree.node(mvhd->children().front());
    QVERIFY(mvhdStruct.has_value());
    QCOMPARE(mvhdStruct->state(), streamview::core::MaterializationState::Unsupported);

    std::vector<QString> fieldNames;
    for (const auto childId : mvhdStruct->children()) {
        const auto child = tree.node(childId);
        QVERIFY(child.has_value());
        fieldNames.push_back(child->name());
    }
    const std::vector<QString> expectedFieldNames = {
        QStringLiteral("size"),
        QStringLiteral("type"),
        QStringLiteral("mvhd_version"),
        QStringLiteral("mvhd_flags"),
    };
    QCOMPARE(fieldNames, expectedFieldNames);
    QCOMPARE(mvhdStruct->diagnostics().size(), std::size_t{1});
    const auto& diagnostic = mvhdStruct->diagnostics().front();
    QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::UnsupportedSyntax);
    QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
    QCOMPARE(diagnostic.fieldPath, QStringLiteral("Box.mvhd_version"));

    const auto mdat = tree.node(batch.boxNodes[2]);
    QVERIFY(mdat.has_value());
    const auto mdatStruct = tree.node(mdat->children().front());
    QVERIFY(mdatStruct.has_value());
    QCOMPARE(mdatStruct->state(), streamview::core::MaterializationState::Materialized);
}

void Mp4IsobmffAnalyzerTest::analyzesSampleTableBoxesV0() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5g_sample_tables_v0.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto& tree = analyzer->tree();

    // Box 1: moov
    const auto moov = tree.node(batch.boxNodes[1]);
    QVERIFY(moov.has_value());
    const auto moovStruct = tree.node(moov->children().front());
    QVERIFY(moovStruct.has_value());
    const auto moovPayload = tree.node(moovStruct->children().back());
    QVERIFY(moovPayload.has_value());

    // trak -> mdia -> minf -> stbl
    const auto trakStruct = tree.node(moovPayload->children().front());
    QVERIFY(trakStruct.has_value());
    const auto trakPayload = tree.node(trakStruct->children().back());
    QVERIFY(trakPayload.has_value());

    const auto mdiaStruct = tree.node(trakPayload->children().front());
    QVERIFY(mdiaStruct.has_value());
    const auto mdiaPayload = tree.node(mdiaStruct->children().back());
    QVERIFY(mdiaPayload.has_value());

    const auto minfStruct = tree.node(mdiaPayload->children().front());
    QVERIFY(minfStruct.has_value());
    const auto minfPayload = tree.node(minfStruct->children().back());
    QVERIFY(minfPayload.has_value());

    const auto stblStruct = tree.node(minfPayload->children().front());
    QVERIFY(stblStruct.has_value());
    const auto stblPayload = tree.node(stblStruct->children().back());
    QVERIFY(stblPayload.has_value());
    QCOMPARE(stblPayload->children().size(), std::size_t{5});

    // 1. stts
    const auto sttsBox = tree.node(stblPayload->children()[0]);
    QVERIFY(sttsBox.has_value());
    const auto sttsPayload = tree.node(sttsBox->children().back());
    QVERIFY(sttsPayload.has_value());
    const auto sttsStruct = tree.node(sttsPayload->children().front());
    QVERIFY(sttsStruct.has_value());
    std::vector<QString> sttsFieldNames;
    for (const auto childId : sttsStruct->children()) {
        sttsFieldNames.push_back(tree.node(childId)->name());
    }
    const std::vector<QString> expectedSttsFieldNames = {
        QStringLiteral("version"),
        QStringLiteral("flags"),
        QStringLiteral("entry_count"),
        QStringLiteral("table_bytes"),
        QStringLiteral("entries"),
    };
    QCOMPARE(sttsFieldNames, expectedSttsFieldNames);
    QCOMPARE(tree.node(sttsStruct->children()[0])->value().toULongLong(), quint64{0});
    QCOMPARE(tree.node(sttsStruct->children()[1])->value().toULongLong(), quint64{0});
    QCOMPARE(tree.node(sttsStruct->children()[2])->value().toULongLong(), quint64{2});
    QCOMPARE(tree.node(sttsStruct->children()[3])->value().toULongLong(), quint64{16});
    const auto sttsWindow = tree.node(sttsStruct->children()[4]);
    QVERIFY(sttsWindow->metadata().window.has_value());
    QCOMPARE(sttsWindow->metadata().window->entryCount, quint64{2});
    QCOMPARE(sttsWindow->metadata().window->entrySizeBits, quint32{64});

    // 2. stsc
    const auto stscBox = tree.node(stblPayload->children()[1]);
    QVERIFY(stscBox.has_value());
    const auto stscPayload = tree.node(stscBox->children().back());
    QVERIFY(stscPayload.has_value());
    const auto stscStruct = tree.node(stscPayload->children().front());
    QVERIFY(stscStruct.has_value());
    std::vector<QString> stscFieldNames;
    for (const auto childId : stscStruct->children()) {
        stscFieldNames.push_back(tree.node(childId)->name());
    }
    const std::vector<QString> expectedStscFieldNames = {
        QStringLiteral("version"),
        QStringLiteral("flags"),
        QStringLiteral("entry_count"),
        QStringLiteral("table_bytes"),
        QStringLiteral("entries"),
    };
    QCOMPARE(stscFieldNames, expectedStscFieldNames);
    QCOMPARE(tree.node(stscStruct->children()[0])->value().toULongLong(), quint64{0});
    QCOMPARE(tree.node(stscStruct->children()[2])->value().toULongLong(), quint64{2});
    QCOMPARE(tree.node(stscStruct->children()[3])->value().toULongLong(), quint64{24});
    const auto stscWindow = tree.node(stscStruct->children()[4]);
    QVERIFY(stscWindow->metadata().window.has_value());
    QCOMPARE(stscWindow->metadata().window->entryCount, quint64{2});
    QCOMPARE(stscWindow->metadata().window->entrySizeBits, quint32{96});

    // 3. stsz
    const auto stszBox = tree.node(stblPayload->children()[2]);
    QVERIFY(stszBox.has_value());
    const auto stszPayload = tree.node(stszBox->children().back());
    QVERIFY(stszPayload.has_value());
    const auto stszStruct = tree.node(stszPayload->children().front());
    QVERIFY(stszStruct.has_value());
    std::vector<QString> stszFieldNames;
    for (const auto childId : stszStruct->children()) {
        stszFieldNames.push_back(tree.node(childId)->name());
    }
    const std::vector<QString> expectedStszFieldNames = {
        QStringLiteral("version"),
        QStringLiteral("flags"),
        QStringLiteral("sample_size"),
        QStringLiteral("sample_count"),
        QStringLiteral("table_bytes"),
        QStringLiteral("entries"),
    };
    QCOMPARE(stszFieldNames, expectedStszFieldNames);
    QCOMPARE(tree.node(stszStruct->children()[0])->value().toULongLong(), quint64{0});
    QCOMPARE(tree.node(stszStruct->children()[2])->value().toULongLong(), quint64{0});
    QCOMPARE(tree.node(stszStruct->children()[3])->value().toULongLong(), quint64{3});
    QCOMPARE(tree.node(stszStruct->children()[4])->value().toULongLong(), quint64{12});
    const auto stszWindow = tree.node(stszStruct->children()[5]);
    QVERIFY(stszWindow->metadata().window.has_value());
    QCOMPARE(stszWindow->metadata().window->entryCount, quint64{3});
    QCOMPARE(stszWindow->metadata().window->entrySizeBits, quint32{32});

    // 4. stco
    const auto stcoBox = tree.node(stblPayload->children()[3]);
    QVERIFY(stcoBox.has_value());
    const auto stcoPayload = tree.node(stcoBox->children().back());
    QVERIFY(stcoPayload.has_value());
    const auto stcoStruct = tree.node(stcoPayload->children().front());
    QVERIFY(stcoStruct.has_value());
    std::vector<QString> stcoFieldNames;
    for (const auto childId : stcoStruct->children()) {
        stcoFieldNames.push_back(tree.node(childId)->name());
    }
    const std::vector<QString> expectedStcoFieldNames = {
        QStringLiteral("version"),
        QStringLiteral("flags"),
        QStringLiteral("entry_count"),
        QStringLiteral("table_bytes"),
        QStringLiteral("entries"),
    };
    QCOMPARE(stcoFieldNames, expectedStcoFieldNames);
    QCOMPARE(tree.node(stcoStruct->children()[0])->value().toULongLong(), quint64{0});
    QCOMPARE(tree.node(stcoStruct->children()[2])->value().toULongLong(), quint64{2});
    QCOMPARE(tree.node(stcoStruct->children()[3])->value().toULongLong(), quint64{8});
    const auto stcoWindow = tree.node(stcoStruct->children()[4]);
    QVERIFY(stcoWindow->metadata().window.has_value());
    QCOMPARE(stcoWindow->metadata().window->entryCount, quint64{2});
    QCOMPARE(stcoWindow->metadata().window->entrySizeBits, quint32{32});

    // 5. co64
    const auto co64Box = tree.node(stblPayload->children()[4]);
    QVERIFY(co64Box.has_value());
    const auto co64Payload = tree.node(co64Box->children().back());
    QVERIFY(co64Payload.has_value());
    const auto co64Struct = tree.node(co64Payload->children().front());
    QVERIFY(co64Struct.has_value());
    std::vector<QString> co64FieldNames;
    for (const auto childId : co64Struct->children()) {
        co64FieldNames.push_back(tree.node(childId)->name());
    }
    const std::vector<QString> expectedCo64FieldNames = {
        QStringLiteral("version"),
        QStringLiteral("flags"),
        QStringLiteral("entry_count"),
        QStringLiteral("table_bytes"),
        QStringLiteral("entries"),
    };
    QCOMPARE(co64FieldNames, expectedCo64FieldNames);
    QCOMPARE(tree.node(co64Struct->children()[0])->value().toULongLong(), quint64{0});
    QCOMPARE(tree.node(co64Struct->children()[2])->value().toULongLong(), quint64{2});
    QCOMPARE(tree.node(co64Struct->children()[3])->value().toULongLong(), quint64{16});
    const auto co64Window = tree.node(co64Struct->children()[4]);
    QVERIFY(co64Window->metadata().window.has_value());
    QCOMPARE(co64Window->metadata().window->entryCount, quint64{2});
    QCOMPARE(co64Window->metadata().window->entrySizeBits, quint32{64});

    QCOMPARE(analyzer->tree().node(analyzer->tree().rootId())->diagnostics().size(), std::size_t{0});
}

void Mp4IsobmffAnalyzerTest::decodesAllSampleTableWindowsWithSourceSpans() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5g_sample_tables_v0.mp4"));
    MemorySource source(bytes);
    QString errorMessage;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    const auto& tree = analyzer->tree();

    struct Expected final {
        quint64 type;
        quint64 windowStartBits;
        quint32 entrySizeBits;
        quint64 entryCount;
        quint64 firstValue;
    };
    const std::vector<Expected> expected = {
        {0x73747473, 64, 64, 2, 10},
        {0x73747363, 64, 96, 2, 1},
        {0x7374737A, 96, 32, 3, 100},
        {0x7374636F, 64, 32, 2, 1000},
        {0x636F3634, 64, 64, 2, 0x100000000ULL},
    };

    for (const auto& expectation : expected) {
        const auto windowNodeId = findSampleTableWindow(tree, batch, expectation.type);
        QVERIFY(windowNodeId.has_value());

        const auto window = tree.node(*windowNodeId);
        QVERIFY(window.has_value());
        QVERIFY(window->location().has_value());
        QCOMPARE(window->location()->logicalRange().start().bitOffset(),
                 expectation.windowStartBits);
        QCOMPARE(window->location()->logicalRange().bitLength(),
                 static_cast<quint64>(expectation.entrySizeBits) * expectation.entryCount);

        auto decoder = analyzer->windowDecoder(*windowNodeId);
        QVERIFY(decoder.has_value());
        const auto result = decoder->decodeWindow({0, 1});
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::Materialized);
        QCOMPARE(result.decodedEntryCount, quint64{1});
        QCOMPARE(result.entryNodes.size(), std::size_t{1});

        const auto entry = tree.node(result.entryNodes.front());
        QVERIFY(entry.has_value());
        QVERIFY(!entry->children().empty());
        quint64 sourceBits = 0;
        for (const auto fieldId : entry->children()) {
            const auto field = tree.node(fieldId);
            QVERIFY(field.has_value());
            QVERIFY(field->location().has_value());
            for (const auto& span : field->location()->sourceSpans()) sourceBits += span.bitLength();
        }
        QCOMPARE(sourceBits, static_cast<quint64>(expectation.entrySizeBits));

        const auto firstField = tree.node(entry->children().front());
        QVERIFY(firstField.has_value());
        QCOMPARE(firstField->location()->logicalRange().start().bitOffset(), quint64{0});
        QCOMPARE(firstField->value().toULongLong(), expectation.firstValue);
    }
}

void Mp4IsobmffAnalyzerTest::analyzesLargeSizeAndEofSampleTableBoxes() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5g_largesize_and_eof_tables.mp4"));
    MemorySource source(bytes);
    QString errorMessage;
    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
    QCOMPARE(batch.boxNodes.size(), std::size_t{3});

    const auto sttsWindowId = findSampleTableWindow(analyzer->tree(), batch, 0x73747473);
    const auto stszWindowId = findSampleTableWindow(analyzer->tree(), batch, 0x7374737A);
    QVERIFY(sttsWindowId.has_value());
    QVERIFY(stszWindowId.has_value());

    const auto sttsWindow = analyzer->tree().node(*sttsWindowId);
    const auto stszWindow = analyzer->tree().node(*stszWindowId);
    QVERIFY(sttsWindow.has_value());
    QVERIFY(stszWindow.has_value());
    QCOMPARE(sttsWindow->metadata().window->entryCount, quint64{2});
    QCOMPARE(sttsWindow->metadata().window->entrySizeBits, quint32{64});
    QCOMPARE(stszWindow->metadata().window->entryCount, quint64{2});
    QCOMPARE(stszWindow->metadata().window->entrySizeBits, quint32{32});

    auto sttsDecoder = analyzer->windowDecoder(*sttsWindowId);
    auto stszDecoder = analyzer->windowDecoder(*stszWindowId);
    QVERIFY(sttsDecoder.has_value());
    QVERIFY(stszDecoder.has_value());
    const auto sttsResult = sttsDecoder->decodeWindow({0, 2});
    const auto stszResult = stszDecoder->decodeWindow({0, 2});
    QCOMPARE(sttsResult.status, streamview::rules::DslExecutionStatus::Materialized);
    QCOMPARE(stszResult.status, streamview::rules::DslExecutionStatus::Materialized);

    const auto sttsFirst = analyzer->tree().node(sttsResult.entryNodes.front());
    const auto stszFirst = analyzer->tree().node(stszResult.entryNodes.front());
    QVERIFY(sttsFirst.has_value());
    QVERIFY(stszFirst.has_value());
    QCOMPARE(analyzer->tree().node(sttsFirst->children().front())->value().toULongLong(), quint64{7});
    QCOMPARE(analyzer->tree().node(stszFirst->children().front())->value().toULongLong(), quint64{111});
}

void Mp4IsobmffAnalyzerTest::analyzesSampleSizeUniformWithoutTable() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5g_stsz_uniform.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);

    const auto& tree = analyzer->tree();
    const auto moov = tree.node(batch.boxNodes[1]);
    QVERIFY(moov.has_value());
    const auto moovStruct = tree.node(moov->children().front());
    QVERIFY(moovStruct.has_value());
    const auto moovPayload = tree.node(moovStruct->children().back());
    QVERIFY(moovPayload.has_value());

    const auto trakStruct = tree.node(moovPayload->children().front());
    QVERIFY(trakStruct.has_value());
    const auto trakPayload = tree.node(trakStruct->children().back());
    QVERIFY(trakPayload.has_value());

    const auto mdiaStruct = tree.node(trakPayload->children().front());
    QVERIFY(mdiaStruct.has_value());
    const auto mdiaPayload = tree.node(mdiaStruct->children().back());
    QVERIFY(mdiaPayload.has_value());

    const auto minfStruct = tree.node(mdiaPayload->children().front());
    QVERIFY(minfStruct.has_value());
    const auto minfPayload = tree.node(minfStruct->children().back());
    QVERIFY(minfPayload.has_value());

    const auto stblStruct = tree.node(minfPayload->children().front());
    QVERIFY(stblStruct.has_value());
    const auto stblPayload = tree.node(stblStruct->children().back());
    QVERIFY(stblPayload.has_value());

    const auto stszBox = tree.node(stblPayload->children().front());
    QVERIFY(stszBox.has_value());
    const auto stszPayload = tree.node(stszBox->children().back());
    QVERIFY(stszPayload.has_value());
    const auto stszStruct = tree.node(stszPayload->children().front());
    QVERIFY(stszStruct.has_value());

    std::vector<QString> stszFieldNames;
    for (const auto childId : stszStruct->children()) {
        stszFieldNames.push_back(tree.node(childId)->name());
    }
    const std::vector<QString> expectedStszFieldNames = {
        QStringLiteral("version"),
        QStringLiteral("flags"),
        QStringLiteral("sample_size"),
        QStringLiteral("sample_count"),
    };
    QCOMPARE(stszFieldNames, expectedStszFieldNames);
    QCOMPARE(tree.node(stszStruct->children()[0])->value().toULongLong(), quint64{0});
    QCOMPARE(tree.node(stszStruct->children()[1])->value().toULongLong(), quint64{0});
    QCOMPARE(tree.node(stszStruct->children()[2])->value().toULongLong(), quint64{1024});
    QCOMPARE(tree.node(stszStruct->children()[3])->value().toULongLong(), quint64{50});
    QCOMPARE(analyzer->tree().node(analyzer->tree().rootId())->diagnostics().size(), std::size_t{0});
}

void Mp4IsobmffAnalyzerTest::decodesSampleTableWindowsWithPagingAndBudget() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5g_large_sample_table.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);

    const auto& tree = analyzer->tree();
    // Locate stbl
    const auto moov = tree.node(batch.boxNodes[2]); // free is 1, moov is 2
    QVERIFY(moov.has_value());
    const auto moovStruct = tree.node(moov->children().front());
    QVERIFY(moovStruct.has_value());
    const auto moovPayload = tree.node(moovStruct->children().back());
    QVERIFY(moovPayload.has_value());

    const auto trakStruct = tree.node(moovPayload->children().front());
    const auto trakPayload = tree.node(trakStruct->children().back());
    const auto mdiaStruct = tree.node(trakPayload->children().front());
    const auto mdiaPayload = tree.node(mdiaStruct->children().back());
    const auto minfStruct = tree.node(mdiaPayload->children().front());
    const auto minfPayload = tree.node(minfStruct->children().back());
    const auto stblStruct = tree.node(minfPayload->children().front());
    const auto stblPayload = tree.node(stblStruct->children().back());

    const auto stszBox = tree.node(stblPayload->children()[1]);
    QVERIFY(stszBox.has_value());
    const auto stszPayload = tree.node(stszBox->children().back());
    QVERIFY(stszPayload.has_value());
    const auto stszStruct = tree.node(stszPayload->children().front());
    QVERIFY(stszStruct.has_value());

    const auto windowNodeId = stszStruct->children()[5];
    const auto windowNode = tree.node(windowNodeId);
    QVERIFY(windowNode.has_value());
    QVERIFY(windowNode->metadata().window.has_value());
    QCOMPARE(windowNode->metadata().window->entryCount, quint64{200});

    auto decoder = analyzer->windowDecoder(windowNodeId);
    QVERIFY(decoder.has_value());

    // Page 0: decode 100 entries.
    streamview::rules::WindowDecodeRequest req1;
    req1.pageIndex = 0;
    req1.pageSize = 100;
    auto res1 = decoder->decodeWindow(req1);
    QCOMPARE(res1.status, streamview::rules::DslExecutionStatus::Materialized);
    QCOMPARE(res1.decodedEntryCount, quint64{100});
    auto winNodeAfter1 = analyzer->tree().node(windowNodeId);
    QVERIFY(winNodeAfter1.has_value());
    QCOMPARE(winNodeAfter1->children().size(), std::size_t{100});

    const auto firstEntry = analyzer->tree().node(winNodeAfter1->children().front());
    QVERIFY(firstEntry.has_value());
    const auto firstEntryVal = analyzer->tree().node(firstEntry->children().front());
    QVERIFY(firstEntryVal.has_value());
    QVERIFY(firstEntryVal->location().has_value());
    QCOMPARE(firstEntryVal->location()->logicalRange().start().bitOffset(), quint64{0});
    QCOMPARE(firstEntryVal->location()->logicalRange().bitLength(), quint64{32});
    QVERIFY(!firstEntryVal->location()->sourceSpans().empty());
    QCOMPARE(firstEntryVal->value().toULongLong(), quint64{500});

    // Page 1 with a smaller page size overlaps entries 75..99 and adds 50 new entries.
    streamview::rules::WindowDecodeRequest req2;
    req2.pageIndex = 1;
    req2.pageSize = 75;
    auto res2 = decoder->decodeWindow(req2);
    QCOMPARE(res2.status, streamview::rules::DslExecutionStatus::Materialized);
    QCOMPARE(res2.decodedEntryCount, quint64{75});
    QCOMPARE(res2.entryNodes.size(), std::size_t{75});
    for (std::size_t i = 0; i < 25; ++i) {
        QCOMPARE(res2.entryNodes[i], res1.entryNodes[i + 75]);
    }
    auto winNodeAfter2 = analyzer->tree().node(windowNodeId);
    QVERIFY(winNodeAfter2.has_value());
    QCOMPARE(winNodeAfter2->children().size(), std::size_t{150});

    // Repeat Page 0: should reuse nodes without duplicate allocation
    auto res3 = decoder->decodeWindow(req1);
    QCOMPARE(res3.status, streamview::rules::DslExecutionStatus::Materialized);
    QCOMPARE(res3.entryNodes, res1.entryNodes);
    auto winNodeAfter3 = analyzer->tree().node(windowNodeId);
    QVERIFY(winNodeAfter3.has_value());
    QCOMPARE(winNodeAfter3->children().size(), std::size_t{150});

    // Budget limit enforcement
    const auto nodeCountBeforeBudget = analyzer->tree().nodeCount();
    analyzer->budget()->remainingNodes = 5;
    streamview::rules::WindowDecodeRequest req3;
    req3.pageIndex = 3;
    req3.pageSize = 50;
    auto res4 = decoder->decodeWindow(req3);
    QCOMPARE(res4.status, streamview::rules::DslExecutionStatus::ResourceLimit);
    QCOMPARE(res4.decodedEntryCount, quint64{2});
    QCOMPARE(analyzer->tree().node(windowNodeId)->children().size(), std::size_t{152});
    QCOMPARE(analyzer->tree().nodeCount(), nodeCountBeforeBudget + 4U);
}

void Mp4IsobmffAnalyzerTest::handlesSampleTableWindowFailures() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5g_sample_tables_v0.mp4"));

    {
        MemorySource source(bytes);
        QString errorMessage;
        auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));
        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
        const auto windowNodeId = findSampleTableWindow(analyzer->tree(), batch, 0x73747473);
        QVERIFY(windowNodeId.has_value());

        streamview::core::CancellationSource cancellation;
        analyzer->budget()->cancellation = cancellation.token();
        (void)cancellation.requestCancellation();
        auto decoder = analyzer->windowDecoder(*windowNodeId);
        QVERIFY(decoder.has_value());
        const auto result = decoder->decodeWindow({0, 1});
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::Cancelled);
        QCOMPARE(analyzer->tree().node(*windowNodeId)->children().size(), std::size_t{0});
    }

    {
        ToggleFailureSource source(bytes);
        QString errorMessage;
        auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));
        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
        const auto windowNodeId = findSampleTableWindow(analyzer->tree(), batch, 0x73747473);
        QVERIFY(windowNodeId.has_value());
        source.failReads(true);

        auto decoder = analyzer->windowDecoder(*windowNodeId);
        QVERIFY(decoder.has_value());
        const auto result = decoder->decodeWindow({0, 1});
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::SourceError);
        QCOMPARE(analyzer->tree().node(*windowNodeId)->children().size(), std::size_t{0});
    }

    {
        ToggleFailureSource source(bytes);
        QString errorMessage;
        auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));
        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);
        const auto windowNodeId = findSampleTableWindow(analyzer->tree(), batch, 0x7374737A);
        QVERIFY(windowNodeId.has_value());
        const auto window = analyzer->tree().node(*windowNodeId);
        QVERIFY(window.has_value());
        QVERIFY(window->location().has_value());
        QVERIFY(!window->location()->sourceSpans().empty());
        const auto firstEntryByte = window->location()->sourceSpans().front().start().byteOffset();
        source.setVisibleSize(firstEntryByte + 4U);

        auto decoder = analyzer->windowDecoder(*windowNodeId);
        QVERIFY(decoder.has_value());
        const auto result = decoder->decodeWindow({0, 3});
        QCOMPARE(result.status, streamview::rules::DslExecutionStatus::TruncatedSource);
        QCOMPARE(result.decodedEntryCount, quint64{1});
        QCOMPARE(analyzer->tree().node(*windowNodeId)->children().size(), std::size_t{1});
    }
}

void Mp4IsobmffAnalyzerTest::handlesUnsupportedSampleTableVersions() {
    const auto bytes = readFixtureBytes(QStringLiteral("mp4_p5g_unsupported_stbl_versions.mp4"));
    MemorySource source(bytes);
    QString errorMessage;

    auto analyzer = streamview::rules::Mp4IsobmffAnalyzer::create(source, &errorMessage);
    QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

    const auto batch = analyzer->analyzeBatch();
    QCOMPARE(batch.status, streamview::rules::Mp4IsobmffAnalysisStatus::Complete);

    const auto& tree = analyzer->tree();
    const auto moov = tree.node(batch.boxNodes[1]);
    QVERIFY(moov.has_value());
    const auto moovStruct = tree.node(moov->children().front());
    QVERIFY(moovStruct.has_value());
    const auto moovPayload = tree.node(moovStruct->children().back());
    QVERIFY(moovPayload.has_value());

    const auto trakStruct = tree.node(moovPayload->children().front());
    const auto trakPayload = tree.node(trakStruct->children().back());
    const auto mdiaStruct = tree.node(trakPayload->children().front());
    const auto mdiaPayload = tree.node(mdiaStruct->children().back());
    const auto minfStruct = tree.node(mdiaPayload->children().front());
    const auto minfPayload = tree.node(minfStruct->children().back());
    const auto stblStruct = tree.node(minfPayload->children().front());
    const auto stblPayload = tree.node(stblStruct->children().back());

    const QString expectedFieldPaths[5] = {
        QStringLiteral("TimeToSampleBox.version"),
        QStringLiteral("SampleToChunkBox.version"),
        QStringLiteral("SampleSizeBox.version"),
        QStringLiteral("ChunkOffsetBox.version"),
        QStringLiteral("ChunkLargeOffsetBox.version"),
    };

    for (std::size_t i = 0; i < 5; ++i) {
        const auto box = tree.node(stblPayload->children()[i]);
        QVERIFY(box.has_value());
        const auto payload = tree.node(box->children().back());
        QVERIFY(payload.has_value());
        const auto payloadStruct = tree.node(payload->children().front());
        QVERIFY(payloadStruct.has_value());
        QCOMPARE(payloadStruct->state(), streamview::core::MaterializationState::Unsupported);
        QVERIFY(!payloadStruct->diagnostics().empty());
        bool foundUnsupported = false;
        for (const auto& diag : payloadStruct->diagnostics()) {
            if (diag.code == streamview::core::DiagnosticCode::UnsupportedSyntax &&
                diag.fieldPath == expectedFieldPaths[i]) {
                foundUnsupported = true;
                break;
            }
        }
        QVERIFY2(foundUnsupported, qPrintable(QStringLiteral("Missing UnsupportedSyntax diagnostic for %1").arg(expectedFieldPaths[i])));
    }
}

QTEST_MAIN(Mp4IsobmffAnalyzerTest)
#include "mp4_isobmff_analyzer_test.moc"
