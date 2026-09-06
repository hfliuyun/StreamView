#include "rule_manager_dialog.h"

#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_package.h>
#include <streamview/rules/rule_package_store.h>

#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <set>
#include <vector>

using streamview::app::RuleManagerDialog;
using streamview::rules::RulePackage;
using streamview::rules::RulePackageCatalog;
using streamview::rules::RulePackageFile;
using streamview::rules::RulePackageStore;

namespace {

[[nodiscard]] QByteArray makeManifest(const QString& id,
                                      const QString& version,
                                      const QString& entryId = QStringLiteral("main"),
                                      const QString& format = QStringLiteral("application.custom")) {
    return QString(
        "manifest-version = 1\n\n"
        "[package]\n"
        "id = \"%1\"\n"
        "version = \"%2\"\n"
        "authors = [\"Custom Author\"]\n"
        "license = \"MIT\"\n"
        "dependencies = []\n\n"
        "[compatibility]\n"
        "language = \"0.1\"\n"
        "engine = \">=0.1.0 <0.2.0\"\n\n"
        "[[entrypoints]]\n"
        "id = \"%3\"\n"
        "format = \"%4\"\n"
        "source = \"src/main.svfmt\"\n"
        "profiles = [\"baseline\"]\n"
        "depth = \"structural\"\n"
    ).arg(id, version, entryId, format).toUtf8();
}

[[nodiscard]] QString createSvruleArchive(const QString& dirPath,
                                         const QString& filename,
                                         const QString& id,
                                         const QString& version,
                                         const QByteArray& sourceCode = QByteArrayLiteral("struct Root { bits<8> v; }\nentry Root;\n")) {
    std::vector<RulePackageFile> files = {
        {QStringLiteral("rule.toml"), makeManifest(id, version)},
        {QStringLiteral("src/main.svfmt"), sourceCode}
    };
    auto loaded = RulePackage::fromFiles(std::move(files));
    if (!loaded.succeeded() || !loaded.package.has_value()) {
        return {};
    }
    const QString archivePath = QDir(dirPath).filePath(filename);
    const auto writeResult = RulePackageStore::writeArchive(*loaded.package, archivePath);
    if (!writeResult.succeeded()) {
        return {};
    }
    return archivePath;
}

[[nodiscard]] QString realPath(const QTemporaryDir& directory) {
    return QFileInfo(directory.path()).canonicalFilePath();
}

} // namespace

class RuleManagerDialogTest : public QObject {
    Q_OBJECT

private slots:
    void dialogPopulatesBundledPackagesOnOpen() {
        RulePackageCatalog catalog;
        auto aac = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(aac.succeeded() && aac.package);
        QVERIFY(catalog.registerPackage(std::move(*aac.package)).succeeded());

        auto h264 = streamview::rules::loadH264AnnexBRulePackage();
        QVERIFY(h264.succeeded() && h264.package);
        QVERIFY(catalog.registerPackage(std::move(*h264.package)).succeeded());

        auto mp4 = streamview::rules::loadMp4IsobmffRulePackage();
        QVERIFY(mp4.succeeded() && mp4.package);
        QVERIFY(catalog.registerPackage(std::move(*mp4.package)).succeeded());

        QTemporaryDir storeDir;
        QVERIFY(storeDir.isValid());

        const std::set<QString> bundledIds = {
            QStringLiteral("org.streamview.aac"),
            QStringLiteral("org.streamview.h264"),
            QStringLiteral("org.streamview.mp4")
        };

        RuleManagerDialog dlg(nullptr, catalog, realPath(storeDir), bundledIds);

        auto* table = dlg.findChild<QTableWidget*>(QStringLiteral("rulePackageTable"));
        auto* headerLabel = dlg.findChild<QLabel*>(QStringLiteral("headerLabel"));
        auto* installButton = dlg.findChild<QPushButton*>(QStringLiteral("installPackageButton"));
        auto* buttonBox = dlg.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"));

        QVERIFY(table != nullptr);
        QVERIFY(headerLabel != nullptr);
        QVERIFY(installButton != nullptr);
        QVERIFY(buttonBox != nullptr);

        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(dlg.packages().size(), std::size_t{3});

        for (int row = 0; row < table->rowCount(); ++row) {
            const QString id = table->item(row, 0)->text();
            const QString version = table->item(row, 1)->text();
            const QString origin = table->item(row, 2)->text();
            const QString hash = table->item(row, 3)->text();
            const QString entryPoints = table->item(row, 4)->text();
            const QString description = table->item(row, 5)->text();

            QVERIFY(bundledIds.find(id) != bundledIds.end());
            QVERIFY(!version.isEmpty());
            QCOMPARE(origin, QStringLiteral("[Bundled]"));
            QVERIFY(hash.endsWith(QStringLiteral("...")));
            QCOMPARE(table->item(row, 3)->toolTip().size(), 64);
            QVERIFY(!entryPoints.isEmpty());
            QVERIFY(!description.isEmpty());
        }
    }

