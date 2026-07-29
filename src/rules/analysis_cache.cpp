#include <streamview/rules/analysis_cache.h>

#include <QByteArrayView>
#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace streamview::rules {

namespace {

constexpr std::array<char, 8> envelopeMagic{'S', 'V', 'C', 'A', 'C', 'H', 'E', '\0'};

void setError(QString* errorMessage, const QString& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

[[nodiscard]] QByteArray bigEndian32(quint32 value) {
    QByteArray bytes(4, Qt::Uninitialized);
    bytes[0] = static_cast<char>((value >> 24U) & 0xFFU);
    bytes[1] = static_cast<char>((value >> 16U) & 0xFFU);
    bytes[2] = static_cast<char>((value >> 8U) & 0xFFU);
    bytes[3] = static_cast<char>(value & 0xFFU);
    return bytes;
}

[[nodiscard]] QByteArray bigEndian64(quint64 value) {
    QByteArray bytes(8, Qt::Uninitialized);
    for (int index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned int>((7 - index) * 8);
        bytes[index] = static_cast<char>((value >> shift) & 0xFFU);
    }
    return bytes;
}

void addFramed(QCryptographicHash* hash, QByteArrayView bytes) {
    hash->addData(bigEndian32(static_cast<quint32>(bytes.size())));
    hash->addData(bytes);
}

[[nodiscard]] std::optional<quint32> pageKindCode(core::PagedCachePageKind kind) {
    switch (kind) {
    case core::PagedCachePageKind::ProgressiveIndex:
        return 1;
    case core::PagedCachePageKind::MaterializedResult:
        return 2;
    }
    return std::nullopt;
}

[[nodiscard]] quint32 payloadVersion(const AnalysisCacheVersions& versions,
                                     core::PagedCachePageKind kind) {
    return kind == core::PagedCachePageKind::ProgressiveIndex
               ? versions.progressiveIndexPayload
               : versions.materializedResultPayload;
}

void append32(std::vector<std::byte>* bytes, quint32 value) {
    for (int shift : {24, 16, 8, 0}) {
        bytes->push_back(static_cast<std::byte>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
    }
}

void append64(std::vector<std::byte>* bytes, quint64 value) {
    for (int shift : {56, 48, 40, 32, 24, 16, 8, 0}) {
        bytes->push_back(static_cast<std::byte>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
    }
}

void appendBytes(std::vector<std::byte>* bytes, QByteArrayView input) {
    std::transform(input.begin(), input.end(), std::back_inserter(*bytes),
                   [](char value) { return static_cast<std::byte>(static_cast<quint8>(value)); });
}

void appendBytes(std::vector<std::byte>* bytes, std::span<const std::byte> input) {
    bytes->insert(bytes->end(), input.begin(), input.end());
}

[[nodiscard]] quint32 read32(std::span<const std::byte> bytes, std::size_t offset) {
    quint32 value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value = (value << 8U) | std::to_integer<quint32>(bytes[offset + index]);
    }
    return value;
}

[[nodiscard]] quint64 read64(std::span<const std::byte> bytes, std::size_t offset) {
    quint64 value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | std::to_integer<quint64>(bytes[offset + index]);
    }
    return value;
}

[[nodiscard]] QByteArray byteArray(std::span<const std::byte> bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

} // namespace

AnalysisCacheNamespace::AnalysisCacheNamespace(QString name, QByteArray digest,
                                               AnalysisCacheVersions versions)
    : name_(std::move(name)), digest_(std::move(digest)), versions_(versions) {}

std::optional<AnalysisCacheNamespace>
AnalysisCacheNamespace::create(const core::SourceFingerprint& sourceFingerprint,
                               const RuleEntryPointIdentity& ruleIdentity,
                               AnalysisCacheVersions versions, QString* errorMessage) {
    if (versions.namespaceFormat != namespaceFormatVersion() ||
        versions.cacheSchema != core::PagedCache::schemaVersion() ||
        versions.payloadEnvelope != AnalysisCachePayloadEnvelope::envelopeFormatVersion() ||
        versions.progressiveIndexPayload == 0U || versions.materializedResultPayload == 0U) {
        setError(errorMessage, QStringLiteral("Analysis cache versions are unsupported"));
        return std::nullopt;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray domain("StreamViewAnalysisCacheNamespace\0", 33);
    hash.addData(domain);
    hash.addData(bigEndian32(versions.namespaceFormat));
    hash.addData(bigEndian32(versions.cacheSchema));
    hash.addData(bigEndian32(versions.payloadEnvelope));
    hash.addData(bigEndian32(versions.progressiveIndexPayload));
    hash.addData(bigEndian32(versions.materializedResultPayload));

    hash.addData(bigEndian32(sourceFingerprint.version()));
    const quint32 sourceMode =
        sourceFingerprint.mode() == core::SourceFingerprintMode::FullContentSha256 ? 1U : 2U;
    hash.addData(bigEndian32(sourceMode));
    hash.addData(bigEndian64(sourceFingerprint.sizeBytes()));
    hash.addData(bigEndian32(sourceFingerprint.modificationTimeNanoseconds().has_value() ? 1U : 0U));
    if (sourceFingerprint.modificationTimeNanoseconds().has_value()) {
        hash.addData(bigEndian64(
            static_cast<quint64>(*sourceFingerprint.modificationTimeNanoseconds())));
    }
    addFramed(&hash, sourceFingerprint.digest());

    const RulePackageIdentity& packageIdentity = ruleIdentity.packageIdentity();
    addFramed(&hash, packageIdentity.packageId().toUtf8());
    addFramed(&hash, packageIdentity.packageVersion().toUtf8());
    addFramed(&hash, packageIdentity.contentHash());
    addFramed(&hash, ruleIdentity.entryPointId().toUtf8());

    QByteArray digest = hash.result();
    QString name = QStringLiteral("sv-cache-v1-sha256:%1").arg(QString::fromLatin1(digest.toHex()));
    if (name.size() > core::PagedCache::maximumNamespaceLength()) {
        setError(errorMessage, QStringLiteral("Analysis cache namespace exceeds storage limits"));
        return std::nullopt;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return AnalysisCacheNamespace(std::move(name), std::move(digest), versions);
}

AnalysisCacheEnvelopeEncodeResult AnalysisCachePayloadEnvelope::encode(
    const AnalysisCacheNamespace& cacheNamespace, core::PagedCachePageKind pageKind,
    std::span<const std::byte> payload) {
    const auto kindCode = pageKindCode(pageKind);
    if (!kindCode.has_value()) {
        return {AnalysisCacheEnvelopeEncodeStatus::InvalidArgument, {},
                QStringLiteral("Analysis cache page kind is invalid")};
    }
    if (payload.size() > maximumPayloadBytes()) {
        return {AnalysisCacheEnvelopeEncodeStatus::PayloadTooLarge, {},
                QStringLiteral("Analysis cache payload exceeds the enveloped page limit")};
    }

    std::vector<std::byte> result;
    result.reserve(headerSizeBytes() + payload.size());
    appendBytes(&result, QByteArrayView(envelopeMagic.data(),
                                       static_cast<qsizetype>(envelopeMagic.size())));
    append32(&result, envelopeFormatVersion());
    append32(&result, *kindCode);
    append32(&result, payloadVersion(cacheNamespace.versions(), pageKind));
    append32(&result, 0);
    appendBytes(&result, cacheNamespace.digest());
    append64(&result, static_cast<quint64>(payload.size()));
    appendBytes(&result, QCryptographicHash::hash(byteArray(payload), QCryptographicHash::Sha256));
    appendBytes(&result, payload);
    return {AnalysisCacheEnvelopeEncodeStatus::Encoded, std::move(result), {}};
}

AnalysisCacheEnvelopeDecodeResult AnalysisCachePayloadEnvelope::decode(
    const AnalysisCacheNamespace& expectedNamespace,
    core::PagedCachePageKind expectedPageKind, std::span<const std::byte> bytes) {
    const auto expectedKindCode = pageKindCode(expectedPageKind);
    if (!expectedKindCode.has_value() || bytes.size() < headerSizeBytes() ||
        bytes.size() > core::PagedCache::pageSizeBytes()) {
        return {AnalysisCacheEnvelopeDecodeStatus::InvalidEnvelope, {},
                QStringLiteral("Analysis cache envelope size or page kind is invalid")};
    }
    if (!std::equal(envelopeMagic.begin(), envelopeMagic.end(), bytes.begin(),
                    [](char expected, std::byte actual) {
                        return static_cast<quint8>(expected) == std::to_integer<quint8>(actual);
                    })) {
        return {AnalysisCacheEnvelopeDecodeStatus::InvalidEnvelope, {},
                QStringLiteral("Analysis cache envelope magic is invalid")};
    }
    if (read32(bytes, 8) != envelopeFormatVersion()) {
        return {AnalysisCacheEnvelopeDecodeStatus::UnsupportedVersion, {},
                QStringLiteral("Analysis cache envelope version is unsupported")};
    }
    if (read32(bytes, 12) != *expectedKindCode) {
        return {AnalysisCacheEnvelopeDecodeStatus::PageKindMismatch, {},
                QStringLiteral("Analysis cache envelope page kind does not match")};
    }
    if (read32(bytes, 16) != payloadVersion(expectedNamespace.versions(), expectedPageKind)) {
        return {AnalysisCacheEnvelopeDecodeStatus::UnsupportedVersion, {},
                QStringLiteral("Analysis cache payload version is unsupported")};
    }
    if (read32(bytes, 20) != 0U) {
        return {AnalysisCacheEnvelopeDecodeStatus::InvalidEnvelope, {},
                QStringLiteral("Analysis cache envelope reserved field is nonzero")};
    }
    const QByteArray namespaceDigest = byteArray(bytes.subspan(24, 32));
    if (namespaceDigest != expectedNamespace.digest()) {
        return {AnalysisCacheEnvelopeDecodeStatus::NamespaceMismatch, {},
                QStringLiteral("Analysis cache envelope belongs to another namespace")};
    }
    const quint64 declaredSize = read64(bytes, 56);
    if (declaredSize > maximumPayloadBytes() ||
        declaredSize != static_cast<quint64>(bytes.size() - headerSizeBytes())) {
        return {AnalysisCacheEnvelopeDecodeStatus::InvalidEnvelope, {},
                QStringLiteral("Analysis cache envelope payload length is invalid")};
    }
    const std::span<const std::byte> payload = bytes.subspan(headerSizeBytes());
    const QByteArray expectedDigest = byteArray(bytes.subspan(64, 32));
    const QByteArray actualDigest =
        QCryptographicHash::hash(byteArray(payload), QCryptographicHash::Sha256);
    if (actualDigest != expectedDigest) {
        return {AnalysisCacheEnvelopeDecodeStatus::CorruptPayload, {},
                QStringLiteral("Analysis cache envelope payload digest does not match")};
    }
    return {AnalysisCacheEnvelopeDecodeStatus::Decoded,
            std::vector<std::byte>(payload.begin(), payload.end()), {}};
}

} // namespace streamview::rules
