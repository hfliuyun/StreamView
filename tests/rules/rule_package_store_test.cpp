#include <streamview/rules/rule_package.h>
#include <streamview/rules/rule_package_store.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#endif

using streamview::rules::RulePackage;
using streamview::rules::RulePackageFile;
using streamview::rules::RulePackageImportStatus;
using streamview::rules::RulePackageInstallStatus;
using streamview::rules::RulePackageLoadResult;
using streamview::rules::RulePackageStore;
using streamview::rules::RulePackageWriteStatus;

namespace {

[[nodiscard]] QByteArray manifest() {
    return QByteArrayLiteral("manifest-version = 1\n"
                             "\n"
                             "[package]\n"
                             "id = \"org.example.packet\"\n"
                             "version = \"0.1.0\"\n"
                             "authors = [\"Example Author\"]\n"
                             "license = \"MIT\"\n"
                             "dependencies = []\n"
                             "\n"
                             "[compatibility]\n"
                             "language = \"0.1\"\n"
                             "engine = \">=0.1.0 <0.2.0\"\n"
                             "\n"
                             "[[entrypoints]]\n"
                             "id = \"packet\"\n"
                             "format = \"application.example.packet\"\n"
                             "source = \"src/packet.svfmt\"\n"
                             "profiles = [\"baseline\"]\n"
                             "depth = \"header\"\n");
}

[[nodiscard]] std::vector<RulePackageFile> packageFiles() {
    return {
        {QStringLiteral("rule.toml"), manifest()},
        {QStringLiteral("src/packet.svfmt"),
         QByteArrayLiteral("struct Packet { bits<8> value; }\nentry Packet;\n")},
    };
}

[[nodiscard]] RulePackageLoadResult loadPackage() { return RulePackage::fromFiles(packageFiles()); }

[[nodiscard]] QString realPath(const QTemporaryDir& directory) {
    return QFileInfo(directory.path()).canonicalFilePath();
}

[[nodiscard]] bool writeFile(const QString& path, const QByteArray& contents) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(contents) == contents.size() && file.flush();
}

