#include "session_document.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

namespace streamview::app {

namespace {

constexpr qsizetype maximumSourceTextBytes = 16 * 1024;
constexpr qsizetype maximumUserTextBytes = 64 * 1024;
constexpr qsizetype maximumAnalysisPathBytes = 16 * 1024;
constexpr qsizetype maximumBookmarks = 4096;
constexpr qsizetype maximumAnnotations = 4096;
constexpr qsizetype maximumExpandedPaths = 16384;
constexpr quint64 rawPageSizeBytes = 64U * 1024U;
constexpr qsizetype maximumJsonDepth = 256;

enum class JsonPreflightStatus {
    Valid,
    DuplicateObjectKey,
    ExcessiveDepth,
};

struct JsonContext final {
    bool object = false;
    bool expectsKey = false;
    QSet<QString> keys;
};

[[nodiscard]] SessionDocumentLoadResult fail(SessionDocumentLoadStatus status,
                                             QString message) {
    return {status, std::nullopt, std::move(message)};
}

void setError(QString* errorMessage, const QString& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

[[nodiscard]] bool validText(const QString& text, qsizetype maximumBytes,
                             bool allowEmpty = false) {
    return (allowEmpty || !text.isEmpty()) && text.toUtf8().size() <= maximumBytes;
}

[[nodiscard]] bool hasExactKeys(const QJsonObject& object,
                                std::initializer_list<QStringView> expected) {
    if (object.size() != static_cast<qsizetype>(expected.size())) {
        return false;
    }
    return std::ranges::all_of(expected, [&object](QStringView key) {
        return object.contains(key.toString());
    });
}

[[nodiscard]] JsonPreflightStatus preflightJsonObjectKeys(const QByteArray& json) {
    std::vector<JsonContext> contexts;
    for (qsizetype index = 0; index < json.size();) {
        const char character = json.at(index);
        if (character == '"') {
            const qsizetype start = index;
            ++index;
            while (index < json.size()) {
                if (json.at(index) == '\\') {
                    index += 2;
                    continue;
                }
                if (json.at(index++) == '"') {
                    break;
                }
            }
            if (!contexts.empty() && contexts.back().object && contexts.back().expectsKey) {
                QByteArray wrapped;
                wrapped.reserve((index - start) + 2);
                wrapped.append('[');
                wrapped.append(json.constData() + start, index - start);
                wrapped.append(']');
                const QString key = QJsonDocument::fromJson(wrapped).array().first().toString();
                if (contexts.back().keys.contains(key)) {
                    return JsonPreflightStatus::DuplicateObjectKey;
                }
                contexts.back().keys.insert(key);
                contexts.back().expectsKey = false;
            }
            continue;
        }
        if (character == '{' || character == '[') {
            if (contexts.size() >= maximumJsonDepth) {
                return JsonPreflightStatus::ExcessiveDepth;
            }
            contexts.push_back({character == '{', character == '{', {}});
        } else if (character == '}' || character == ']') {
            if (!contexts.empty()) {
                contexts.pop_back();
            }
        } else if (character == ',' && !contexts.empty() && contexts.back().object) {
            contexts.back().expectsKey = true;
        }
        ++index;
    }
    return JsonPreflightStatus::Valid;
}

[[nodiscard]] std::optional<quint64> parseUnsignedDecimal(const QJsonValue& value) {
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString text = value.toString();
    if (text.isEmpty() || (text.size() > 1 && text.front() == u'0')) {
        return std::nullopt;
    }
    quint64 result = 0;
    for (const QChar character : text) {
        if (character < u'0' || character > u'9') {
            return std::nullopt;
        }
        const quint64 digit = static_cast<quint64>(character.unicode() - u'0');
        if (result > (std::numeric_limits<quint64>::max() - digit) / 10U) {
            return std::nullopt;
        }
        result = (result * 10U) + digit;
    }
    return result;
}

[[nodiscard]] std::optional<qint64> parseSignedDecimal(const QJsonValue& value) {
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString text = value.toString();
    if (text.isEmpty() || text.front() == u'+' || text == QStringLiteral("-0")) {
        return std::nullopt;
    }
    qsizetype offset = text.front() == u'-' ? 1 : 0;
    if (offset == text.size() || (text.size() - offset > 1 && text.at(offset) == u'0')) {
        return std::nullopt;
    }
    quint64 magnitude = 0;
    for (qsizetype index = offset; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character < u'0' || character > u'9') {
            return std::nullopt;
        }
        const quint64 digit = static_cast<quint64>(character.unicode() - u'0');
        if (magnitude > (std::numeric_limits<quint64>::max() - digit) / 10U) {
            return std::nullopt;
        }
        magnitude = (magnitude * 10U) + digit;
    }
    if (offset == 0) {
        if (magnitude > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
            return std::nullopt;
        }
        return static_cast<qint64>(magnitude);
    }
    constexpr quint64 minimumMagnitude = quint64{1} << 63U;
    if (magnitude > minimumMagnitude) {
        return std::nullopt;
    }
    if (magnitude == minimumMagnitude) {
        return std::numeric_limits<qint64>::min();
    }
    return -static_cast<qint64>(magnitude);
}

[[nodiscard]] std::optional<QByteArray> parseSha256(const QJsonValue& value) {
    if (!value.isString()) {
        return std::nullopt;
    }
    const QByteArray text = value.toString().toLatin1();
    if (text.size() != 64 || std::ranges::any_of(text, [](char character) {
            return !((character >= '0' && character <= '9') ||
                     (character >= 'a' && character <= 'f'));
        })) {
        return std::nullopt;
    }
    return QByteArray::fromHex(text);
}

[[nodiscard]] bool sourceBitExists(quint64 sourceSizeBytes, quint64 bitOffset) {
    return bitOffset / 8U < sourceSizeBytes;
}

[[nodiscard]] bool sourceRangeExists(quint64 sourceSizeBytes, quint64 bitOffset,
                                     quint64 bitLength) {
    if (bitLength == 0 || !sourceBitExists(sourceSizeBytes, bitOffset)) {
        return false;
    }
    const quint64 remainingAfterStartByte = sourceSizeBytes - (bitOffset / 8U);
    if (remainingAfterStartByte > std::numeric_limits<quint64>::max() / 8U) {
        return true;
    }
    const quint64 availableBits = (remainingAfterStartByte * 8U) - (bitOffset % 8U);
    return bitLength <= availableBits;
}

[[nodiscard]] QString modeText(core::SourceFingerprintMode mode) {
    return mode == core::SourceFingerprintMode::FullContentSha256
               ? QStringLiteral("full-content-sha256")
               : QStringLiteral("sampled-sha256");
}

[[nodiscard]] QString rawModeText(RawDisplayMode mode) {
    switch (mode) {
    case RawDisplayMode::Hex:
        return QStringLiteral("hex");
    case RawDisplayMode::Binary:
        return QStringLiteral("binary");
    case RawDisplayMode::Combined:
        return QStringLiteral("combined");
    }
    return {};
}

[[nodiscard]] bool validateUserState(const SessionUserState& state, quint64 sourceSizeBytes,
                                     QString* errorMessage) {
    if (state.bookmarks.size() > maximumBookmarks) {
        setError(errorMessage, QStringLiteral("Session contains too many bookmarks"));
        return false;
    }
    for (const SessionBookmark& bookmark : state.bookmarks) {
        if (!validText(bookmark.label, maximumUserTextBytes) ||
            !sourceBitExists(sourceSizeBytes, bookmark.sourceBitOffset)) {
            setError(errorMessage, QStringLiteral("Session contains an invalid bookmark"));
            return false;
        }
    }
    if (state.annotations.size() > maximumAnnotations) {
        setError(errorMessage, QStringLiteral("Session contains too many annotations"));
        return false;
    }
    for (const SessionAnnotation& annotation : state.annotations) {
        if (!validText(annotation.text, maximumUserTextBytes) ||
            !sourceRangeExists(sourceSizeBytes, annotation.sourceBitOffset,
                               annotation.bitLength)) {
            setError(errorMessage, QStringLiteral("Session contains an invalid annotation"));
            return false;
        }
    }
    if (state.expandedPaths.size() > maximumExpandedPaths) {
        setError(errorMessage, QStringLiteral("Session contains too many expanded paths"));
        return false;
    }
    QSet<QString> paths;
    for (const QString& path : state.expandedPaths) {
        if (!validText(path, maximumAnalysisPathBytes) || paths.contains(path)) {
            setError(errorMessage, QStringLiteral("Session contains an invalid expanded path"));
            return false;
        }
        paths.insert(path);
    }
    const quint64 pageCount = sourceSizeBytes == 0
                                  ? 0
                                  : 1U + ((sourceSizeBytes - 1U) / rawPageSizeBytes);
    if ((pageCount == 0 && state.view.rawPageIndex != 0) ||
        (pageCount > 0 && state.view.rawPageIndex >= pageCount) ||
        rawModeText(state.view.rawDisplayMode).isEmpty()) {
        setError(errorMessage, QStringLiteral("Session contains an invalid raw view state"));
        return false;
    }
    if (state.view.selectedSourceBitOffset &&
        !sourceBitExists(sourceSizeBytes, *state.view.selectedSourceBitOffset)) {
        setError(errorMessage, QStringLiteral("Session selection is outside the source"));
        return false;
    }
    if (state.view.selectedAnalysisPath &&
        !validText(*state.view.selectedAnalysisPath, maximumAnalysisPathBytes)) {
        setError(errorMessage, QStringLiteral("Session contains an invalid selected path"));
        return false;
    }
    return true;
}

} // namespace

bool SessionDocumentLoadResult::succeeded() const noexcept {
    return status == SessionDocumentLoadStatus::Loaded && document.has_value();
}

SessionDocument::SessionDocument(QString sourcePath, QString sourceIdentity,
                                 core::SourceFingerprint sourceFingerprint,
                                 rules::RuleEntryPointIdentity ruleIdentity,
                                 SessionUserState userState)
    : sourcePath_(std::move(sourcePath)), sourceIdentity_(std::move(sourceIdentity)),
      sourceFingerprint_(std::move(sourceFingerprint)), ruleIdentity_(std::move(ruleIdentity)),
      userState_(std::move(userState)) {}

std::optional<SessionDocument>
SessionDocument::create(QString sourcePath, QString sourceIdentity,
                        core::SourceFingerprint sourceFingerprint,
                        rules::RuleEntryPointIdentity ruleIdentity,
                        SessionUserState userState, QString* errorMessage) {
    if (!validText(sourcePath, maximumSourceTextBytes) ||
        !validText(sourceIdentity, maximumSourceTextBytes)) {
        setError(errorMessage, QStringLiteral("Session source path and identity are required"));
        return std::nullopt;
    }
    if (!validateUserState(userState, sourceFingerprint.sizeBytes(), errorMessage)) {
        return std::nullopt;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return SessionDocument(std::move(sourcePath), std::move(sourceIdentity),
                           std::move(sourceFingerprint), std::move(ruleIdentity),
                           std::move(userState));
}

QByteArray SessionDocument::toJson() const {
    QJsonObject fingerprint{
        {QStringLiteral("version"), static_cast<int>(sourceFingerprint_.version())},
        {QStringLiteral("mode"), modeText(sourceFingerprint_.mode())},
        {QStringLiteral("sizeBytes"), QString::number(sourceFingerprint_.sizeBytes())},
        {QStringLiteral("sha256"), sourceFingerprint_.digestText()},
    };
    if (sourceFingerprint_.modificationTimeNanoseconds()) {
        fingerprint.insert(QStringLiteral("modificationTimeNanoseconds"),
                           QString::number(*sourceFingerprint_.modificationTimeNanoseconds()));
    }

    const auto& package = ruleIdentity_.packageIdentity();
    QJsonArray bookmarks;
    for (const SessionBookmark& bookmark : userState_.bookmarks) {
        bookmarks.append(QJsonObject{{QStringLiteral("label"), bookmark.label},
                                     {QStringLiteral("sourceBitOffset"),
                                      QString::number(bookmark.sourceBitOffset)}});
    }
    QJsonArray annotations;
    for (const SessionAnnotation& annotation : userState_.annotations) {
        annotations.append(QJsonObject{
            {QStringLiteral("text"), annotation.text},
            {QStringLiteral("sourceBitOffset"), QString::number(annotation.sourceBitOffset)},
            {QStringLiteral("bitLength"), QString::number(annotation.bitLength)},
        });
    }
    QJsonArray expandedPaths;
    for (const QString& path : userState_.expandedPaths) {
        expandedPaths.append(path);
    }
    QJsonObject view{
        {QStringLiteral("rawPageIndex"), QString::number(userState_.view.rawPageIndex)},
        {QStringLiteral("rawDisplayMode"), rawModeText(userState_.view.rawDisplayMode)},
        {QStringLiteral("selectedSourceBitOffset"),
         userState_.view.selectedSourceBitOffset
             ? QJsonValue(QString::number(*userState_.view.selectedSourceBitOffset))
             : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("selectedAnalysisPath"),
         userState_.view.selectedAnalysisPath ? QJsonValue(*userState_.view.selectedAnalysisPath)
                                              : QJsonValue(QJsonValue::Null)},
    };

    const QJsonObject root{
        {QStringLiteral("schemaVersion"), static_cast<int>(schemaVersion())},
        {QStringLiteral("source"),
         QJsonObject{{QStringLiteral("path"), sourcePath_},
                     {QStringLiteral("identity"), sourceIdentity_},
                     {QStringLiteral("fingerprint"), fingerprint}}},
        {QStringLiteral("rule"),
         QJsonObject{{QStringLiteral("packageId"), package.packageId()},
                     {QStringLiteral("packageVersion"), package.packageVersion()},
                     {QStringLiteral("contentSha256"),
                      QString::fromLatin1(package.contentHash().toHex())},
                     {QStringLiteral("entryPointId"), ruleIdentity_.entryPointId()}}},
        {QStringLiteral("bookmarks"), bookmarks},
        {QStringLiteral("annotations"), annotations},
        {QStringLiteral("expandedPaths"), expandedPaths},
        {QStringLiteral("view"), view},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool SessionDocument::save(const QString& path, QString* errorMessage) const {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorMessage, QStringLiteral("Unable to open session for atomic save: %1")
                                   .arg(file.errorString()));
        return false;
    }
    const QByteArray bytes = toJson();
    if (file.write(bytes) != bytes.size()) {
        setError(errorMessage,
                 QStringLiteral("Unable to write session: %1").arg(file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(errorMessage,
                 QStringLiteral("Unable to commit session atomically: %1").arg(file.errorString()));
        return false;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

SessionDocumentLoadResult SessionDocument::load(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(SessionDocumentLoadStatus::IoError,
                    QStringLiteral("Unable to open session: %1").arg(file.errorString()));
    }
    if (file.size() < 0 || file.size() > maximumDocumentBytes()) {
        return fail(SessionDocumentLoadStatus::TooLarge,
                    QStringLiteral("Session exceeds the 1 MiB limit"));
    }
    const QByteArray bytes = file.read(maximumDocumentBytes() + 1);
    if (file.error() != QFileDevice::NoError) {
        return fail(SessionDocumentLoadStatus::IoError,
                    QStringLiteral("Unable to read session: %1").arg(file.errorString()));
    }
    return parse(bytes);
}

SessionDocumentLoadResult SessionDocument::parse(const QByteArray& json) {
    if (json.size() > maximumDocumentBytes()) {
        return fail(SessionDocumentLoadStatus::TooLarge,
                    QStringLiteral("Session exceeds the 1 MiB limit"));
    }
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        return fail(SessionDocumentLoadStatus::InvalidJson,
                    QStringLiteral("Session is not valid JSON: %1").arg(parseError.errorString()));
    }
    const JsonPreflightStatus preflight = preflightJsonObjectKeys(json);
    if (preflight == JsonPreflightStatus::DuplicateObjectKey) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session JSON contains a duplicate object key"));
    }
    if (preflight == JsonPreflightStatus::ExcessiveDepth) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session JSON exceeds the nesting-depth limit"));
    }
    const QJsonObject root = parsed.object();
    if (!hasExactKeys(root, {u"schemaVersion", u"source", u"rule", u"bookmarks",
                             u"annotations", u"expandedPaths", u"view"})) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session root fields do not match schema version 1"));
    }
    const QJsonValue schema = root.value(QStringLiteral("schemaVersion"));
    if (!schema.isDouble() || schema.toDouble() != schemaVersion()) {
        return fail(SessionDocumentLoadStatus::UnsupportedVersion,
                    QStringLiteral("Unsupported session schema version"));
    }
    if (!root.value(QStringLiteral("source")).isObject() ||
        !root.value(QStringLiteral("rule")).isObject() ||
        !root.value(QStringLiteral("bookmarks")).isArray() ||
        !root.value(QStringLiteral("annotations")).isArray() ||
        !root.value(QStringLiteral("expandedPaths")).isArray() ||
        !root.value(QStringLiteral("view")).isObject()) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session fields have invalid JSON types"));
    }

