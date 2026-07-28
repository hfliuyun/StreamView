#include <streamview/rules/rule_package.h>

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace streamview::rules {

namespace {

constexpr qsizetype maximumFileCount = 1'024;
constexpr qsizetype maximumPathComponents = 16;
constexpr qsizetype maximumPathBytes = 240;
constexpr qsizetype maximumComponentBytes = 80;
constexpr qsizetype maximumFileBytes = 8 * 1024 * 1024;
constexpr qsizetype maximumManifestBytes = 64 * 1024;
constexpr quint64 maximumTotalBytes = 64U * 1024U * 1024U;

void setError(QString* errorMessage, const QString& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

[[nodiscard]] bool isCanonicalNumber(QStringView text) {
    if (text.isEmpty() || (text.size() > 1 && text.front() == u'0')) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](QChar character) {
        return character >= u'0' && character <= u'9';
    });
}

[[nodiscard]] int compareCanonicalNumbers(QStringView left, QStringView right) noexcept {
    if (left.size() != right.size()) {
        return left.size() < right.size() ? -1 : 1;
    }
    const int compared = left.compare(right, Qt::CaseSensitive);
    return compared < 0 ? -1 : (compared > 0 ? 1 : 0);
}

[[nodiscard]] bool isPrereleaseIdentifier(QStringView text) {
    if (text.isEmpty()) {
        return false;
    }
    const bool valid = std::all_of(text.begin(), text.end(), [](QChar character) {
        return (character >= u'a' && character <= u'z') ||
               (character >= u'A' && character <= u'Z') ||
               (character >= u'0' && character <= u'9') || character == u'-';
    });
    if (!valid) {
        return false;
    }
    const bool numeric = std::all_of(text.begin(), text.end(), [](QChar character) {
        return character >= u'0' && character <= u'9';
    });
    return !numeric || isCanonicalNumber(text);
}

[[nodiscard]] bool isEntryToken(QStringView text) {
    if (text.isEmpty() || text.size() > 64 || text.front() < u'a' || text.front() > u'z') {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](QChar character) {
        return (character >= u'a' && character <= u'z') ||
               (character >= u'0' && character <= u'9') || character == u'-';
    });
}

[[nodiscard]] bool isDottedIdentifier(QStringView text) {
    if (text.isEmpty() || text.size() > 128) {
        return false;
    }
    const QStringList segments = text.toString().split(u'.', Qt::KeepEmptyParts);
    if (segments.size() < 2 || segments.size() > 8) {
        return false;
    }
    return std::all_of(segments.begin(), segments.end(), [](const QString& segment) {
        if (segment.isEmpty() || segment.size() > 32 || segment.front() < u'a' ||
            segment.front() > u'z') {
            return false;
        }
        return std::all_of(segment.begin(), segment.end(), [](QChar character) {
            return (character >= u'a' && character <= u'z') ||
                   (character >= u'0' && character <= u'9') || character == u'-';
        });
    });
}

[[nodiscard]] bool isPrintableAscii(QStringView text, qsizetype maximumBytes) {
    if (text.isEmpty() || text.size() > maximumBytes) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](QChar character) {
        return character.unicode() >= 0x20U && character.unicode() <= 0x7EU;
    });
}

[[nodiscard]] bool hasForbiddenNativeSuffix(QStringView path) {
    const QString fileName = path.mid(path.lastIndexOf(u'/') + 1).toString().toLower();
    static const std::array<QStringView, 9> suffixes{
        u".exe", u".com", u".dll", u".dylib", u".bundle",
        u".app", u".msi", u".sys", u".drv",
    };
    if (std::any_of(suffixes.begin(), suffixes.end(), [&fileName](QStringView suffix) {
            return fileName.endsWith(suffix);
        })) {
        return true;
    }
    static const QRegularExpression sharedObjectPattern(QStringLiteral(R"([.]so(?:[.][0-9]+)*$)"));
    return sharedObjectPattern.match(fileName).hasMatch();
}

