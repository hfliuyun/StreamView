#pragma once

#include <streamview/core/paged_cache.h>
#include <streamview/core/source_fingerprint.h>
#include <streamview/rules/rule_package.h>

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace streamview::rules {

struct AnalysisCacheVersions final {
    quint32 namespaceFormat = 1;
    quint32 cacheSchema = core::PagedCache::schemaVersion();
    quint32 payloadEnvelope = 1;
    quint32 progressiveIndexPayload = 1;
    quint32 materializedResultPayload = 1;

    [[nodiscard]] static constexpr AnalysisCacheVersions current() noexcept { return {}; }
    [[nodiscard]] bool operator==(const AnalysisCacheVersions&) const = default;
};

class AnalysisCacheNamespace final {
public:
    [[nodiscard]] static constexpr quint32 namespaceFormatVersion() noexcept { return 1; }

    [[nodiscard]] static std::optional<AnalysisCacheNamespace>
    create(const core::SourceFingerprint& sourceFingerprint,
           const RuleEntryPointIdentity& ruleIdentity,
           AnalysisCacheVersions versions = AnalysisCacheVersions::current(),
           QString* errorMessage = nullptr);

    [[nodiscard]] const QString& name() const noexcept { return name_; }
    [[nodiscard]] const QByteArray& digest() const noexcept { return digest_; }
    [[nodiscard]] const AnalysisCacheVersions& versions() const noexcept { return versions_; }

    [[nodiscard]] bool operator==(const AnalysisCacheNamespace&) const = default;

private:
    AnalysisCacheNamespace(QString name, QByteArray digest, AnalysisCacheVersions versions);

    QString name_;
    QByteArray digest_;
    AnalysisCacheVersions versions_;
};

enum class AnalysisCacheEnvelopeEncodeStatus : quint8 {
    Encoded,
    InvalidArgument,
    PayloadTooLarge,
};

struct AnalysisCacheEnvelopeEncodeResult final {
    AnalysisCacheEnvelopeEncodeStatus status = AnalysisCacheEnvelopeEncodeStatus::PayloadTooLarge;
    std::vector<std::byte> bytes;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisCacheEnvelopeEncodeStatus::Encoded;
    }
};

enum class AnalysisCacheEnvelopeDecodeStatus : quint8 {
    Decoded,
    InvalidEnvelope,
    NamespaceMismatch,
    PageKindMismatch,
    UnsupportedVersion,
    CorruptPayload,
};

struct AnalysisCacheEnvelopeDecodeResult final {
    AnalysisCacheEnvelopeDecodeStatus status = AnalysisCacheEnvelopeDecodeStatus::InvalidEnvelope;
    std::vector<std::byte> payload;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == AnalysisCacheEnvelopeDecodeStatus::Decoded;
    }
};

class AnalysisCachePayloadEnvelope final {
public:
    [[nodiscard]] static constexpr quint32 envelopeFormatVersion() noexcept { return 1; }
    [[nodiscard]] static constexpr std::size_t headerSizeBytes() noexcept { return 96; }
    [[nodiscard]] static constexpr std::size_t maximumPayloadBytes() noexcept {
        return core::PagedCache::pageSizeBytes() - headerSizeBytes();
    }

    [[nodiscard]] static AnalysisCacheEnvelopeEncodeResult
    encode(const AnalysisCacheNamespace& cacheNamespace, core::PagedCachePageKind pageKind,
           std::span<const std::byte> payload);

    [[nodiscard]] static AnalysisCacheEnvelopeDecodeResult
    decode(const AnalysisCacheNamespace& expectedNamespace,
           core::PagedCachePageKind expectedPageKind, std::span<const std::byte> bytes);
};

} // namespace streamview::rules
