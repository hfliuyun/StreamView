#include <streamview/rules/rule_package_store.h>

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace streamview::rules {

namespace {

constexpr qsizetype maximumArchiveBytes = 64 * 1024 * 1024;
constexpr quint32 maximumFileBytes = 8U * 1024U * 1024U;
constexpr quint32 maximumManifestBytes = 64U * 1024U;
constexpr quint32 maximumPathBytes = 240U;
constexpr quint64 maximumTotalBytes = 64U * 1024U * 1024U;
constexpr quint16 zipVersion = 20;
constexpr quint16 zipMadeByUnix20 = 0x0314;
constexpr quint16 zipDosTime = 0;
constexpr quint16 zipDosDate = 0x0021;
constexpr quint32 zipRegularReadOnlyAttributes = 0100444U << 16U;
constexpr quint32 localSignature = 0x04034B50U;
constexpr quint32 centralSignature = 0x02014B50U;
constexpr quint32 endSignature = 0x06054B50U;

struct ArchiveEntry final {
    QString path;
    QByteArray contents;
    quint32 crc = 0;
    quint32 localOffset = 0;
};

void append16(QByteArray& output, quint16 value) {
    output.append(static_cast<char>(value & 0xFFU));
    output.append(static_cast<char>((value >> 8U) & 0xFFU));
}

void append32(QByteArray& output, quint32 value) {
    output.append(static_cast<char>(value & 0xFFU));
    output.append(static_cast<char>((value >> 8U) & 0xFFU));
    output.append(static_cast<char>((value >> 16U) & 0xFFU));
    output.append(static_cast<char>((value >> 24U) & 0xFFU));
}

[[nodiscard]] std::optional<quint16> read16(const QByteArray& input, qsizetype* offset) {
    if (*offset < 0 || *offset > input.size() - 2) {
        return std::nullopt;
    }
    const auto first = static_cast<quint8>(input.at(*offset));
    const auto second = static_cast<quint8>(input.at(*offset + 1));
    *offset += 2;
    return static_cast<quint16>(static_cast<quint16>(first) | (static_cast<quint16>(second) << 8U));
}

[[nodiscard]] std::optional<quint32> read32(const QByteArray& input, qsizetype* offset) {
    if (*offset < 0 || *offset > input.size() - 4) {
        return std::nullopt;
    }
    quint32 value = 0;
    for (unsigned int index = 0; index < 4U; ++index) {
        value |= static_cast<quint32>(
                     static_cast<quint8>(input.at(*offset + static_cast<qsizetype>(index))))
                 << (index * 8U);
    }
    *offset += 4;
    return value;
}

[[nodiscard]] const std::array<quint32, 256>& crcTable() {
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> result{};
        for (quint32 index = 0; index < result.size(); ++index) {
            quint32 value = index;
            for (unsigned int bit = 0; bit < 8U; ++bit) {
                value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U : value >> 1U;
            }
            result.at(index) = value;
        }
        return result;
    }();
    return table;
}

[[nodiscard]] quint32 crc32(const QByteArray& data) {
    quint32 crc = 0xFFFFFFFFU;
    for (const char byte : data) {
        const quint8 tableIndex =
            static_cast<quint8>((crc ^ static_cast<quint32>(static_cast<quint8>(byte))) & 0xFFU);
        crc = crcTable().at(tableIndex) ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

[[nodiscard]] bool appendWouldExceed(QByteArray& output, qsizetype additional,
                                     QString* errorMessage) {
    if (additional < 0 || output.size() > maximumArchiveBytes - additional) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Canonical .svrule exceeds the 64 MiB limit");
        }
        return true;
    }
    return false;
}

[[nodiscard]] QByteArray canonicalArchive(const RulePackage& package, QString* errorMessage) {
    std::vector<ArchiveEntry> entries;
    entries.reserve(package.files().size());
    for (const RulePackageFile& file : package.files()) {
        const QByteArray path = file.path.toLatin1();
        const qsizetype recordBytes = 30 + path.size() + file.contents.size();
        if (path.size() > std::numeric_limits<quint16>::max() ||
            file.contents.size() > std::numeric_limits<quint32>::max() || recordBytes < 0) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Package file cannot be represented in ZIP32");
            }
            return {};
        }
        entries.push_back(ArchiveEntry{file.path, file.contents, crc32(file.contents), 0});
    }
    if (entries.size() > std::numeric_limits<quint16>::max()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Package has too many ZIP32 entries");
        }
        return {};
    }

    QByteArray output;
    output.reserve(std::min(maximumArchiveBytes, qsizetype{1'048'576}));
    for (ArchiveEntry& entry : entries) {
        const QByteArray path = entry.path.toLatin1();
        const qsizetype additional = 30 + path.size() + entry.contents.size();
        if (appendWouldExceed(output, additional, errorMessage) ||
            output.size() > std::numeric_limits<quint32>::max()) {
            return {};
        }
        entry.localOffset = static_cast<quint32>(output.size());
        const auto size = static_cast<quint32>(entry.contents.size());
        append32(output, localSignature);
        append16(output, zipVersion);
        append16(output, 0);
        append16(output, 0);
        append16(output, zipDosTime);
        append16(output, zipDosDate);
        append32(output, entry.crc);
        append32(output, size);
        append32(output, size);
        append16(output, static_cast<quint16>(path.size()));
        append16(output, 0);
        output.append(path);
        output.append(entry.contents);
    }

    if (output.size() > std::numeric_limits<quint32>::max()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Central directory offset exceeds ZIP32");
        }
        return {};
    }
    const quint32 centralOffset = static_cast<quint32>(output.size());
    for (const ArchiveEntry& entry : entries) {
        const QByteArray path = entry.path.toLatin1();
        if (appendWouldExceed(output, 46 + path.size(), errorMessage)) {
            return {};
        }
        const auto size = static_cast<quint32>(entry.contents.size());
        append32(output, centralSignature);
        append16(output, zipMadeByUnix20);
        append16(output, zipVersion);
        append16(output, 0);
        append16(output, 0);
        append16(output, zipDosTime);
        append16(output, zipDosDate);
        append32(output, entry.crc);
        append32(output, size);
        append32(output, size);
        append16(output, static_cast<quint16>(path.size()));
        append16(output, 0);
        append16(output, 0);
        append16(output, 0);
        append16(output, 0);
        append32(output, zipRegularReadOnlyAttributes);
        append32(output, entry.localOffset);
        output.append(path);
    }
    const qsizetype centralSizeValue = output.size() - static_cast<qsizetype>(centralOffset);
    if (centralSizeValue < 0 || centralSizeValue > std::numeric_limits<quint32>::max() ||
        appendWouldExceed(output, 22, errorMessage)) {
        return {};
    }
    const auto count = static_cast<quint16>(entries.size());
    append32(output, endSignature);
    append16(output, 0);
    append16(output, 0);
    append16(output, count);
    append16(output, count);
    append32(output, static_cast<quint32>(centralSizeValue));
    append32(output, centralOffset);
    append16(output, 0);
    return output;
}

