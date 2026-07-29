#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace streamview::core {

enum class SourceFingerprintMode : quint8 {
    FullContentSha256,
    SampledSha256,
};

class SourceFingerprint final {
public:
    [[nodiscard]] static constexpr quint32 algorithmVersion() noexcept { return 1; }
    [[nodiscard]] static constexpr quint64 sampleSizeBytes() noexcept { return 1024U * 1024U; }
    [[nodiscard]] static constexpr quint64 fullContentLimitBytes() noexcept {
        return 3U * sampleSizeBytes();
    }

    [[nodiscard]] static std::optional<SourceFingerprint>
    create(quint32 version, SourceFingerprintMode mode, quint64 sizeBytes,
           std::optional<qint64> modificationTimeNanoseconds, QByteArray digest,
           QString* errorMessage = nullptr);

    [[nodiscard]] quint32 version() const noexcept { return version_; }
    [[nodiscard]] SourceFingerprintMode mode() const noexcept { return mode_; }
    [[nodiscard]] quint64 sizeBytes() const noexcept { return sizeBytes_; }
    [[nodiscard]] std::optional<qint64> modificationTimeNanoseconds() const noexcept {
        return modificationTimeNanoseconds_;
    }
    [[nodiscard]] const QByteArray& digest() const noexcept { return digest_; }
    [[nodiscard]] QString digestText() const;

    [[nodiscard]] bool operator==(const SourceFingerprint&) const = default;

private:
    SourceFingerprint(quint32 version, SourceFingerprintMode mode, quint64 sizeBytes,
                      std::optional<qint64> modificationTimeNanoseconds, QByteArray digest);

    quint32 version_ = algorithmVersion();
    SourceFingerprintMode mode_ = SourceFingerprintMode::FullContentSha256;
    quint64 sizeBytes_ = 0;
    std::optional<qint64> modificationTimeNanoseconds_;
    QByteArray digest_;
};

enum class SourceFingerprintStatus : quint8 {
    Computed,
    UnsupportedMetadata,
    SourceChanged,
    IoError,
};

struct SourceFingerprintResult final {
    SourceFingerprintStatus status = SourceFingerprintStatus::IoError;
    std::optional<SourceFingerprint> fingerprint;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == SourceFingerprintStatus::Computed && fingerprint.has_value();
    }
};

} // namespace streamview::core
