#include <streamview/rules/aac_adts_analyzer.h>

#include <streamview/core/cancellation.h>
#include <streamview/core/source.h>
#include <streamview/core/version.h>
#include <streamview/rules/language_version.h>
#include <streamview/rules/rule_catalog.h>

#include <QObject>
#include <QTest>

#include <algorithm>
#include <cstddef>
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

[[nodiscard]] std::vector<std::byte> makeAdtsFrame(quint16 frameLength,
                                                    bool protectionAbsent = true,
                                                    quint8 profile = 1,
                                                    quint8 samplingFreqIndex = 4,
                                                    quint8 channelConfig = 2,
                                                    std::byte fillByte = std::byte{0xAB}) {
    const std::size_t headerLen = protectionAbsent ? 7U : 9U;
    std::vector<std::byte> frame(frameLength, fillByte);
    if (frameLength < headerLen) {
        return frame;
    }
    frame[0] = std::byte{0xFF};
    frame[1] = std::byte{static_cast<quint8>(0xF0U | (protectionAbsent ? 1U : 0U))};
    frame[2] = std::byte{static_cast<quint8>(
        ((profile & 0x03U) << 6U) |
        ((samplingFreqIndex & 0x0FU) << 2U) |
        ((channelConfig >> 2U) & 0x01U))};
    frame[3] = std::byte{static_cast<quint8>(
        ((channelConfig & 0x03U) << 6U) |
        ((frameLength >> 11U) & 0x03U))};
    frame[4] = std::byte{static_cast<quint8>((frameLength >> 3U) & 0xFFU)};
    frame[5] = std::byte{static_cast<quint8>(
        ((frameLength & 0x07U) << 5U) | 0x1FU)};
    frame[6] = std::byte{0xFC};
    if (!protectionAbsent) {
        frame[7] = std::byte{0x12};
        frame[8] = std::byte{0x34};
    }
    return frame;
}

[[nodiscard]] streamview::rules::RulePackageLoadResult makeTestAacPackage() {
    const QByteArray toml = QByteArrayLiteral(
        "manifest-version = 1\n\n"
        "[package]\n"
        "id = \"org.streamview.aac\"\n"
        "version = \"0.1.0\"\n"
        "authors = [\"StreamView Contributors\"]\n"
        "license = \"Apache-2.0\"\n"
        "dependencies = []\n\n"
        "[compatibility]\n"
        "language = \"0.1\"\n"
        "engine = \">=0.1.0 <0.2.0\"\n\n"
        "[[entrypoints]]\n"
        "id = \"adts\"\n"
        "format = \"audio.aac.adts\"\n"
        "source = \"src/adts.svfmt\"\n"
        "profiles = [\"aac-adts\"]\n"
        "depth = \"structural\"\n");

    const QByteArray svfmt = QByteArrayLiteral(
        "struct AdtsHeader {\n"
        "    bits<12> syncword @equals(4095);\n"
        "    bits<1> id;\n"
        "    bits<2> layer;\n"
        "    bits<1> protection_absent;\n"
        "}\n\n"
        "@index(progressive) sequence<AdtsHeader> frames = scan(adts_frame);\n"
        "entry frames;\n");

    std::vector<streamview::rules::RulePackageFile> files{
        {QStringLiteral("rule.toml"), toml},
        {QStringLiteral("src/adts.svfmt"), svfmt}};
    return streamview::rules::RulePackage::fromFiles(std::move(files));
}

} // namespace

