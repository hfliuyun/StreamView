#include <streamview/rules/rule_package_store.h>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace streamview::rules {

namespace {

constexpr qsizetype maximumFileCount = 1'024;
constexpr qsizetype maximumPathComponents = 16;
constexpr qsizetype maximumComponentBytes = 80;
constexpr qsizetype maximumPathBytes = 240;
constexpr qsizetype maximumFileBytes = 8 * 1024 * 1024;
constexpr qsizetype maximumManifestBytes = 64 * 1024;
constexpr quint64 maximumTotalBytes = 64U * 1024U * 1024U;

struct ScanState final {
    std::vector<RulePackageFile> files;
    QSet<QString> foldedPaths;
    quint64 totalBytes = 0;
    RulePackageImportStatus failureStatus = RulePackageImportStatus::InvalidInput;
    QString errorMessage;
};

[[nodiscard]] bool fail(ScanState& state, RulePackageImportStatus status, const QString& message) {
    state.failureStatus = status;
    state.errorMessage = message;
    return false;
}

[[nodiscard]] bool isPortableComponent(QStringView component) {
    if (component.isEmpty() || component == u"." || component == u".." ||
        component.toString().toLatin1().size() > maximumComponentBytes ||
        component.endsWith(u'.')) {
        return false;
    }
    return std::all_of(component.begin(), component.end(), [](QChar character) {
        return (character >= u'a' && character <= u'z') ||
               (character >= u'A' && character <= u'Z') ||
               (character >= u'0' && character <= u'9') || character == u'.' || character == u'_' ||
               character == u'-';
    });
}

[[nodiscard]] bool registerLogicalPath(ScanState& state, const QString& logicalPath, bool directory,
                                       qsizetype depth) {
    if (depth > maximumPathComponents || logicalPath.toLatin1().size() > maximumPathBytes ||
        !isPortableComponent(logicalPath.sliced(logicalPath.lastIndexOf(u'/') + 1))) {
        return fail(
            state, RulePackageImportStatus::InvalidInput,
            QStringLiteral("Directory contains a noncanonical package path: %1").arg(logicalPath));
    }
    if (!logicalPath.contains(u'/')) {
        if ((directory && logicalPath != QStringLiteral("src") &&
             logicalPath != QStringLiteral("docs") && logicalPath != QStringLiteral("tests")) ||
            (!directory && logicalPath != QStringLiteral("rule.toml"))) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory contains an invalid root entry: %1").arg(logicalPath));
        }
    }
    const QString folded = logicalPath.toLower();
    if (state.foldedPaths.contains(folded)) {
        return fail(state, RulePackageImportStatus::InvalidInput,
                    QStringLiteral("Directory paths collide after ASCII case folding: %1")
                        .arg(logicalPath));
    }
    state.foldedPaths.insert(folded);
    return true;
}

[[nodiscard]] bool reserveFile(ScanState& state, const QString& logicalPath, quint64 fileSize) {
    if (state.files.size() >= static_cast<std::size_t>(maximumFileCount)) {
        return fail(state, RulePackageImportStatus::InvalidInput,
                    QStringLiteral("Directory contains more than 1024 package files"));
    }
    const quint64 fileLimit = logicalPath == QStringLiteral("rule.toml")
                                  ? static_cast<quint64>(maximumManifestBytes)
                                  : static_cast<quint64>(maximumFileBytes);
    if (fileSize > fileLimit || fileSize > maximumTotalBytes - state.totalBytes) {
        return fail(
            state, RulePackageImportStatus::InvalidInput,
            QStringLiteral("Directory package file exceeds a size limit: %1").arg(logicalPath));
    }
    state.totalBytes += fileSize;
    return true;
}

#if defined(Q_OS_WIN)

class UniqueHandle final {
  public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

[[nodiscard]] QString windowsError(DWORD error) {
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

[[nodiscard]] UniqueHandle openWindowsPath(const QString& path) {
    const QString nativePath = QDir::toNativeSeparators(path);
    return UniqueHandle(CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr));
}

struct WindowsRoot final {
    QString path;
    std::vector<UniqueHandle> anchors;

