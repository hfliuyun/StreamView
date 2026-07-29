#include <streamview/core/source.h>

#include <QFileDevice>
#include <QCryptographicHash>
#include <QIODevice>
#include <QMutexLocker>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#if defined(Q_OS_WIN)
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace streamview::core {

namespace {

struct FileSnapshot final {
    quint64 sizeBytes = 0;
    qint64 modificationTimeNanoseconds = 0;

    [[nodiscard]] bool operator==(const FileSnapshot&) const = default;
};

#if defined(Q_OS_WIN)
[[nodiscard]] QString nativeError(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString message =
        length == 0U ? QStringLiteral("Windows error %1").arg(error)
                     : QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed();
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

[[nodiscard]] std::optional<qint64> windowsModificationTimeNanoseconds(qint64 ticks) {
    constexpr quint64 windowsToUnixEpochTicks = 116'444'736'000'000'000ULL;
    constexpr quint64 maximumConvertibleTicks =
        static_cast<quint64>(std::numeric_limits<qint64>::max()) / 100U;
    if (ticks < 0) {
        return std::nullopt;
    }
    const quint64 value = static_cast<quint64>(ticks);
    if (value >= windowsToUnixEpochTicks) {
        const quint64 elapsed = value - windowsToUnixEpochTicks;
        if (elapsed > maximumConvertibleTicks) {
            return std::nullopt;
        }
        return static_cast<qint64>(elapsed * 100U);
    }
    const quint64 elapsed = windowsToUnixEpochTicks - value;
    if (elapsed > maximumConvertibleTicks) {
        return std::nullopt;
    }
    return -static_cast<qint64>(elapsed * 100U);
}
#else
[[nodiscard]] QString nativeError(int error) { return QString::fromLocal8Bit(std::strerror(error)); }

[[nodiscard]] std::optional<qint64> unixModificationTimeNanoseconds(qint64 seconds,
                                                                    qint64 nanoseconds) {
    constexpr qint64 nanosecondsPerSecond = 1'000'000'000LL;
    if (nanoseconds < 0 || nanoseconds >= nanosecondsPerSecond ||
        seconds > std::numeric_limits<qint64>::max() / nanosecondsPerSecond ||
        seconds < std::numeric_limits<qint64>::min() / nanosecondsPerSecond) {
        return std::nullopt;
    }
    const qint64 base = seconds * nanosecondsPerSecond;
    if (seconds >= 0 && nanoseconds > std::numeric_limits<qint64>::max() - base) {
        return std::nullopt;
    }
    return base + nanoseconds;
}
#endif

[[nodiscard]] std::optional<FileSnapshot>
captureSnapshot(const QFile& file, SourceFingerprintStatus* failureStatus, QString* errorMessage) {
    const int descriptor = file.handle();
    if (descriptor < 0) {
        *failureStatus = SourceFingerprintStatus::IoError;
        *errorMessage = QStringLiteral("Source file has no native handle");
        return std::nullopt;
    }
#if defined(Q_OS_WIN)
    const intptr_t native = _get_osfhandle(descriptor);
    if (native == -1) {
        *failureStatus = SourceFingerprintStatus::IoError;
        *errorMessage = QString::fromLocal8Bit(std::strerror(errno));
        return std::nullopt;
    }
    const HANDLE handle = reinterpret_cast<HANDLE>(native);
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) {
        *failureStatus = SourceFingerprintStatus::IoError;
        *errorMessage = nativeError(GetLastError());
        return std::nullopt;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK || standard.Directory ||
        standard.EndOfFile.QuadPart < 0) {
        *failureStatus = SourceFingerprintStatus::UnsupportedMetadata;
        *errorMessage = QStringLiteral("Source handle is not a supported regular file");
        return std::nullopt;
    }
    const auto modificationTime =
        windowsModificationTimeNanoseconds(basic.LastWriteTime.QuadPart);
    if (!modificationTime.has_value()) {
        *failureStatus = SourceFingerprintStatus::UnsupportedMetadata;
        *errorMessage = QStringLiteral("Source modification time cannot be represented in nanoseconds");
        return std::nullopt;
    }
    return FileSnapshot{static_cast<quint64>(standard.EndOfFile.QuadPart), *modificationTime};
#else
    struct stat information {};
    if (::fstat(descriptor, &information) != 0) {
        *failureStatus = SourceFingerprintStatus::IoError;
        *errorMessage = nativeError(errno);
        return std::nullopt;
    }
    if (!S_ISREG(information.st_mode) || information.st_size < 0) {
        *failureStatus = SourceFingerprintStatus::UnsupportedMetadata;
        *errorMessage = QStringLiteral("Source handle is not a supported regular file");
        return std::nullopt;
    }
#if defined(Q_OS_MACOS)
    const qint64 seconds = static_cast<qint64>(information.st_mtimespec.tv_sec);
    const qint64 nanoseconds = static_cast<qint64>(information.st_mtimespec.tv_nsec);
#else
    const qint64 seconds = static_cast<qint64>(information.st_mtim.tv_sec);
    const qint64 nanoseconds = static_cast<qint64>(information.st_mtim.tv_nsec);
#endif
    const auto modificationTime = unixModificationTimeNanoseconds(seconds, nanoseconds);
    if (!modificationTime.has_value()) {
        *failureStatus = SourceFingerprintStatus::UnsupportedMetadata;
        *errorMessage = QStringLiteral("Source modification time cannot be represented in nanoseconds");
        return std::nullopt;
    }
    return FileSnapshot{static_cast<quint64>(information.st_size), *modificationTime};
#endif
}

[[nodiscard]] bool addFileRange(QFile* file, quint64 offset, quint64 length,
                                QCryptographicHash* hash, QString* errorMessage) {
    constexpr qsizetype bufferSize = 64 * 1024;
    std::array<char, static_cast<std::size_t>(bufferSize)> buffer{};
    if (offset > static_cast<quint64>(std::numeric_limits<qint64>::max()) ||
        !file->seek(static_cast<qint64>(offset))) {
        *errorMessage = file->errorString();
        return false;
    }
    quint64 remaining = length;
    while (remaining > 0) {
        const qsizetype requested = static_cast<qsizetype>(
            std::min<quint64>(remaining, static_cast<quint64>(buffer.size())));
        const qint64 bytesRead = file->read(buffer.data(), requested);
        if (bytesRead <= 0) {
            *errorMessage = file->error() == QFileDevice::NoError
                                ? QStringLiteral("Source ended while computing its fingerprint")
                                : file->errorString();
            return false;
        }
        hash->addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(bytesRead)));
        remaining -= static_cast<quint64>(bytesRead);
    }
    return true;
}

} // namespace

