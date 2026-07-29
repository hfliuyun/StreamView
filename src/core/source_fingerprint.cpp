#include <streamview/core/source_fingerprint.h>

#include <QCryptographicHash>

#include <utility>

namespace streamview::core {

namespace {

void setError(QString* errorMessage, const QString& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

} // namespace

SourceFingerprint::SourceFingerprint(quint32 version, SourceFingerprintMode mode,
                                     quint64 sizeBytes,
                                     std::optional<qint64> modificationTimeNanoseconds,
                                     QByteArray digest)
    : version_(version), mode_(mode), sizeBytes_(sizeBytes),
      modificationTimeNanoseconds_(modificationTimeNanoseconds), digest_(std::move(digest)) {}

std::optional<SourceFingerprint>
SourceFingerprint::create(quint32 version, SourceFingerprintMode mode, quint64 sizeBytes,
                          std::optional<qint64> modificationTimeNanoseconds, QByteArray digest,
                          QString* errorMessage) {
    if (version != algorithmVersion()) {
        setError(errorMessage, QStringLiteral("Unsupported source fingerprint version"));
        return std::nullopt;
    }
    const bool fullContent = mode == SourceFingerprintMode::FullContentSha256;
    const bool sampled = mode == SourceFingerprintMode::SampledSha256;
    if ((!fullContent && !sampled) ||
        (fullContent && sizeBytes > fullContentLimitBytes()) ||
        (sampled && sizeBytes <= fullContentLimitBytes()) ||
        (fullContent && modificationTimeNanoseconds.has_value()) ||
        (sampled && !modificationTimeNanoseconds.has_value())) {
        setError(errorMessage, QStringLiteral("Source fingerprint mode does not match its size"));
        return std::nullopt;
    }
    if (digest.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        setError(errorMessage, QStringLiteral("Source fingerprint digest must be SHA-256"));
        return std::nullopt;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return SourceFingerprint(version, mode, sizeBytes, modificationTimeNanoseconds,
                             std::move(digest));
}

QString SourceFingerprint::digestText() const { return QString::fromLatin1(digest_.toHex()); }

} // namespace streamview::core