    void installPackageSuccessfullyImportsAndRegistersSvrule() {
        RulePackageCatalog catalog;
        QTemporaryDir storeDir;
        QVERIFY(storeDir.isValid());
        QTemporaryDir workDir;
        QVERIFY(workDir.isValid());

        const QString archivePath = createSvruleArchive(
            workDir.path(),
            QStringLiteral("custom-0.1.0.svrule"),
            QStringLiteral("org.example.custom"),
            QStringLiteral("0.1.0")
        );
        QVERIFY(!archivePath.isEmpty());

        RuleManagerDialog dlg(nullptr, catalog, realPath(storeDir));

        dlg.setFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return archivePath;
        });

        QString capturedTitle;
        QString capturedMessage;
        dlg.setMessageDialogHandlerForTesting([&](QWidget*, const QString& title, const QString& msg) {
            capturedTitle = title;
            capturedMessage = msg;
        });

        dlg.installPackage();

        QCOMPARE(capturedTitle, QStringLiteral("Package Installed"));
        QVERIFY(capturedMessage.contains(QStringLiteral("org.example.custom")));
        QVERIFY(capturedMessage.contains(QStringLiteral("0.1.0")));

        QCOMPARE(dlg.packages().size(), std::size_t{1});
        auto* table = dlg.findChild<QTableWidget*>(QStringLiteral("rulePackageTable"));
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 1);

        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("org.example.custom"));
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("0.1.0"));
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("[Installed]"));
        QCOMPARE(table->item(0, 4)->text(), QStringLiteral("main"));
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("application.custom"));

        // Verify disk discovery via RulePackageStore
        const auto installed = RulePackageStore::discoverInstalled(realPath(storeDir));
        QCOMPARE(installed.size(), std::size_t{1});
        QCOMPARE(installed[0].identity().packageId(), QStringLiteral("org.example.custom"));
        QCOMPARE(installed[0].identity().packageVersion(), QStringLiteral("0.1.0"));
    }

    void installPackageRejectsInvalidArchiveWithErrorMessage() {
        RulePackageCatalog catalog;
        QTemporaryDir storeDir;
        QVERIFY(storeDir.isValid());
        QTemporaryDir workDir;
        QVERIFY(workDir.isValid());

        const QString corruptPath = workDir.filePath(QStringLiteral("corrupted.svrule"));
        {
            QFile file(corruptPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("Not A Valid Zip Or Rule Archive At All");
            file.close();
        }

        RuleManagerDialog dlg(nullptr, catalog, realPath(storeDir));

        dlg.setFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return corruptPath;
        });

        QString capturedTitle;
        QString capturedMessage;
        dlg.setMessageDialogHandlerForTesting([&](QWidget*, const QString& title, const QString& msg) {
            capturedTitle = title;
            capturedMessage = msg;
        });

        dlg.installPackage();

        QCOMPARE(capturedTitle, QStringLiteral("Invalid Rule Package"));
        QVERIFY(capturedMessage.contains(corruptPath));
        QCOMPARE(dlg.packages().size(), std::size_t{0});
    }

    void installPackageRejectsOverwritingBundledPackage() {
        RulePackageCatalog catalog;
        auto h264 = streamview::rules::loadH264AnnexBRulePackage();
        QVERIFY(h264.succeeded() && h264.package);
        QVERIFY(catalog.registerPackage(std::move(*h264.package)).succeeded());

        QTemporaryDir storeDir;
        QVERIFY(storeDir.isValid());
        QTemporaryDir workDir;
        QVERIFY(workDir.isValid());

        const QString spoofedPath = createSvruleArchive(
            workDir.path(),
            QStringLiteral("spoofed-h264.svrule"),
            QStringLiteral("org.streamview.h264"),
            QStringLiteral("9.9.9")
        );
        QVERIFY(!spoofedPath.isEmpty());

        const std::set<QString> bundledIds = {QStringLiteral("org.streamview.h264")};
        RuleManagerDialog dlg(nullptr, catalog, realPath(storeDir), bundledIds);

        dlg.setFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return spoofedPath;
        });

        QString capturedTitle;
        QString capturedMessage;
        dlg.setMessageDialogHandlerForTesting([&](QWidget*, const QString& title, const QString& msg) {
            capturedTitle = title;
            capturedMessage = msg;
        });

        dlg.installPackage();

        QCOMPARE(capturedTitle, QStringLiteral("Package Installation Rejected"));
        QVERIFY(capturedMessage.contains(QStringLiteral("Cannot overwrite or replace bundled official rule package: org.streamview.h264")));
        QCOMPARE(dlg.packages().size(), std::size_t{1});
        QCOMPARE(dlg.packages()[0].identity.packageVersion(), QStringLiteral("0.1.40"));
    }

    void installPackageIdempotentlyHandlesAlreadyInstalledPackage() {
        RulePackageCatalog catalog;
        QTemporaryDir storeDir;
        QVERIFY(storeDir.isValid());
        QTemporaryDir workDir;
        QVERIFY(workDir.isValid());

        const QString archivePath = createSvruleArchive(
            workDir.path(),
            QStringLiteral("custom-0.1.0.svrule"),
            QStringLiteral("org.example.custom"),
            QStringLiteral("0.1.0")
        );
        QVERIFY(!archivePath.isEmpty());

        RuleManagerDialog dlg(nullptr, catalog, realPath(storeDir));

        dlg.setFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return archivePath;
        });

        QString capturedTitle;
        dlg.setMessageDialogHandlerForTesting([&](QWidget*, const QString& title, const QString&) {
            capturedTitle = title;
        });

        // First install
        dlg.installPackage();
        QCOMPARE(capturedTitle, QStringLiteral("Package Installed"));
        QCOMPARE(dlg.packages().size(), std::size_t{1});

        // Second install of the same package
        capturedTitle.clear();
        dlg.installPackage();
        QCOMPARE(capturedTitle, QStringLiteral("Package Installed"));
        QCOMPARE(dlg.packages().size(), std::size_t{1});
    }

    void installPackageHandlesRegistrationConflict() {
        RulePackageCatalog catalog;
        QTemporaryDir storeDir;
        QVERIFY(storeDir.isValid());
        QTemporaryDir workDir;
        QVERIFY(workDir.isValid());

        const QString firstPath = createSvruleArchive(
            workDir.path(),
            QStringLiteral("custom-a.svrule"),
            QStringLiteral("org.example.custom"),
            QStringLiteral("0.1.0"),
            QByteArrayLiteral("struct V1 { bits<8> a; }\nentry V1;\n")
        );
        const QString secondConflictPath = createSvruleArchive(
            workDir.path(),
            QStringLiteral("custom-b.svrule"),
            QStringLiteral("org.example.custom"),
            QStringLiteral("0.1.0"),
            QByteArrayLiteral("struct V2 { bits<16> b; }\nentry V2;\n")
        );

        RuleManagerDialog dlg(nullptr, catalog, realPath(storeDir));

        QString nextPath = firstPath;
        dlg.setFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return nextPath;
        });

        QString capturedTitle;
        dlg.setMessageDialogHandlerForTesting([&](QWidget*, const QString& title, const QString&) {
            capturedTitle = title;
        });

        // First installation succeeds
        dlg.installPackage();
        QCOMPARE(capturedTitle, QStringLiteral("Package Installed"));
        QCOMPARE(dlg.packages().size(), std::size_t{1});

        // Second installation with different content triggers conflict
        nextPath = secondConflictPath;
        capturedTitle.clear();
        dlg.installPackage();
        QCOMPARE(capturedTitle, QStringLiteral("Registration Conflict"));
        QCOMPARE(dlg.packages().size(), std::size_t{1});
    }
};

QTEST_MAIN(RuleManagerDialogTest)

#include "rule_manager_dialog_test.moc"