FileSource::FileSource(QString path) : path_(std::move(path)), file_(path_) {}

std::unique_ptr<FileSource> FileSource::open(const QString& path, QString* errorMessage) {
    auto source = std::unique_ptr<FileSource>(new FileSource(path));
    if (!source->file_.open(QIODevice::ReadOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = source->file_.errorString();
        }
        return nullptr;
    }

    const qint64 fileSize = source->file_.size();
    if (fileSize < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = source->file_.errorString();
        }
        return nullptr;
    }

    source->sizeBytes_ = static_cast<quint64>(fileSize);
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return source;
}

SourceReadResult FileSource::readAt(quint64 byteOffset,
                                    std::span<std::byte> destination) const {
    if (destination.empty()) {
        return {SourceReadStatus::Complete, 0, {}};
    }
    if (byteOffset >= sizeBytes_) {
        return {SourceReadStatus::EndOfSource, 0, {}};
    }
    if (byteOffset > static_cast<quint64>(std::numeric_limits<qint64>::max()) ||
        destination.size() > static_cast<std::size_t>(std::numeric_limits<qint64>::max())) {
        return {SourceReadStatus::Error,
                0,
                QStringLiteral("Read offset or size exceeds Qt limits")};
    }

    const quint64 available = sizeBytes_ - byteOffset;
    const quint64 requested =
        std::min<quint64>(available, static_cast<quint64>(destination.size()));

    QMutexLocker locker(&mutex_);
    if (!file_.seek(static_cast<qint64>(byteOffset))) {
        return {SourceReadStatus::Error, 0, file_.errorString()};
    }

    const qint64 bytesRead = file_.read(reinterpret_cast<char*>(destination.data()),
                                        static_cast<qint64>(requested));
    if (bytesRead < 0) {
        return {SourceReadStatus::Error, 0, file_.errorString()};
    }

    const auto count = static_cast<std::size_t>(bytesRead);
    if (count == destination.size()) {
        return {SourceReadStatus::Complete, count, {}};
    }
    if (file_.error() != QFileDevice::NoError) {
        return {SourceReadStatus::Error, count, file_.errorString()};
    }
    return {SourceReadStatus::EndOfSource, count, {}};
}

SourceFingerprintResult FileSource::fingerprint() const {
    QMutexLocker locker(&mutex_);
    SourceFingerprintStatus failureStatus = SourceFingerprintStatus::IoError;
    QString errorMessage;
    const auto before = captureSnapshot(file_, &failureStatus, &errorMessage);
    if (!before.has_value()) {
        return {failureStatus, std::nullopt, std::move(errorMessage)};
    }
    if (before->sizeBytes != sizeBytes_) {
        return {SourceFingerprintStatus::SourceChanged, std::nullopt,
                QStringLiteral("Source size changed after it was opened")};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const SourceFingerprintMode mode =
        sizeBytes_ <= SourceFingerprint::fullContentLimitBytes()
            ? SourceFingerprintMode::FullContentSha256
            : SourceFingerprintMode::SampledSha256;
    bool hashed = false;
    if (mode == SourceFingerprintMode::FullContentSha256) {
        hashed = addFileRange(&file_, 0, sizeBytes_, &hash, &errorMessage);
    } else {
        const quint64 sampleSize = SourceFingerprint::sampleSizeBytes();
        const quint64 middleOffset = (sizeBytes_ - sampleSize) / 2U;
        const quint64 tailOffset = sizeBytes_ - sampleSize;
        hashed = addFileRange(&file_, 0, sampleSize, &hash, &errorMessage) &&
                 addFileRange(&file_, middleOffset, sampleSize, &hash, &errorMessage) &&
                 addFileRange(&file_, tailOffset, sampleSize, &hash, &errorMessage);
    }

    const auto after = captureSnapshot(file_, &failureStatus, &errorMessage);
    if (!after.has_value()) {
        return {failureStatus, std::nullopt, std::move(errorMessage)};
    }
    if (*after != *before) {
        return {SourceFingerprintStatus::SourceChanged, std::nullopt,
                QStringLiteral("Source changed while computing its fingerprint")};
    }
    if (!hashed) {
        return {SourceFingerprintStatus::IoError, std::nullopt, std::move(errorMessage)};
    }

    auto fingerprint = SourceFingerprint::create(
        SourceFingerprint::algorithmVersion(), mode, before->sizeBytes,
        mode == SourceFingerprintMode::SampledSha256
            ? std::optional<qint64>(before->modificationTimeNanoseconds)
            : std::nullopt,
        hash.result(), &errorMessage);
    if (!fingerprint.has_value()) {
        return {SourceFingerprintStatus::UnsupportedMetadata, std::nullopt,
                std::move(errorMessage)};
    }
    return {SourceFingerprintStatus::Computed, std::move(fingerprint), {}};
}

} // namespace streamview::core
