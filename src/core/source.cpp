#include <streamview/core/source.h>

#include <QFileDevice>
#include <QIODevice>
#include <QMutexLocker>

#include <algorithm>
#include <limits>
#include <utility>

namespace streamview::core {

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

} // namespace streamview::core
