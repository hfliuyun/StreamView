#include "main_window.h"
#include "raw_data_model.h"
#include "raw_data_view.h"

#include <QFile>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>

using streamview::app::MainWindow;
using streamview::app::RawDataModel;
using streamview::app::RawDataView;
using streamview::app::RawDisplayMode;

namespace {

QString writeFixture(QTemporaryDir& directory, const QString& name, const QByteArray& bytes) {
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

} // namespace

class MainWindowTest final : public QObject {
    Q_OBJECT

private slots:
    void opensOneSessionIntoTheRawAndAnalysisViews() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("valid.264"), QByteArray::fromHex("00000165"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        QString errorMessage;

        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));

        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        QVERIFY(rawView != nullptr);
        QCOMPARE(rawView, window.centralWidget());
        QCOMPARE(rawView->model()->rowCount(), 1);
        QCOMPARE(rawView->model()
                     ->data(rawView->model()->index(0, RawDataModel::FirstByte))
                     .toString(),
                 QStringLiteral("00"));

        auto* treeView =
            window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(treeView != nullptr);
        QCOMPARE(treeView->model()->rowCount(), 1);
        QCOMPARE(window.currentSourceIdentity(), path);
    }

    void keepsTheRenderedSessionWhenAnotherFileCannotOpen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("current.264"), QByteArray::fromHex("00000141"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* treeView =
            window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(rawView != nullptr);
        QVERIFY(treeView != nullptr);
        const int treeRows = treeView->model()->rowCount();

        QVERIFY(!window.openMediaSource(directory.filePath(QStringLiteral("missing.264")),
                                        &errorMessage));

        QVERIFY(!errorMessage.isEmpty());
        QCOMPARE(window.currentSourceIdentity(), path);
        QCOMPARE(treeView->model()->rowCount(), treeRows);
        QCOMPARE(rawView->model()
                     ->data(rawView->model()->index(0, RawDataModel::FirstByte + 3))
                     .toString(),
                 QStringLiteral("41"));
    }

    void switchesRawDisplayModeThroughTheViewControls() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("mode.264"), QByteArray::fromHex("00000165"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        QVERIFY(rawView != nullptr);
        auto* binaryButton =
            rawView->findChild<QToolButton*>(QStringLiteral("binaryModeButton"));
        QVERIFY(binaryButton != nullptr);

        binaryButton->click();

        QCOMPARE(rawView->model()->displayMode(), RawDisplayMode::Binary);
        QCOMPARE(rawView->model()
                     ->data(rawView->model()->index(0, RawDataModel::FirstByte + 3))
                     .toString(),
                 QStringLiteral("01100101"));
    }

    void keepsRawBytesVisibleForATruncatedNalUnit() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("truncated.264"), QByteArray::fromHex("000001"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        QString errorMessage;

        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));

        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* treeView =
            window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(rawView != nullptr);
        QVERIFY(treeView != nullptr);
        QCOMPARE(rawView->model()->rowCount(), 1);
        QCOMPARE(treeView->model()->rowCount(), 1);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("partial")));
    }

    void keepsUnrecognizedBytesVisibleWithAnInvalidAnalysis() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("unknown.bin"), QByteArray::fromHex("112233"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        QString errorMessage;

        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));

        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* treeView =
            window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(rawView != nullptr);
        QVERIFY(treeView != nullptr);
        QCOMPARE(rawView->model()
                     ->data(rawView->model()->index(0, RawDataModel::FirstByte))
                     .toString(),
                 QStringLiteral("11"));
        QCOMPARE(treeView->model()->rowCount(), 0);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("partial")));
    }
};

QTEST_MAIN(MainWindowTest)

#include "main_window_test.moc"