    [[nodiscard]] HANDLE handle() const noexcept { return anchors.back().get(); }
};

[[nodiscard]] std::optional<WindowsRoot> openWindowsDirectoryRoot(const QString& rootPath,
                                                                  ScanState& state) {
    const QString absoluteRoot =
        QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(rootPath).absoluteFilePath()));
    QString currentPath;
    QStringList components;
    if (absoluteRoot.size() >= 3 && absoluteRoot.at(1) == u':' && absoluteRoot.at(2) == u'/') {
        currentPath = absoluteRoot.first(3);
        components = absoluteRoot.sliced(3).split(u'/', Qt::SkipEmptyParts);
    } else if (absoluteRoot.startsWith(QStringLiteral("//"))) {
        QStringList uncParts = absoluteRoot.sliced(2).split(u'/', Qt::SkipEmptyParts);
        if (uncParts.size() < 2) {
            static_cast<void>(fail(state, RulePackageImportStatus::InvalidInput,
                                   QStringLiteral("Rule package directory path is invalid")));
            return std::nullopt;
        }
        currentPath = QStringLiteral("//%1/%2/").arg(uncParts.at(0), uncParts.at(1));
        uncParts.removeFirst();
        uncParts.removeFirst();
        components = std::move(uncParts);
    } else {
        static_cast<void>(fail(state, RulePackageImportStatus::InvalidInput,
                               QStringLiteral("Rule package directory path is not absolute")));
        return std::nullopt;
    }

    WindowsRoot root;
    root.path = absoluteRoot;
    const auto openRealDirectory = [&root, &state](const QString& path) {
        UniqueHandle handle = openWindowsPath(path);
        if (!handle.valid()) {
            return fail(state, RulePackageImportStatus::IoError, windowsError(GetLastError()));
        }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        FILE_STANDARD_INFO standard{};
        if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard,
                                          sizeof(standard))) {
            return fail(state, RulePackageImportStatus::IoError, windowsError(GetLastError()));
        }
        if (!standard.Directory || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return fail(state, RulePackageImportStatus::InvalidInput,
                        QStringLiteral("Rule package root path traverses a reparse point"));
        }
        root.anchors.push_back(std::move(handle));
        return true;
    };

    if (!openRealDirectory(currentPath)) {
        return std::nullopt;
    }
    for (const QString& component : components) {
        if (!currentPath.endsWith(u'/')) {
            currentPath += u'/';
        }
        currentPath += component;
        if (!openRealDirectory(currentPath)) {
            return std::nullopt;
        }
    }
    return root;
}

[[nodiscard]] std::optional<QString> finalWindowsPath(HANDLE handle) {
    const DWORD required =
        GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0U) {
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
    const DWORD length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0U || length >= buffer.size()) {
        return std::nullopt;
    }
    return QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(length));
}

[[nodiscard]] quint64 windowsFileId(const BY_HANDLE_FILE_INFORMATION& information) {
    return (static_cast<quint64>(information.nFileIndexHigh) << 32U) | information.nFileIndexLow;
}

struct WindowsEntry final {
    QString name;
    DWORD attributes = 0;
    quint64 fileId = 0;
};

[[nodiscard]] bool listWindowsDirectory(HANDLE directory, std::vector<WindowsEntry>* entries,
                                        ScanState& state) {
    alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64 * 1024> buffer{};
    constexpr std::size_t headerBytes = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
    bool restart = true;
    while (true) {
        const FILE_INFO_BY_HANDLE_CLASS infoClass =
            restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
        if (!GetFileInformationByHandleEx(directory, infoClass, buffer.data(),
                                          static_cast<DWORD>(buffer.size()))) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) {
                break;
            }
            return fail(state, RulePackageImportStatus::IoError, windowsError(error));
        }
        restart = false;
        std::size_t offset = 0;
        while (true) {
            if (offset > buffer.size() - headerBytes) {
                return fail(state, RulePackageImportStatus::IoError,
                            QStringLiteral("Windows directory enumeration returned invalid data"));
            }
            const auto* information =
                reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
            if ((information->FileNameLength % sizeof(wchar_t)) != 0U ||
                information->FileNameLength > buffer.size() - offset - headerBytes) {
                return fail(state, RulePackageImportStatus::IoError,
                            QStringLiteral("Windows directory enumeration returned invalid data"));
            }
            const qsizetype nameLength =
                static_cast<qsizetype>(information->FileNameLength / sizeof(wchar_t));
            const QString name = QString::fromWCharArray(information->FileName, nameLength);
            if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
                entries->push_back(WindowsEntry{
                    name,
                    information->FileAttributes,
                    static_cast<quint64>(information->FileId.QuadPart),
                });
            }
            if (information->NextEntryOffset == 0U) {
                break;
            }
            if (information->NextEntryOffset >= buffer.size() - offset) {
                return fail(state, RulePackageImportStatus::IoError,
                            QStringLiteral("Windows directory enumeration returned invalid data"));
            }
            offset += information->NextEntryOffset;
        }
    }
    std::sort(entries->begin(), entries->end(),
              [](const WindowsEntry& left, const WindowsEntry& right) {
                  return left.name.toLatin1() < right.name.toLatin1();
              });
    return true;
}