class AacAdtsAnalyzerTest : public QObject {
    Q_OBJECT

private slots:
    void failsToCreateAnalyzerWhenNoBundledPackageExists() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(150, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        const MemorySource source(std::move(stream));

        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, &error);
        QVERIFY(!analyzer.has_value());
        QVERIFY(!error.isEmpty());
    }

    void failsToCreateAnalyzerWhenResolvedRuleIsInvalid() {
        const MemorySource source(std::vector<std::byte>{std::byte{0x00}});
        streamview::rules::RuleCatalogLookupResult invalidResolved;
        invalidResolved.status = streamview::rules::RuleCatalogLookupStatus::MissingContent;
        invalidResolved.errorMessage = QStringLiteral("Missing rule package content");

        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, invalidResolved, &error);
        QVERIFY(!analyzer.has_value());
        QCOMPARE(error, QStringLiteral("Missing rule package content"));
    }

    void createsAnalyzerFromRulePackageAndDecodesFieldsViaDsl() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(150, true);
        const auto f2 = makeAdtsFrame(200, true);
        const auto f3 = makeAdtsFrame(180, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        auto loadedPkg = makeTestAacPackage();
        QVERIFY2(loadedPkg.succeeded(), qPrintable(loadedPkg.errorMessage));
        const auto identity = loadedPkg.package->identity();

        streamview::rules::RulePackageCatalog catalog;
        QVERIFY(catalog.registerPackage(std::move(*loadedPkg.package)).succeeded());

        const auto resolved = catalog.resolve(
            identity, QStringLiteral("adts"),
            streamview::rules::languageVersion(), streamview::core::version());
        QVERIFY(resolved.succeeded());

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, resolved, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));
        QCOMPARE(analyzer->ruleIdentity().packageIdentity().packageId(),
                 QStringLiteral("org.streamview.aac"));
        QCOMPARE(analyzer->ruleIdentity().entryPointId(), QStringLiteral("adts"));

        const auto batch = analyzer->analyzeBatch();
        QVERIFY(batch.complete());
        QCOMPARE(batch.frameNodes.size(), std::size_t(3));
        QVERIFY(analyzer->finished());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->children().size(), std::size_t(3));

        // Verify frame 0 was materialized and its children decoded by DSL engine
        const auto node1 = analyzer->tree().node(batch.frameNodes[0]);
        QVERIFY(node1.has_value());
        QCOMPARE(node1->name(), QStringLiteral("adts_frame[0]"));
        QVERIFY(node1->location().has_value());
        QCOMPARE(node1->location()->logicalRange().bitLength(), quint64(150 * 8));
        QCOMPARE(node1->children().size(), std::size_t(1));

        const auto headerNode = analyzer->tree().node(node1->children()[0]);
        QVERIFY(headerNode.has_value());
        QCOMPARE(headerNode->name(), QStringLiteral("AdtsHeader"));
        QCOMPARE(headerNode->children().size(), std::size_t(4));

        const auto syncword = analyzer->tree().node(headerNode->children()[0]);
        QVERIFY(syncword.has_value());
        QCOMPARE(syncword->name(), QStringLiteral("syncword"));
        QCOMPARE(syncword->value().toULongLong(), quint64(4095));

        const auto id = analyzer->tree().node(headerNode->children()[1]);
        QVERIFY(id.has_value());
        QCOMPARE(id->name(), QStringLiteral("id"));
        QCOMPARE(id->value().toULongLong(), quint64(0));

        const auto layer = analyzer->tree().node(headerNode->children()[2]);
        QVERIFY(layer.has_value());
        QCOMPARE(layer->name(), QStringLiteral("layer"));
        QCOMPARE(layer->value().toULongLong(), quint64(0));

        const auto protectionAbsent = analyzer->tree().node(headerNode->children()[3]);
        QVERIFY(protectionAbsent.has_value());
        QCOMPARE(protectionAbsent->name(), QStringLiteral("protection_absent"));
        QCOMPARE(protectionAbsent->value().toULongLong(), quint64(1));
    }

    void respectsBatchLimits() {
        std::vector<std::byte> stream;
        const auto f1 = makeAdtsFrame(100, true);
        const auto f2 = makeAdtsFrame(100, true);
        const auto f3 = makeAdtsFrame(100, true);
        stream.insert(stream.end(), f1.begin(), f1.end());
        stream.insert(stream.end(), f2.begin(), f2.end());
        stream.insert(stream.end(), f3.begin(), f3.end());

        auto loadedPkg = makeTestAacPackage();
        QVERIFY2(loadedPkg.succeeded(), qPrintable(loadedPkg.errorMessage));
        const auto identity = loadedPkg.package->identity();

        streamview::rules::RulePackageCatalog catalog;
        QVERIFY(catalog.registerPackage(std::move(*loadedPkg.package)).succeeded());
        const auto resolved = catalog.resolve(
            identity, QStringLiteral("adts"),
            streamview::rules::languageVersion(), streamview::core::version());
        QVERIFY(resolved.succeeded());

        const MemorySource source(std::move(stream));
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, resolved, &error);
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        const auto b1 = analyzer->analyzeBatch(1);
        QCOMPARE(b1.status, streamview::rules::AacAdtsAnalysisStatus::InProgress);
        QCOMPARE(b1.frameNodes.size(), std::size_t(1));

        const auto b2 = analyzer->analyzeBatch(1);
        QCOMPARE(b2.status, streamview::rules::AacAdtsAnalysisStatus::InProgress);
        QCOMPARE(b2.frameNodes.size(), std::size_t(1));

        const auto b3 = analyzer->analyzeBatch(1);
        QCOMPARE(b3.status, streamview::rules::AacAdtsAnalysisStatus::Complete);
        QCOMPARE(b3.frameNodes.size(), std::size_t(1));
        QVERIFY(analyzer->finished());
    }

    void respectsCancellationAndResumes() {
        std::vector<std::byte> stream;
        for (int i = 0; i < 10; ++i) {
            const auto f = makeAdtsFrame(100, true);
            stream.insert(stream.end(), f.begin(), f.end());
        }

        auto loadedPkg = makeTestAacPackage();
        QVERIFY2(loadedPkg.succeeded(), qPrintable(loadedPkg.errorMessage));
        const auto identity = loadedPkg.package->identity();

        streamview::rules::RulePackageCatalog catalog;
        QVERIFY(catalog.registerPackage(std::move(*loadedPkg.package)).succeeded());
        const auto resolved = catalog.resolve(
            identity, QStringLiteral("adts"),
            streamview::rules::languageVersion(), streamview::core::version());
        QVERIFY(resolved.succeeded());

        const MemorySource source(std::move(stream));
        streamview::core::CancellationSource cancelSource;
        QString error;
        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(
            source, resolved, &error, cancelSource.token());
        QVERIFY2(analyzer.has_value(), qPrintable(error));

        (void)cancelSource.requestCancellation();
        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, streamview::rules::AacAdtsAnalysisStatus::Cancelled);

        QVERIFY(analyzer->resumeAfterCancellation());
        const auto resumeBatch = analyzer->analyzeBatch();
        QCOMPARE(resumeBatch.status, streamview::rules::AacAdtsAnalysisStatus::Complete);
        QVERIFY(analyzer->finished());
    }

    void handlesInvalidBatchSize() {
        const MemorySource source(std::vector<std::byte>{std::byte{0x00}});
        auto loadedPkg = makeTestAacPackage();
        QVERIFY(loadedPkg.succeeded());
        const auto identity = loadedPkg.package->identity();

        streamview::rules::RulePackageCatalog catalog;
        QVERIFY(catalog.registerPackage(std::move(*loadedPkg.package)).succeeded());
        const auto resolved = catalog.resolve(
            identity, QStringLiteral("adts"),
            streamview::rules::languageVersion(), streamview::core::version());
        QVERIFY(resolved.succeeded());

        auto analyzer = streamview::rules::AacAdtsAnalyzer::create(source, resolved);
        QVERIFY(analyzer.has_value());

        const auto b1 = analyzer->analyzeBatch(0, 100);
        QCOMPARE(b1.status, streamview::rules::AacAdtsAnalysisStatus::InvalidBatchSize);

        const auto b2 = analyzer->analyzeBatch(10, 0);
        QCOMPARE(b2.status, streamview::rules::AacAdtsAnalysisStatus::InvalidBatchSize);
    }
};

QTEST_MAIN(AacAdtsAnalyzerTest)
#include "aac_adts_analyzer_test.moc"