[[nodiscard]] bool writePackageDirectory(const QString& rootPath) {
    for (const RulePackageFile& packageFile : packageFiles()) {
        if (!writeFile(QDir(rootPath).filePath(packageFile.path), packageFile.contents)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<QByteArray> readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return std::nullopt;
    }
    return contents;
}

[[nodiscard]] quint16 little16(const QByteArray& bytes, qsizetype offset) {
    return static_cast<quint16>(static_cast<quint8>(bytes.at(offset))) |
           static_cast<quint16>(static_cast<quint16>(static_cast<quint8>(bytes.at(offset + 1)))
                                << 8U);
}

[[nodiscard]] quint32 little32(const QByteArray& bytes, qsizetype offset) {
    quint32 value = 0;
    for (unsigned int index = 0; index < 4U; ++index) {
        value |= static_cast<quint32>(
                     static_cast<quint8>(bytes.at(offset + static_cast<qsizetype>(index))))
                 << (index * 8U);
    }
    return value;
}

void write16(QByteArray& bytes, qsizetype offset, quint16 value) {
    bytes[offset] = static_cast<char>(value & 0xFFU);
    bytes[offset + 1] = static_cast<char>((value >> 8U) & 0xFFU);
}

void write32(QByteArray& bytes, qsizetype offset, quint32 value) {
    for (unsigned int index = 0; index < 4U; ++index) {
        bytes[offset + static_cast<qsizetype>(index)] =
            static_cast<char>((value >> (index * 8U)) & 0xFFU);
    }
}

struct ZipLayout final {
    std::vector<qsizetype> localStarts;
    std::vector<qsizetype> localEnds;
    std::vector<qsizetype> centralStarts;
    std::vector<qsizetype> centralEnds;
    qsizetype endRecord = 0;
};

[[nodiscard]] std::optional<ZipLayout> inspectCanonicalZip(const QByteArray& bytes) {
    constexpr quint32 localSignature = 0x04034B50U;
    constexpr quint32 centralSignature = 0x02014B50U;
    constexpr quint32 endSignature = 0x06054B50U;
    if (bytes.size() < 22) {
        return std::nullopt;
    }
    ZipLayout layout;
    layout.endRecord = bytes.size() - 22;
    if (little32(bytes, layout.endRecord) != endSignature) {
        return std::nullopt;
    }
    const quint16 count = little16(bytes, layout.endRecord + 10);
    const qsizetype centralStart = static_cast<qsizetype>(little32(bytes, layout.endRecord + 16));
    qsizetype offset = 0;
    for (quint16 index = 0; index < count; ++index) {
        if (offset < 0 || offset > centralStart - 30 || little32(bytes, offset) != localSignature) {
            return std::nullopt;
        }
        layout.localStarts.push_back(offset);
        const quint64 end = static_cast<quint64>(offset) + 30U + little16(bytes, offset + 26) +
                            little16(bytes, offset + 28) + little32(bytes, offset + 18);
        if (end > static_cast<quint64>(centralStart)) {
            return std::nullopt;
        }
        offset = static_cast<qsizetype>(end);
        layout.localEnds.push_back(offset);
    }
    if (offset != centralStart) {
        return std::nullopt;
    }
    for (quint16 index = 0; index < count; ++index) {
        if (offset < 0 || offset > layout.endRecord - 46 ||
            little32(bytes, offset) != centralSignature) {
            return std::nullopt;
        }
        layout.centralStarts.push_back(offset);
        const quint64 end = static_cast<quint64>(offset) + 46U + little16(bytes, offset + 28) +
                            little16(bytes, offset + 30) + little16(bytes, offset + 32);
        if (end > static_cast<quint64>(layout.endRecord)) {
            return std::nullopt;
        }
        offset = static_cast<qsizetype>(end);
        layout.centralEnds.push_back(offset);
    }
    if (offset != layout.endRecord) {
        return std::nullopt;
    }
    return layout;
}

[[nodiscard]] QByteArray reorderTwoEntryArchive(const QByteArray& canonical,
                                                const ZipLayout& layout) {
    if (layout.localStarts.size() != 2U || layout.centralStarts.size() != 2U) {
        return {};
    }
    const QByteArray secondLocal = canonical.sliced(
        layout.localStarts.at(1), layout.localEnds.at(1) - layout.localStarts.at(1));
    const QByteArray firstLocal = canonical.sliced(
        layout.localStarts.at(0), layout.localEnds.at(0) - layout.localStarts.at(0));
    QByteArray secondCentral = canonical.sliced(
        layout.centralStarts.at(1), layout.centralEnds.at(1) - layout.centralStarts.at(1));
    QByteArray firstCentral = canonical.sliced(
        layout.centralStarts.at(0), layout.centralEnds.at(0) - layout.centralStarts.at(0));
    write32(secondCentral, 42, 0);
    write32(firstCentral, 42, static_cast<quint32>(secondLocal.size()));

    QByteArray reordered;
    reordered += secondLocal;
    reordered += firstLocal;
    const quint32 centralOffset = static_cast<quint32>(reordered.size());
    reordered += secondCentral;
    reordered += firstCentral;
    QByteArray endRecord = canonical.sliced(layout.endRecord);
    write32(endRecord, 12, static_cast<quint32>(secondCentral.size() + firstCentral.size()));
    write32(endRecord, 16, centralOffset);
    reordered += endRecord;
    return reordered;
}

} // namespace

class RulePackageStoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void importsDirectoryAndRoundTripsCanonicalArchive() {
        const auto inMemory = loadPackage();
        QVERIFY2(inMemory.succeeded(), qPrintable(inMemory.errorMessage));
        QTemporaryDir packageDirectory;
        QTemporaryDir outputDirectory;
        QVERIFY(packageDirectory.isValid());
        QVERIFY(outputDirectory.isValid());
        const QString packageRoot = realPath(packageDirectory);
        QVERIFY(!packageRoot.isEmpty());
        QVERIFY(writePackageDirectory(packageRoot));

        const auto importedDirectory = RulePackageStore::importDirectory(packageRoot);
        QVERIFY2(importedDirectory.succeeded(), qPrintable(importedDirectory.errorMessage));
        QCOMPARE(importedDirectory.package->identity(), inMemory.package->identity());