[[nodiscard]] bool validatePackagePath(QStringView path, QString* errorMessage) {
    const QByteArray ascii = path.toString().toLatin1();
    if (path.isEmpty() || QString::fromLatin1(ascii) != path ||
        ascii.size() > maximumPathBytes || path.startsWith(u'/') ||
        path.contains(u'\\')) {
        setError(errorMessage, QStringLiteral("Package path is not canonical ASCII: %1").arg(path));
        return false;
    }

    const QStringList components = path.toString().split(u'/', Qt::KeepEmptyParts);
    if (components.isEmpty() || components.size() > maximumPathComponents) {
        setError(errorMessage, QStringLiteral("Package path has an invalid depth: %1").arg(path));
        return false;
    }
    for (const QString& component : components) {
        if (component.isEmpty() || component == QStringLiteral(".") ||
            component == QStringLiteral("..") || component.toLatin1().size() > maximumComponentBytes ||
            component.endsWith(u'.')) {
            setError(errorMessage,
                     QStringLiteral("Package path has an invalid component: %1").arg(path));
            return false;
        }
        const bool valid = std::all_of(component.begin(), component.end(), [](QChar character) {
            return (character >= u'a' && character <= u'z') ||
                   (character >= u'A' && character <= u'Z') ||
                   (character >= u'0' && character <= u'9') || character == u'.' ||
                   character == u'_' || character == u'-';
        });
        if (!valid) {
            setError(errorMessage,
                     QStringLiteral("Package path has an invalid component: %1").arg(path));
            return false;
        }
    }

    const bool atRoot = components.size() == 1;
    if ((atRoot && path != u"rule.toml") ||
        (!atRoot && components.front() != QStringLiteral("src") &&
         components.front() != QStringLiteral("docs") &&
         components.front() != QStringLiteral("tests"))) {
        setError(errorMessage,
                 QStringLiteral("Package file is outside an accepted package location: %1")
                     .arg(path));
        return false;
    }
    if (hasForbiddenNativeSuffix(path)) {
        setError(errorMessage,
                 QStringLiteral("Package file uses a forbidden native-code suffix: %1").arg(path));
        return false;
    }
    return true;
}

[[nodiscard]] QByteArray bigEndian32(quint32 value) {
    QByteArray bytes(4, Qt::Uninitialized);
    bytes[0] = static_cast<char>((value >> 24U) & 0xFFU);
    bytes[1] = static_cast<char>((value >> 16U) & 0xFFU);
    bytes[2] = static_cast<char>((value >> 8U) & 0xFFU);
    bytes[3] = static_cast<char>(value & 0xFFU);
    return bytes;
}

[[nodiscard]] QByteArray bigEndian64(quint64 value) {
    QByteArray bytes(8, Qt::Uninitialized);
    for (int index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned int>((7 - index) * 8);
        bytes[index] = static_cast<char>((value >> shift) & 0xFFU);
    }
    return bytes;
}

[[nodiscard]] QByteArray hashFiles(const std::vector<RulePackageFile>& files) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray domain("StreamViewRulePackage", 21);
    domain.append('\0');
    hash.addData(domain);
    hash.addData(bigEndian32(1));
    hash.addData(bigEndian32(static_cast<quint32>(files.size())));
    for (const RulePackageFile& file : files) {
        const QByteArray path = file.path.toLatin1();
        hash.addData(bigEndian32(static_cast<quint32>(path.size())));
        hash.addData(path);
        hash.addData(bigEndian64(static_cast<quint64>(file.contents.size())));
        hash.addData(file.contents);
    }
    return hash.result();
}