[[nodiscard]] bool readWindowsFile(HANDLE handle, quint64 size, QByteArray* contents,
                                   ScanState& state) {
    contents->resize(static_cast<qsizetype>(size));
    quint64 offset = 0;
    while (offset < size) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<quint64>(size - offset, std::numeric_limits<DWORD>::max()));
        DWORD bytesRead = 0;
        if (!ReadFile(handle, contents->data() + static_cast<qsizetype>(offset), chunk, &bytesRead,
                      nullptr) ||
            bytesRead == 0U) {
            return fail(state, RulePackageImportStatus::IoError, windowsError(GetLastError()));
        }
        offset += bytesRead;
    }
    return true;
}

[[nodiscard]] bool scanWindowsDirectory(const QString& filesystemPath, HANDLE directory,
                                        const QString& rootFinalPath, const QString& logicalPrefix,
                                        qsizetype depth, ScanState& state) {
    std::vector<WindowsEntry> entries;
    if (!listWindowsDirectory(directory, &entries, state)) {
        return false;
    }
    for (const WindowsEntry& entry : entries) {
        if (!isPortableComponent(entry.name)) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory contains a noncanonical name: %1").arg(entry.name));
        }
        const QString logicalPath =
            logicalPrefix.isEmpty() ? entry.name : logicalPrefix + u'/' + entry.name;
        const QString childPath = filesystemPath + u'/' + entry.name;
        UniqueHandle child = openWindowsPath(childPath);
        if (!child.valid()) {
            return fail(state, RulePackageImportStatus::IoError,
                        QStringLiteral("Unable to open %1: %2")
                            .arg(logicalPath, windowsError(GetLastError())));
        }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        FILE_STANDARD_INFO standard{};
        FILE_BASIC_INFO basicBefore{};
        BY_HANDLE_FILE_INFORMATION identity{};
        if (!GetFileInformationByHandleEx(child.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !GetFileInformationByHandleEx(child.get(), FileStandardInfo, &standard,
                                          sizeof(standard)) ||
            !GetFileInformationByHandleEx(child.get(), FileBasicInfo, &basicBefore,
                                          sizeof(basicBefore)) ||
            !GetFileInformationByHandle(child.get(), &identity)) {
            return fail(state, RulePackageImportStatus::IoError, windowsError(GetLastError()));
        }
        if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
            (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory package contains a reparse point: %1").arg(logicalPath));
        }
        const quint64 openedFileId = windowsFileId(identity);
        if (entry.fileId != 0U && openedFileId != entry.fileId) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory entry changed while importing: %1").arg(logicalPath));
        }
        const auto finalPath = finalWindowsPath(child.get());
        QString rootPrefix = rootFinalPath;
        if (!rootPrefix.endsWith(u'\\')) {
            rootPrefix += u'\\';
        }
        if (!finalPath || !finalPath->startsWith(rootPrefix, Qt::CaseInsensitive)) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory entry escapes the package root: %1").arg(logicalPath));
        }
        if (!registerLogicalPath(state, logicalPath, standard.Directory, depth + 1)) {
            return false;
        }
        if (standard.Directory) {
            if (!scanWindowsDirectory(childPath, child.get(), rootFinalPath, logicalPath, depth + 1,
                                      state)) {
                return false;
            }
            continue;
        }
        if (standard.EndOfFile.QuadPart < 0) {
            return fail(state, RulePackageImportStatus::InvalidInput,
                        QStringLiteral("Directory file has an invalid size: %1").arg(logicalPath));
        }
        const quint64 size = static_cast<quint64>(standard.EndOfFile.QuadPart);
        if (!reserveFile(state, logicalPath, size)) {
            return false;
        }
        QByteArray contents;
        if (!readWindowsFile(child.get(), size, &contents, state)) {
            return false;
        }
        FILE_STANDARD_INFO standardAfter{};
        FILE_BASIC_INFO basicAfter{};
        if (!GetFileInformationByHandleEx(child.get(), FileStandardInfo, &standardAfter,
                                          sizeof(standardAfter)) ||
            !GetFileInformationByHandleEx(child.get(), FileBasicInfo, &basicAfter,
                                          sizeof(basicAfter)) ||
            standardAfter.EndOfFile.QuadPart != standard.EndOfFile.QuadPart ||
            basicAfter.LastWriteTime.QuadPart != basicBefore.LastWriteTime.QuadPart ||
            basicAfter.ChangeTime.QuadPart != basicBefore.ChangeTime.QuadPart) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory file changed while importing: %1").arg(logicalPath));
        }
        state.files.push_back(RulePackageFile{logicalPath, std::move(contents)});
    }
    return true;
}