        const QString firstPath = outputDirectory.filePath(QStringLiteral("first.svrule"));
        const QString secondPath = outputDirectory.filePath(QStringLiteral("second.svrule"));
        QCOMPARE(RulePackageStore::writeArchive(*inMemory.package, firstPath).status,
                 RulePackageWriteStatus::Written);
        QCOMPARE(RulePackageStore::writeArchive(*inMemory.package, secondPath).status,
                 RulePackageWriteStatus::Written);
        const auto firstBytes = readFile(firstPath);
        const auto secondBytes = readFile(secondPath);
        QVERIFY(firstBytes.has_value());
        QVERIFY(secondBytes.has_value());
        QCOMPARE(*firstBytes, *secondBytes);

        const auto importedArchive = RulePackageStore::importArchive(firstPath);
        QVERIFY2(importedArchive.succeeded(), qPrintable(importedArchive.errorMessage));
        QCOMPARE(importedArchive.package->identity(), inMemory.package->identity());
        QCOMPARE(importedArchive.package->files().size(), inMemory.package->files().size());
        for (std::size_t index = 0; index < inMemory.package->files().size(); ++index) {
            QCOMPARE(importedArchive.package->files().at(index).path,
                     inMemory.package->files().at(index).path);
            QCOMPARE(importedArchive.package->files().at(index).contents,
                     inMemory.package->files().at(index).contents);
        }
    }

    void rejectsNoncanonicalAndMalformedArchives() {
        const auto loaded = loadPackage();
        QVERIFY(loaded.succeeded());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString canonicalPath = directory.filePath(QStringLiteral("canonical.svrule"));
        QVERIFY(RulePackageStore::writeArchive(*loaded.package, canonicalPath).succeeded());
        const auto canonical = readFile(canonicalPath);
        QVERIFY(canonical.has_value());
        const auto layout = inspectCanonicalZip(*canonical);
        QVERIFY(layout.has_value());
        QCOMPARE(layout->localStarts.size(), std::size_t{2});
        QCOMPARE(layout->centralStarts.size(), std::size_t{2});

        struct InvalidArchiveCase final {
            QString name;
            QByteArray bytes;
            QString expectedMessage;
            RulePackageImportStatus expectedStatus = RulePackageImportStatus::InvalidArchive;
        };
        std::vector<InvalidArchiveCase> cases;

        QByteArray badMethod = *canonical;
        write16(badMethod, layout->localStarts.at(0) + 8, 8);
        cases.push_back({QStringLiteral("bad-method"), std::move(badMethod),
                         QStringLiteral("noncanonical local header")});

        QByteArray badCrc = *canonical;
        badCrc[layout->localStarts.at(0) + 14] =
            static_cast<char>(badCrc.at(layout->localStarts.at(0) + 14) ^ 0x01);
        cases.push_back({QStringLiteral("bad-crc"), std::move(badCrc), QStringLiteral("CRC-32")});

        QByteArray zip64Sentinel = *canonical;
        write32(zip64Sentinel, layout->endRecord + 16, std::numeric_limits<quint32>::max());
        cases.push_back({QStringLiteral("zip64-sentinel"), std::move(zip64Sentinel),
                         QStringLiteral("noncanonical end record")});

        QByteArray zip64Count = *canonical;
        write16(zip64Count, layout->endRecord + 8, std::numeric_limits<quint16>::max());
        write16(zip64Count, layout->endRecord + 10, std::numeric_limits<quint16>::max());
        cases.push_back({QStringLiteral("zip64-count"), std::move(zip64Count),
                         QStringLiteral("noncanonical end record")});

        QByteArray traversalName = *canonical;
        const QByteArray traversal = QByteArrayLiteral("src/../packet.xx");
        QCOMPARE(traversal.size(), little16(*canonical, layout->localStarts.at(1) + 26));
        traversalName.replace(layout->localStarts.at(1) + 30, traversal.size(), traversal);
        traversalName.replace(layout->centralStarts.at(1) + 46, traversal.size(), traversal);
        cases.push_back({QStringLiteral("traversal-name"), std::move(traversalName),
                         QStringLiteral("invalid package"),
                         RulePackageImportStatus::InvalidPackage});

        QByteArray unicodeName = *canonical;
        unicodeName[layout->localStarts.at(1) + 34] = static_cast<char>(0xFF);
        unicodeName[layout->centralStarts.at(1) + 50] = static_cast<char>(0xFF);
        cases.push_back({QStringLiteral("unicode-name"), std::move(unicodeName),
                         QStringLiteral("invalid package"),
                         RulePackageImportStatus::InvalidPackage});

        QByteArray badAttributes = *canonical;
        write32(badAttributes, layout->centralStarts.at(0) + 38, 0);
        cases.push_back({QStringLiteral("bad-attributes"), std::move(badAttributes),
                         QStringLiteral("noncanonical central record")});

        QByteArray badLocalOffset = *canonical;
        write32(badLocalOffset, layout->centralStarts.at(0) + 42, 1);
        cases.push_back({QStringLiteral("bad-local-offset"), std::move(badLocalOffset),
                         QStringLiteral("noncanonical central record")});

        QByteArray badCentralOffset = *canonical;
        write32(badCentralOffset, layout->endRecord + 16,
                little32(*canonical, layout->endRecord + 16) + 1U);
        cases.push_back({QStringLiteral("bad-central-offset"), std::move(badCentralOffset),
                         QStringLiteral("central directory bounds")});

        QByteArray badCentralSize = *canonical;
        write32(badCentralSize, layout->endRecord + 12,
                little32(*canonical, layout->endRecord + 12) + 1U);
        cases.push_back({QStringLiteral("bad-central-size"), std::move(badCentralSize),
                         QStringLiteral("central directory bounds")});

        QByteArray overlappingLocalData = *canonical;
        write32(overlappingLocalData, layout->localStarts.at(0) + 18,
                little32(*canonical, layout->localStarts.at(0) + 18) + 1U);
        write32(overlappingLocalData, layout->localStarts.at(0) + 22,
                little32(*canonical, layout->localStarts.at(0) + 22) + 1U);
        cases.push_back({QStringLiteral("overlapping-local-data"), std::move(overlappingLocalData),
                         QStringLiteral("CRC-32")});

        QByteArray trailingBytes = *canonical;
        trailingBytes.append('\0');
        cases.push_back({QStringLiteral("trailing-byte"), std::move(trailingBytes),
                         QStringLiteral("noncanonical end record")});

        cases.push_back({QStringLiteral("noncanonical-order"),
                         reorderTwoEntryArchive(*canonical, *layout),
                         QStringLiteral("canonical bounded path order")});

        QByteArray mismatchedCentralOrder = canonical->first(layout->centralStarts.at(0));
        mismatchedCentralOrder += canonical->sliced(
            layout->centralStarts.at(1), layout->centralEnds.at(1) - layout->centralStarts.at(1));
        mismatchedCentralOrder += canonical->sliced(
            layout->centralStarts.at(0), layout->centralEnds.at(0) - layout->centralStarts.at(0));
        mismatchedCentralOrder += canonical->sliced(layout->endRecord);
        cases.push_back({QStringLiteral("mismatched-central-order"),
                         std::move(mismatchedCentralOrder),
                         QStringLiteral("noncanonical central record")});

        qsizetype index = 0;
        for (const InvalidArchiveCase& invalid : cases) {
            QVERIFY2(!invalid.bytes.isEmpty(), qPrintable(invalid.name));
            const QString path =
                directory.filePath(QStringLiteral("invalid-%1.svrule").arg(index++));
            QVERIFY2(writeFile(path, invalid.bytes), qPrintable(invalid.name));
            const auto imported = RulePackageStore::importArchive(path);
            QCOMPARE(imported.status, invalid.expectedStatus);
            QVERIFY2(imported.errorMessage.contains(invalid.expectedMessage),
                     qPrintable(invalid.name + QStringLiteral(": ") + imported.errorMessage));
        }
    }

    void rejectsUnicodeDirectoryPaths() {
        QTemporaryDir unicodeDirectory;
        QVERIFY(unicodeDirectory.isValid());
        const QString unicodeRoot = realPath(unicodeDirectory);
        QVERIFY(writePackageDirectory(unicodeRoot));
        QVERIFY(writeFile(QDir(unicodeRoot).filePath(QString::fromUtf8("src/caf\xC3\xA9.svfmt")),
                          QByteArrayLiteral("data")));
        QCOMPARE(RulePackageStore::importDirectory(unicodeRoot).status,
                 RulePackageImportStatus::InvalidInput);
    }

    void rejectsOversizedEmptyDirectoryPaths() {
        QTemporaryDir longPathDirectory;
        QVERIFY(longPathDirectory.isValid());
        const QString longPathRoot = realPath(longPathDirectory);
        QVERIFY(writePackageDirectory(longPathRoot));
        const QString longPath = QStringLiteral("docs/%1/%2/%3")
                                     .arg(QString(80, u'a'), QString(80, u'b'), QString(80, u'c'));
        QVERIFY(QDir(longPathRoot).mkpath(longPath));
        QCOMPARE(RulePackageStore::importDirectory(longPathRoot).status,
                 RulePackageImportStatus::InvalidInput);
    }

    void rejectsSymbolicLinksAndExecutableFiles() {
#if defined(Q_OS_WIN)
        QTemporaryDir linkedEntryDirectory;
        QVERIFY(linkedEntryDirectory.isValid());
        const QString linkedEntryRoot = realPath(linkedEntryDirectory);
        QVERIFY(writePackageDirectory(linkedEntryRoot));
        const QString linkPath = QDir(linkedEntryRoot).filePath(QStringLiteral("src/packet.svfmt"));
        QVERIFY(QFile::remove(linkPath));
        const QString nativeLinkPath = QDir::toNativeSeparators(linkPath);
        const QString target = QStringLiteral("..\\rule.toml");
        constexpr DWORD allowUnprivilegedCreate = 0x2U;
        if (!CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(nativeLinkPath.utf16()),
                                 reinterpret_cast<LPCWSTR>(target.utf16()),
                                 allowUnprivilegedCreate)) {
            QSKIP(qPrintable(QStringLiteral("Windows cannot create a test symbolic link: %1")
                                 .arg(GetLastError())));
        }
        const auto linkedEntry = RulePackageStore::importDirectory(linkedEntryRoot);
        QCOMPARE(linkedEntry.status, RulePackageImportStatus::InvalidInput);
        QVERIFY(linkedEntry.errorMessage.contains(QStringLiteral("reparse point")));
