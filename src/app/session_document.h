#pragma once

#include "raw_data_model.h"

#include <streamview/core/source_fingerprint.h>
#include <streamview/rules/rule_package.h>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>
#include <vector>

namespace streamview::app {

struct SessionBookmark final {
    QString label;
    quint64 sourceBitOffset = 0;

    [[nodiscard]] bool operator==(const SessionBookmark&) const = default;
};

struct SessionAnnotation final {
    QString text;
    quint64 sourceBitOffset = 0;
    quint64 bitLength = 0;

    [[nodiscard]] bool operator==(const SessionAnnotation&) const = default;
};

struct SessionViewState final {
    quint64 rawPageIndex = 0;
    RawDisplayMode rawDisplayMode = RawDisplayMode::Hex;
    std::optional<quint64> selectedSourceBitOffset;
    std::optional<QString> selectedAnalysisPath;

    [[nodiscard]] bool operator==(const SessionViewState&) const = default;
};

struct SessionUserState final {
    std::vector<SessionBookmark> bookmarks;
    std::vector<SessionAnnotation> annotations;
    QStringList expandedPaths;
    SessionViewState view;

    [[nodiscard]] bool operator==(const SessionUserState&) const = default;
};

enum class SessionDocumentLoadStatus : quint8 {
    Loaded,
    IoError,
    TooLarge,
    InvalidJson,
    InvalidSchema,
    UnsupportedVersion,
};

class SessionDocument;
struct SessionDocumentLoadResult;

class SessionDocument final {
public:
    [[nodiscard]] static constexpr quint32 schemaVersion() noexcept { return 1; }
    [[nodiscard]] static constexpr qsizetype maximumDocumentBytes() noexcept {
        return 1024 * 1024;
    }

    [[nodiscard]] static std::optional<SessionDocument>
    create(QString sourcePath, QString sourceIdentity,
           core::SourceFingerprint sourceFingerprint,
           rules::RuleEntryPointIdentity ruleIdentity, SessionUserState userState = {},
           QString* errorMessage = nullptr);

    [[nodiscard]] static SessionDocumentLoadResult parse(const QByteArray& json);
    [[nodiscard]] static SessionDocumentLoadResult load(const QString& path);

    [[nodiscard]] QByteArray toJson() const;
    [[nodiscard]] bool save(const QString& path, QString* errorMessage = nullptr) const;

    [[nodiscard]] const QString& sourcePath() const noexcept { return sourcePath_; }
    [[nodiscard]] const QString& sourceIdentity() const noexcept { return sourceIdentity_; }
    [[nodiscard]] const core::SourceFingerprint& sourceFingerprint() const noexcept {
        return sourceFingerprint_;
    }
    [[nodiscard]] const rules::RuleEntryPointIdentity& ruleIdentity() const noexcept {
        return ruleIdentity_;
    }
    [[nodiscard]] const SessionUserState& userState() const noexcept { return userState_; }

private:
    SessionDocument(QString sourcePath, QString sourceIdentity,
                    core::SourceFingerprint sourceFingerprint,
                    rules::RuleEntryPointIdentity ruleIdentity, SessionUserState userState);

    QString sourcePath_;
    QString sourceIdentity_;
    core::SourceFingerprint sourceFingerprint_;
    rules::RuleEntryPointIdentity ruleIdentity_;
    SessionUserState userState_;
};

struct SessionDocumentLoadResult final {
    SessionDocumentLoadStatus status = SessionDocumentLoadStatus::InvalidSchema;
    std::optional<SessionDocument> document;
    QString errorMessage;

    [[nodiscard]] bool succeeded() const noexcept;
};

} // namespace streamview::app