[[nodiscard]] RulePackageImportResult invalidArchive(const QString& message) {
    return {RulePackageImportStatus::InvalidArchive, std::nullopt, message};
}

[[nodiscard]] RulePackageImportResult parseCanonicalArchive(const QByteArray& bytes) {
    if (bytes.size() > maximumArchiveBytes) {
        return invalidArchive(QStringLiteral(".svrule exceeds the 64 MiB limit"));
    }
    if (bytes.size() < 22) {
        return invalidArchive(QStringLiteral(".svrule is shorter than its end record"));
    }
    qsizetype endOffset = bytes.size() - 22;
    const auto signature = read32(bytes, &endOffset);
    const auto disk = read16(bytes, &endOffset);
    const auto centralDisk = read16(bytes, &endOffset);
    const auto diskCount = read16(bytes, &endOffset);
    const auto totalCount = read16(bytes, &endOffset);
    const auto centralSize = read32(bytes, &endOffset);
    const auto centralOffset = read32(bytes, &endOffset);
    const auto commentLength = read16(bytes, &endOffset);
    if (!signature || !disk || !centralDisk || !diskCount || !totalCount || !centralSize ||
        !centralOffset || !commentLength || *signature != endSignature || *disk != 0U ||
        *centralDisk != 0U || *diskCount != *totalCount || *totalCount == 0U ||
        *totalCount > 1'024U || *centralSize == std::numeric_limits<quint32>::max() ||
        *centralOffset == std::numeric_limits<quint32>::max() || *commentLength != 0U ||
        endOffset != bytes.size()) {
        return invalidArchive(QStringLiteral(".svrule has a noncanonical end record"));
    }
    const quint64 centralEnd = static_cast<quint64>(*centralOffset) + *centralSize;
    if (centralEnd != static_cast<quint64>(bytes.size() - 22)) {
        return invalidArchive(QStringLiteral(".svrule central directory bounds are invalid"));
    }

    std::vector<ArchiveEntry> entries;
    entries.reserve(*totalCount);
    qsizetype offset = 0;
    QByteArray previousPath;
    quint64 totalUncompressed = 0;
    for (quint16 index = 0; index < *totalCount; ++index) {
        if (offset < 0 || static_cast<quint64>(offset) >= *centralOffset) {
            return invalidArchive(QStringLiteral(".svrule local records are truncated"));
        }
        const qsizetype localOffsetValue = offset;
        const auto localHeader = read32(bytes, &offset);
        const auto needed = read16(bytes, &offset);
        const auto flags = read16(bytes, &offset);
        const auto method = read16(bytes, &offset);
        const auto time = read16(bytes, &offset);
        const auto date = read16(bytes, &offset);
        const auto crc = read32(bytes, &offset);
        const auto compressedSize = read32(bytes, &offset);
        const auto uncompressedSize = read32(bytes, &offset);
        const auto nameLength = read16(bytes, &offset);
        const auto extraLength = read16(bytes, &offset);
        if (!localHeader || !needed || !flags || !method || !time || !date || !crc ||
            !compressedSize || !uncompressedSize || !nameLength || !extraLength ||
            *localHeader != localSignature || *needed != zipVersion || *flags != 0U ||
            *method != 0U || *time != zipDosTime || *date != zipDosDate ||
            *compressedSize != *uncompressedSize ||
            *compressedSize == std::numeric_limits<quint32>::max() || *nameLength == 0U ||
            *nameLength > maximumPathBytes || *extraLength != 0U) {
            return invalidArchive(QStringLiteral(".svrule has a noncanonical local header"));
        }
        const quint64 dataEnd = static_cast<quint64>(offset) + *nameLength + *compressedSize;
        if (dataEnd > *centralOffset || dataEnd > static_cast<quint64>(bytes.size())) {
            return invalidArchive(QStringLiteral(".svrule local file data is out of bounds"));
        }
        const QByteArray pathBytes = bytes.mid(offset, *nameLength);
        offset += *nameLength;
        if ((!previousPath.isEmpty() && !(previousPath < pathBytes)) ||
            *compressedSize > maximumFileBytes ||
            (pathBytes == QByteArrayLiteral("rule.toml") &&
             *compressedSize > maximumManifestBytes) ||
            *compressedSize > maximumTotalBytes - totalUncompressed) {
            return invalidArchive(
                QStringLiteral(".svrule entries are not in canonical bounded path order"));
        }
        previousPath = pathBytes;
        totalUncompressed += *compressedSize;
        QByteArray contents = bytes.mid(offset, static_cast<qsizetype>(*compressedSize));
        offset += static_cast<qsizetype>(*compressedSize);
        if (crc32(contents) != *crc) {
            return invalidArchive(QStringLiteral(".svrule file CRC-32 does not match"));
        }
        entries.push_back(ArchiveEntry{QString::fromLatin1(pathBytes), std::move(contents), *crc,
                                       static_cast<quint32>(localOffsetValue)});
    }
    if (static_cast<quint64>(offset) != *centralOffset) {
        return invalidArchive(QStringLiteral(".svrule local records are not contiguous"));
    }

    for (const ArchiveEntry& entry : entries) {
        const auto header = read32(bytes, &offset);
        const auto madeBy = read16(bytes, &offset);
        const auto needed = read16(bytes, &offset);
        const auto flags = read16(bytes, &offset);
        const auto method = read16(bytes, &offset);
        const auto time = read16(bytes, &offset);
        const auto date = read16(bytes, &offset);
        const auto crc = read32(bytes, &offset);
        const auto compressedSize = read32(bytes, &offset);
        const auto uncompressedSize = read32(bytes, &offset);
        const auto nameLength = read16(bytes, &offset);
        const auto extraLength = read16(bytes, &offset);
        const auto entryCommentLength = read16(bytes, &offset);
        const auto startDisk = read16(bytes, &offset);
        const auto internalAttributes = read16(bytes, &offset);
        const auto externalAttributes = read32(bytes, &offset);
        const auto localOffset = read32(bytes, &offset);
        const QByteArray expectedPath = entry.path.toLatin1();
        if (!header || !madeBy || !needed || !flags || !method || !time || !date || !crc ||
            !compressedSize || !uncompressedSize || !nameLength || !extraLength ||
            !entryCommentLength || !startDisk || !internalAttributes || !externalAttributes ||
            !localOffset || *header != centralSignature || *madeBy != zipMadeByUnix20 ||
            *needed != zipVersion || *flags != 0U || *method != 0U || *time != zipDosTime ||
            *date != zipDosDate || *crc != entry.crc || *compressedSize != entry.contents.size() ||
            *uncompressedSize != entry.contents.size() || *nameLength != expectedPath.size() ||
            *extraLength != 0U || *entryCommentLength != 0U || *startDisk != 0U ||
            *internalAttributes != 0U || *externalAttributes != zipRegularReadOnlyAttributes ||
            *localOffset != entry.localOffset ||
            offset > bytes.size() - static_cast<qsizetype>(*nameLength) ||
            bytes.mid(offset, *nameLength) != expectedPath) {
            return invalidArchive(QStringLiteral(".svrule has a noncanonical central record"));
        }
        offset += *nameLength;
    }
    if (offset != bytes.size() - 22) {
        return invalidArchive(QStringLiteral(".svrule central records are not contiguous"));
    }

    std::vector<RulePackageFile> files;
    files.reserve(entries.size());
    for (ArchiveEntry& entry : entries) {
        files.push_back(RulePackageFile{std::move(entry.path), std::move(entry.contents)});
    }
    RulePackageLoadResult loaded = RulePackage::fromFiles(std::move(files));
    if (!loaded.succeeded()) {
        return {RulePackageImportStatus::InvalidPackage, std::nullopt,
                QStringLiteral(".svrule contains an invalid package: %1").arg(loaded.errorMessage)};
    }
    return {RulePackageImportStatus::Imported, std::move(loaded.package), {}};
}

} // namespace