#else
        QTemporaryDir linkedEntryDirectory;
        QVERIFY(linkedEntryDirectory.isValid());
        const QString linkedEntryRoot = realPath(linkedEntryDirectory);
        QVERIFY(writePackageDirectory(linkedEntryRoot));
        const QByteArray linkPath =
            QFile::encodeName(QDir(linkedEntryRoot).filePath(QStringLiteral("src/packet.svfmt")));
        QVERIFY(QFile::remove(QString::fromLocal8Bit(linkPath)));
        if (::symlink("../rule.toml", linkPath.constData()) != 0) {
            QSKIP(qPrintable(QString::fromLocal8Bit(std::strerror(errno))));
        }
        const auto linkedEntry = RulePackageStore::importDirectory(linkedEntryRoot);
        QCOMPARE(linkedEntry.status, RulePackageImportStatus::InvalidInput);
        QVERIFY(linkedEntry.errorMessage.contains(QStringLiteral("symbolic link")));

        QTemporaryDir linkedRootParent;
        QVERIFY(linkedRootParent.isValid());
        const QString parentRoot = realPath(linkedRootParent);
        const QString realRoot = QDir(parentRoot).filePath(QStringLiteral("real"));
        QVERIFY(QDir().mkpath(realRoot));
        QVERIFY(writePackageDirectory(realRoot));
        const QByteArray linkedRoot =
            QFile::encodeName(QDir(parentRoot).filePath(QStringLiteral("linked")));
        if (::symlink("real", linkedRoot.constData()) != 0) {
            QSKIP(qPrintable(QString::fromLocal8Bit(std::strerror(errno))));
        }
        QCOMPARE(RulePackageStore::importDirectory(QString::fromLocal8Bit(linkedRoot)).status,
                 RulePackageImportStatus::InvalidInput);

        QTemporaryDir executableDirectory;
        QVERIFY(executableDirectory.isValid());
        const QString executableRoot = realPath(executableDirectory);
        QVERIFY(writePackageDirectory(executableRoot));
        const QByteArray executablePath =
            QFile::encodeName(QDir(executableRoot).filePath(QStringLiteral("src/packet.svfmt")));
        QVERIFY(::chmod(executablePath.constData(), 0744) == 0);
        const auto executable = RulePackageStore::importDirectory(executableRoot);
        QCOMPARE(executable.status, RulePackageImportStatus::InvalidInput);
        QVERIFY(executable.errorMessage.contains(QStringLiteral("executable")));
