#include <streamview/core/coordinates.h>
#include <streamview/core/paged_cache.h>
#include <streamview/core/source_fingerprint.h>
#include <streamview/rules/analysis_cache_payload.h>
#include <streamview/rules/rule_package.h>

#include <QByteArray>
#include <QMetaType>
#include <QTemporaryDir>
#include <QTest>

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using streamview::core::AnalysisNodeId;
using streamview::core::AnalysisNodeKind;
using streamview::core::AnalysisTree;
using streamview::core::DiagnosticCode;
using streamview::core::DiagnosticSeverity;
using streamview::core::FieldLocation;
using streamview::core::LogicalBitAddress;
using streamview::core::LogicalRange;
using streamview::core::LogicalViewId;
using streamview::core::MaterializationState;
using streamview::core::PagedCache;
using streamview::core::PagedCachePageKey;
using streamview::core::PagedCachePageKind;
using streamview::core::PagedCachePageWrite;
using streamview::core::SourceBitAddress;
using streamview::core::SourceFingerprint;
using streamview::core::SourceFingerprintMode;
using streamview::core::SourceSpan;
using streamview::rules::AnalysisCacheBodyDecodeStatus;
using streamview::rules::AnalysisCacheBodyEncodeStatus;
using streamview::rules::AnalysisCacheNamespace;
using streamview::rules::AnalysisCachePageBodyCodec;
using streamview::rules::AnalysisCachePayloadEnvelope;
using streamview::rules::H264ProgressiveIndexCachePage;
using streamview::rules::H264StartCodeRecord;
using streamview::rules::MaterializedResultCacheNode;
using streamview::rules::MaterializedResultCachePage;
using streamview::rules::MaterializedResultCacheExportStatus;
using streamview::rules::RuleEntryPointIdentity;
using streamview::rules::RulePackageIdentity;
using streamview::rules::exportMaterializedResultCachePages;

