#include "session_document.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <optional>
#include <functional>

using streamview::app::RawDisplayMode;
using streamview::app::SessionAnnotation;
using streamview::app::SessionBookmark;
using streamview::app::SessionDocument;
using streamview::app::SessionDocumentLoadStatus;
using streamview::app::SessionUserState;
using streamview::core::SourceFingerprint;
using streamview::core::SourceFingerprintMode;
using streamview::rules::RuleEntryPointIdentity;
using streamview::rules::RulePackageIdentity;

namespace {

[[nodiscard]] SourceFingerprint fingerprint() {
    auto value = SourceFingerprint::create(SourceFingerprint::algorithmVersion(),
                                           SourceFingerprintMode::FullContentSha256, 5,
                                           std::nullopt, QByteArray(32, '\x11'));
    Q_ASSERT(value.has_value());
    return std::move(*value);
}

[[nodiscard]] RuleEntryPointIdentity ruleIdentity() {
    auto package = RulePackageIdentity::create(QStringLiteral("org.example.packet"),
                                               QStringLiteral("1.2.3"),
                                               QByteArray(32, '\x22'));
    Q_ASSERT(package.has_value());
    auto entry = RuleEntryPointIdentity::create(std::move(*package), QStringLiteral("packet"));
    Q_ASSERT(entry.has_value());
    return std::move(*entry);
}

[[nodiscard]] SessionUserState userState() {
    SessionUserState state;
    state.bookmarks = {SessionBookmark{QStringLiteral("NAL header"), 8}};
    state.annotations = {SessionAnnotation{QStringLiteral("Check this flag"), 9, 3}};
    state.expandedPaths = {QStringLiteral("root/nal_unit[0]"),
                           QStringLiteral("root/nal_unit[0]/NalUnitHeader")};
    state.view.rawPageIndex = 0;
    state.view.rawDisplayMode = RawDisplayMode::Combined;
    state.view.selectedSourceBitOffset = 10;
    state.view.selectedAnalysisPath = QStringLiteral("root/nal_unit[0]/NalUnitHeader/nal_ref_idc");
    return state;
}

[[nodiscard]] SessionDocument document() {
    QString errorMessage;
    auto value = SessionDocument::create(QStringLiteral("/media/fixture.264"),
                                         QStringLiteral("/media/fixture.264"), fingerprint(),
                                         ruleIdentity(), userState(), &errorMessage);
    Q_ASSERT_X(value.has_value(), "document", qPrintable(errorMessage));
    return std::move(*value);
}

[[nodiscard]] QByteArray mutateRoot(
    const QByteArray& json, const std::function<void(QJsonObject&)>& mutation) {
    QJsonObject root = QJsonDocument::fromJson(json).object();
    mutation(root);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

class SessionDocumentTest final : public QObject {
    Q_OBJECT

private slots:
    void roundTripsEveryVersionOneField() {
        const SessionDocument original = document();
        const QByteArray json = original.toJson();

        const auto loaded = SessionDocument::parse(json);

        QVERIFY2(loaded.succeeded(), qPrintable(loaded.errorMessage));
        QCOMPARE(loaded.document->sourcePath(), original.sourcePath());
        QCOMPARE(loaded.document->sourceIdentity(), original.sourceIdentity());
        QCOMPARE(loaded.document->sourceFingerprint(), original.sourceFingerprint());
        QCOMPARE(loaded.document->ruleIdentity(), original.ruleIdentity());
        QVERIFY(loaded.document->userState() == original.userState());
        const QJsonObject root = QJsonDocument::fromJson(json).object();
        QCOMPARE(root.value(QStringLiteral("schemaVersion")).toInt(), 1);
        QVERIFY(!root.contains(QStringLiteral("cache")));
        QCOMPARE(root.value(QStringLiteral("source"))
                     .toObject()
                     .value(QStringLiteral("fingerprint"))
                     .toObject()
                     .value(QStringLiteral("sizeBytes"))
                     .toString(),
                 QStringLiteral("5"));
    }

    void savesAndLoadsThroughAnAtomicFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("analysis.svsession"));
        QString errorMessage;

        QVERIFY2(document().save(path, &errorMessage), qPrintable(errorMessage));
        const auto loaded = SessionDocument::load(path);

        QVERIFY2(loaded.succeeded(), qPrintable(loaded.errorMessage));
        QCOMPARE(loaded.document->ruleIdentity(), document().ruleIdentity());
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().startsWith("{\n"));
    }

    void rejectsMalformedJsonAndClosedSchemaViolations() {
        const QByteArray valid = document().toJson();
        const auto invalidJson = SessionDocument::parse(QByteArrayLiteral("{not-json"));
        QCOMPARE(invalidJson.status, SessionDocumentLoadStatus::InvalidJson);

        const auto missing = SessionDocument::parse(mutateRoot(valid, [](QJsonObject& root) {
            root.remove(QStringLiteral("rule"));
        }));
        QCOMPARE(missing.status, SessionDocumentLoadStatus::InvalidSchema);

        const auto unknown = SessionDocument::parse(mutateRoot(valid, [](QJsonObject& root) {
            root.insert(QStringLiteral("cachePages"), QJsonObject{});
        }));
        QCOMPARE(unknown.status, SessionDocumentLoadStatus::InvalidSchema);

        const auto unsupported = SessionDocument::parse(mutateRoot(valid, [](QJsonObject& root) {
            root.insert(QStringLiteral("schemaVersion"), 2);
        }));
        QCOMPARE(unsupported.status, SessionDocumentLoadStatus::UnsupportedVersion);

        QByteArray duplicate = valid;
        duplicate.insert(duplicate.indexOf('{') + 1,
                         QByteArrayLiteral("\n    \"\\u0073chemaVersion\": 1,"));
        const auto duplicateKey = SessionDocument::parse(duplicate);
        QCOMPARE(duplicateKey.status, SessionDocumentLoadStatus::InvalidSchema);
        QVERIFY(duplicateKey.errorMessage.contains(QStringLiteral("duplicate"),
                                                   Qt::CaseInsensitive));
    }

    void rejectsMalformedFingerprintsAndRulePins() {
        const QByteArray valid = document().toJson();
        const auto badFingerprint = SessionDocument::parse(
            mutateRoot(valid, [](QJsonObject& root) {
                QJsonObject source = root.value(QStringLiteral("source")).toObject();
                QJsonObject fingerprint =
                    source.value(QStringLiteral("fingerprint")).toObject();
                fingerprint.insert(QStringLiteral("sha256"), QStringLiteral("11ff"));
                source.insert(QStringLiteral("fingerprint"), fingerprint);
                root.insert(QStringLiteral("source"), source);
            }));
        QCOMPARE(badFingerprint.status, SessionDocumentLoadStatus::InvalidSchema);

        const auto badRule = SessionDocument::parse(mutateRoot(valid, [](QJsonObject& root) {
            QJsonObject rule = root.value(QStringLiteral("rule")).toObject();
            rule.insert(QStringLiteral("contentSha256"), QString(64, u'G'));
            root.insert(QStringLiteral("rule"), rule);
        }));
        QCOMPARE(badRule.status, SessionDocumentLoadStatus::InvalidSchema);

        const auto noncanonicalCoordinate =
            SessionDocument::parse(mutateRoot(valid, [](QJsonObject& root) {
                QJsonArray bookmarks = root.value(QStringLiteral("bookmarks")).toArray();
                QJsonObject first = bookmarks.first().toObject();
                first.insert(QStringLiteral("sourceBitOffset"), QStringLiteral("08"));
                bookmarks.replace(0, first);
                root.insert(QStringLiteral("bookmarks"), bookmarks);
            }));
        QCOMPARE(noncanonicalCoordinate.status, SessionDocumentLoadStatus::InvalidSchema);
    }

    void rejectsUserStateOutsideThePinnedSource() {
        const QByteArray valid = document().toJson();
        const auto outside = SessionDocument::parse(mutateRoot(valid, [](QJsonObject& root) {
            QJsonObject view = root.value(QStringLiteral("view")).toObject();
            view.insert(QStringLiteral("selectedSourceBitOffset"), QStringLiteral("40"));
            root.insert(QStringLiteral("view"), view);
        }));
        QCOMPARE(outside.status, SessionDocumentLoadStatus::InvalidSchema);

        const auto duplicatePath =
            SessionDocument::parse(mutateRoot(valid, [](QJsonObject& root) {
                QJsonArray paths = root.value(QStringLiteral("expandedPaths")).toArray();
                paths.append(paths.first());
                root.insert(QStringLiteral("expandedPaths"), paths);
            }));
        QCOMPARE(duplicatePath.status, SessionDocumentLoadStatus::InvalidSchema);
    }

    void boundsInputBeforeParsingOrAllocatingState() {
        const QByteArray oversized(SessionDocument::maximumDocumentBytes() + 1, ' ');
        QCOMPARE(SessionDocument::parse(oversized).status, SessionDocumentLoadStatus::TooLarge);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("large.svsession")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(oversized), oversized.size());
        file.close();
        QCOMPARE(SessionDocument::load(file.fileName()).status,
                 SessionDocumentLoadStatus::TooLarge);
    }
};

QTEST_GUILESS_MAIN(SessionDocumentTest)

#include "session_document_test.moc"