#else

class UniqueFd final {
  public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  private:
    int fd_ = -1;
};

[[nodiscard]] QString posixError(int error) { return QString::fromLocal8Bit(std::strerror(error)); }

[[nodiscard]] UniqueFd openPosixDirectoryRoot(const QString& rootPath, ScanState& state) {
    constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    UniqueFd current(::open("/", directoryFlags));
    if (!current.valid()) {
        static_cast<void>(fail(state, RulePackageImportStatus::IoError, posixError(errno)));
        return UniqueFd();
    }

    const QString absoluteRoot = QDir::cleanPath(QFileInfo(rootPath).absoluteFilePath());
    const QStringList components = absoluteRoot.split(u'/', Qt::SkipEmptyParts);
    for (const QString& component : components) {
        const QByteArray nativeComponent = component.toLocal8Bit();
        if (nativeComponent.isEmpty() || nativeComponent.contains('\0')) {
            static_cast<void>(fail(state, RulePackageImportStatus::InvalidInput,
                                   QStringLiteral("Rule package directory path is invalid")));
            return UniqueFd();
        }
        UniqueFd next(::openat(current.get(), nativeComponent.constData(), directoryFlags));
        if (!next.valid()) {
            const int error = errno;
            const RulePackageImportStatus status = error == ELOOP || error == ENOTDIR
                                                       ? RulePackageImportStatus::InvalidInput
                                                       : RulePackageImportStatus::IoError;
            static_cast<void>(fail(state, status, posixError(error)));
            return UniqueFd();
        }
        current = std::move(next);
    }
    return current;
}

[[nodiscard]] bool sameSnapshot(const struct stat& before, const struct stat& after) {
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtime != after.st_mtime ||
        before.st_ctime != after.st_ctime) {
        return false;
    }
#if defined(Q_OS_MACOS)
    return before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    return before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
}

[[nodiscard]] bool readPosixFile(int fd, quint64 size, QByteArray* contents, ScanState& state) {
    contents->resize(static_cast<qsizetype>(size));
    quint64 offset = 0;
    while (offset < size) {
        const std::size_t chunk = static_cast<std::size_t>(std::min<quint64>(
            size - offset, static_cast<quint64>(std::numeric_limits<ssize_t>::max())));
        const ssize_t bytesRead =
            ::read(fd, contents->data() + static_cast<qsizetype>(offset), chunk);
        if (bytesRead < 0 && errno == EINTR) {
            continue;
        }
        if (bytesRead <= 0) {
            return fail(state, RulePackageImportStatus::IoError, posixError(errno));
        }
        offset += static_cast<quint64>(bytesRead);
    }
    return true;
}