namespace {

[[nodiscard]] SourceSpan span(quint64 start, quint64 length) {
    const auto result = SourceSpan::create(SourceBitAddress(start), length);
    Q_ASSERT(result.has_value());
    return *result;
}

[[nodiscard]] FieldLocation location(LogicalViewId viewId, quint64 logicalStart,
                                     std::vector<SourceSpan> spans) {
    quint64 length = 0;
    for (const SourceSpan& sourceSpan : spans) {
        length += sourceSpan.bitLength();
    }
    const auto range = LogicalRange::create(LogicalBitAddress(viewId, logicalStart), length);
    Q_ASSERT(range.has_value());
    const auto result = FieldLocation::create(*range, std::move(spans));
    Q_ASSERT(result.has_value());
    return *result;
}

[[nodiscard]] H264StartCodeRecord record(quint64 startOffset, quint8 startLength,
                                         quint64 nalLength, quint64 trailingLength) {
    H264StartCodeRecord result;
    result.startCodeOffset = startOffset;
    result.startCodeLength = startLength;
    result.nalUnitOffset = startOffset + startLength;
    result.nalUnitLength = nalLength;
    result.trailingZero8BitsOffset = result.nalUnitOffset + nalLength;
    result.trailingZero8BitsLength = trailingLength;
    result.startCode = span(startOffset * 8U, static_cast<quint64>(startLength) * 8U);
    if (nalLength != 0U) {
        result.nalUnit = span(result.nalUnitOffset * 8U, nalLength * 8U);
    }
    if (trailingLength != 0U) {
        result.trailingZero8Bits =
            span(result.trailingZero8BitsOffset * 8U, trailingLength * 8U);
    }
    return result;
}

[[nodiscard]] MaterializedResultCacheNode rootNode(const QVariant& value = {}) {
    MaterializedResultCacheNode node;
    node.id = AnalysisNodeId(1);
    node.spec.kind = AnalysisNodeKind::Root;
    node.spec.name = QStringLiteral("root");
    node.spec.state = MaterializationState::Materialized;
    node.spec.value = value;
    return node;
}

[[nodiscard]] MaterializedResultCachePage materializedPage() {
    MaterializedResultCachePage page;
    page.key = {PagedCachePageKind::MaterializedResult, 12, 3};
    page.nodes.push_back(rootNode());

    MaterializedResultCacheNode syntax;
    syntax.id = AnalysisNodeId(2);
    syntax.parentId = AnalysisNodeId(1);
    syntax.spec.kind = AnalysisNodeKind::SyntaxField;
    syntax.spec.name = QStringLiteral("unit_type");
    syntax.spec.state = MaterializationState::Materialized;
    syntax.spec.value = QVariant::fromValue<qulonglong>(
        std::numeric_limits<qulonglong>::max());
    syntax.spec.location = location(LogicalViewId(7), 32, {span(80, 8), span(104, 8)});
    syntax.spec.metadata.typeName = QStringLiteral("u16");
    syntax.spec.metadata.description = QStringLiteral("NAL unit type");
    syntax.spec.metadata.specification =
        streamview::core::AnalysisSpecification{QStringLiteral("ITU-T H.264"),
                                                QStringLiteral("7.3.1")};
    streamview::core::ParseDiagnostic diagnostic;
    diagnostic.code = DiagnosticCode::InvalidSyntax;
    diagnostic.severity = DiagnosticSeverity::Warning;
    diagnostic.message = QStringLiteral("Reserved value");
    diagnostic.fieldPath = QStringLiteral("root.unit_type");
    diagnostic.location = *syntax.spec.location;
    syntax.diagnostics.push_back(std::move(diagnostic));
    page.nodes.push_back(std::move(syntax));

    MaterializedResultCacheNode computed;
    computed.id = AnalysisNodeId(3);
    computed.parentId = AnalysisNodeId(1);
    computed.spec.kind = AnalysisNodeKind::ComputedField;
    computed.spec.name = QStringLiteral("valid");
    computed.spec.state = MaterializationState::Materialized;
    computed.spec.value = true;
    page.nodes.push_back(std::move(computed));

    MaterializedResultCacheNode signedNode;
    signedNode.id = AnalysisNodeId(4);
    signedNode.parentId = AnalysisNodeId(1);
    signedNode.spec.kind = AnalysisNodeKind::Structure;
    signedNode.spec.name = QStringLiteral("delta");
    signedNode.spec.state = MaterializationState::Cancelled;
    signedNode.spec.value = QVariant::fromValue<qlonglong>(
        std::numeric_limits<qlonglong>::min());
    page.nodes.push_back(std::move(signedNode));

    MaterializedResultCacheNode textNode;
    textNode.id = AnalysisNodeId(5);
    textNode.parentId = AnalysisNodeId(4);
    textNode.spec.kind = AnalysisNodeKind::Region;
    textNode.spec.name = QStringLiteral("label");
    textNode.spec.state = MaterializationState::Lazy;
    textNode.spec.value = QStringLiteral("slice");
    page.nodes.push_back(std::move(textNode));
    return page;
}

[[nodiscard]] AnalysisCacheNamespace cacheNamespace() {
    const QByteArray sourceDigest(32, '\x11');
    auto source = SourceFingerprint::create(
        SourceFingerprint::algorithmVersion(), SourceFingerprintMode::FullContentSha256, 5,
        std::nullopt, sourceDigest);
    Q_ASSERT(source.has_value());
    auto package = RulePackageIdentity::create(QStringLiteral("org.example.packet"),
                                               QStringLiteral("1.2.3"), QByteArray(32, '\x22'));
    Q_ASSERT(package.has_value());
    auto rule = RuleEntryPointIdentity::create(*package, QStringLiteral("packet"));
    Q_ASSERT(rule.has_value());
    auto result = AnalysisCacheNamespace::create(*source, *rule);
    Q_ASSERT(result.has_value());
    return *result;
}

[[nodiscard]] std::optional<AnalysisTree> oneNodePerCachePageTree(std::size_t nodeCount) {
    if (nodeCount == 0U) {
        return std::nullopt;
    }
    const QString name(32U * 1024U, u'x');
    auto tree = AnalysisTree::create(name);
    if (!tree) {
        return std::nullopt;
    }
    for (std::size_t index = 1; index < nodeCount; ++index) {
        streamview::core::AnalysisNodeSpec spec;
        spec.kind = AnalysisNodeKind::Structure;
        spec.name = name;
        spec.state = MaterializationState::Materialized;
        if (!tree->appendChild(tree->rootId(), std::move(spec))) {
            return std::nullopt;
        }
    }
    if (!tree->transition(tree->rootId(), MaterializationState::Materialized)) {
        return std::nullopt;
    }
    return tree;
}

[[nodiscard]] QByteArray byteArray(std::span<const std::byte> bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

void write32(std::vector<std::byte>* bytes, std::size_t offset, quint32 value) {
    Q_ASSERT(offset + 4U <= bytes->size());
    for (std::size_t index = 0; index < 4U; ++index) {
        const auto shift = static_cast<unsigned int>((3U - index) * 8U);
        (*bytes)[offset + index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

void write64(std::vector<std::byte>* bytes, std::size_t offset, quint64 value) {
    Q_ASSERT(offset + 8U <= bytes->size());
    for (std::size_t index = 0; index < 8U; ++index) {
        const auto shift = static_cast<unsigned int>((7U - index) * 8U);
        (*bytes)[offset + index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

} // namespace

class AnalysisCachePayloadTest final : public QObject {
    Q_OBJECT

private slots:
    void roundTripsProgressiveIndexThroughPagedCache() {
        H264ProgressiveIndexCachePage page;
        page.key = {PagedCachePageKind::ProgressiveIndex, 4, 2};
        page.firstRecordIndex = 256;
        page.records = {record(2, 3, 8, 2), record(20, 4, 5, 0)};
        page.indexedThroughByteOffset = 29;
        page.endOfSource = true;

        const auto encoded = AnalysisCachePageBodyCodec::encodeProgressiveIndex(page);
        QVERIFY2(encoded.succeeded(), qPrintable(encoded.errorMessage));
        const auto cacheId = cacheNamespace();
        const auto enveloped = AnalysisCachePayloadEnvelope::encode(
            cacheId, PagedCachePageKind::ProgressiveIndex, encoded.bytes);
        QVERIFY2(enveloped.succeeded(), qPrintable(enveloped.errorMessage));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto opened = PagedCache::open(directory.filePath(QStringLiteral("cache.sqlite")),
                                       cacheId.name());
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));
        const std::vector<PagedCachePageWrite> writes{{page.key, enveloped.bytes}};
        QVERIFY(opened.cache->commitBatch(writes).succeeded());
        const auto stored = opened.cache->readPage(page.key);
        QVERIFY(stored.found());
        const auto envelope = AnalysisCachePayloadEnvelope::decode(
            cacheId, PagedCachePageKind::ProgressiveIndex, stored.bytes);
        QVERIFY2(envelope.succeeded(), qPrintable(envelope.errorMessage));
        const auto decoded =
            AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, envelope.payload);
        QVERIFY2(decoded.succeeded(), qPrintable(decoded.errorMessage));
        QCOMPARE(decoded.page->key, page.key);
        QCOMPARE(decoded.page->firstRecordIndex, quint64{256});
        QCOMPARE(decoded.page->indexedThroughByteOffset, quint64{29});
        QVERIFY(decoded.page->endOfSource);
        QCOMPARE(decoded.page->records.size(), std::size_t{2});
        QCOMPARE(decoded.page->records.at(0).nalUnitLength, quint64{8});
        QCOMPARE(decoded.page->records.at(0).startCode->start().absoluteBitOffset(),
                 quint64{16});
        QCOMPARE(decoded.page->records.at(0).trailingZero8Bits->bitLength(), quint64{16});
        QVERIFY(!decoded.page->records.at(1).trailingZero8Bits.has_value());
    }

    void hasStableProgressiveIndexBinaryLayout() {
        H264ProgressiveIndexCachePage page;
        page.key = {PagedCachePageKind::ProgressiveIndex, 0x0102030405060708ULL,
                    0x1112131415161718ULL};
        page.firstRecordIndex = 0x2122232425262728ULL;
        page.indexedThroughByteOffset = 0x20;
        page.endOfSource = true;
        page.records = {record(2, 3, 0x10, 2)};

        const auto encoded = AnalysisCachePageBodyCodec::encodeProgressiveIndex(page);
        QVERIFY(encoded.succeeded());
        QCOMPARE(byteArray(encoded.bytes).toHex(),
                 QByteArrayLiteral(
                     "53565049445800000000000100000001"
                     "01020304050607081112131415161718"
                     "21222324252627280000000000000020"
                     "00000001000000000000000000000002"
                     "00000003000000000000000000000005"
                     "00000000000000100000000000000015"
                     "0000000000000002"));
    }

    void rejectsInvalidProgressiveIndexBodies() {
        H264ProgressiveIndexCachePage page;
        page.key = {PagedCachePageKind::ProgressiveIndex, 4, 2};
        page.firstRecordIndex = 10;
        page.indexedThroughByteOffset = 16;
        page.records = {record(1, 3, 8, 1)};
        const auto encoded = AnalysisCachePageBodyCodec::encodeProgressiveIndex(page);
        QVERIFY(encoded.succeeded());

        auto wrongKey = page.key;
        ++wrongKey.pageIndex;
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(wrongKey, encoded.bytes).status,
                 AnalysisCacheBodyDecodeStatus::PageKeyMismatch);

        auto invalidMagic = encoded.bytes;
        invalidMagic[0] ^= std::byte{1};
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, invalidMagic).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto futureVersion = encoded.bytes;
        write32(&futureVersion, 8,
                AnalysisCachePageBodyCodec::progressiveIndexFormatVersion() + 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, futureVersion).status,
                 AnalysisCacheBodyDecodeStatus::UnsupportedVersion);
        auto unknownFlag = encoded.bytes;
        write32(&unknownFlag, 12, 2U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, unknownFlag).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto reservedHeader = encoded.bytes;
        write32(&reservedHeader, 52, 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, reservedHeader)
                     .status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto reservedRecord = encoded.bytes;
        write32(&reservedRecord, 68, 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, reservedRecord)
                     .status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto badNalOffset = encoded.bytes;
        write64(&badNalOffset, 72, 7U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, badNalOffset).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto truncated = encoded.bytes;
        truncated.pop_back();
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, truncated).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto trailing = encoded.bytes;
        trailing.push_back(std::byte{0});
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, trailing).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);

        page.records.front().startCode = span(0, 24);
        QCOMPARE(AnalysisCachePageBodyCodec::encodeProgressiveIndex(page).status,
                 AnalysisCacheBodyEncodeStatus::InvalidArgument);
        const std::size_t maximumRecords =
            (AnalysisCachePayloadEnvelope::maximumPayloadBytes() - 56U) / 48U;
        page.records.resize(maximumRecords + 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::encodeProgressiveIndex(page).status,
                 AnalysisCacheBodyEncodeStatus::PayloadTooLarge);
        const std::vector<std::byte> oversized(
            AnalysisCachePayloadEnvelope::maximumPayloadBytes() + 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeProgressiveIndex(page.key, oversized).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
    }

    void roundTripsCompleteMaterializedResults() {
        const auto page = materializedPage();
        const auto encoded = AnalysisCachePageBodyCodec::encodeMaterializedResult(page);
        QVERIFY2(encoded.succeeded(), qPrintable(encoded.errorMessage));
        const auto decoded =
            AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, encoded.bytes);
        QVERIFY2(decoded.succeeded(), qPrintable(decoded.errorMessage));
        QCOMPARE(decoded.page->key, page.key);
        QCOMPARE(decoded.page->nodes.size(), std::size_t{5});
        QVERIFY(!decoded.page->nodes.at(0).spec.value.isValid());
        QCOMPARE(decoded.page->nodes.at(1).spec.value.typeId(), QMetaType::ULongLong);
        QCOMPARE(decoded.page->nodes.at(1).spec.value.toULongLong(),
                 std::numeric_limits<qulonglong>::max());
        QCOMPARE(decoded.page->nodes.at(2).spec.value.typeId(), QMetaType::Bool);
        QVERIFY(decoded.page->nodes.at(2).spec.value.toBool());
        QCOMPARE(decoded.page->nodes.at(3).spec.value.typeId(), QMetaType::LongLong);
        QCOMPARE(decoded.page->nodes.at(3).spec.value.toLongLong(),
                 std::numeric_limits<qlonglong>::min());
        QCOMPARE(decoded.page->nodes.at(4).spec.value, QVariant(QStringLiteral("slice")));

        const auto& syntax = decoded.page->nodes.at(1);
        QCOMPARE(syntax.parentId, std::optional<AnalysisNodeId>(AnalysisNodeId(1)));
        QCOMPARE(syntax.spec.location->logicalRange().start().viewId(), LogicalViewId(7));
        QCOMPARE(syntax.spec.location->sourceSpans().size(), std::size_t{2});
        QCOMPARE(syntax.spec.location->sourceSpans().at(1).start().absoluteBitOffset(),
                 quint64{104});
        QCOMPARE(syntax.spec.metadata.specification->standard, QStringLiteral("ITU-T H.264"));
        QCOMPARE(syntax.diagnostics.size(), std::size_t{1});
        QCOMPARE(syntax.diagnostics.front().location->sourceSpans().size(), std::size_t{2});
        QCOMPARE(syntax.diagnostics.front().fieldPath, QStringLiteral("root.unit_type"));
    }

    void persistsMaterializedResultsThroughPagedCache() {
        const auto page = materializedPage();
        const auto body = AnalysisCachePageBodyCodec::encodeMaterializedResult(page);
        QVERIFY(body.succeeded());
        const auto cacheId = cacheNamespace();
        const auto envelope = AnalysisCachePayloadEnvelope::encode(
            cacheId, PagedCachePageKind::MaterializedResult, body.bytes);
        QVERIFY(envelope.succeeded());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto opened = PagedCache::open(directory.filePath(QStringLiteral("cache.sqlite")),
                                       cacheId.name());
        QVERIFY(opened.succeeded());
        const std::vector<PagedCachePageWrite> writes{{page.key, envelope.bytes}};
        QVERIFY(opened.cache->commitBatch(writes).succeeded());
        const auto stored = opened.cache->readPage(page.key);
        QVERIFY(stored.found());
        const auto unwrapped = AnalysisCachePayloadEnvelope::decode(
            cacheId, PagedCachePageKind::MaterializedResult, stored.bytes);
        QVERIFY(unwrapped.succeeded());
        QVERIFY(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, unwrapped.payload)
                    .succeeded());
    }

    void exportsStableTreesAsDeterministicConsecutivePages() {
        auto tree = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(tree.has_value());
        for (std::size_t index = 0; index < 1500U; ++index) {
            streamview::core::AnalysisNodeSpec spec;
            spec.kind = AnalysisNodeKind::Structure;
            spec.name = QStringLiteral("node-%1-").arg(index) + QString(96, u'x');
            spec.state = MaterializationState::Materialized;
            if (index == 0U) {
                spec.kind = AnalysisNodeKind::SyntaxField;
                spec.value = QVariant::fromValue<qulonglong>(17);
                spec.location = location(LogicalViewId(9), 16, {span(32, 8), span(56, 8)});
                spec.metadata.typeName = QStringLiteral("u16");
                spec.metadata.description = QStringLiteral("Export fidelity field");
                spec.metadata.specification = streamview::core::AnalysisSpecification{
                    QStringLiteral("Example"), QStringLiteral("1.2")};
            }
            const auto child = tree->appendChild(tree->rootId(), std::move(spec));
            QVERIFY(child.has_value());
            if (index == 0U) {
                streamview::core::ParseDiagnostic diagnostic;
                diagnostic.code = DiagnosticCode::InvalidSyntax;
                diagnostic.severity = DiagnosticSeverity::Warning;
                diagnostic.message = QStringLiteral("Export fidelity diagnostic");
                diagnostic.fieldPath = QStringLiteral("root.node-0");
                diagnostic.location = location(LogicalViewId(9), 16,
                                               {span(32, 8), span(56, 8)});
                QVERIFY(tree->addDiagnostic(*child, std::move(diagnostic)));
            }
        }
        QVERIFY(tree->transition(tree->rootId(), MaterializationState::Materialized));

        const auto exported = exportMaterializedResultCachePages(*tree, 3, 7);

        QVERIFY2(exported.succeeded(), qPrintable(exported.errorMessage));
        QVERIFY(exported.pages.size() > 1U);
        const auto fidelitySource = tree->node(AnalysisNodeId(2));
        QVERIFY(fidelitySource.has_value());
        quint64 expectedNodeId = 1;
        for (std::size_t pageIndex = 0; pageIndex < exported.pages.size(); ++pageIndex) {
            const auto& page = exported.pages[pageIndex];
            QCOMPARE(page.key,
                     PagedCachePageKey(PagedCachePageKind::MaterializedResult, 3,
                                       7 + static_cast<quint64>(pageIndex)));
            const auto encoded = AnalysisCachePageBodyCodec::encodeMaterializedResult(page);
            QVERIFY2(encoded.succeeded(), qPrintable(encoded.errorMessage));
            const auto decoded =
                AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, encoded.bytes);
            QVERIFY2(decoded.succeeded(), qPrintable(decoded.errorMessage));
            for (const auto& node : decoded.page->nodes) {
                QCOMPARE(node.id, AnalysisNodeId(expectedNodeId));
                if (node.id == AnalysisNodeId(2)) {
                    QCOMPARE(node.parentId, fidelitySource->parentId());
                    QCOMPARE(node.spec.value, fidelitySource->value());
                    QCOMPARE(node.spec.location->logicalRange().start().viewId(),
                             LogicalViewId(9));
                    QCOMPARE(node.spec.location->sourceSpans().size(), std::size_t{2});
                    QCOMPARE(node.spec.metadata.typeName,
                             fidelitySource->metadata().typeName);
                    QCOMPARE(node.spec.metadata.description,
                             fidelitySource->metadata().description);
                    QCOMPARE(node.spec.metadata.specification->standard,
                             fidelitySource->metadata().specification->standard);
                    QCOMPARE(node.diagnostics.size(), std::size_t{1});
                    QCOMPARE(node.diagnostics.front().message,
                             fidelitySource->diagnostics().front().message);
                }
                ++expectedNodeId;
            }
        }
        QCOMPARE(expectedNodeId, static_cast<quint64>(tree->nodeCount()) + 1U);
    }

    void enforcesTheAtomicMaterializedExportPageLimit() {
        auto maximum = oneNodePerCachePageTree(PagedCache::maximumBatchPages());
        QVERIFY(maximum.has_value());
        const auto accepted = exportMaterializedResultCachePages(*maximum, 0);
        QVERIFY2(accepted.succeeded(), qPrintable(accepted.errorMessage));
        QCOMPARE(accepted.pages.size(), PagedCache::maximumBatchPages());

        auto oversized = oneNodePerCachePageTree(PagedCache::maximumBatchPages() + 1U);
        QVERIFY(oversized.has_value());
        const auto rejected = exportMaterializedResultCachePages(*oversized, 0);
        QCOMPARE(rejected.status, MaterializedResultCacheExportStatus::TooManyPages);
        QVERIFY(rejected.pages.empty());
    }

    void rejectsTransientAndIndividuallyOversizedTreeExports() {
        auto transient = AnalysisTree::create(QStringLiteral("root"));
        QVERIFY(transient.has_value());
        QCOMPARE(exportMaterializedResultCachePages(*transient, 0).status,
                 MaterializedResultCacheExportStatus::InvalidTree);

        auto oversized = AnalysisTree::create(QString(32U * 1024U + 1U, u'x'));
        QVERIFY(oversized.has_value());
        QVERIFY(oversized->transition(oversized->rootId(),
                                      MaterializationState::Materialized));
        QCOMPARE(exportMaterializedResultCachePages(*oversized, 0).status,
                 MaterializedResultCacheExportStatus::PayloadTooLarge);
    }

    void rejectsUnsupportedAndInvalidMaterializedInput() {
        MaterializedResultCachePage page;
        page.key = {PagedCachePageKind::MaterializedResult, 1, 0};
        QCOMPARE(AnalysisCachePageBodyCodec::encodeMaterializedResult(page).status,
                 AnalysisCacheBodyEncodeStatus::InvalidArgument);

        page.nodes = {rootNode(QVariant::fromValue<int>(7))};
        QCOMPARE(AnalysisCachePageBodyCodec::encodeMaterializedResult(page).status,
                 AnalysisCacheBodyEncodeStatus::UnsupportedValue);
        page.nodes = {rootNode()};
        page.nodes.front().spec.state = MaterializationState::Indexing;
        QCOMPARE(AnalysisCachePageBodyCodec::encodeMaterializedResult(page).status,
                 AnalysisCacheBodyEncodeStatus::InvalidArgument);
        page.nodes = {rootNode()};
        page.nodes.front().id = AnalysisNodeId(2);
        QCOMPARE(AnalysisCachePageBodyCodec::encodeMaterializedResult(page).status,
                 AnalysisCacheBodyEncodeStatus::InvalidArgument);
        page.nodes = {rootNode()};
        page.nodes.front().spec.name = QString(32U * 1024U + 1U, u'x');
        QCOMPARE(AnalysisCachePageBodyCodec::encodeMaterializedResult(page).status,
                 AnalysisCacheBodyEncodeStatus::PayloadTooLarge);
        page.nodes = {rootNode()};
        page.nodes.front().spec.name = QString(32U * 1024U, u'x');
        page.nodes.front().spec.metadata.typeName = QString(32U * 1024U, u'y');
        QCOMPARE(AnalysisCachePageBodyCodec::encodeMaterializedResult(page).status,
                 AnalysisCacheBodyEncodeStatus::PayloadTooLarge);
        page.nodes = {rootNode()};
        page.nodes.front().spec.name = QString(1, QChar(0xD800));
        QCOMPARE(AnalysisCachePageBodyCodec::encodeMaterializedResult(page).status,
                 AnalysisCacheBodyEncodeStatus::InvalidArgument);
        page.nodes = {rootNode(QString(1, QChar(0xD800)))};
        QCOMPARE(AnalysisCachePageBodyCodec::encodeMaterializedResult(page).status,
                 AnalysisCacheBodyEncodeStatus::InvalidArgument);
        page.nodes.resize(1025);
        QCOMPARE(AnalysisCachePageBodyCodec::encodeMaterializedResult(page).status,
                 AnalysisCacheBodyEncodeStatus::PayloadTooLarge);
    }

    void rejectsInvalidMaterializedBodies() {
        MaterializedResultCachePage page;
        page.key = {PagedCachePageKind::MaterializedResult, 1, 0};
        page.nodes = {rootNode(true)};
        const auto encoded = AnalysisCachePageBodyCodec::encodeMaterializedResult(page);
        QVERIFY(encoded.succeeded());

        auto wrongKey = page.key;
        ++wrongKey.streamId;
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(wrongKey, encoded.bytes)
                     .status,
                 AnalysisCacheBodyDecodeStatus::PageKeyMismatch);
        auto invalidMagic = encoded.bytes;
        invalidMagic[0] ^= std::byte{1};
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, invalidMagic)
                     .status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto futureVersion = encoded.bytes;
        write32(&futureVersion, 8,
                AnalysisCachePageBodyCodec::materializedResultFormatVersion() + 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, futureVersion)
                     .status,
                 AnalysisCacheBodyDecodeStatus::UnsupportedVersion);
        auto reserved = encoded.bytes;
        write32(&reserved, 12, 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, reserved).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto countReserved = encoded.bytes;
        write32(&countReserved, 36, 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, countReserved)
                     .status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto unknownKind = encoded.bytes;
        write32(&unknownKind, 56, 99U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, unknownKind).status,
                 AnalysisCacheBodyDecodeStatus::UnsupportedValue);
        auto transientState = encoded.bytes;
        write32(&transientState, 60,
                static_cast<quint32>(MaterializationState::Indexing) + 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, transientState)
                     .status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto unknownValue = encoded.bytes;
        write32(&unknownValue, 64, 99U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, unknownValue)
                     .status,
                 AnalysisCacheBodyDecodeStatus::UnsupportedValue);
        auto invalidFlags = encoded.bytes;
        write32(&invalidFlags, 68, 4U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, invalidFlags)
                     .status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto invalidTopology = encoded.bytes;
        write64(&invalidTopology, 40, 2U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, invalidTopology)
                     .status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto invalidUtf8 = encoded.bytes;
        invalidUtf8[76] = std::byte{0xFF};
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, invalidUtf8).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto invalidBoolean = encoded.bytes;
        write64(&invalidBoolean, 88, 2U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, invalidBoolean)
                     .status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto truncated = encoded.bytes;
        truncated.pop_back();
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, truncated).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        auto trailing = encoded.bytes;
        trailing.push_back(std::byte{0});
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, trailing).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
        const std::vector<std::byte> oversized(
            AnalysisCachePayloadEnvelope::maximumPayloadBytes() + 1U);
        QCOMPARE(AnalysisCachePageBodyCodec::decodeMaterializedResult(page.key, oversized).status,
                 AnalysisCacheBodyDecodeStatus::InvalidBody);
    }
};

QTEST_GUILESS_MAIN(AnalysisCachePayloadTest)

#include "analysis_cache_payload_test.moc"