    const QJsonObject source = root.value(QStringLiteral("source")).toObject();
    if (!hasExactKeys(source, {u"path", u"identity", u"fingerprint"}) ||
        !source.value(QStringLiteral("path")).isString() ||
        !source.value(QStringLiteral("identity")).isString() ||
        !source.value(QStringLiteral("fingerprint")).isObject()) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session source fields are invalid"));
    }
    const QString sourcePath = source.value(QStringLiteral("path")).toString();
    const QString sourceIdentity = source.value(QStringLiteral("identity")).toString();
    if (!validText(sourcePath, maximumSourceTextBytes) ||
        !validText(sourceIdentity, maximumSourceTextBytes)) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session source path and identity are invalid"));
    }

    const QJsonObject fingerprint = source.value(QStringLiteral("fingerprint")).toObject();
    const bool hasMtime = fingerprint.contains(QStringLiteral("modificationTimeNanoseconds"));
    if ((!hasMtime && !hasExactKeys(fingerprint, {u"version", u"mode", u"sizeBytes",
                                                  u"sha256"})) ||
        (hasMtime && !hasExactKeys(fingerprint, {u"version", u"mode", u"sizeBytes", u"sha256",
                                                u"modificationTimeNanoseconds"}))) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session source fingerprint fields are invalid"));
    }
    const QJsonValue fingerprintVersion = fingerprint.value(QStringLiteral("version"));
    if (!fingerprintVersion.isDouble() ||
        fingerprintVersion.toDouble() != core::SourceFingerprint::algorithmVersion()) {
        return fail(SessionDocumentLoadStatus::UnsupportedVersion,
                    QStringLiteral("Unsupported source fingerprint version"));
    }
    const QString fingerprintMode = fingerprint.value(QStringLiteral("mode")).toString();
    const auto sizeBytes = parseUnsignedDecimal(fingerprint.value(QStringLiteral("sizeBytes")));
    const auto digest = parseSha256(fingerprint.value(QStringLiteral("sha256")));
    std::optional<qint64> modificationTime;
    if (hasMtime) {
        modificationTime = parseSignedDecimal(
            fingerprint.value(QStringLiteral("modificationTimeNanoseconds")));
    }
    core::SourceFingerprintMode mode{};
    if (fingerprintMode == QStringLiteral("full-content-sha256")) {
        mode = core::SourceFingerprintMode::FullContentSha256;
    } else if (fingerprintMode == QStringLiteral("sampled-sha256")) {
        mode = core::SourceFingerprintMode::SampledSha256;
    } else {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session source fingerprint mode is invalid"));
    }
    QString valueError;
    auto sourceFingerprint = sizeBytes && digest && (!hasMtime || modificationTime)
                                 ? core::SourceFingerprint::create(
                                       core::SourceFingerprint::algorithmVersion(), mode,
                                       *sizeBytes, modificationTime, *digest, &valueError)
                                 : std::nullopt;
    if (!sourceFingerprint) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    valueError.isEmpty()
                        ? QStringLiteral("Session source fingerprint value is invalid")
                        : std::move(valueError));
    }

    const QJsonObject rule = root.value(QStringLiteral("rule")).toObject();
    if (!hasExactKeys(rule, {u"packageId", u"packageVersion", u"contentSha256",
                             u"entryPointId"}) ||
        !rule.value(QStringLiteral("packageId")).isString() ||
        !rule.value(QStringLiteral("packageVersion")).isString() ||
        !rule.value(QStringLiteral("entryPointId")).isString()) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session rule identity fields are invalid"));
    }
    const auto contentHash = parseSha256(rule.value(QStringLiteral("contentSha256")));
    auto packageIdentity = contentHash
                               ? rules::RulePackageIdentity::create(
                                     rule.value(QStringLiteral("packageId")).toString(),
                                     rule.value(QStringLiteral("packageVersion")).toString(),
                                     *contentHash, &valueError)
                               : std::nullopt;
    auto ruleIdentity = packageIdentity
                            ? rules::RuleEntryPointIdentity::create(
                                  std::move(*packageIdentity),
                                  rule.value(QStringLiteral("entryPointId")).toString(),
                                  &valueError)
                            : std::nullopt;
    if (!ruleIdentity) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    valueError.isEmpty() ? QStringLiteral("Session rule identity is invalid")
                                         : std::move(valueError));
    }

    SessionUserState userState;
    const QJsonArray bookmarks = root.value(QStringLiteral("bookmarks")).toArray();
    if (bookmarks.size() > maximumBookmarks) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session contains too many bookmarks"));
    }
    for (const QJsonValue& item : bookmarks) {
        if (!item.isObject()) {
            return fail(SessionDocumentLoadStatus::InvalidSchema,
                        QStringLiteral("Session bookmark is not an object"));
        }
        const QJsonObject bookmark = item.toObject();
        const auto offset = parseUnsignedDecimal(bookmark.value(QStringLiteral("sourceBitOffset")));
        if (!hasExactKeys(bookmark, {u"label", u"sourceBitOffset"}) ||
            !bookmark.value(QStringLiteral("label")).isString() || !offset) {
            return fail(SessionDocumentLoadStatus::InvalidSchema,
                        QStringLiteral("Session bookmark fields are invalid"));
        }
        userState.bookmarks.push_back(
            {bookmark.value(QStringLiteral("label")).toString(), *offset});
    }

    const QJsonArray annotations = root.value(QStringLiteral("annotations")).toArray();
    if (annotations.size() > maximumAnnotations) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session contains too many annotations"));
    }
    for (const QJsonValue& item : annotations) {
        if (!item.isObject()) {
            return fail(SessionDocumentLoadStatus::InvalidSchema,
                        QStringLiteral("Session annotation is not an object"));
        }
        const QJsonObject annotation = item.toObject();
        const auto offset = parseUnsignedDecimal(annotation.value(QStringLiteral("sourceBitOffset")));
        const auto length = parseUnsignedDecimal(annotation.value(QStringLiteral("bitLength")));
        if (!hasExactKeys(annotation, {u"text", u"sourceBitOffset", u"bitLength"}) ||
            !annotation.value(QStringLiteral("text")).isString() || !offset || !length) {
            return fail(SessionDocumentLoadStatus::InvalidSchema,
                        QStringLiteral("Session annotation fields are invalid"));
        }
        userState.annotations.push_back(
            {annotation.value(QStringLiteral("text")).toString(), *offset, *length});
    }

    const QJsonArray expandedPaths = root.value(QStringLiteral("expandedPaths")).toArray();
    if (expandedPaths.size() > maximumExpandedPaths) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session contains too many expanded paths"));
    }
    for (const QJsonValue& item : expandedPaths) {
        if (!item.isString()) {
            return fail(SessionDocumentLoadStatus::InvalidSchema,
                        QStringLiteral("Session expanded path is not a string"));
        }
        userState.expandedPaths.append(item.toString());
    }

    const QJsonObject view = root.value(QStringLiteral("view")).toObject();
    if (!hasExactKeys(view, {u"rawPageIndex", u"rawDisplayMode",
                             u"selectedSourceBitOffset", u"selectedAnalysisPath"})) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session view fields are invalid"));
    }
    const auto pageIndex = parseUnsignedDecimal(view.value(QStringLiteral("rawPageIndex")));
    const QString rawMode = view.value(QStringLiteral("rawDisplayMode")).toString();
    if (!pageIndex || !view.value(QStringLiteral("rawDisplayMode")).isString()) {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session raw view fields are invalid"));
    }
    userState.view.rawPageIndex = *pageIndex;
    if (rawMode == QStringLiteral("hex")) {
        userState.view.rawDisplayMode = RawDisplayMode::Hex;
    } else if (rawMode == QStringLiteral("binary")) {
        userState.view.rawDisplayMode = RawDisplayMode::Binary;
    } else if (rawMode == QStringLiteral("combined")) {
        userState.view.rawDisplayMode = RawDisplayMode::Combined;
    } else {
        return fail(SessionDocumentLoadStatus::InvalidSchema,
                    QStringLiteral("Session raw display mode is invalid"));
    }
    const QJsonValue selectedBit = view.value(QStringLiteral("selectedSourceBitOffset"));
    if (!selectedBit.isNull()) {
        userState.view.selectedSourceBitOffset = parseUnsignedDecimal(selectedBit);
        if (!userState.view.selectedSourceBitOffset) {
            return fail(SessionDocumentLoadStatus::InvalidSchema,
                        QStringLiteral("Session source selection is invalid"));
        }
    }
    const QJsonValue selectedPath = view.value(QStringLiteral("selectedAnalysisPath"));
    if (!selectedPath.isNull()) {
        if (!selectedPath.isString()) {
            return fail(SessionDocumentLoadStatus::InvalidSchema,
                        QStringLiteral("Session selected analysis path is invalid"));
        }
        userState.view.selectedAnalysisPath = selectedPath.toString();
    }

    auto document = SessionDocument::create(
        sourcePath, sourceIdentity, std::move(*sourceFingerprint), std::move(*ruleIdentity),
        std::move(userState), &valueError);
    if (!document) {
        return fail(SessionDocumentLoadStatus::InvalidSchema, std::move(valueError));
    }
    return {SessionDocumentLoadStatus::Loaded, std::move(document), {}};
}

} // namespace streamview::app