[[nodiscard]] bool scanPosixDirectory(int directoryFd, const QString& logicalPrefix,
                                      qsizetype depth, ScanState& state) {
    const int duplicateFd = ::dup(directoryFd);
    if (duplicateFd < 0) {
        return fail(state, RulePackageImportStatus::IoError, posixError(errno));
    }
    DIR* directory = ::fdopendir(duplicateFd);
    if (directory == nullptr) {
        const int error = errno;
        ::close(duplicateFd);
        return fail(state, RulePackageImportStatus::IoError, posixError(error));
    }
    std::vector<QByteArray> names;
    errno = 0;
    while (const dirent* entry = ::readdir(directory)) {
        const QByteArray name(entry->d_name);
        if (name != QByteArrayLiteral(".") && name != QByteArrayLiteral("..")) {
            names.push_back(name);
        }
        errno = 0;
    }
    const int readError = errno;
    ::closedir(directory);
    if (readError != 0) {
        return fail(state, RulePackageImportStatus::IoError, posixError(readError));
    }
    std::sort(names.begin(), names.end());

    for (const QByteArray& nativeName : names) {
        const QString name = QString::fromLatin1(nativeName);
        if (!isPortableComponent(name)) {
            return fail(state, RulePackageImportStatus::InvalidInput,
                        QStringLiteral("Directory contains a noncanonical name"));
        }
        const QString logicalPath = logicalPrefix.isEmpty() ? name : logicalPrefix + u'/' + name;
        struct stat entryMetadata{};
        if (::fstatat(directoryFd, nativeName.constData(), &entryMetadata, AT_SYMLINK_NOFOLLOW) !=
            0) {
            return fail(state, RulePackageImportStatus::IoError, posixError(errno));
        }
        if (S_ISLNK(entryMetadata.st_mode)) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory package contains a symbolic link: %1").arg(logicalPath));
        }
        const bool isDirectory = S_ISDIR(entryMetadata.st_mode);
        const bool isFile = S_ISREG(entryMetadata.st_mode);
        if (!isDirectory && !isFile) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory package contains a special file: %1").arg(logicalPath));
        }
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (isDirectory ? O_DIRECTORY : 0);
        UniqueFd opened(::openat(directoryFd, nativeName.constData(), flags));
        if (!opened.valid()) {
            return fail(state, RulePackageImportStatus::IoError, posixError(errno));
        }
        struct stat openedMetadata{};
        if (::fstat(opened.get(), &openedMetadata) != 0) {
            return fail(state, RulePackageImportStatus::IoError, posixError(errno));
        }
        if (openedMetadata.st_dev != entryMetadata.st_dev ||
            openedMetadata.st_ino != entryMetadata.st_ino ||
            S_ISDIR(openedMetadata.st_mode) != isDirectory ||
            S_ISREG(openedMetadata.st_mode) != isFile) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory entry changed while importing: %1").arg(logicalPath));
        }
        if (!registerLogicalPath(state, logicalPath, isDirectory, depth + 1)) {
            return false;
        }
        if (isDirectory) {
            if (!scanPosixDirectory(opened.get(), logicalPath, depth + 1, state)) {
                return false;
            }
            continue;
        }
        if ((openedMetadata.st_mode & 0111) != 0 || openedMetadata.st_size < 0) {
            return fail(state, RulePackageImportStatus::InvalidInput,
                        QStringLiteral("Directory file is executable or has invalid size: %1")
                            .arg(logicalPath));
        }
        const quint64 size = static_cast<quint64>(openedMetadata.st_size);
        if (!reserveFile(state, logicalPath, size)) {
            return false;
        }
        QByteArray contents;
        if (!readPosixFile(opened.get(), size, &contents, state)) {
            return false;
        }
        struct stat after{};
        if (::fstat(opened.get(), &after) != 0) {
            return fail(state, RulePackageImportStatus::IoError, posixError(errno));
        }
        if (!sameSnapshot(openedMetadata, after)) {
            return fail(
                state, RulePackageImportStatus::InvalidInput,
                QStringLiteral("Directory file changed while importing: %1").arg(logicalPath));
        }
        state.files.push_back(RulePackageFile{logicalPath, std::move(contents)});
    }
    return true;
}

#endif

} // namespace

RulePackageImportResult RulePackageStore::importDirectory(const QString& rootPath) {
    if (rootPath.isEmpty()) {
        return {RulePackageImportStatus::InvalidInput, std::nullopt,
                QStringLiteral("Rule package directory path is empty")};
    }
    ScanState state;
    state.files.reserve(16);

#if defined(Q_OS_WIN)
    auto root = openWindowsDirectoryRoot(rootPath, state);
    if (!root) {
        return {state.failureStatus, std::nullopt, std::move(state.errorMessage)};
    }
    const auto rootFinalPath = finalWindowsPath(root->handle());
    if (!rootFinalPath) {
        return {RulePackageImportStatus::IoError, std::nullopt, windowsError(GetLastError())};
    }
    if (!scanWindowsDirectory(root->path, root->handle(), *rootFinalPath, {}, 0, state)) {
        return {state.failureStatus, std::nullopt, std::move(state.errorMessage)};
    }
#else
    UniqueFd root = openPosixDirectoryRoot(rootPath, state);
    if (!root.valid()) {
        return {state.failureStatus, std::nullopt, std::move(state.errorMessage)};
    }
    if (!scanPosixDirectory(root.get(), {}, 0, state)) {
        return {state.failureStatus, std::nullopt, std::move(state.errorMessage)};
    }
#endif

    RulePackageLoadResult loaded = RulePackage::fromFiles(std::move(state.files));
    if (!loaded.succeeded()) {
        return {RulePackageImportStatus::InvalidPackage, std::nullopt,
                QStringLiteral("Directory contains an invalid rule package: %1")
                    .arg(loaded.errorMessage)};
    }
    return {RulePackageImportStatus::Imported, std::move(loaded.package), {}};
}

} // namespace streamview::rules