#endif
    }

    void installsIdempotentlyAndRequiresReadOnlyContent() {
        const auto loaded = loadPackage();
        QVERIFY(loaded.succeeded());
        QTemporaryDir store;
        QVERIFY(store.isValid());
        const QString storeRoot = realPath(store);

        const auto installed = RulePackageStore::install(*loaded.package, storeRoot);
        QVERIFY2(installed.succeeded(), qPrintable(installed.errorMessage));
        QCOMPARE(installed.status, RulePackageInstallStatus::Installed);
        const QString digest =
            QString::fromLatin1(loaded.package->identity().contentHash().toHex());
        QCOMPARE(QDir::cleanPath(installed.installedPath),
                 QDir::cleanPath(QDir(storeRoot).filePath(
                     QStringLiteral("sha256/%1/%2").arg(digest.first(2), digest))));

        constexpr QFileDevice::Permissions writePermissions =
            QFileDevice::WriteOwner | QFileDevice::WriteGroup | QFileDevice::WriteOther;
        for (const RulePackageFile& packageFile : loaded.package->files()) {
            const QFileInfo information(QDir(installed.installedPath).filePath(packageFile.path));
            QVERIFY(information.isFile());
            QCOMPARE(information.permissions() & writePermissions, QFileDevice::Permissions{});
        }

        const auto repeated = RulePackageStore::install(*loaded.package, storeRoot);
        QCOMPARE(repeated.status, RulePackageInstallStatus::AlreadyInstalled);
        QCOMPARE(repeated.installedPath, installed.installedPath);

        const QString sourcePath =
            QDir(installed.installedPath).filePath(QStringLiteral("src/packet.svfmt"));
        QVERIFY(
            QFile::setPermissions(sourcePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        const auto writable = RulePackageStore::install(*loaded.package, storeRoot);
        QCOMPARE(writable.status, RulePackageInstallStatus::CorruptExistingContent);
        QVERIFY(writable.errorMessage.contains(QStringLiteral("writable")));
    }

    void reportsCorruptExistingContentWithoutRepairingIt() {
        const auto loaded = loadPackage();
        QVERIFY(loaded.succeeded());
        QTemporaryDir store;
        QVERIFY(store.isValid());
        const QString storeRoot = realPath(store);
        const auto installed = RulePackageStore::install(*loaded.package, storeRoot);
        QVERIFY(installed.succeeded());
        const QString sourcePath =
            QDir(installed.installedPath).filePath(QStringLiteral("src/packet.svfmt"));
        QVERIFY(
            QFile::setPermissions(sourcePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        const QByteArray corruptBytes = QByteArrayLiteral("corrupt but still present\n");
        QVERIFY(writeFile(sourcePath, corruptBytes));
        QVERIFY(QFile::setPermissions(sourcePath, QFileDevice::ReadOwner | QFileDevice::ReadGroup |
                                                      QFileDevice::ReadOther));

        const auto reinstalled = RulePackageStore::install(*loaded.package, storeRoot);
        QCOMPARE(reinstalled.status, RulePackageInstallStatus::CorruptExistingContent);
        QVERIFY(reinstalled.errorMessage.contains(QStringLiteral("content identity")));
        const auto preserved = readFile(sourcePath);
        QVERIFY(preserved.has_value());
        QCOMPARE(*preserved, corruptBytes);
    }
};

QTEST_GUILESS_MAIN(RulePackageStoreTest)

#include "rule_package_store_test.moc"