RulePackageWriteResult RulePackageStore::writeArchive(const RulePackage& package,
                                                      const QString& archivePath) {
    if (archivePath.isEmpty() || !archivePath.endsWith(QStringLiteral(".svrule"))) {
        return {RulePackageWriteStatus::InvalidDestination,
                QStringLiteral("Rule package archive path must end in .svrule")};
    }
    QString archiveError;
    const QByteArray bytes = canonicalArchive(package, &archiveError);
    if (bytes.isEmpty()) {
        return {RulePackageWriteStatus::ArchiveTooLarge, std::move(archiveError)};
    }
    QSaveFile file(archivePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return {RulePackageWriteStatus::IoError, file.errorString()};
    }
    if (file.write(bytes) != bytes.size()) {
        return {RulePackageWriteStatus::IoError, file.errorString()};
    }
    if (!file.commit()) {
        return {RulePackageWriteStatus::IoError, file.errorString()};
    }
    return {RulePackageWriteStatus::Written, {}};
}

RulePackageImportResult RulePackageStore::importArchive(const QString& archivePath) {
    if (archivePath.isEmpty() || !archivePath.endsWith(QStringLiteral(".svrule"))) {
        return {RulePackageImportStatus::InvalidInput, std::nullopt,
                QStringLiteral("Rule package archive path must end in .svrule")};
    }
    QFile file(archivePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {RulePackageImportStatus::IoError, std::nullopt, file.errorString()};
    }
    if (file.size() < 0 || file.size() > maximumArchiveBytes) {
        return invalidArchive(QStringLiteral(".svrule exceeds the 64 MiB limit"));
    }
    const qint64 initialSize = file.size();
    const QByteArray bytes = file.read(maximumArchiveBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        return {RulePackageImportStatus::IoError, std::nullopt, file.errorString()};
    }
    if (bytes.size() > maximumArchiveBytes || !file.atEnd()) {
        return invalidArchive(QStringLiteral(".svrule exceeds the 64 MiB limit"));
    }
    if (bytes.size() != initialSize || file.size() != initialSize) {
        return invalidArchive(QStringLiteral(".svrule changed while it was being read"));
    }
    return parseCanonicalArchive(bytes);
}

} // namespace streamview::rules