[[nodiscard]] QString fromUtf8(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] bool tableHasOnly(const toml::table& table,
                                std::initializer_list<std::string_view> keys,
                                QStringView tableName,
                                QString* errorMessage) {
    for (const auto& [key, value] : table) {
        Q_UNUSED(value);
        const std::string_view keyText = key.str();
        if (std::find(keys.begin(), keys.end(), keyText) == keys.end()) {
            setError(errorMessage,
                     QStringLiteral("Unknown key in %1: %2")
                         .arg(tableName, fromUtf8(keyText)));
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<QString> requiredString(const toml::table& table,
                                                    std::string_view key,
                                                    QStringView context,
                                                    QString* errorMessage) {
    const auto value = table[key].value<std::string>();
    if (!value) {
        setError(errorMessage,
                 QStringLiteral("%1.%2 must be a string")
                     .arg(context, fromUtf8(key)));
        return std::nullopt;
    }
    return fromUtf8(*value);
}

[[nodiscard]] std::optional<QStringList>
requiredStringArray(const toml::table& table,
                    std::string_view key,
                    QStringView context,
                    QString* errorMessage) {
    const toml::array* array = table[key].as_array();
    if (array == nullptr) {
        setError(errorMessage,
                 QStringLiteral("%1.%2 must be an array of strings")
                     .arg(context, fromUtf8(key)));
        return std::nullopt;
    }
    QStringList result;
    result.reserve(static_cast<qsizetype>(array->size()));
    for (const toml::node& item : *array) {
        const auto value = item.value<std::string>();
        if (!value) {
            setError(errorMessage,
                     QStringLiteral("%1.%2 must contain only strings")
                         .arg(context, fromUtf8(key)));
            return std::nullopt;
        }
        result.push_back(fromUtf8(*value));
    }
    return result;
}

[[nodiscard]] std::optional<RulePackageManifest>
parseManifest(const QByteArray& bytes, QString* errorMessage) {
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        setError(errorMessage, QStringLiteral("rule.toml must not contain a UTF-8 BOM"));
        return std::nullopt;
    }

    toml::table root;
    try {
        root = toml::parse(std::string_view(bytes.constData(),
                                            static_cast<std::size_t>(bytes.size())));
    } catch (const toml::parse_error& error) {
        setError(errorMessage,
                 QStringLiteral("rule.toml is not valid TOML: %1")
                     .arg(fromUtf8(error.description())));
        return std::nullopt;
    }

    if (!tableHasOnly(root,
                      {"manifest-version", "package", "compatibility", "entrypoints",
                       "documentation"},
                      u"manifest",
                      errorMessage)) {
        return std::nullopt;
    }
    const auto manifestVersion = root["manifest-version"].value<std::int64_t>();
    if (!manifestVersion || *manifestVersion != 1) {
        setError(errorMessage, QStringLiteral("manifest-version must be the integer 1"));
        return std::nullopt;
    }

    const toml::table* packageTable = root["package"].as_table();
    const toml::table* compatibilityTable = root["compatibility"].as_table();
    const toml::array* entryArray = root["entrypoints"].as_array();
    if (packageTable == nullptr || compatibilityTable == nullptr || entryArray == nullptr) {
        setError(errorMessage,
                 QStringLiteral("rule.toml requires package, compatibility, and entrypoints"));
        return std::nullopt;
    }
    if (!tableHasOnly(*packageTable,
                      {"id", "version", "authors", "license", "dependencies"},
                      u"package",
                      errorMessage) ||
        !tableHasOnly(*compatibilityTable,
                      {"language", "engine"},
                      u"compatibility",
                      errorMessage)) {
        return std::nullopt;
    }

    auto packageId = requiredString(*packageTable, "id", u"package", errorMessage);
    auto packageVersionText =
        requiredString(*packageTable, "version", u"package", errorMessage);
    auto authors = requiredStringArray(*packageTable, "authors", u"package", errorMessage);
    auto license = requiredString(*packageTable, "license", u"package", errorMessage);
    auto dependencies =
        requiredStringArray(*packageTable, "dependencies", u"package", errorMessage);
    auto languageText =
        requiredString(*compatibilityTable, "language", u"compatibility", errorMessage);
    auto engineText =
        requiredString(*compatibilityTable, "engine", u"compatibility", errorMessage);
    if (!packageId || !packageVersionText || !authors || !license || !dependencies ||
        !languageText || !engineText) {
        return std::nullopt;
    }
    if (!isDottedIdentifier(*packageId)) {
        setError(errorMessage, QStringLiteral("package.id is not a canonical package ID"));
        return std::nullopt;
    }
    auto packageVersion = SemanticVersion::parse(*packageVersionText, errorMessage);
    if (!packageVersion) {
        return std::nullopt;
    }
    auto language = LanguageContract::parse(*languageText, errorMessage);
    if (!language) {
        return std::nullopt;
    }
    auto engineRange = EngineCompatibilityRange::parse(*engineText, errorMessage);
    if (!engineRange) {
        return std::nullopt;
    }
    if (authors->isEmpty() || authors->size() > 16 ||
        std::any_of(authors->begin(), authors->end(), [](const QString& author) {
            return author.trimmed().isEmpty() || author.toUtf8().size() > 256;
        })) {
        setError(errorMessage, QStringLiteral("package.authors has invalid entries"));
        return std::nullopt;
    }
    if (!isPrintableAscii(*license, 256)) {
        setError(errorMessage, QStringLiteral("package.license must be printable ASCII"));
        return std::nullopt;
    }
    if (!dependencies->isEmpty()) {
        setError(errorMessage, QStringLiteral("package.dependencies must be empty in version 1"));
        return std::nullopt;
    }
    if (entryArray->empty() || entryArray->size() > 64U) {
        setError(errorMessage, QStringLiteral("entrypoints must contain 1 through 64 entries"));
        return std::nullopt;
    }

    RulePackageManifest manifest;
    manifest.packageId = std::move(*packageId);
    manifest.packageVersion = std::move(*packageVersion);
    manifest.authors = std::move(*authors);
    manifest.license = std::move(*license);
    manifest.languageContract = std::move(*language);
    manifest.engineRange = std::move(*engineRange);

    QSet<QString> entryIds;
    QSet<QString> sourcePaths;
    for (std::size_t index = 0; index < entryArray->size(); ++index) {
        const toml::table* table = (*entryArray)[index].as_table();
        if (table == nullptr ||
            !tableHasOnly(*table,
                          {"id", "format", "source", "profiles", "depth", "detector"},
                          u"entrypoints",
                          errorMessage)) {
            if (table == nullptr) {
                setError(errorMessage, QStringLiteral("Each entrypoint must be a table"));
            }
            return std::nullopt;
        }
        auto id = requiredString(*table, "id", u"entrypoints", errorMessage);
        auto format = requiredString(*table, "format", u"entrypoints", errorMessage);
        auto source = requiredString(*table, "source", u"entrypoints", errorMessage);
        auto profiles = requiredStringArray(*table, "profiles", u"entrypoints", errorMessage);
        auto depth = requiredString(*table, "depth", u"entrypoints", errorMessage);
        if (!id || !format || !source || !profiles || !depth) {
            return std::nullopt;
        }
        std::optional<QString> detector;
        if (table->contains("detector")) {
            auto detectorValue =
                requiredString(*table, "detector", u"entrypoints", errorMessage);
            if (!detectorValue) {
                return std::nullopt;
            }
            detector = std::move(*detectorValue);
        }
        if (!isEntryToken(*id) || !isDottedIdentifier(*format) ||
            !source->startsWith(QStringLiteral("src/")) ||
            !source->endsWith(QStringLiteral(".svfmt")) ||
            !validatePackagePath(*source, errorMessage) || profiles->isEmpty() ||
            profiles->size() > 64 ||
            std::any_of(profiles->begin(), profiles->end(), [](const QString& profile) {
                return !isEntryToken(profile);
            }) ||
            !isEntryToken(*depth) || (detector && !isEntryToken(*detector)) ||
            entryIds.contains(*id) || sourcePaths.contains(*source)) {
            if (errorMessage != nullptr && errorMessage->isEmpty()) {
                *errorMessage = QStringLiteral("Entrypoint metadata is invalid or duplicated");
            }
            return std::nullopt;
        }
        QSet<QString> uniqueProfiles;
        for (const QString& profile : *profiles) {
            if (uniqueProfiles.contains(profile)) {
                setError(errorMessage, QStringLiteral("Entrypoint profiles must be unique"));
                return std::nullopt;
            }
            uniqueProfiles.insert(profile);
        }
        entryIds.insert(*id);
        sourcePaths.insert(*source);
        manifest.entryPoints.push_back(RulePackageEntryPoint{
            std::move(*id), std::move(*format), std::move(*source), std::move(*profiles),
            std::move(*depth), std::move(detector)});
    }

    if (const toml::node* documentationNode = root.get("documentation")) {
        const toml::array* documentationArray = documentationNode->as_array();
        if (documentationArray == nullptr || documentationArray->size() > 64U) {
            setError(errorMessage, QStringLiteral("documentation must be an array of tables"));
            return std::nullopt;
        }
        static const QRegularExpression languagePattern(
            QStringLiteral(R"(^[a-z]{2,3}(?:-[A-Z]{2})?$)"));
        QSet<QString> languages;
        QSet<QString> documentationPaths;
        for (const toml::node& node : *documentationArray) {
            const toml::table* table = node.as_table();
            if (table == nullptr ||
                !tableHasOnly(*table, {"language", "path"}, u"documentation", errorMessage)) {
                if (table == nullptr) {
                    setError(errorMessage,
                             QStringLiteral("Each documentation entry must be a table"));
                }
                return std::nullopt;
            }
            auto documentationLanguage =
                requiredString(*table, "language", u"documentation", errorMessage);
            auto path = requiredString(*table, "path", u"documentation", errorMessage);
            if (!documentationLanguage || !path ||
                !languagePattern.match(*documentationLanguage).hasMatch() ||
                !path->startsWith(QStringLiteral("docs/")) ||
                !validatePackagePath(*path, errorMessage) ||
                languages.contains(*documentationLanguage) ||
                documentationPaths.contains(*path)) {
                if (errorMessage != nullptr && errorMessage->isEmpty()) {
                    *errorMessage =
                        QStringLiteral("Documentation metadata is invalid or duplicated");
                }
                return std::nullopt;
            }
            languages.insert(*documentationLanguage);
            documentationPaths.insert(*path);
            manifest.documentation.push_back(
                RulePackageDocumentation{std::move(*documentationLanguage), std::move(*path)});
        }
    }
    return manifest;
}

} // namespace

std::optional<SemanticVersion> SemanticVersion::parse(QStringView text,
                                                      QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (text.isEmpty() || text.contains(u'+')) {
        setError(errorMessage, QStringLiteral("Semantic version is not canonical"));
        return std::nullopt;
    }
    const qsizetype hyphen = text.indexOf(u'-');
    const QStringView core = hyphen < 0 ? text : text.first(hyphen);
    const QStringView prerelease = hyphen < 0 ? QStringView{} : text.sliced(hyphen + 1);
    const QStringList coreParts = core.toString().split(u'.', Qt::KeepEmptyParts);
    if (coreParts.size() != 3 ||
        std::any_of(coreParts.begin(), coreParts.end(), [](const QString& part) {
            return !isCanonicalNumber(part);
        })) {
        setError(errorMessage, QStringLiteral("Semantic version core is not canonical"));
        return std::nullopt;
    }
    QStringList prereleaseParts;
    if (hyphen >= 0) {
        prereleaseParts = prerelease.toString().split(u'.', Qt::KeepEmptyParts);
        if (prereleaseParts.isEmpty() ||
            std::any_of(prereleaseParts.begin(), prereleaseParts.end(), [](const QString& part) {
                return !isPrereleaseIdentifier(part);
            })) {
            setError(errorMessage, QStringLiteral("Semantic version prerelease is not canonical"));
            return std::nullopt;
        }
    }
    SemanticVersion result;
    result.text_ = text.toString();
    result.core_ = coreParts;
    result.prerelease_ = std::move(prereleaseParts);
    return result;
}

int compareSemanticVersions(const SemanticVersion& left,
                            const SemanticVersion& right) noexcept {
    for (qsizetype index = 0; index < 3; ++index) {
        const int compared = compareCanonicalNumbers(left.core_.at(index), right.core_.at(index));
        if (compared != 0) {
            return compared;
        }
    }
    if (left.prerelease_.isEmpty() || right.prerelease_.isEmpty()) {
        if (left.prerelease_.isEmpty() == right.prerelease_.isEmpty()) {
            return 0;
        }
        return left.prerelease_.isEmpty() ? 1 : -1;
    }
    const qsizetype common = std::min(left.prerelease_.size(), right.prerelease_.size());
    for (qsizetype index = 0; index < common; ++index) {
        const QStringView leftPart = left.prerelease_.at(index);
        const QStringView rightPart = right.prerelease_.at(index);
        const bool leftNumeric = isCanonicalNumber(leftPart);
        const bool rightNumeric = isCanonicalNumber(rightPart);
        if (leftNumeric != rightNumeric) {
            return leftNumeric ? -1 : 1;
        }
        const int compared = leftNumeric
                                 ? compareCanonicalNumbers(leftPart, rightPart)
                                 : leftPart.compare(rightPart, Qt::CaseSensitive);
        if (compared != 0) {
            return compared < 0 ? -1 : 1;
        }
    }
    if (left.prerelease_.size() == right.prerelease_.size()) {
        return 0;
    }
    return left.prerelease_.size() < right.prerelease_.size() ? -1 : 1;
}

std::optional<LanguageContract> LanguageContract::parse(QStringView text,
                                                        QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    const QStringList parts = text.toString().split(u'.', Qt::KeepEmptyParts);
    if (parts.size() != 2 || !isCanonicalNumber(parts.at(0)) ||
        !isCanonicalNumber(parts.at(1))) {
        setError(errorMessage, QStringLiteral("Language contract must be canonical MAJOR.MINOR"));
        return std::nullopt;
    }
    LanguageContract result;
    result.text_ = text.toString();
    result.major_ = parts.at(0);
    result.minor_ = parts.at(1);
    return result;
}

bool LanguageContract::accepts(QStringView runningLanguage) const {
    const auto running = parse(runningLanguage);
    if (!running || compareCanonicalNumbers(major_, running->major_) != 0) {
        return false;
    }
    if (major_ == QStringLiteral("0")) {
        return compareCanonicalNumbers(minor_, running->minor_) == 0;
    }
    return compareCanonicalNumbers(running->minor_, minor_) >= 0;
}

std::optional<EngineCompatibilityRange>
EngineCompatibilityRange::parse(QStringView text, QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (!text.startsWith(u">=") || text.count(u' ') != 1) {
        setError(errorMessage,
                 QStringLiteral("Engine range must have the form >=LOWER <UPPER"));
        return std::nullopt;
    }
    const qsizetype separator = text.indexOf(u" <");
    if (separator < 0) {
        setError(errorMessage,
                 QStringLiteral("Engine range must have the form >=LOWER <UPPER"));
        return std::nullopt;
    }
    const QStringView lowerText = text.sliced(2, separator - 2);
    const QStringView upperText = text.sliced(separator + 2);
    auto lower = SemanticVersion::parse(lowerText, errorMessage);
    auto upper = SemanticVersion::parse(upperText, errorMessage);
    if (!lower || !upper || lower->hasPrerelease() || upper->hasPrerelease() ||
        compareSemanticVersions(*lower, *upper) >= 0) {
        if (errorMessage != nullptr && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("Engine range endpoints are invalid");
        }
        return std::nullopt;
    }
    EngineCompatibilityRange result;
    result.text_ = text.toString();
    result.lower_ = std::move(*lower);
    result.upper_ = std::move(*upper);
    return result;
}

bool EngineCompatibilityRange::contains(QStringView runningEngine) const {
    const auto running = SemanticVersion::parse(runningEngine);
    return running && !running->hasPrerelease() &&
           compareSemanticVersions(*running, lower_) >= 0 &&
           compareSemanticVersions(*running, upper_) < 0;
}

std::optional<RulePackageIdentity>
RulePackageIdentity::create(QString packageId,
                            QString packageVersion,
                            QByteArray contentHash,
                            QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (!isDottedIdentifier(packageId)) {
        setError(errorMessage, QStringLiteral("Package identity has an invalid package ID"));
        return std::nullopt;
    }
    if (!SemanticVersion::parse(packageVersion, errorMessage)) {
        return std::nullopt;
    }
    if (contentHash.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        setError(errorMessage, QStringLiteral("Package identity must contain one SHA-256 digest"));
        return std::nullopt;
    }
    return RulePackageIdentity(
        std::move(packageId), std::move(packageVersion), std::move(contentHash));
}

RulePackageIdentity::RulePackageIdentity(QString packageId,
                                         QString packageVersion,
                                         QByteArray contentHash)
    : packageId_(std::move(packageId)), packageVersion_(std::move(packageVersion)),
      contentHash_(std::move(contentHash)) {}

QString RulePackageIdentity::contentHashText() const {
    return QStringLiteral("sha256:%1").arg(QString::fromLatin1(contentHash_.toHex()));
}

QString RulePackageIdentity::toString() const {
    return QStringLiteral("%1@%2#%3").arg(packageId_, packageVersion_, contentHashText());
}

RulePackage::RulePackage(RulePackageManifest manifest,
                         RulePackageIdentity identity,
                         std::vector<RulePackageFile> files)
    : manifest_(std::move(manifest)), identity_(std::move(identity)),
      files_(std::move(files)) {}

RulePackageLoadResult RulePackage::fromFiles(std::vector<RulePackageFile> files) {
    const auto fail = [](RulePackageLoadStatus status, QString message) {
        return RulePackageLoadResult{status, std::nullopt, std::move(message)};
    };
    if (files.empty() || files.size() > static_cast<std::size_t>(maximumFileCount)) {
        return fail(RulePackageLoadStatus::InvalidTree,
                    QStringLiteral("Rule package must contain 1 through 1024 files"));
    }

    QSet<QString> foldedPaths;
    quint64 totalBytes = 0;
    for (const RulePackageFile& file : files) {
        QString pathError;
        if (!validatePackagePath(file.path, &pathError)) {
            return fail(RulePackageLoadStatus::InvalidTree, std::move(pathError));
        }
        const QString folded = file.path.toLower();
        if (foldedPaths.contains(folded)) {
            return fail(RulePackageLoadStatus::InvalidTree,
                        QStringLiteral("Rule package paths collide: %1").arg(file.path));
        }
        foldedPaths.insert(folded);
        if (file.contents.size() > maximumFileBytes ||
            (file.path == QStringLiteral("rule.toml") &&
             file.contents.size() > maximumManifestBytes)) {
            return fail(RulePackageLoadStatus::InvalidTree,
                        QStringLiteral("Rule package file exceeds its size limit: %1")
                            .arg(file.path));
        }
        const quint64 fileSize = static_cast<quint64>(file.contents.size());
        if (fileSize > maximumTotalBytes - totalBytes) {
            return fail(RulePackageLoadStatus::InvalidTree,
                        QStringLiteral("Rule package exceeds the total size limit"));
        }
        totalBytes += fileSize;
    }

    std::sort(files.begin(), files.end(), [](const RulePackageFile& left,
                                             const RulePackageFile& right) {
        return left.path.toLatin1() < right.path.toLatin1();
    });
    QStringList foldedSorted = foldedPaths.values();
    std::sort(foldedSorted.begin(), foldedSorted.end());
    for (qsizetype index = 1; index < foldedSorted.size(); ++index) {
        if (foldedSorted.at(index).startsWith(foldedSorted.at(index - 1) + u'/')) {
            return fail(RulePackageLoadStatus::InvalidTree,
                        QStringLiteral("A package file path is also used as a directory"));
        }
    }
    const auto manifestFile = std::find_if(files.begin(), files.end(), [](const auto& file) {
        return file.path == QStringLiteral("rule.toml");
    });
    if (manifestFile == files.end()) {
        return fail(RulePackageLoadStatus::InvalidTree,
                    QStringLiteral("Rule package has no rule.toml"));
    }
    QString manifestError;
    auto manifest = parseManifest(manifestFile->contents, &manifestError);
    if (!manifest) {
        return fail(RulePackageLoadStatus::InvalidManifest, std::move(manifestError));
    }

    const QSet<QString> paths = [&files] {
        QSet<QString> result;
        for (const RulePackageFile& file : files) {
            result.insert(file.path);
        }
        return result;
    }();
    for (const RulePackageEntryPoint& entry : manifest->entryPoints) {
        if (!paths.contains(entry.sourcePath)) {
            return fail(RulePackageLoadStatus::InvalidManifest,
                        QStringLiteral("Entrypoint source is missing: %1")
                            .arg(entry.sourcePath));
        }
    }
    for (const RulePackageDocumentation& documentation : manifest->documentation) {
        if (!paths.contains(documentation.path)) {
            return fail(RulePackageLoadStatus::InvalidManifest,
                        QStringLiteral("Documentation file is missing: %1")
                            .arg(documentation.path));
        }
    }

    RulePackageIdentity identity(
        manifest->packageId, manifest->packageVersion.text(), hashFiles(files));
    RulePackage package(std::move(*manifest), std::move(identity), std::move(files));
    return RulePackageLoadResult{RulePackageLoadStatus::Loaded,
                                 std::optional<RulePackage>(std::move(package)),
                                 {}};
}

const QByteArray* RulePackage::fileContents(QStringView path) const noexcept {
    const auto found = std::lower_bound(
        files_.begin(), files_.end(), path, [](const RulePackageFile& file, QStringView value) {
            return file.path.toLatin1() < value.toString().toLatin1();
        });
    if (found == files_.end() || found->path != path) {
        return nullptr;
    }
    return &found->contents;
}

} // namespace streamview::rules
