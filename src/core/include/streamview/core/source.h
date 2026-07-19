#pragma once

#include <QFile>
#include <QMutex>
#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <memory>
#include <span>

namespace streamview::core {

enum class SourceReadStatus : quint8 {
    Complete,
    EndOfSource,
    Error,
};

struct SourceReadResult final {
    SourceReadStatus status = SourceReadStatus::Error;
    std::size_t bytesRead = 0;
    QString errorMessage;

    [[nodiscard]] bool complete() const noexcept { return status == SourceReadStatus::Complete; }
};

class RandomAccessSource {
public:
    virtual ~RandomAccessSource() = default;

    [[nodiscard]] virtual quint64 sizeBytes() const noexcept = 0;
    [[nodiscard]] virtual QString identity() const = 0;
    [[nodiscard]] virtual SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const = 0;
};

class FileSource final : public RandomAccessSource {
public:
    [[nodiscard]] static std::unique_ptr<FileSource>
    open(const QString& path, QString* errorMessage = nullptr);

    ~FileSource() override = default;

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return sizeBytes_; }
    [[nodiscard]] QString identity() const override { return path_; }
    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override;

private:
    explicit FileSource(QString path);

    QString path_;
    quint64 sizeBytes_ = 0;
    mutable QFile file_;
    mutable QMutex mutex_;
};

} // namespace streamview::core
