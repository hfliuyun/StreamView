#include <streamview/core/paged_cache.h>
#include <streamview/core/source_fingerprint.h>
#include <streamview/rules/analysis_cache.h>
#include <streamview/rules/rule_package.h>

#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using streamview::core::PagedCache;
using streamview::core::PagedCachePageKind;
using streamview::core::PagedCachePageWrite;
using streamview::core::SourceFingerprint;
using streamview::core::SourceFingerprintMode;
using streamview::rules::AnalysisCacheEnvelopeDecodeStatus;
using streamview::rules::AnalysisCacheEnvelopeEncodeStatus;
using streamview::rules::AnalysisCacheNamespace;
using streamview::rules::AnalysisCachePayloadEnvelope;
using streamview::rules::AnalysisCacheVersions;
using streamview::rules::RuleEntryPointIdentity;
using streamview::rules::RulePackageIdentity;

namespace {

[[nodiscard]] QByteArray digest(char value) { return QByteArray(32, value); }

[[nodiscard]] SourceFingerprint sourceFingerprint(char digestByte = '\x11', quint64 size = 5) {
    auto fingerprint = SourceFingerprint::create(
        SourceFingerprint::algorithmVersion(), SourceFingerprintMode::FullContentSha256,
        size, std::nullopt, digest(digestByte));
    Q_ASSERT(fingerprint.has_value());
    return std::move(*fingerprint);
}

[[nodiscard]] SourceFingerprint sampledSourceFingerprint(qint64 modificationTime) {
    auto fingerprint = SourceFingerprint::create(
        SourceFingerprint::algorithmVersion(), SourceFingerprintMode::SampledSha256,
        SourceFingerprint::fullContentLimitBytes() + 1U, modificationTime, digest('\x55'));
    Q_ASSERT(fingerprint.has_value());
    return std::move(*fingerprint);
}

[[nodiscard]] RuleEntryPointIdentity ruleIdentity(
    const QString& version = QStringLiteral("1.2.3"), char digestByte = '\x22',
    const QString& entryPoint = QStringLiteral("packet"),
    const QString& packageId = QStringLiteral("org.example.packet")) {
    auto package = RulePackageIdentity::create(packageId, version, digest(digestByte));
    Q_ASSERT(package.has_value());
    auto rule = RuleEntryPointIdentity::create(std::move(*package), entryPoint);
    Q_ASSERT(rule.has_value());
    return std::move(*rule);
}

[[nodiscard]] AnalysisCacheNamespace cacheNamespace(
    const SourceFingerprint& source = sourceFingerprint(),
    const RuleEntryPointIdentity& rule = ruleIdentity(),
    AnalysisCacheVersions versions = AnalysisCacheVersions::current()) {
    auto cache = AnalysisCacheNamespace::create(source, rule, versions);
    Q_ASSERT(cache.has_value());
    return std::move(*cache);
}

[[nodiscard]] std::vector<std::byte> payload(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    std::transform(values.begin(), values.end(), std::back_inserter(result),
                   [](unsigned int value) { return static_cast<std::byte>(value); });
    return result;
}

void write32(std::vector<std::byte>* bytes, std::size_t offset, quint32 value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        const auto shift = static_cast<unsigned int>((3U - index) * 8U);
        (*bytes)[offset + index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

} // namespace

class AnalysisCacheTest final : public QObject {
    Q_OBJECT

private slots:
    void validatesCompleteRuleEntryPointIdentity() {
        const auto package = RulePackageIdentity::create(
            QStringLiteral("org.example.packet"), QStringLiteral("1.2.3"), digest('\x33'));
        QVERIFY(package.has_value());
        QString error;
        const auto entry =
            RuleEntryPointIdentity::create(*package, QStringLiteral("packet"), &error);
        QVERIFY2(entry.has_value(), qPrintable(error));
        QCOMPARE(entry->packageIdentity(), *package);
        QCOMPARE(entry->entryPointId(), QStringLiteral("packet"));
        QVERIFY(entry->toString().endsWith(QStringLiteral("!packet")));

        for (const QString& invalid : {QString(), QStringLiteral("Packet"),
                                       QStringLiteral("bad_entry"), QString(65, u'a')}) {
            QVERIFY(!RuleEntryPointIdentity::create(*package, invalid, &error).has_value());
            QVERIFY(!error.isEmpty());
        }
    }

    void framesEveryDurableNamespaceInput() {
        const auto source = sourceFingerprint();
        const auto rule = ruleIdentity();
        QString error;
        const auto first = AnalysisCacheNamespace::create(source, rule, {}, &error);
        const auto second = AnalysisCacheNamespace::create(source, rule, {}, &error);
        QVERIFY2(first.has_value(), qPrintable(error));
        QCOMPARE(first, second);
        QVERIFY(first->name().startsWith(QStringLiteral("sv-cache-v1-sha256:")));
        QCOMPARE(first->name(),
                 QStringLiteral(
                     "sv-cache-v1-sha256:"
                     "1124c34a93761f7a143791132e1dd4acd61143a8c607d6a690a4d7d22845bcc0"));
        QCOMPARE(first->digest().size(), QCryptographicHash::hashLength(QCryptographicHash::Sha256));
        QVERIFY(first->name().size() <= PagedCache::maximumNamespaceLength());

        QVERIFY(cacheNamespace(sourceFingerprint('\x12'), rule) != *first);
        QVERIFY(cacheNamespace(sourceFingerprint('\x11', 6), rule) != *first);
        QVERIFY(cacheNamespace(sampledSourceFingerprint(100), rule) !=
                cacheNamespace(sampledSourceFingerprint(101), rule));
        QVERIFY(cacheNamespace(source, ruleIdentity(QStringLiteral("1.2.3"), '\x22',
                                                    QStringLiteral("packet"),
                                                    QStringLiteral("org.example.other"))) != *first);
        QVERIFY(cacheNamespace(source, ruleIdentity(QStringLiteral("1.2.4"))) != *first);
        QVERIFY(cacheNamespace(source, ruleIdentity(QStringLiteral("1.2.3"), '\x23')) != *first);
        QVERIFY(cacheNamespace(source, ruleIdentity(QStringLiteral("1.2.3"), '\x22',
                                                    QStringLiteral("alternate"))) != *first);

        AnalysisCacheVersions payloadVersion = AnalysisCacheVersions::current();
        ++payloadVersion.progressiveIndexPayload;
        QVERIFY(cacheNamespace(source, rule, payloadVersion) != *first);
        payloadVersion = AnalysisCacheVersions::current();
        ++payloadVersion.materializedResultPayload;
        QVERIFY(cacheNamespace(source, rule, payloadVersion) != *first);

        AnalysisCacheVersions invalid = AnalysisCacheVersions::current();
        ++invalid.namespaceFormat;
        QVERIFY(!AnalysisCacheNamespace::create(source, rule, invalid, &error).has_value());
        invalid = AnalysisCacheVersions::current();
        ++invalid.cacheSchema;
        QVERIFY(!AnalysisCacheNamespace::create(source, rule, invalid, &error).has_value());
        invalid = AnalysisCacheVersions::current();
        ++invalid.payloadEnvelope;
        QVERIFY(!AnalysisCacheNamespace::create(source, rule, invalid, &error).has_value());
        invalid = AnalysisCacheVersions::current();
        invalid.materializedResultPayload = 0;
        QVERIFY(!AnalysisCacheNamespace::create(source, rule, invalid, &error).has_value());
    }

    void storesAndRestoresEnvelopedPages() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto cacheId = cacheNamespace();
        auto opened = PagedCache::open(directory.filePath(QStringLiteral("cache.sqlite")),
                                       cacheId.name());
        QVERIFY2(opened.succeeded(), qPrintable(opened.errorMessage));

        const auto indexPayload = payload({0x00, 0x11, 0xFF});
        const auto resultPayload = payload({0x22, 0x00, 0x33});
        auto indexEnvelope = AnalysisCachePayloadEnvelope::encode(
            cacheId, PagedCachePageKind::ProgressiveIndex, indexPayload);
        auto resultEnvelope = AnalysisCachePayloadEnvelope::encode(
            cacheId, PagedCachePageKind::MaterializedResult, resultPayload);
        QVERIFY2(indexEnvelope.succeeded(), qPrintable(indexEnvelope.errorMessage));
        QVERIFY2(resultEnvelope.succeeded(), qPrintable(resultEnvelope.errorMessage));
        const std::vector<PagedCachePageWrite> pages{
            {{PagedCachePageKind::ProgressiveIndex, 4, 0}, std::move(indexEnvelope.bytes)},
            {{PagedCachePageKind::MaterializedResult, 8, 3}, std::move(resultEnvelope.bytes)},
        };
        QVERIFY(opened.cache->commitBatch(pages).succeeded());

        for (std::size_t index = 0; index < pages.size(); ++index) {
            const auto stored = opened.cache->readPage(pages[index].key);
            QVERIFY(stored.found());
            const auto decoded = AnalysisCachePayloadEnvelope::decode(
                cacheId, pages[index].key.kind, stored.bytes);
            QVERIFY2(decoded.succeeded(), qPrintable(decoded.errorMessage));
            QCOMPARE(decoded.payload, index == 0U ? indexPayload : resultPayload);
        }
    }

    void rejectsMismatchedStaleAndCorruptEnvelopes() {
        const auto cacheId = cacheNamespace();
        const auto body = payload({0x10, 0x20, 0x30, 0x40});
        const auto encoded = AnalysisCachePayloadEnvelope::encode(
            cacheId, PagedCachePageKind::ProgressiveIndex, body);
        QVERIFY(encoded.succeeded());

        QCOMPARE(AnalysisCachePayloadEnvelope::decode(
                     cacheNamespace(sourceFingerprint('\x44')), PagedCachePageKind::ProgressiveIndex,
                     encoded.bytes)
                     .status,
                 AnalysisCacheEnvelopeDecodeStatus::NamespaceMismatch);
        QCOMPARE(AnalysisCachePayloadEnvelope::decode(
                     cacheId, PagedCachePageKind::MaterializedResult, encoded.bytes)
                     .status,
                 AnalysisCacheEnvelopeDecodeStatus::PageKindMismatch);

        auto invalidMagic = encoded.bytes;
        invalidMagic[0] ^= std::byte{0x01};
        QCOMPARE(AnalysisCachePayloadEnvelope::decode(
                     cacheId, PagedCachePageKind::ProgressiveIndex, invalidMagic)
                     .status,
                 AnalysisCacheEnvelopeDecodeStatus::InvalidEnvelope);

        auto oldEnvelope = encoded.bytes;
        write32(&oldEnvelope, 8, AnalysisCachePayloadEnvelope::envelopeFormatVersion() + 1U);
        QCOMPARE(AnalysisCachePayloadEnvelope::decode(
                     cacheId, PagedCachePageKind::ProgressiveIndex, oldEnvelope)
                     .status,
                 AnalysisCacheEnvelopeDecodeStatus::UnsupportedVersion);

        auto oldPayload = encoded.bytes;
        write32(&oldPayload, 16, cacheId.versions().progressiveIndexPayload + 1U);
        QCOMPARE(AnalysisCachePayloadEnvelope::decode(
                     cacheId, PagedCachePageKind::ProgressiveIndex, oldPayload)
                     .status,
                 AnalysisCacheEnvelopeDecodeStatus::UnsupportedVersion);

        auto nonzeroReserved = encoded.bytes;
        write32(&nonzeroReserved, 20, 1);
        QCOMPARE(AnalysisCachePayloadEnvelope::decode(
                     cacheId, PagedCachePageKind::ProgressiveIndex, nonzeroReserved)
                     .status,
                 AnalysisCacheEnvelopeDecodeStatus::InvalidEnvelope);

        auto corrupt = encoded.bytes;
        corrupt.back() ^= std::byte{0x80};
        QCOMPARE(AnalysisCachePayloadEnvelope::decode(
                     cacheId, PagedCachePageKind::ProgressiveIndex, corrupt)
                     .status,
                 AnalysisCacheEnvelopeDecodeStatus::CorruptPayload);

        auto truncated = encoded.bytes;
        truncated.pop_back();
        QCOMPARE(AnalysisCachePayloadEnvelope::decode(
                     cacheId, PagedCachePageKind::ProgressiveIndex, truncated)
                     .status,
                 AnalysisCacheEnvelopeDecodeStatus::InvalidEnvelope);
    }

    void enforcesEnvelopePageBounds() {
        const auto cacheId = cacheNamespace();
        const std::vector<std::byte> maximum(AnalysisCachePayloadEnvelope::maximumPayloadBytes(),
                                             std::byte{0x5A});
        const auto encoded = AnalysisCachePayloadEnvelope::encode(
            cacheId, PagedCachePageKind::MaterializedResult, maximum);
        QVERIFY(encoded.succeeded());
        QCOMPARE(encoded.bytes.size(), PagedCache::pageSizeBytes());
        QCOMPARE(AnalysisCachePayloadEnvelope::decode(
                     cacheId, PagedCachePageKind::MaterializedResult, encoded.bytes)
                     .payload,
                 maximum);

        const std::vector<std::byte> oversized(
            AnalysisCachePayloadEnvelope::maximumPayloadBytes() + 1U);
        QCOMPARE(AnalysisCachePayloadEnvelope::encode(
                     cacheId, PagedCachePageKind::MaterializedResult, oversized)
                     .status,
                 AnalysisCacheEnvelopeEncodeStatus::PayloadTooLarge);
        QCOMPARE(AnalysisCachePayloadEnvelope::encode(
                     cacheId, static_cast<PagedCachePageKind>(99), {})
                     .status,
                 AnalysisCacheEnvelopeEncodeStatus::InvalidArgument);
    }
};

QTEST_GUILESS_MAIN(AnalysisCacheTest)

#include "analysis_cache_test.moc"
