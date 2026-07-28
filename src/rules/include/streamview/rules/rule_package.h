#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>
#include <vector>

namespace streamview::rules {

class SemanticVersion final {
public:
    [[nodiscard]] static std::optional<SemanticVersion>
    parse(QStringView text, QString* errorMessage = nullptr);

    [[nodiscard]] const QString& text() const noexcept { return text_; }
    [[nodiscard]] bool hasPrerelease() const noexcept { return !prerelease_.isEmpty(); }

private:
    QString text_;
    QStringList core_;
    QStringList prerelease_;

    friend int compareSemanticVersions(const SemanticVersion& left,
                                       const SemanticVersion& right) noexcept;
};

[[nodiscard]] int compareSemanticVersions(const SemanticVersion& left,
                                          const SemanticVersion& right) noexcept;

class LanguageContract final {
public:
    [[nodiscard]] static std::optional<LanguageContract>
    parse(QStringView text, QString* errorMessage = nullptr);

    [[nodiscard]] const QString& text() const noexcept { return text_; }
    [[nodiscard]] bool accepts(QStringView runningLanguage) const;

private:
    QString text_;
    QString major_;
    QString minor_;
};

class EngineCompatibilityRange final {
public:
    [[nodiscard]] static std::optional<EngineCompatibilityRange>
    parse(QStringView text, QString* errorMessage = nullptr);

    [[nodiscard]] const QString& text() const noexcept { return text_; }
    [[nodiscard]] bool contains(QStringView runningEngine) const;

private:
    QString text_;
    SemanticVersion lower_;
    SemanticVersion upper_;
};

struct RulePackageEntryPoint final {
    QString id;
    QString format;
    QString sourcePath;
    QStringList profiles;
    QString depth;
    std::optional<QString> detector;
};

struct RulePackageDocumentation final {
    QString language;
    QString path;
};

struct RulePackageManifest final {
    QString packageId;
    SemanticVersion packageVersion;
    QStringList authors;
    QString license;
    LanguageContract languageContract;
    EngineCompatibilityRange engineRange;
    std::vector<RulePackageEntryPoint> entryPoints;
    std::vector<RulePackageDocumentation> documentation;
};

struct RulePackageFile final {
    // Importers enforce filesystem type, link, and permission rules before
    // producing this logical-tree value.
    QString path;
    QByteArray contents;
};

class RulePackageIdentity final {
public:
    [[nodiscard]] static std::optional<RulePackageIdentity>
    create(QString packageId,
           QString packageVersion,
           QByteArray contentHash,
           QString* errorMessage = nullptr);

    [[nodiscard]] const QString& packageId() const noexcept { return packageId_; }
    [[nodiscard]] const QString& packageVersion() const noexcept { return packageVersion_; }
    [[nodiscard]] const QByteArray& contentHash() const noexcept { return contentHash_; }

    [[nodiscard]] QString contentHashText() const;
    [[nodiscard]] QString toString() const;

    [[nodiscard]] bool operator==(const RulePackageIdentity&) const = default;

private:
    RulePackageIdentity(QString packageId, QString packageVersion, QByteArray contentHash);

    QString packageId_;
    QString packageVersion_;
    QByteArray contentHash_;

    friend class RulePackage;
};

enum class RulePackageLoadStatus {
    Loaded,
    InvalidTree,
    InvalidManifest,
};

struct RulePackageLoadResult;

class RulePackage final {
public:
    [[nodiscard]] static RulePackageLoadResult
    fromFiles(std::vector<RulePackageFile> files);

    [[nodiscard]] const RulePackageManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] const RulePackageIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] const std::vector<RulePackageFile>& files() const noexcept { return files_; }
    [[nodiscard]] const QByteArray* fileContents(QStringView path) const noexcept;

private:
    RulePackage(RulePackageManifest manifest,
                RulePackageIdentity identity,
                std::vector<RulePackageFile> files);

    RulePackageManifest manifest_;
    RulePackageIdentity identity_;
    std::vector<RulePackageFile> files_;
};

struct RulePackageLoadResult final {
    RulePackageLoadStatus status = RulePackageLoadStatus::InvalidTree;
    std::optional<RulePackage> package;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == RulePackageLoadStatus::Loaded && package.has_value();
    }
};

} // namespace streamview::rules
