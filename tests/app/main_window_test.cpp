#include "analysis_tree_model.h"
#include "diagnostics_summary_dock.h"
#include "format_override_dialog.h"
#include "main_window.h"
#include "raw_data_model.h"
#include "raw_data_view.h"
#include "timeline_table_model.h"

#include <streamview/core/analysis_model.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStatusBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>
#include <QWidget>

#include <optional>

using streamview::app::AnalysisSessionCacheOptions;
using streamview::app::AnalysisTreeModel;
using streamview::app::DiagnosticsSummaryDock;
using streamview::app::FormatOverrideDialog;
using streamview::app::MainWindow;
using streamview::app::Language;
using streamview::app::LocalizationManager;
using streamview::app::RawDataModel;
using streamview::app::RawDataView;
using streamview::app::RawDisplayMode;
using streamview::app::ThemeManager;
using streamview::app::ThemeMode;
using streamview::app::TimelineTableModel;

namespace {

class DirectSqliteConnection final {
public:
    explicit DirectSqliteConnection(const QString& path)
        : name_(QStringLiteral("streamview-main-window-test-%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))),
          database_(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_)) {
        database_.setDatabaseName(path);
        database_.open();
    }

    ~DirectSqliteConnection() {
        database_.close();
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }

    DirectSqliteConnection(const DirectSqliteConnection&) = delete;
    DirectSqliteConnection& operator=(const DirectSqliteConnection&) = delete;

    [[nodiscard]] bool isOpen() const noexcept { return database_.isOpen(); }
    [[nodiscard]] QString errorMessage() const { return database_.lastError().text(); }

    [[nodiscard]] std::optional<qlonglong> queryInteger(const QString& statement,
                                                        QString* errorMessage = nullptr) {
        QSqlQuery query(database_);
        if (!query.exec(statement) || !query.next()) {
            if (errorMessage != nullptr) {
                *errorMessage = query.lastError().text();
            }
            return std::nullopt;
        }
        bool converted = false;
        const qlonglong value = query.value(0).toLongLong(&converted);
        return converted ? std::optional<qlonglong>(value) : std::nullopt;
    }

private:
    QString name_;
    QSqlDatabase database_;
};

QString writeFixture(QTemporaryDir& directory, const QString& name, const QByteArray& bytes) {
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

QByteArray makeAmbiguousMp4Bytes() {
    constexpr quint32 firstBoxSize = 0x0105U;
    QByteArray bytes;
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x01));
    bytes.append(char(0x05));
    bytes.append("free", 4);
    for (quint32 i = 0; i < firstBoxSize - 8U; ++i) {
        if (i == 100U) {
            bytes.append(char(0x00));
            bytes.append(char(0x00));
            bytes.append(char(0x01));
            bytes.append(char(0x67));
            i += 3U;
            continue;
        }
        bytes.append(char(0xEE));
    }
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x18));
    bytes.append("ftyp", 4);
    bytes.append(QByteArray(16, '\0'));
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x00));
    bytes.append(char(0x28));
    bytes.append("mdat", 4);
    bytes.append(QByteArray(32, '3'));
    return bytes;
}

QModelIndex findIndexByName(const QAbstractItemModel& model,
                            const QString& name,
                            const QModelIndex& parent = {}) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (model.data(index).toString() == name) {
            return index;
        }
        const QModelIndex descendant = findIndexByName(model, name, index);
        if (descendant.isValid()) {
            return descendant;
        }
    }
    return {};
}

QRect geometryInWindow(const QWidget& widget, const QWidget& window) {
    return {widget.mapTo(&window, QPoint{}), widget.size()};
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

    void publishesAnalysisBatchesAfterTheFirstVisibleBatch() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("many.264"), QByteArray::fromHex("0000016c0000014c"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        QString errorMessage;

        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));
        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(treeView != nullptr);
        QCOMPARE(treeView->model()->rowCount(), 1);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Analyzing")));

        const QModelIndex firstNode = treeView->model()->index(0, 0);
        QVERIFY(firstNode.isValid());
        treeView->setCurrentIndex(firstNode);
        const QModelIndex heldIndex = treeView->currentIndex();

        QTRY_COMPARE(treeView->model()->rowCount(), 2);
        QVERIFY(heldIndex.isValid());
        QCOMPARE(treeView->currentIndex(), heldIndex);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Analysis complete")));
    }

    void keepsTheRenderedSessionWhenAnotherFileCannotOpen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("current.264"), QByteArray::fromHex("0000014c"));
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
        const QModelIndex nalUnitType =
            findIndexByName(*treeView->model(), QStringLiteral("nal_unit_type"));
        QVERIFY(nalUnitType.isValid());
        treeView->setCurrentIndex(nalUnitType);
        const QModelIndex fourthByte =
            rawView->model()->index(0, RawDataModel::FirstByte + 3);
        QCOMPARE(rawView->model()->data(fourthByte, RawDataModel::SelectedBitsRole).toUInt(),
                 0x1FU);

        QVERIFY(!window.openMediaSource(directory.filePath(QStringLiteral("missing.264")),
                                        &errorMessage));

        QVERIFY(!errorMessage.isEmpty());
        QCOMPARE(window.currentSourceIdentity(), path);
        QCOMPARE(treeView->model()->rowCount(), treeRows);
        QCOMPARE(rawView->model()
                     ->data(rawView->model()->index(0, RawDataModel::FirstByte + 3))
                     .toString(),
                 QStringLiteral("4C"));
        QCOMPARE(rawView->model()->data(fourthByte, RawDataModel::SelectedBitsRole).toUInt(),
                 0x1FU);
        QCOMPARE(treeView->currentIndex(), nalUnitType);
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

    void highlightsExactSourceBitsWhenAFieldIsSelected() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("selection.264"), QByteArray::fromHex("00000165"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* treeView =
            window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(rawView != nullptr);
        QVERIFY(treeView != nullptr);
        const QModelIndex nalUnitType =
            findIndexByName(*treeView->model(), QStringLiteral("nal_unit_type"));
        QVERIFY(nalUnitType.isValid());

        treeView->setCurrentIndex(nalUnitType);

        QCOMPARE(rawView->model()
                     ->data(rawView->model()->index(0, RawDataModel::FirstByte + 3),
                            RawDataModel::SelectedBitsRole)
                     .toUInt(),
                 0x1FU);
    }

    void presentsSelectedFieldMetadataInTheInspector() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("inspector.264"), QByteArray::fromHex("00000165"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));

        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* inspector = window.findChild<QWidget*>(QStringLiteral("fieldInspector"));
        QVERIFY(treeView != nullptr);
        QVERIFY(inspector != nullptr);
        const QModelIndex field =
            findIndexByName(*treeView->model(), QStringLiteral("nal_unit_type"));
        QVERIFY(field.isValid());
        treeView->setCurrentIndex(field);

        auto* name = inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorName"));
        auto* value = inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorValue"));
        auto* type = inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorType"));
        auto* width = inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorWidth"));
        auto* spans = inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorSourceSpans"));
        auto* logical =
            inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorLogicalRange"));
        auto* description =
            inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorDescription"));
        auto* specification =
            inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorSpecification"));
        auto* diagnostics =
            inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorDiagnostics"));
        QVERIFY(name != nullptr);
        QVERIFY(value != nullptr);
        QVERIFY(type != nullptr);
        QVERIFY(width != nullptr);
        QVERIFY(spans != nullptr);
        QVERIFY(logical != nullptr);
        QVERIFY(description != nullptr);
        QVERIFY(specification != nullptr);
        QVERIFY(diagnostics != nullptr);

        QCOMPARE(name->text(), QStringLiteral("nal_unit_type"));
        QCOMPARE(value->text(), QStringLiteral("5"));
        QCOMPARE(type->text(), QStringLiteral("bits"));
        QCOMPARE(width->text(), QStringLiteral("5 bits"));
        QCOMPARE(spans->text(), QStringLiteral("[27, 32)"));
        QVERIFY(logical->text().contains(QStringLiteral("[3, 8)")));
        QVERIFY(!description->text().isEmpty());
        QVERIFY(specification->text().contains(QStringLiteral("ITU-T H.264")));
        QCOMPARE(diagnostics->text(), QStringLiteral("-"));
    }

    void locatesTheMostSpecificAnalysisNodeForARawSourceBit() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("reverse-selection.264"),
            QByteArray::fromHex("00000165"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        window.resize(1280, 800);
        window.show();
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));
        QCoreApplication::processEvents();
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* table = window.findChild<QTableView*>(QStringLiteral("rawDataTable"));
        auto* treeView =
            window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(rawView != nullptr);
        QVERIFY(table != nullptr);
        QVERIFY(treeView != nullptr);
        const QModelIndex fourthByte =
            rawView->model()->index(0, RawDataModel::FirstByte + 3);
        const QRect cell = table->visualRect(fourthByte);
        QVERIFY(cell.isValid());
        constexpr int selectedBitInByte = 4;
        const QPoint clickPoint(
            cell.left() + ((selectedBitInByte * 2 + 1) * cell.width()) / 16,
            cell.center().y());

        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, clickPoint);

        QVERIFY(treeView->currentIndex().isValid());
        QCOMPARE(treeView->currentIndex().data().toString(), QStringLiteral("nal_unit_type"));
        QCOMPARE(rawView->model()
                     ->data(fourthByte, RawDataModel::SelectedBitsRole)
                     .toUInt(),
                 0x08U);
        auto* inspector = window.findChild<QWidget*>(QStringLiteral("fieldInspector"));
        QVERIFY(inspector != nullptr);
        QCOMPARE(inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorValue"))->text(),
                 QStringLiteral("5"));
    }

    void keepsAnUnmatchedRawBitSelectedWhileClearingTheTreeSelection() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("leading-byte.264"),
            QByteArray::fromHex("FF00000165"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        window.resize(1280, 800);
        window.show();
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));
        QCoreApplication::processEvents();
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* table = window.findChild<QTableView*>(QStringLiteral("rawDataTable"));
        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(rawView != nullptr);
        QVERIFY(table != nullptr);
        QVERIFY(treeView != nullptr);
        const QModelIndex nalUnitType =
            findIndexByName(*treeView->model(), QStringLiteral("nal_unit_type"));
        QVERIFY(nalUnitType.isValid());
        treeView->setCurrentIndex(nalUnitType);

        const QModelIndex leadingByte =
            rawView->model()->index(0, RawDataModel::FirstByte);
        const QRect cell = table->visualRect(leadingByte);
        QVERIFY(cell.isValid());
        const QPoint clickPoint(cell.left() + cell.width() / 16, cell.center().y());
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, clickPoint);

        QVERIFY(!treeView->currentIndex().isValid());
        QCOMPARE(rawView->model()
                     ->data(leadingByte, RawDataModel::SelectedBitsRole)
                     .toUInt(),
                 0x80U);
    }

    void clearsSelectionWhenAValidSessionIsReplaced() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstPath = writeFixture(
            directory, QStringLiteral("first.264"), QByteArray::fromHex("00000165"));
        const QString secondPath = writeFixture(
            directory, QStringLiteral("second.264"), QByteArray::fromHex("0000014c"));
        QVERIFY(!firstPath.isEmpty());
        QVERIFY(!secondPath.isEmpty());
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(firstPath, &errorMessage), qPrintable(errorMessage));
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(rawView != nullptr);
        QVERIFY(treeView != nullptr);
        const QModelIndex firstNalUnitType =
            findIndexByName(*treeView->model(), QStringLiteral("nal_unit_type"));
        QVERIFY(firstNalUnitType.isValid());
        treeView->setCurrentIndex(firstNalUnitType);
        QCOMPARE(rawView->model()
                     ->data(rawView->model()->index(0, RawDataModel::FirstByte + 3),
                            RawDataModel::SelectedBitsRole)
                     .toUInt(),
                 0x1FU);

        QVERIFY2(window.openMediaSource(secondPath, &errorMessage), qPrintable(errorMessage));

        QCOMPARE(window.currentSourceIdentity(), secondPath);
        QVERIFY(!treeView->currentIndex().isValid());
        QCOMPARE(rawView->model()
                     ->data(rawView->model()->index(0, RawDataModel::FirstByte + 3),
                            RawDataModel::SelectedBitsRole)
                     .toUInt(),
                 0U);
        auto* inspector = window.findChild<QWidget*>(QStringLiteral("fieldInspector"));
        QVERIFY(inspector != nullptr);
        QCOMPARE(inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorValue"))->text(),
                 QStringLiteral("-"));
    }

    void replacesTheProductionCacheOwnerBeforeEnablingTheNextSession() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstPath = writeFixture(
            directory, QStringLiteral("first-cache.264"), QByteArray::fromHex("00000165"));
        const QString secondPath = writeFixture(
            directory, QStringLiteral("second-cache.264"), QByteArray::fromHex("0000014c"));
        const QString cachePath = directory.filePath(QStringLiteral("analysis-cache.sqlite"));
        QVERIFY(!firstPath.isEmpty());
        QVERIFY(!secondPath.isEmpty());

        AnalysisSessionCacheOptions cacheOptions;
        cacheOptions.databasePath = cachePath;
        QString errorMessage;
        {
            MainWindow window(cacheOptions);
            QVERIFY2(window.openMediaSource(firstPath, &errorMessage), qPrintable(errorMessage));
            QVERIFY2(window.openMediaSource(secondPath, &errorMessage), qPrintable(errorMessage));
            QCOMPARE(window.currentSourceIdentity(), secondPath);
        }

        DirectSqliteConnection database(cachePath);
        QVERIFY2(database.isOpen(), qPrintable(database.errorMessage()));
        const auto namespaceCount = database.queryInteger(
            QStringLiteral("SELECT COUNT(*) FROM cache_namespaces"), &errorMessage);
        QVERIFY2(namespaceCount.has_value(), qPrintable(errorMessage));
        QCOMPARE(*namespaceCount, qlonglong{2});
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

        const QModelIndex header =
            findIndexByName(*treeView->model(), QStringLiteral("NalUnitHeader"));
        QVERIFY(header.isValid());
        treeView->setCurrentIndex(header);
        auto* inspector = window.findChild<QWidget*>(QStringLiteral("fieldInspector"));
        QVERIFY(inspector != nullptr);
        auto* diagnostics =
            inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorDiagnostics"));
        QVERIFY(diagnostics != nullptr);
        QVERIFY(diagnostics->text().contains(QStringLiteral("truncated-source")));
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

    void navigatesIntoAacAscViaDoubleClickAndReturnsViaBackButton() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        auto* breadcrumbLabel = window.findChild<QLabel*>(QStringLiteral("navigationBreadcrumbLabel"));
        auto* inspector = window.findChild<QWidget*>(QStringLiteral("fieldInspector"));

        QVERIFY(treeView != nullptr);
        QVERIFY(rawView != nullptr);
        QVERIFY(backButton != nullptr);
        QVERIFY(breadcrumbLabel != nullptr);
        QVERIFY(inspector != nullptr);

        // Initially at root
        QVERIFY(!backButton->isEnabled());
        QCOMPARE(backButton->toolTip(), QStringLiteral("Return to parent"));
        QCOMPARE(backButton->accessibleName(), QStringLiteral("Return to parent format"));

        QModelIndex ascIndex;
        QTRY_VERIFY((ascIndex = findIndexByName(*treeView->model(), QStringLiteral("asc_bytes1"))).isValid());

        // Double click on asc_bytes1
        Q_EMIT treeView->doubleClicked(ascIndex);

        // Sub-format entered
        QVERIFY(backButton->isEnabled());
        QVERIFY(breadcrumbLabel->text().contains(QStringLiteral("audio.aac.asc")));

        // Analysis tree model switched to ASC tree
        const QModelIndex aotIndex = findIndexByName(*treeView->model(), QStringLiteral("audio_object_type"));
        QVERIFY(aotIndex.isValid());
        treeView->setCurrentIndex(aotIndex);

        // Field inspector updated
        auto* inspectorValue = inspector->findChild<QLabel*>(QStringLiteral("fieldInspectorValue"));
        QVERIFY(inspectorValue != nullptr);
        QCOMPARE(inspectorValue->text(), QStringLiteral("2"));

        // Raw highlight covers audio_object_type (5 bits: 0xF8) at byte 146
        const auto selectedBits = rawView->model()->data(
            rawView->model()->index(146 / 16, RawDataModel::FirstByte + (146 % 16)),
            RawDataModel::SelectedBitsRole).toUInt();
        QCOMPARE(selectedBits, 0xF8U);

        // Click back button to return to root
        backButton->click();

        // Restored to root MP4
        QVERIFY(!backButton->isEnabled());
        const QModelIndex restoredAscIndex = treeView->currentIndex();
        QVERIFY(restoredAscIndex.isValid());
        QCOMPARE(treeView->model()->data(restoredAscIndex).toString(), QStringLiteral("asc_bytes1"));
    }

    void navigatesIntoH264SpsAndPpsWithContextSharingViaKeyboard() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_avc1_avcC.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        auto* breadcrumbLabel = window.findChild<QLabel*>(QStringLiteral("navigationBreadcrumbLabel"));
        QVERIFY(treeView != nullptr);
        QVERIFY(backButton != nullptr);
        QVERIFY(breadcrumbLabel != nullptr);

        // 1. Find and enter SPS NAL via Enter key
        QModelIndex spsIndex;
        QTRY_VERIFY((spsIndex = findIndexByName(*treeView->model(), QStringLiteral("sequenceParameterSetNALUnit[0]"))).isValid());
        treeView->setCurrentIndex(spsIndex);
        QTest::keyClick(treeView, Qt::Key_Return);

        // Child tree has SequenceParameterSetRbsp
        QVERIFY(backButton->isEnabled());
        QVERIFY(breadcrumbLabel->text().contains(QStringLiteral("video.h264.nal")));
        const QModelIndex spsRbspIndex = findIndexByName(*treeView->model(), QStringLiteral("SequenceParameterSetRbsp"));
        QVERIFY(spsRbspIndex.isValid());

        // The RBSP structure crosses two excluded emulation-prevention bytes.
        // Selecting it must preserve the forwarded disjoint source spans.
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        QVERIFY(rawView != nullptr);
        const auto selectedBitsAt = [rawView](int byteOffset) {
            return rawView->model()
                ->data(rawView->model()->index(byteOffset / 16,
                                               RawDataModel::FirstByte + (byteOffset % 16)),
                       RawDataModel::SelectedBitsRole)
                .toUInt();
        };
        const QModelIndex numUnitsInTick =
            findIndexByName(*treeView->model(), QStringLiteral("num_units_in_tick"));
        QVERIFY(numUnitsInTick.isValid());
        treeView->setCurrentIndex(numUnitsInTick);
        QCOMPARE(selectedBitsAt(0xc0), 0xFFU);
        QCOMPARE(selectedBitsAt(0xc1), 0U);
        QCOMPARE(selectedBitsAt(0xc2), 0xFFU);

        const QModelIndex timeScale =
            findIndexByName(*treeView->model(), QStringLiteral("time_scale"));
        QVERIFY(timeScale.isValid());
        treeView->setCurrentIndex(timeScale);
        QCOMPARE(selectedBitsAt(0xc5), 0xFFU);
        QCOMPARE(selectedBitsAt(0xc6), 0U);
        QCOMPARE(selectedBitsAt(0xc7), 0xFFU);

        // 2. Return to MP4 root
        backButton->click();
        QVERIFY(!backButton->isEnabled());
        QCOMPARE(treeView->model()->data(treeView->currentIndex()).toString(), QStringLiteral("sequenceParameterSetNALUnit[0]"));

        // 3. Find and enter PPS NAL via Enter key (imports SPS context)
        const QModelIndex ppsIndex = findIndexByName(*treeView->model(), QStringLiteral("pictureParameterSetNALUnit[0]"));
        QVERIFY(ppsIndex.isValid());
        treeView->setCurrentIndex(ppsIndex);
        QTest::keyClick(treeView, Qt::Key_Enter);

        // Child tree has PictureParameterSetRbsp
        QVERIFY(backButton->isEnabled());
        const QModelIndex ppsRbspIndex = findIndexByName(*treeView->model(), QStringLiteral("PictureParameterSetRbsp"));
        QVERIFY(ppsRbspIndex.isValid());

        // 4. Return to MP4 root
        backButton->click();
        QVERIFY(!backButton->isEnabled());
        QCOMPARE(treeView->model()->data(treeView->currentIndex()).toString(), QStringLiteral("pictureParameterSetNALUnit[0]"));
    }

    void navigationFailureLeavesStateUnchanged() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        auto* breadcrumbLabel = window.findChild<QLabel*>(QStringLiteral("navigationBreadcrumbLabel"));
        QVERIFY(treeView != nullptr);
        QVERIFY(backButton != nullptr);
        QVERIFY(breadcrumbLabel != nullptr);

        // Find a node without target format (e.g. size)
        QModelIndex sizeIndex;
        QTRY_VERIFY((sizeIndex = findIndexByName(*treeView->model(), QStringLiteral("size"))).isValid());
        treeView->setCurrentIndex(sizeIndex);

        // Double click on non-target node
        Q_EMIT treeView->doubleClicked(sizeIndex);

        // State remains at root
        QVERIFY(!backButton->isEnabled());
        QCOMPARE(treeView->currentIndex(), sizeIndex);
    }

    void rootReturnIsDeterministicNoOp() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        QVERIFY(backButton != nullptr);
        QVERIFY(!backButton->isEnabled());

        // Calling back button at root is no-op
        backButton->click();
        QVERIFY(!backButton->isEnabled());
    }

    void supportsRepeatedEnterAndReturnCyclesWithoutStaleIndices() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        QVERIFY(treeView != nullptr);
        QVERIFY(backButton != nullptr);

        for (int cycle = 0; cycle < 3; ++cycle) {
            QModelIndex ascIndex;
            QTRY_VERIFY((ascIndex = findIndexByName(*treeView->model(), QStringLiteral("asc_bytes1"))).isValid());
            Q_EMIT treeView->doubleClicked(ascIndex);
            QVERIFY(backButton->isEnabled());

            const QModelIndex aotIndex = findIndexByName(*treeView->model(), QStringLiteral("audio_object_type"));
            QVERIFY(aotIndex.isValid());

            backButton->click();
            QVERIFY(!backButton->isEnabled());
        }
    }

    void bidirectionalCoordinateSelectionUsesActiveTree() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        QVERIFY(treeView != nullptr);
        QVERIFY(rawView != nullptr);
        QVERIFY(backButton != nullptr);

        // Enter ASC
        QModelIndex ascIndex;
        QTRY_VERIFY((ascIndex = findIndexByName(*treeView->model(), QStringLiteral("asc_bytes1"))).isValid());
        Q_EMIT treeView->doubleClicked(ascIndex);
        QVERIFY(backButton->isEnabled());

        // 1. Click bit in raw view at bit offset 1173 (inside ASC sampling_frequency_index)
        Q_EMIT rawView->sourceBitSelected(1173);

        const QModelIndex currentIndex = treeView->currentIndex();
        QVERIFY(currentIndex.isValid());
        QCOMPARE(treeView->model()->data(currentIndex).toString(), QStringLiteral("sampling_frequency_index"));

        // 2. Click bit in raw view outside child tree (e.g. bit 0 of MP4)
        Q_EMIT rawView->sourceBitSelected(0);
        // Tree selection is cleared because bit 0 is not in the active child tree
        QVERIFY(!treeView->currentIndex().isValid());
        // Raw view still has selection
        QCOMPARE(rawView->model()->data(rawView->model()->index(0, RawDataModel::FirstByte),
                                       RawDataModel::SelectedBitsRole).toUInt(), 0x80U);
    }

    void openingNewFileResetsNavigationState() {
        const QString firstPath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        const QString secondPath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_avc1_avcC.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(firstPath, &errorMessage), qPrintable(errorMessage));

        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        auto* breadcrumbLabel = window.findChild<QLabel*>(QStringLiteral("navigationBreadcrumbLabel"));
        QVERIFY(treeView != nullptr);
        QVERIFY(backButton != nullptr);
        QVERIFY(breadcrumbLabel != nullptr);

        // Enter ASC
        QModelIndex ascIndex;
        QTRY_VERIFY((ascIndex = findIndexByName(*treeView->model(), QStringLiteral("asc_bytes1"))).isValid());
        Q_EMIT treeView->doubleClicked(ascIndex);
        QVERIFY(backButton->isEnabled());

        // Open second file
        QVERIFY2(window.openMediaSource(secondPath, &errorMessage), qPrintable(errorMessage));

        // Navigation state reset
        QVERIFY(!backButton->isEnabled());
        QCOMPARE(window.currentSourceIdentity(), secondPath);
    }

    void pendingRootAnalysisDoesNotReplaceActiveChildTree() {
        const QString fixturePath = QStringLiteral(
            STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        QVERIFY(treeView != nullptr);
        QVERIFY(backButton != nullptr);

        bool entryRequested = false;
        connect(treeView->model(), &QAbstractItemModel::rowsInserted, &window,
                [&entryRequested, treeView] {
                    if (entryRequested) {
                        return;
                    }
                    const QModelIndex ascIndex =
                        findIndexByName(*treeView->model(), QStringLiteral("asc_bytes1"));
                    if (ascIndex.isValid()) {
                        entryRequested = true;
                        Q_EMIT treeView->doubleClicked(ascIndex);
                    }
                },
                Qt::QueuedConnection);

        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        QTRY_VERIFY(entryRequested);
        QTRY_VERIFY(backButton->isEnabled());
        QTRY_VERIFY(findIndexByName(*treeView->model(),
                                    QStringLiteral("audio_object_type")).isValid());
        QTest::qWait(50);

        QVERIFY(findIndexByName(*treeView->model(), QStringLiteral("audio_object_type")).isValid());
        QVERIFY(!findIndexByName(*treeView->model(), QStringLiteral("asc_bytes1")).isValid());
    }

    void layoutAndVisualFitAtDifferentResolutions() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        auto* breadcrumbLabel = window.findChild<QLabel*>(QStringLiteral("navigationBreadcrumbLabel"));
        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* inspector = window.findChild<QWidget*>(QStringLiteral("fieldInspector"));
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        auto* navBar = window.findChild<QWidget*>(QStringLiteral("analysisTreeNavBar"));
        auto* analysisDock = window.findChild<QDockWidget*>(QStringLiteral("analysisTreeDock"));
        auto* inspectorDock = window.findChild<QDockWidget*>(QStringLiteral("fieldInspectorDock"));

        QVERIFY(backButton != nullptr);
        QVERIFY(breadcrumbLabel != nullptr);
        QVERIFY(treeView != nullptr);
        QVERIFY(inspector != nullptr);
        QVERIFY(rawView != nullptr);
        QVERIFY(navBar != nullptr);
        QVERIFY(analysisDock != nullptr);
        QVERIFY(inspectorDock != nullptr);

        QModelIndex ascIndex;
        QTRY_VERIFY((ascIndex = findIndexByName(*treeView->model(),
                                               QStringLiteral("asc_bytes1"))).isValid());
        Q_EMIT treeView->doubleClicked(ascIndex);
        QVERIFY(backButton->isEnabled());

        const auto verifyLayout = [&] {
            const QRect windowRect = window.rect();
            const QRect navRect = geometryInWindow(*navBar, window);
            const QRect treeRect = geometryInWindow(*treeView, window);
            const QRect rawRect = geometryInWindow(*rawView, window);
            const QRect analysisDockRect = geometryInWindow(*analysisDock, window);
            const QRect inspectorDockRect = geometryInWindow(*inspectorDock, window);

            QVERIFY(windowRect.contains(navRect));
            QVERIFY(windowRect.contains(treeRect));
            QVERIFY(windowRect.contains(rawRect));
            QVERIFY(!navRect.isEmpty());
            QVERIFY(!treeRect.isEmpty());
            QVERIFY(!rawRect.isEmpty());
            QVERIFY(!navRect.intersects(treeRect));
            QVERIFY(!analysisDockRect.intersects(rawRect));
            QVERIFY(!analysisDockRect.intersects(inspectorDockRect));
            QVERIFY(!rawRect.intersects(inspectorDockRect));
            QVERIFY(backButton->geometry().right() < breadcrumbLabel->geometry().left());
            QVERIFY(breadcrumbLabel->fontMetrics().horizontalAdvance(breadcrumbLabel->text()) <=
                    breadcrumbLabel->contentsRect().width());
        };

        // Test 900x600
        window.resize(900, 600);
        window.show();
        QTest::qWait(50);
        QVERIFY(backButton->isVisible());
        QVERIFY(breadcrumbLabel->isVisible());
        QVERIFY(treeView->isVisible());
        QVERIFY(inspector->isVisible());
        QVERIFY(rawView->isVisible());
        verifyLayout();

        // Test 1280x800
        window.resize(1280, 800);
        QTest::qWait(50);
        QVERIFY(backButton->isVisible());
        QVERIFY(breadcrumbLabel->isVisible());
        QVERIFY(treeView->isVisible());
        QVERIFY(inspector->isVisible());
        QVERIFY(rawView->isVisible());
        verifyLayout();
    }

    void timelineDockAndAmbiguitySurfacePresent() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("valid.264"), QByteArray::fromHex("00000165"));
        QVERIFY(!path.isEmpty());
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));

        auto* timelineDock = window.findChild<QDockWidget*>(QStringLiteral("timelineDock"));
        auto* trackCombo = window.findChild<QComboBox*>(QStringLiteral("timelineTrackComboBox"));
        auto* statusLabel = window.findChild<QLabel*>(QStringLiteral("timelineStatusLabel"));
        auto* prevBtn = window.findChild<QToolButton*>(QStringLiteral("timelinePrevPageButton"));
        auto* nextBtn = window.findChild<QToolButton*>(QStringLiteral("timelineNextPageButton"));
        auto* pageLabel = window.findChild<QLabel*>(QStringLiteral("timelinePageLabel"));
        auto* tableView = window.findChild<QTableView*>(QStringLiteral("timelineTableView"));
        auto* ambiguityLabel = window.findChild<QLabel*>(QStringLiteral("formatAmbiguityLabel"));

        QVERIFY(timelineDock != nullptr);
        QVERIFY(trackCombo != nullptr);
        QVERIFY(statusLabel != nullptr);
        QVERIFY(prevBtn != nullptr);
        QVERIFY(nextBtn != nullptr);
        QVERIFY(pageLabel != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(ambiguityLabel != nullptr);

        // Elementary stream has no tracks, so status indicates container tracks unavailable
        QTRY_VERIFY(!statusLabel->isHidden());
        QVERIFY(statusLabel->text().contains(QStringLiteral("Container tracks unavailable")));
        QVERIFY(!prevBtn->isEnabled());
        QVERIFY(!nextBtn->isEnabled());
        QCOMPARE(trackCombo->count(), 0);
        QCOMPARE(tableView->model()->rowCount(), 0);

        // Ambiguity label is hidden for clean elementary stream
        QVERIFY(ambiguityLabel->isHidden());
    }

    void surfacesFormatAmbiguityWarningWhenDetectionIsAmbiguous() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        constexpr quint32 firstBoxSize = 0x0105U;
        QByteArray bytes;
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x01));
        bytes.append(char(0x05));
        bytes.append("free", 4);
        for (quint32 i = 0; i < firstBoxSize - 8U; ++i) {
            if (i == 100U) {
                bytes.append(char(0x00));
                bytes.append(char(0x00));
                bytes.append(char(0x01));
                bytes.append(char(0x67));
                i += 3U;
                continue;
            }
            bytes.append(char(0xEE));
        }
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x18));
        bytes.append("ftyp", 4);
        bytes.append(QByteArray(16, '\0'));
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x28));
        bytes.append("mdat", 4);
        bytes.append(QByteArray(32, '3'));

        const QString path = writeFixture(directory, QStringLiteral("ambiguous.mp4"), bytes);
        QVERIFY(!path.isEmpty());

        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));

        auto* ambiguityLabel = window.findChild<QLabel*>(QStringLiteral("formatAmbiguityLabel"));
        QVERIFY(ambiguityLabel != nullptr);
        QTRY_VERIFY(!ambiguityLabel->isHidden());
        QVERIFY(ambiguityLabel->text().contains(QStringLiteral("Ambiguous format")));
    }

    void rendersTruncatedTrackListWarning() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QByteArray bytes;
        // Box 1: ftyp (24 bytes)
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x18));
        bytes.append("ftyp", 4);
        bytes.append(QByteArray(16, '\0'));

        // Box 2: free (16 bytes)
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x10));
        bytes.append("free", 4);
        bytes.append(QByteArray(8, '\0'));

        // Box 3: moov (32 bytes)
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x20));
        bytes.append("moov", 4);
        bytes.append(QByteArray(24, '\0'));

        // Box 4: mdat (declares 5000 bytes, but truncated after 20 bytes)
        bytes.append(char(0x00));
        bytes.append(char(0x00));
        bytes.append(char(0x13));
        bytes.append(char(0x88));
        bytes.append("mdat", 4);
        bytes.append(QByteArray(12, '\0'));

        const QString path = writeFixture(directory, QStringLiteral("truncated.mp4"), bytes);
        QVERIFY(!path.isEmpty());

        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));

        auto* statusLabel = window.findChild<QLabel*>(QStringLiteral("timelineStatusLabel"));
        QVERIFY(statusLabel != nullptr);
        QTRY_VERIFY(!statusLabel->isHidden() &&
                    statusLabel->text().contains(
                        QStringLiteral("File is truncated; track list may be incomplete")));
    }

    void populatesTracksAndTimelineTable() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_avc_multi_nal.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* trackCombo = window.findChild<QComboBox*>(QStringLiteral("timelineTrackComboBox"));
        auto* tableView = window.findChild<QTableView*>(QStringLiteral("timelineTableView"));
        auto* pageLabel = window.findChild<QLabel*>(QStringLiteral("timelinePageLabel"));
        QVERIFY(trackCombo != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(pageLabel != nullptr);

        QTRY_COMPARE(trackCombo->count(), 1);
        QVERIFY(trackCombo->currentText().contains(QStringLiteral("Track 1")));
        QVERIFY(trackCombo->currentText().contains(QStringLiteral("3 samples")));

        auto* model = tableView->model();
        QVERIFY(model != nullptr);
        QCOMPARE(model->rowCount(), 3);
        QCOMPARE(model->columnCount(), TimelineTableModel::ColumnCount);

        QCOMPARE(model->headerData(TimelineTableModel::SampleIndex, Qt::Horizontal).toString(),
                 QStringLiteral("Sample #"));
        QCOMPARE(model->headerData(TimelineTableModel::SyncType, Qt::Horizontal).toString(),
                 QStringLiteral("Type"));
        QCOMPARE(model->headerData(TimelineTableModel::Dts, Qt::Horizontal).toString(),
                 QStringLiteral("DTS"));
        QCOMPARE(model->headerData(TimelineTableModel::Pts, Qt::Horizontal).toString(),
                 QStringLiteral("PTS"));
        QCOMPARE(model->headerData(TimelineTableModel::Duration, Qt::Horizontal).toString(),
                 QStringLiteral("Duration"));
        QCOMPARE(model->headerData(TimelineTableModel::SizeBytes, Qt::Horizontal).toString(),
                 QStringLiteral("Size (B)"));

        QCOMPARE(model->data(model->index(0, TimelineTableModel::SampleIndex)).toString(),
                 QStringLiteral("0"));
        QCOMPARE(model->data(model->index(0, TimelineTableModel::SyncType)).toString(),
                 QStringLiteral("[Sync]"));
        QCOMPARE(model->data(model->index(0, TimelineTableModel::Dts)).toString(),
                 QStringLiteral("0"));
        QCOMPARE(model->data(model->index(0, TimelineTableModel::Pts)).toString(),
                 QStringLiteral("0"));
        const quint64 size0 =
            model->data(model->index(0, TimelineTableModel::SizeBytes)).toULongLong();
        QVERIFY(size0 > 0);

        QCOMPARE(model->data(model->index(1, TimelineTableModel::SampleIndex)).toString(),
                 QStringLiteral("1"));
        QCOMPARE(model->data(model->index(1, TimelineTableModel::SyncType)).toString(),
                 QStringLiteral("-"));

        QVERIFY(pageLabel->text().contains(QStringLiteral("Page 1 of 1")));
        QVERIFY(pageLabel->text().contains(QStringLiteral("3")));
    }

    void selectingSampleRowHighlightsSourceBytesInRawDataView() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_avc_multi_nal.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* trackCombo = window.findChild<QComboBox*>(QStringLiteral("timelineTrackComboBox"));
        auto* tableView = window.findChild<QTableView*>(QStringLiteral("timelineTableView"));
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        QVERIFY(trackCombo != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(rawView != nullptr);

        QTRY_COMPARE(trackCombo->count(), 1);
        QCOMPARE(tableView->model()->rowCount(), 3);

        tableView->selectRow(0);
        QTRY_VERIFY([&] {
            const quint64 bitOffset0 =
                tableView->model()->data(tableView->model()->index(0, TimelineTableModel::BitOffset)).toULongLong();
            const quint64 byteIndex0 = bitOffset0 / 8U;
            const QModelIndex rawByte0 = rawView->model()->index(
                int(byteIndex0 / RawDataModel::ByteColumnCount),
                RawDataModel::FirstByte + int(byteIndex0 % RawDataModel::ByteColumnCount));
            return rawByte0.isValid() && rawView->model()->data(rawByte0, RawDataModel::SelectedBitsRole).toUInt() > 0;
        }());

        tableView->selectRow(1);
        QTRY_VERIFY([&] {
            const quint64 bitOffset1 =
                tableView->model()->data(tableView->model()->index(1, TimelineTableModel::BitOffset)).toULongLong();
            const quint64 byteIndex1 = bitOffset1 / 8U;
            const QModelIndex rawByte1 = rawView->model()->index(
                int(byteIndex1 / RawDataModel::ByteColumnCount),
                RawDataModel::FirstByte + int(byteIndex1 % RawDataModel::ByteColumnCount));
            return rawByte1.isValid() && rawView->model()->data(rawByte1, RawDataModel::SelectedBitsRole).toUInt() > 0;
        }());
    }

    void navigatesIntoSampleAndReturnsToContainer() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_avc_multi_nal.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* trackCombo = window.findChild<QComboBox*>(QStringLiteral("timelineTrackComboBox"));
        auto* tableView = window.findChild<QTableView*>(QStringLiteral("timelineTableView"));
        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* backButton = window.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
        auto* breadcrumbLabel = window.findChild<QLabel*>(QStringLiteral("navigationBreadcrumbLabel"));
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        QVERIFY(trackCombo != nullptr);
        QVERIFY(tableView != nullptr);
        QVERIFY(treeView != nullptr);
        QVERIFY(backButton != nullptr);
        QVERIFY(breadcrumbLabel != nullptr);
        QVERIFY(rawView != nullptr);

        QTRY_COMPARE(trackCombo->count(), 1);
        QCOMPARE(tableView->model()->rowCount(), 3);
        QVERIFY(!backButton->isEnabled());

        const QModelIndex sample0Index = tableView->model()->index(0, 0);
        Q_EMIT tableView->doubleClicked(sample0Index);

        QTRY_VERIFY(backButton->isEnabled());
        QVERIFY(breadcrumbLabel->text().contains(QStringLiteral("Track 1")));
        QVERIFY(breadcrumbLabel->text().contains(QStringLiteral("Sample #0 [Sync]")));

        QTRY_VERIFY(findIndexByName(*treeView->model(), QStringLiteral("NalUnitHeader")).isValid());

        backButton->click();

        QTRY_VERIFY(!backButton->isEnabled());
        QVERIFY(!breadcrumbLabel->text().contains(QStringLiteral("Sample #0")));
        QVERIFY(findIndexByName(*treeView->model(), QStringLiteral("major_brand")).isValid());
        QCOMPARE(tableView->currentIndex().row(), 0);

        const quint64 bitOffset0 =
            tableView->model()->data(tableView->model()->index(0, TimelineTableModel::BitOffset)).toULongLong();
        const quint64 byteIndex0 = bitOffset0 / 8U;
        const QModelIndex rawByte0 = rawView->model()->index(
            int(byteIndex0 / RawDataModel::ByteColumnCount),
            RawDataModel::FirstByte + int(byteIndex0 % RawDataModel::ByteColumnCount));
        QVERIFY(rawByte0.isValid());
        QVERIFY(rawView->model()->data(rawByte0, RawDataModel::SelectedBitsRole).toUInt() > 0);
    }

    void twoTracksSelectionSwitchesSampleList() {
        const QString fixturePath =
            QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_two_tracks.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        auto* trackCombo = window.findChild<QComboBox*>(QStringLiteral("timelineTrackComboBox"));
        auto* tableView = window.findChild<QTableView*>(QStringLiteral("timelineTableView"));
        QVERIFY(trackCombo != nullptr);
        QVERIFY(tableView != nullptr);

        QTRY_COMPARE(trackCombo->count(), 2);
        QVERIFY(trackCombo->itemText(0).contains(QStringLiteral("Track 1")));
        QVERIFY(trackCombo->itemText(1).contains(QStringLiteral("Track 2")));

        QCOMPARE(trackCombo->currentIndex(), 0);
        QCOMPARE(tableView->model()->rowCount(), 2);

        trackCombo->setCurrentIndex(1);

        QTRY_COMPARE(trackCombo->currentIndex(), 1);
        QCOMPARE(tableView->model()->rowCount(), 2);
    }

    void sessionSaveActionsEnabledOnlyWhenSessionLoaded() {
        MainWindow window;
        auto* saveAction = window.findChild<QAction*>(QStringLiteral("actionSaveSession"));
        auto* saveAsAction = window.findChild<QAction*>(QStringLiteral("actionSaveSessionAs"));
        auto* openSessionAction = window.findChild<QAction*>(QStringLiteral("actionOpenSession"));
        auto* exitAction = window.findChild<QAction*>(QStringLiteral("actionExit"));
        QVERIFY(saveAction != nullptr);
        QVERIFY(saveAsAction != nullptr);
        QVERIFY(openSessionAction != nullptr);
        QVERIFY(exitAction != nullptr);
        QVERIFY(!saveAction->isEnabled());
        QVERIFY(!saveAsAction->isEnabled());
        QVERIFY(openSessionAction->isEnabled());
        QVERIFY(exitAction->isEnabled());

        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        QVERIFY(saveAction->isEnabled());
        QVERIFY(saveAsAction->isEnabled());
    }

    void sessionDirtyStateTracksBookmarksAndAnnotations() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sessionPath = directory.filePath(QStringLiteral("dirty_track.svsession"));
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return sessionPath;
        });

        // 1. Initially clean after opening media
        QVERIFY(!window.isWindowModified());
        QVERIFY(window.windowTitle().contains(QStringLiteral("[*]")));

        // 2. Adding bookmark marks dirty
        window.addBookmark({.label = QStringLiteral("BM1"), .sourceBitOffset = 8});
        QVERIFY(window.isWindowModified());
        QCOMPARE(window.bookmarks().size(), 1);

        // 3. Saving resets dirty to clean
        QVERIFY(window.saveSession());
        QVERIFY(!window.isWindowModified());

        // 4. Removing bookmark marks dirty
        window.removeBookmark(0);
        QVERIFY(window.isWindowModified());
        QVERIFY(window.bookmarks().empty());

        // 5. Saving resets dirty to clean
        QVERIFY(window.saveSession());
        QVERIFY(!window.isWindowModified());

        // 6. Adding annotation marks dirty
        window.addAnnotation({.text = QStringLiteral("Note1"), .sourceBitOffset = 16, .bitLength = 8});
        QVERIFY(window.isWindowModified());
        QCOMPARE(window.annotations().size(), 1);

        // 7. Clearing annotations marks dirty
        window.clearAnnotations();
        QVERIFY(window.isWindowModified());
        QVERIFY(window.annotations().empty());

        // 8. Saving resets dirty to clean
        QVERIFY(window.saveSession());
        QVERIFY(!window.isWindowModified());

        // 9. Navigation/page interactions do not dirty session
        auto* rawView = window.findChild<RawDataView*>(QStringLiteral("rawDataView"));
        QVERIFY(rawView != nullptr);
        rawView->model()->setDisplayMode(RawDisplayMode::Binary);
        QVERIFY(!window.isWindowModified());
    }

    void sessionSaveAndSaveAsPersistence() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sessionPath = directory.filePath(QStringLiteral("test.svsession"));

        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

        window.addBookmark({.label = QStringLiteral("BM1"), .sourceBitOffset = 8});
        window.addAnnotation({.text = QStringLiteral("Note1"), .sourceBitOffset = 16, .bitLength = 8});
        QVERIFY(window.isWindowModified());

        // Configure save file dialog handler to return sessionPath
        window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return sessionPath;
        });

        // Save session triggers Save As when path is empty
        QVERIFY(window.saveSession());
        QVERIFY(window.currentSessionFilePath().has_value());
        QCOMPARE(*window.currentSessionFilePath(), sessionPath);
        QVERIFY(!window.isWindowModified());
        QVERIFY(QFile::exists(sessionPath));

        // Verify JSON contents
        QFile file(sessionPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        QVERIFY(doc.isObject());
        const QJsonObject root = doc.object();
        QCOMPARE(root.value(QStringLiteral("schemaVersion")).toInt(), 1);
        QCOMPARE(root.value(QStringLiteral("bookmarks")).toArray().size(), 1);
        QCOMPARE(root.value(QStringLiteral("annotations")).toArray().size(), 1);

        // Modify state again (dirty)
        window.addBookmark({.label = QStringLiteral("BM2"), .sourceBitOffset = 24});
        QVERIFY(window.isWindowModified());

        // Direct save without dialog now overwrites sessionPath
        bool dialogCalled = false;
        window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            dialogCalled = true;
            return QString{};
        });
        QVERIFY(window.saveSession());
        QVERIFY(!dialogCalled);
        QVERIFY(!window.isWindowModified());

        // Verify updated count in file
        QFile fileUpdated(sessionPath);
        QVERIFY(fileUpdated.open(QIODevice::ReadOnly));
        const QJsonDocument docUpdated = QJsonDocument::fromJson(fileUpdated.readAll());
        fileUpdated.close();
        QCOMPARE(docUpdated.object().value(QStringLiteral("bookmarks")).toArray().size(), 2);
    }

    void maybeSavePromptHandlesSaveDiscardAndCancel() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sessionPath = directory.filePath(QStringLiteral("prompt.svsession"));
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");

        // Case A: Save accepted
        {
            MainWindow window;
            QString errorMessage;
            QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
            window.addBookmark({.label = QStringLiteral("B"), .sourceBitOffset = 8});
            QVERIFY(window.isWindowModified());

            window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Save; });
            window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) { return sessionPath; });

            QVERIFY(window.maybeSave());
            QVERIFY(!window.isWindowModified());
            QVERIFY(window.currentSessionFilePath().has_value());
            QCOMPARE(*window.currentSessionFilePath(), sessionPath);
            QVERIFY(QFile::exists(sessionPath));
        }

        // Case B: Discard accepted
        {
            MainWindow window;
            QString errorMessage;
            QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
            window.addBookmark({.label = QStringLiteral("B"), .sourceBitOffset = 8});
            QVERIFY(window.isWindowModified());

            window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Discard; });

            QVERIFY(window.maybeSave());
            // Returned true (proceed allowed) without saving
        }

        // Case C: Cancel rejected
        {
            MainWindow window;
            QString errorMessage;
            QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
            window.addBookmark({.label = QStringLiteral("B"), .sourceBitOffset = 8});
            QVERIFY(window.isWindowModified());

            window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Cancel; });

            QVERIFY(!window.maybeSave());
            // Proceed rejected, still modified
            QVERIFY(window.isWindowModified());
        }
    }

    void maybeSaveHandlesSaveAsDialogCancelCleanly() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        window.addBookmark({.label = QStringLiteral("B"), .sourceBitOffset = 8});
        QVERIFY(window.isWindowModified());

        bool messageDialogShown = false;
        window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Save; });
        window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) { return QString{}; });
        window.setMessageDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            messageDialogShown = true;
        });

        // User cancelled save file selection -> maybeSave should return false, but NOT show critical error dialog
        QVERIFY(!window.maybeSave());
        QVERIFY(!messageDialogShown);
        QVERIFY(window.isWindowModified());
    }

    void maybeSaveHandlesSaveIoErrorWithModalDialog() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        window.addBookmark({.label = QStringLiteral("B"), .sourceBitOffset = 8});
        QVERIFY(window.isWindowModified());

        bool messageDialogShown = false;
        QString reportedTitle;
        QString reportedText;

        window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Save; });
        window.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
            return QStringLiteral("/non_existent_directory_streamview_test_xyz/session.svsession");
        });
        window.setMessageDialogHandlerForTesting([&](QWidget*, const QString& title, const QString& text) {
            messageDialogShown = true;
            reportedTitle = title;
            reportedText = text;
        });

        // Save fails due to FileIoError -> maybeSave returns false, and modal critical dialog is shown
        QVERIFY(!window.maybeSave());
        QVERIFY(messageDialogShown);
        QVERIFY(!reportedTitle.isEmpty());
        QVERIFY(!reportedText.isEmpty());
        QVERIFY(window.isWindowModified());
    }

    void closeEventRejectsWhenMaybeSaveCancelled() {
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");
        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));
        window.addBookmark({.label = QStringLiteral("B"), .sourceBitOffset = 8});
        QVERIFY(window.isWindowModified());

        // Cancel branch ignores close
        window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Cancel; });
        QCloseEvent cancelEvent;
        QCoreApplication::sendEvent(&window, &cancelEvent);
        QVERIFY(!cancelEvent.isAccepted());

        // Discard branch accepts close
        window.setSavePromptHandlerForTesting([&](QWidget*) { return QMessageBox::Discard; });
        QCloseEvent discardEvent;
        QCoreApplication::sendEvent(&window, &discardEvent);
        QVERIFY(discardEvent.isAccepted());
    }

    void openSessionFileRestoresUserStateAndUI() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sessionPath = directory.filePath(QStringLiteral("restore.svsession"));
        const QString fixturePath = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5h_mp4a_esds.mp4");

        {
            MainWindow window1;
            QString errorMessage;
            QVERIFY2(window1.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

            auto* treeView = window1.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
            QVERIFY(treeView != nullptr);

            QModelIndex boxIndex;
            QTRY_VERIFY((boxIndex = findIndexByName(*treeView->model(), QStringLiteral("box[0]"))).isValid());
            treeView->expand(boxIndex);
            treeView->setCurrentIndex(boxIndex);

            // Change display mode in raw view
            auto* rawView = window1.findChild<RawDataView*>(QStringLiteral("rawDataView"));
            QVERIFY(rawView != nullptr);
            rawView->model()->setDisplayMode(RawDisplayMode::Binary);

            // Add bookmarks and annotations
            window1.addBookmark({.label = QStringLiteral("BM_BOX0"), .sourceBitOffset = 64});
            window1.addAnnotation({.text = QStringLiteral("Note on box0"), .sourceBitOffset = 64, .bitLength = 32});

            window1.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
                return sessionPath;
            });
            QVERIFY(window1.saveSession());
            QVERIFY(QFile::exists(sessionPath));
        }

        // Open session in a second window
        {
            MainWindow window2;
            QString errorMessage;
            QVERIFY2(window2.openSessionFile(sessionPath, &errorMessage), qPrintable(errorMessage));

            QVERIFY(window2.currentSessionFilePath().has_value());
            QCOMPARE(*window2.currentSessionFilePath(), sessionPath);
            QCOMPARE(window2.currentSourceIdentity(), fixturePath);
            QVERIFY(!window2.isWindowModified());

            // Bookmarks restored
            const auto bookmarks = window2.bookmarks();
            QCOMPARE(bookmarks.size(), 1);
            QCOMPARE(bookmarks[0].label, QStringLiteral("BM_BOX0"));
            QCOMPARE(bookmarks[0].sourceBitOffset, 64ULL);

            // Annotations restored
            const auto annotations = window2.annotations();
            QCOMPARE(annotations.size(), 1);
            QCOMPARE(annotations[0].text, QStringLiteral("Note on box0"));
            QCOMPARE(annotations[0].sourceBitOffset, 64ULL);
            QCOMPARE(annotations[0].bitLength, 32ULL);

            // UI view restored
            auto* rawView = window2.findChild<RawDataView*>(QStringLiteral("rawDataView"));
            QVERIFY(rawView != nullptr);
            QCOMPARE(rawView->model()->displayMode(), RawDisplayMode::Binary);

            auto* treeView = window2.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
            QVERIFY(treeView != nullptr);
            const QModelIndex currentIdx = treeView->currentIndex();
            QVERIFY(currentIdx.isValid());
            QCOMPARE(treeView->model()->data(currentIdx).toString(), QStringLiteral("box[0]"));
            QVERIFY(treeView->isExpanded(currentIdx));
        }
    }

    void overrideFormatActionEnabledOnlyWhenSessionLoaded() {
        MainWindow window;
        auto* actionOverride = window.findChild<QAction*>(QStringLiteral("actionOverrideFormat"));
        QVERIFY(actionOverride != nullptr);
        QVERIFY(!actionOverride->isEnabled());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("sample.264"), QByteArray::fromHex("00000165"));
        QVERIFY(!path.isEmpty());

        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));
        QVERIFY(actionOverride->isEnabled());
    }

    void ambiguityBannerShowsResolveAmbiguityButtonOnAmbiguousFormat() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString ambiguousPath = writeFixture(
            directory, QStringLiteral("ambiguous.mp4"), makeAmbiguousMp4Bytes());
        QVERIFY(!ambiguousPath.isEmpty());

        MainWindow window;
        auto* banner = window.findChild<QWidget*>(QStringLiteral("formatAmbiguityBanner"));
        auto* label = window.findChild<QLabel*>(QStringLiteral("formatAmbiguityLabel"));
        auto* btn = window.findChild<QPushButton*>(QStringLiteral("resolveAmbiguityButton"));
        QVERIFY(banner != nullptr);
        QVERIFY(label != nullptr);
        QVERIFY(btn != nullptr);
        QCOMPARE(btn->text(), QStringLiteral("Resolve Ambiguity..."));

        // Initially hidden
        QVERIFY(banner->isHidden());

        QString errorMessage;
        QVERIFY2(window.openMediaSource(ambiguousPath, &errorMessage), qPrintable(errorMessage));

        // Ambiguous format surfaces banner, label and button
        QTRY_VERIFY(!banner->isHidden());
        QVERIFY(!label->isHidden());
        QVERIFY(!btn->isHidden());
        QVERIFY(label->text().contains(QStringLiteral("Ambiguous format")));

        // Clean elementary stream in another window hides banner
        const QString cleanPath = writeFixture(
            directory, QStringLiteral("clean.264"), QByteArray::fromHex("00000165"));
        MainWindow cleanWindow;
        QVERIFY2(cleanWindow.openMediaSource(cleanPath, &errorMessage), qPrintable(errorMessage));
        auto* cleanBanner = cleanWindow.findChild<QWidget*>(QStringLiteral("formatAmbiguityBanner"));
        QVERIFY(cleanBanner != nullptr);
        QVERIFY(cleanBanner->isHidden());
    }

    void resolveAmbiguityButtonTriggersOverrideFormatAndReanalyzes() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("ambiguous.mp4"), makeAmbiguousMp4Bytes());
        QVERIFY(!path.isEmpty());

        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));

        auto* banner = window.findChild<QWidget*>(QStringLiteral("formatAmbiguityBanner"));
        auto* btn = window.findChild<QPushButton*>(QStringLiteral("resolveAmbiguityButton"));
        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        auto* timelineStatus = window.findChild<QLabel*>(QStringLiteral("timelineStatusLabel"));
        QVERIFY(banner != nullptr && btn != nullptr && treeView != nullptr && timelineStatus != nullptr);
        QTRY_VERIFY(!banner->isHidden());

        // Initially detected as MP4 ISOBMFF: root nodes are boxes
        QVERIFY(findIndexByName(*treeView->model(), QStringLiteral("box[0]")).isValid());
        QVERIFY(!window.isWindowModified());

        auto h264Pkg = streamview::rules::loadH264AnnexBRulePackage();
        QVERIFY(h264Pkg.succeeded() && h264Pkg.package.has_value());
        auto h264Entry = streamview::rules::RuleEntryPointIdentity::create(
            h264Pkg.package->identity(), QStringLiteral("annex-b"));
        QVERIFY(h264Entry.has_value());

        bool handlerCalled = false;
        bool handlerSelectionAmbiguous = false;
        QString handlerCurrentEntryPoint;
        window.setFormatOverrideDialogHandlerForTesting([&](
            QWidget*,
            const streamview::rules::RulePackageCatalog&,
            const streamview::rules::FormatSelection& selection,
            const streamview::rules::RuleEntryPointIdentity& current)
            -> std::optional<streamview::rules::RuleEntryPointIdentity> {
            handlerCalled = true;
            handlerSelectionAmbiguous = selection.ambiguous();
            handlerCurrentEntryPoint = current.entryPointId();
            return *h264Entry;
        });

        btn->click();
        QVERIFY(handlerCalled);
        QVERIFY(handlerSelectionAmbiguous);
        QCOMPARE(handlerCurrentEntryPoint, QStringLiteral("main"));

        // Ambiguity banner should now be hidden (ambiguity resolved)
        QVERIFY(banner->isHidden());
        QVERIFY(window.isWindowModified());

        // Tree now has NAL units instead of boxes
        QVERIFY(!findIndexByName(*treeView->model(), QStringLiteral("box[0]")).isValid());
        QTRY_VERIFY(treeView->model()->rowCount() > 0);

        // Timeline status shows container tracks unavailable for elementary stream
        QTRY_VERIFY(!timelineStatus->isHidden());
        QVERIFY(timelineStatus->text().contains(QStringLiteral("Container tracks unavailable")));
    }

    void overrideFormatMenuActionAllowsFormatSwitching() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("sample.264"), QByteArray::fromHex("00000165"));
        QVERIFY(!path.isEmpty());

        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));
        QVERIFY(!window.isWindowModified());

        auto* actionOverride = window.findChild<QAction*>(QStringLiteral("actionOverrideFormat"));
        QVERIFY(actionOverride != nullptr);

        auto aacPkg = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(aacPkg.succeeded() && aacPkg.package.has_value());
        auto aacEntry = streamview::rules::RuleEntryPointIdentity::create(
            aacPkg.package->identity(), QStringLiteral("adts"));
        QVERIFY(aacEntry.has_value());

        bool handlerCalled = false;
        QString handlerCurrentEntryPoint;
        window.setFormatOverrideDialogHandlerForTesting([&](
            QWidget*,
            const streamview::rules::RulePackageCatalog&,
            const streamview::rules::FormatSelection&,
            const streamview::rules::RuleEntryPointIdentity& current)
            -> std::optional<streamview::rules::RuleEntryPointIdentity> {
            handlerCalled = true;
            handlerCurrentEntryPoint = current.entryPointId();
            return *aacEntry;
        });

        actionOverride->trigger();
        QVERIFY(handlerCalled);
        QCOMPARE(handlerCurrentEntryPoint, QStringLiteral("annex-b"));
        QVERIFY(window.isWindowModified());
    }

    void overrideFormatCancelledPreservesCurrentSession() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("ambiguous.mp4"), makeAmbiguousMp4Bytes());
        QVERIFY(!path.isEmpty());

        MainWindow window;
        QString errorMessage;
        QVERIFY2(window.openMediaSource(path, &errorMessage), qPrintable(errorMessage));

        auto* banner = window.findChild<QWidget*>(QStringLiteral("formatAmbiguityBanner"));
        auto* btn = window.findChild<QPushButton*>(QStringLiteral("resolveAmbiguityButton"));
        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(banner != nullptr && btn != nullptr && treeView != nullptr);
        QTRY_VERIFY(!banner->isHidden());

        bool handlerCalled = false;
        window.setFormatOverrideDialogHandlerForTesting([&](
            QWidget*,
            const streamview::rules::RulePackageCatalog&,
            const streamview::rules::FormatSelection&,
            const streamview::rules::RuleEntryPointIdentity&)
            -> std::optional<streamview::rules::RuleEntryPointIdentity> {
            handlerCalled = true;
            return std::nullopt; // User cancelled
        });

        btn->click();
        QVERIFY(handlerCalled);

        // Banner remains visible (ambiguity not resolved)
        QVERIFY(!banner->isHidden());
        QVERIFY(!window.isWindowModified());
        QVERIFY(findIndexByName(*treeView->model(), QStringLiteral("box[0]")).isValid());
    }

    void formatOverrideDialogConstructsAndSelectsCandidate() {
        streamview::rules::RulePackageCatalog catalog;
        auto mp4Pkg = streamview::rules::loadMp4IsobmffRulePackage();
        auto h264Pkg = streamview::rules::loadH264AnnexBRulePackage();
        auto aacPkg = streamview::rules::loadAacAdtsRulePackage();
        QVERIFY(mp4Pkg.succeeded() && mp4Pkg.package.has_value());
        QVERIFY(h264Pkg.succeeded() && h264Pkg.package.has_value());
        QVERIFY(aacPkg.succeeded() && aacPkg.package.has_value());

        const auto mp4Identity = mp4Pkg.package->identity();
        const auto h264Identity = h264Pkg.package->identity();
        const auto aacIdentity = aacPkg.package->identity();

        static_cast<void>(catalog.registerPackage(std::move(*mp4Pkg.package)));
        static_cast<void>(catalog.registerPackage(std::move(*h264Pkg.package)));
        static_cast<void>(catalog.registerPackage(std::move(*aacPkg.package)));

        auto mp4Entry = streamview::rules::RuleEntryPointIdentity::create(
            mp4Identity, QStringLiteral("main"));
        auto h264Entry = streamview::rules::RuleEntryPointIdentity::create(
            h264Identity, QStringLiteral("annex-b"));
        QVERIFY(mp4Entry.has_value() && h264Entry.has_value());

        // Test with ambiguous format selection
        streamview::rules::FormatSelection ambiguousSelection;
        ambiguousSelection.format = streamview::rules::DetectedFormat::Mp4Isobmff;
        ambiguousSelection.reason = streamview::rules::DetectedFormatReason::AmbiguousContainerVersusElementaryStream;
        QVERIFY(ambiguousSelection.ambiguous());

        FormatOverrideDialog dlg(nullptr, catalog, ambiguousSelection, *mp4Entry);
        auto* noticeBanner = dlg.findChild<QLabel*>(QStringLiteral("dialogAmbiguityBanner"));
        auto* listWidget = dlg.findChild<QListWidget*>(QStringLiteral("optionsListWidget"));
        auto* descLabel = dlg.findChild<QLabel*>(QStringLiteral("descriptionLabel"));
        auto* buttonBox = dlg.findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"));

        QVERIFY(noticeBanner != nullptr);
        QVERIFY(listWidget != nullptr);
        QVERIFY(descLabel != nullptr);
        QVERIFY(buttonBox != nullptr);

        QVERIFY(!noticeBanner->isHidden());
        QVERIFY(noticeBanner->text().contains(QStringLiteral("Notice: Multiple conflicting formats")));
        QCOMPARE(listWidget->count(), 3);

        // Pre-selection on ambiguous selection should be the competing candidate (H.264)
        auto selected = dlg.selectedRuleEntryPoint();
        QVERIFY(selected.has_value());
        QCOMPARE(*selected, *h264Entry);
        QVERIFY(!descLabel->text().isEmpty());

        // Verify items contain proper annotations
        bool foundCurrentAndCandidate = false;
        bool foundCandidateOnly = false;
        for (int i = 0; i < listWidget->count(); ++i) {
            const QString text = listWidget->item(i)->text();
            if (text.contains(QStringLiteral("[Current]")) && text.contains(QStringLiteral("[Candidate]"))) {
                foundCurrentAndCandidate = true;
            } else if (text.contains(QStringLiteral("[Candidate]"))) {
                foundCandidateOnly = true;
            }
        }
        QVERIFY(foundCurrentAndCandidate);
        QVERIFY(foundCandidateOnly);

        // Selecting MP4 item updates selection
        listWidget->setCurrentRow(0);
        selected = dlg.selectedRuleEntryPoint();
        QVERIFY(selected.has_value());
        QCOMPARE(*selected, *mp4Entry);

        // Test with clean non-ambiguous selection
        streamview::rules::FormatSelection cleanSelection;
        cleanSelection.format = streamview::rules::DetectedFormat::H264AnnexB;
        cleanSelection.reason = streamview::rules::DetectedFormatReason::H264AnchoredStartCodes;
        QVERIFY(!cleanSelection.ambiguous());

        FormatOverrideDialog cleanDlg(nullptr, catalog, cleanSelection, *h264Entry);
        auto* cleanNoticeBanner = cleanDlg.findChild<QLabel*>(QStringLiteral("dialogAmbiguityBanner"));
        QVERIFY(cleanNoticeBanner != nullptr);
        QVERIFY(cleanNoticeBanner->isHidden());
        auto cleanSelected = cleanDlg.selectedRuleEntryPoint();
        QVERIFY(cleanSelected.has_value());
        QCOMPARE(*cleanSelected, *h264Entry);
    }

    void manageRulesMenuActionOpensRuleManagerDialog() {
        MainWindow window;
        auto* action = window.findChild<QAction*>(QStringLiteral("actionManageRules"));
        QVERIFY(action != nullptr);
        QVERIFY(action->isEnabled());

        bool handlerInvoked = false;
        QString capturedStorePath;
        std::set<QString> capturedBundledIds;

        window.setRuleManagerDialogHandlerForTesting(
            [&](QWidget*,
                streamview::rules::RulePackageCatalog& catalog,
                const QString& storePath,
                const std::set<QString>& bundledIds) {
                handlerInvoked = true;
                capturedStorePath = storePath;
                capturedBundledIds = bundledIds;
                QCOMPARE(catalog.allPackages().size(), std::size_t{3});
            });

        action->trigger();

        QVERIFY(handlerInvoked);
        QCOMPARE(capturedStorePath, window.ruleStorePath());
        QCOMPARE(capturedBundledIds.size(), std::size_t{3});
        QVERIFY(capturedBundledIds.find(QStringLiteral("org.streamview.aac")) != capturedBundledIds.end());
        QVERIFY(capturedBundledIds.find(QStringLiteral("org.streamview.h264")) != capturedBundledIds.end());
        QVERIFY(capturedBundledIds.find(QStringLiteral("org.streamview.mp4")) != capturedBundledIds.end());
    }

    void progressBarAndCancelButtonPresence() {
        MainWindow window;
        auto* progressBar = window.findChild<QProgressBar*>(QStringLiteral("analysisProgressBar"));
        QVERIFY(progressBar != nullptr);
        QVERIFY(progressBar->isHidden());
        QCOMPARE(progressBar->minimum(), 0);
        QCOMPARE(progressBar->maximum(), 100);

        auto* cancelButton = window.findChild<QPushButton*>(QStringLiteral("cancelAnalysisButton"));
        QVERIFY(cancelButton != nullptr);
        QVERIFY(cancelButton->isHidden());
        QCOMPARE(cancelButton->text(), QStringLiteral("Cancel"));
    }

    void diagnosticsDockPresenceAndViewMenuAction() {
        MainWindow window;
        window.show();
        auto* dock = window.findChild<DiagnosticsSummaryDock*>(QStringLiteral("diagnosticsSummaryDock"));
        QVERIFY(dock != nullptr);
        QCOMPARE(dock->windowTitle(), QStringLiteral("Diagnostics"));

        auto* menuView = window.findChild<QMenu*>(QStringLiteral("menuView"));
        QVERIFY(menuView != nullptr);

        auto* toggleAction = window.findChild<QAction*>(QStringLiteral("actionToggleDiagnosticsDock"));
        QVERIFY(toggleAction != nullptr);
        QCOMPARE(toggleAction->text(), QStringLiteral("&Diagnostics"));

        // Toggle action controls visibility
        QVERIFY(dock->isVisible());
        toggleAction->trigger();
        QVERIFY(dock->isHidden());
        toggleAction->trigger();
        QVERIFY(dock->isVisible());
    }

    void analysisCancellation() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // Create an Annex B stream with multiple NAL units
        QByteArray bytes;
        for (int i = 0; i < 20; ++i) {
            bytes.append(QByteArray::fromHex("00000001658884"));
        }
        const QString path = writeFixture(directory, QStringLiteral("multi.264"), bytes);
        QVERIFY(!path.isEmpty());

        MainWindow window;
        QString errorMessage;
        // Request cancellation immediately
        window.cancelAnalysis();
        QVERIFY(window.openMediaSource(path, &errorMessage));

        auto* cancelButton = window.findChild<QPushButton*>(QStringLiteral("cancelAnalysisButton"));
        auto* progressBar = window.findChild<QProgressBar*>(QStringLiteral("analysisProgressBar"));
        auto* statusBar = window.statusBar();
        QVERIFY(cancelButton != nullptr);
        QVERIFY(progressBar != nullptr);
        QVERIFY(statusBar != nullptr);

        // Cancel analysis
        window.cancelAnalysis();
        QCoreApplication::processEvents();

        // Progress widgets should be hidden
        QVERIFY(progressBar->isHidden());
        QVERIFY(cancelButton->isHidden());

        // Tree view retains materialized nodes
        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(treeView != nullptr);
        auto* model = treeView->model();
        QVERIFY(model != nullptr);
        QVERIFY(model->rowCount() >= 1);
    }

    void diagnosticsSummaryDockBidirectionalSelection() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = writeFixture(
            directory, QStringLiteral("truncated.264"), QByteArray::fromHex("000001"));
        QVERIFY(!path.isEmpty());

        MainWindow window;
        QString errorMessage;
        QVERIFY(window.openMediaSource(path, &errorMessage));

        auto* dock = window.findChild<DiagnosticsSummaryDock*>(QStringLiteral("diagnosticsSummaryDock"));
        QVERIFY(dock != nullptr);
        auto* table = dock->findChild<QTableWidget*>(QStringLiteral("diagnosticsTableWidget"));
        QVERIFY(table != nullptr);

        auto* treeView = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
        QVERIFY(treeView != nullptr);

        QTRY_VERIFY(table->rowCount() > 0);

        // Forward selection: selecting a row in the diagnostics table updates treeView
        table->setCurrentCell(0, 0);
        QTRY_VERIFY(treeView->currentIndex().isValid());

        // Reverse selection: selecting node in treeView updates diagnostics table selection
        const QModelIndex header =
            findIndexByName(*treeView->model(), QStringLiteral("NalUnitHeader"));
        QVERIFY(header.isValid());
        treeView->setCurrentIndex(header);
        QTRY_VERIFY(table->currentRow() >= 0);
    }

    void themeMenuActionsToggleThemeMode() {
        MainWindow window;
        auto* menuTheme = window.findChild<QMenu*>(QStringLiteral("menuTheme"));
        QVERIFY(menuTheme != nullptr);

        auto* actionSystem = window.findChild<QAction*>(QStringLiteral("actionThemeSystem"));
        auto* actionLight = window.findChild<QAction*>(QStringLiteral("actionThemeLight"));
        auto* actionDark = window.findChild<QAction*>(QStringLiteral("actionThemeDark"));
        QVERIFY(actionSystem != nullptr);
        QVERIFY(actionLight != nullptr);
        QVERIFY(actionDark != nullptr);

        // Switch to Dark
        actionDark->trigger();
        QCOMPARE(ThemeManager::instance().themeMode(), ThemeMode::Dark);
        QCOMPARE(ThemeManager::instance().isDark(), true);
        QVERIFY(actionDark->isChecked());

        // Switch to Light
        actionLight->trigger();
        QCOMPARE(ThemeManager::instance().themeMode(), ThemeMode::Light);
        QCOMPARE(ThemeManager::instance().isDark(), false);
        QVERIFY(actionLight->isChecked());

        // Restore System
        actionSystem->trigger();
        QCOMPARE(ThemeManager::instance().themeMode(), ThemeMode::System);
        QVERIFY(actionSystem->isChecked());
    }

    void languageMenuActionsDynamicRetranslation() {
        MainWindow window;
        auto* menuLanguage = window.findChild<QMenu*>(QStringLiteral("menuLanguage"));
        QVERIFY(menuLanguage != nullptr);

        auto* actionEnglish = window.findChild<QAction*>(QStringLiteral("actionLanguageEnglish"));
        auto* actionChinese = window.findChild<QAction*>(QStringLiteral("actionLanguageChinese"));
        QVERIFY(actionEnglish != nullptr);
        QVERIFY(actionChinese != nullptr);

        auto* menuFile = window.findChild<QMenu*>(QStringLiteral("menuFile"));
        auto* menuView = window.findChild<QMenu*>(QStringLiteral("menuView"));
        auto* dock = window.findChild<DiagnosticsSummaryDock*>(QStringLiteral("diagnosticsSummaryDock"));
        auto* cancelButton = window.findChild<QPushButton*>(QStringLiteral("cancelAnalysisButton"));
        auto* tableWidget = dock ? dock->findChild<QTableWidget*>(QStringLiteral("diagnosticsTableWidget")) : nullptr;

        QVERIFY(menuFile != nullptr);
        QVERIFY(menuView != nullptr);
        QVERIFY(dock != nullptr);
        QVERIFY(cancelButton != nullptr);
        QVERIFY(tableWidget != nullptr);

        // Initially in English
        QCOMPARE(menuFile->title(), QStringLiteral("&File"));
        QCOMPARE(menuView->title(), QStringLiteral("&View"));
        QCOMPARE(dock->windowTitle(), QStringLiteral("Diagnostics"));
        QCOMPARE(cancelButton->text(), QStringLiteral("Cancel"));

        // Switch to Chinese
        actionChinese->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(LocalizationManager::instance().currentLanguage(), Language::SimplifiedChinese);
        QCOMPARE(menuFile->title(), QString::fromUtf8("文件(&F)"));
        QCOMPARE(menuView->title(), QString::fromUtf8("视图(&V)"));
        QCOMPARE(dock->windowTitle(), QString::fromUtf8("诊断"));
        QCOMPARE(cancelButton->text(), QString::fromUtf8("取消"));
        QCOMPARE(tableWidget->horizontalHeaderItem(0)->text(), QString::fromUtf8("严重性"));

        // Switch back to English
        actionEnglish->trigger();
        QCoreApplication::processEvents();

        QCOMPARE(LocalizationManager::instance().currentLanguage(), Language::English);
        QCOMPARE(menuFile->title(), QStringLiteral("&File"));
        QCOMPARE(menuView->title(), QStringLiteral("&View"));
        QCOMPARE(dock->windowTitle(), QStringLiteral("Diagnostics"));
        QCOMPARE(cancelButton->text(), QStringLiteral("Cancel"));
        QCOMPARE(tableWidget->horizontalHeaderItem(0)->text(), QStringLiteral("Severity"));
    }

    void openSessionFileRestoresDisambiguatedSameNamedSiblingsByNodeId() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString fixturePath = writeFixture(
            directory,
            QStringLiteral("sps_poc_cycle.264"),
            QByteArray::fromHex("000000016742001ed36140508944"));
        QVERIFY(!fixturePath.isEmpty());
        const QString sessionPath = directory.filePath(QStringLiteral("disambiguated.svsession"));

        streamview::core::AnalysisNodeId selectedNodeId{0};

        {
            MainWindow window1;
            QString errorMessage;
            QVERIFY2(window1.openMediaSource(fixturePath, &errorMessage), qPrintable(errorMessage));

            auto* treeView = window1.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
            QVERIFY(treeView != nullptr);
            auto* treeModel = qobject_cast<AnalysisTreeModel*>(treeView->model());
            QVERIFY(treeModel != nullptr);

            // Wait until analysis finishes and NAL node is published
            QModelIndex nalIndex;
            QTRY_VERIFY((nalIndex = findIndexByName(*treeModel, QStringLiteral("nal_unit[0]"))).isValid());
            treeView->expand(nalIndex);

            // Find rbsp_payload child
            QModelIndex rbspIndex;
            const int nalRows = treeModel->rowCount(nalIndex);
            for (int r = 0; r < nalRows; ++r) {
                const QModelIndex child = treeModel->index(r, AnalysisTreeModel::Name, nalIndex);
                if (treeModel->data(child, Qt::DisplayRole).toString() == QStringLiteral("rbsp_payload")) {
                    rbspIndex = child;
                    break;
                }
            }
            QVERIFY(rbspIndex.isValid());
            treeView->expand(rbspIndex);

            // Inside rbsp_payload, locate SequenceParameterSetRbsp
            QModelIndex spsIndex;
            const int rbspRows = treeModel->rowCount(rbspIndex);
            for (int r = 0; r < rbspRows; ++r) {
                const QModelIndex child = treeModel->index(r, AnalysisTreeModel::Name, rbspIndex);
                if (treeModel->data(child, Qt::DisplayRole).toString() == QStringLiteral("SequenceParameterSetRbsp")) {
                    spsIndex = child;
                    break;
                }
            }
            const QModelIndex parentOfFields = spsIndex.isValid() ? spsIndex : rbspIndex;
            treeView->expand(parentOfFields);

            // Locate all siblings named offset_for_ref_frame
            std::vector<QModelIndex> offsetIndices;
            const int fieldRows = treeModel->rowCount(parentOfFields);
            for (int r = 0; r < fieldRows; ++r) {
                const QModelIndex child = treeModel->index(r, AnalysisTreeModel::Name, parentOfFields);
                if (treeModel->data(child, Qt::DisplayRole).toString().startsWith(QStringLiteral("offset_for_ref_frame"))) {
                    offsetIndices.push_back(child);
                }
            }
            QCOMPARE(offsetIndices.size(), std::size_t{2});
            const auto id0 = treeModel->nodeIdAt(offsetIndices[0]);
            const auto id1 = treeModel->nodeIdAt(offsetIndices[1]);
            QVERIFY(id0.has_value() && id1.has_value());
            QVERIFY(*id0 != *id1);

            // Select the second sibling with the identical display name prefix
            selectedNodeId = *id1;
            treeView->setCurrentIndex(offsetIndices[1]);
            QCOMPARE(treeModel->nodeIdAt(treeView->currentIndex()),
                     std::optional<streamview::core::AnalysisNodeId>(selectedNodeId));

            window1.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
                return sessionPath;
            });
            QVERIFY(window1.saveSession());
            QVERIFY(QFile::exists(sessionPath));
        }

        // Open in a fresh second window and assert exact NodeId restoration
        {
            MainWindow window2;
            QString errorMessage;
            QVERIFY2(window2.openSessionFile(sessionPath, &errorMessage), qPrintable(errorMessage));

            auto* treeView2 = window2.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
            QVERIFY(treeView2 != nullptr);
            auto* treeModel2 = qobject_cast<AnalysisTreeModel*>(treeView2->model());
            QVERIFY(treeModel2 != nullptr);

            const QModelIndex restoredIndex = treeView2->currentIndex();
            QVERIFY(restoredIndex.isValid());
            QCOMPARE(treeModel2->data(restoredIndex, Qt::DisplayRole).toString(), QStringLiteral("offset_for_ref_frame[1]"));

            // Exact AnalysisNodeId identity match (proving cursor did NOT misplace to sibling 0)
            const auto restoredId = treeModel2->nodeIdAt(restoredIndex);
            QVERIFY(restoredId.has_value());
            QCOMPARE(*restoredId, selectedNodeId);
        }

        // 2. Truly identical display names (multiple sibling "Box" nodes under moov_payload)
        {
            const QString mp4Path = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_two_tracks.mp4");
            const QString mp4SessionPath = directory.filePath(QStringLiteral("disambiguated_mp4.svsession"));
            streamview::core::AnalysisNodeId selectedBoxId{0};

            {
                MainWindow window1;
                QString errorMessage;
                QVERIFY2(window1.openMediaSource(mp4Path, &errorMessage), qPrintable(errorMessage));

                auto* treeView = window1.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
                QVERIFY(treeView != nullptr);
                auto* treeModel = qobject_cast<AnalysisTreeModel*>(treeView->model());
                QVERIFY(treeModel != nullptr);

                QModelIndex moovBoxIndex;
                QTRY_VERIFY((moovBoxIndex = findIndexByName(*treeModel, QStringLiteral("box[1]"))).isValid());
                treeView->expand(moovBoxIndex);

                // Child of box[1] is Box (moov)
                const QModelIndex outerBoxIndex = treeModel->index(0, AnalysisTreeModel::Name, moovBoxIndex);
                QVERIFY(outerBoxIndex.isValid());
                treeView->expand(outerBoxIndex);

                // Find moov_payload under outerBoxIndex
                QModelIndex moovPayloadIndex;
                const int outerRows = treeModel->rowCount(outerBoxIndex);
                for (int r = 0; r < outerRows; ++r) {
                    const QModelIndex child = treeModel->index(r, AnalysisTreeModel::Name, outerBoxIndex);
                    if (treeModel->data(child, Qt::DisplayRole).toString() == QStringLiteral("moov_payload")) {
                        moovPayloadIndex = child;
                        break;
                    }
                }
                QVERIFY(moovPayloadIndex.isValid());
                treeView->expand(moovPayloadIndex);

                // moov_payload has 3 children, all named "Box"
                const int payloadRows = treeModel->rowCount(moovPayloadIndex);
                QCOMPARE(payloadRows, 3);
                for (int r = 0; r < payloadRows; ++r) {
                    const QModelIndex child = treeModel->index(r, AnalysisTreeModel::Name, moovPayloadIndex);
                    QCOMPARE(treeModel->data(child, Qt::DisplayRole).toString(), QStringLiteral("Box"));
                }

                const auto id0 = treeModel->nodeIdAt(treeModel->index(0, AnalysisTreeModel::Name, moovPayloadIndex));
                const auto id1 = treeModel->nodeIdAt(treeModel->index(1, AnalysisTreeModel::Name, moovPayloadIndex));
                const auto id2 = treeModel->nodeIdAt(treeModel->index(2, AnalysisTreeModel::Name, moovPayloadIndex));
                QVERIFY(id0.has_value() && id1.has_value() && id2.has_value());
                QVERIFY(*id0 != *id1 && *id1 != *id2 && *id0 != *id2);

                // Select row 1 (trak 1, which has identical display name "Box" as row 0 and row 2)
                selectedBoxId = *id1;
                const QModelIndex selectedIndex = treeModel->index(1, AnalysisTreeModel::Name, moovPayloadIndex);
                treeView->setCurrentIndex(selectedIndex);
                QCOMPARE(treeModel->nodeIdAt(treeView->currentIndex()),
                         std::optional<streamview::core::AnalysisNodeId>(selectedBoxId));

                window1.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
                    return mp4SessionPath;
                });
                QVERIFY(window1.saveSession());
                QVERIFY(QFile::exists(mp4SessionPath));
            }

            {
                MainWindow window2;
                QString errorMessage;
                QVERIFY2(window2.openSessionFile(mp4SessionPath, &errorMessage), qPrintable(errorMessage));

                auto* treeView2 = window2.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
                QVERIFY(treeView2 != nullptr);
                auto* treeModel2 = qobject_cast<AnalysisTreeModel*>(treeView2->model());
                QVERIFY(treeModel2 != nullptr);

                QTRY_VERIFY(treeView2->currentIndex().isValid());
                const QModelIndex restoredIndex = treeView2->currentIndex();
                QCOMPARE(treeModel2->data(restoredIndex, Qt::DisplayRole).toString(), QStringLiteral("Box"));

                // Exact AnalysisNodeId identity match proves cursor restored to row 1 (trak 1)
                // and did NOT misplace to row 0 (mvhd)
                const auto restoredId = treeModel2->nodeIdAt(restoredIndex);
                QVERIFY(restoredId.has_value());
                QCOMPARE(*restoredId, selectedBoxId);
            }
        }
    }

    void overrideFormatResetsPendingTreeState() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mp4Path = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_two_tracks.mp4");
        const QString sessionPath = directory.filePath(QStringLiteral("pending_override.svsession"));

        {
            MainWindow window1;
            QString errorMessage;
            QVERIFY2(window1.openMediaSource(mp4Path, &errorMessage), qPrintable(errorMessage));
            auto* treeView = window1.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
            QVERIFY(treeView != nullptr);
            auto* treeModel = qobject_cast<AnalysisTreeModel*>(treeView->model());
            QVERIFY(treeModel != nullptr);
            QModelIndex moovBoxIndex;
            QTRY_VERIFY((moovBoxIndex = findIndexByName(*treeModel, QStringLiteral("box[1]"))).isValid());
            treeView->setCurrentIndex(moovBoxIndex);

            window1.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
                return sessionPath;
            });
            QVERIFY(window1.saveSession());
            QVERIFY(QFile::exists(sessionPath));
        }

        {
            MainWindow window2;
            QString errorMessage;
            QVERIFY2(window2.openSessionFile(sessionPath, &errorMessage), qPrintable(errorMessage));

            auto h264Pkg = streamview::rules::loadH264AnnexBRulePackage();
            QVERIFY(h264Pkg.succeeded() && h264Pkg.package.has_value());
            auto h264Entry = streamview::rules::RuleEntryPointIdentity::create(
                h264Pkg.package->identity(), QStringLiteral("annex-b"));
            QVERIFY(h264Entry.has_value());

            window2.setFormatOverrideDialogHandlerForTesting(
                [&](QWidget*,
                    const streamview::rules::RulePackageCatalog&,
                    const streamview::rules::FormatSelection&,
                    const streamview::rules::RuleEntryPointIdentity&)
                    -> std::optional<streamview::rules::RuleEntryPointIdentity> {
                    return *h264Entry;
                });
            window2.overrideFormat();
            QVERIFY(!window2.pendingSelectedAnalysisPathForTesting().has_value());
            QVERIFY(window2.pendingExpandedPathsForTesting().isEmpty());
        }
    }

    void userManualSelectionPreemptsPendingRestoredPath() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mp4Path = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_two_tracks.mp4");
        const QString sessionPath = directory.filePath(QStringLiteral("pending_preempt.svsession"));

        {
            MainWindow window1;
            QString errorMessage;
            QVERIFY2(window1.openMediaSource(mp4Path, &errorMessage), qPrintable(errorMessage));
            auto* treeView = window1.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
            QVERIFY(treeView != nullptr);
            auto* treeModel = qobject_cast<AnalysisTreeModel*>(treeView->model());
            QVERIFY(treeModel != nullptr);
            QModelIndex moovBoxIndex;
            QTRY_VERIFY((moovBoxIndex = findIndexByName(*treeModel, QStringLiteral("box[1]"))).isValid());
            treeView->setCurrentIndex(moovBoxIndex);

            window1.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
                return sessionPath;
            });
            QVERIFY(window1.saveSession());
            QVERIFY(QFile::exists(sessionPath));
        }

        {
            MainWindow window2;
            QString errorMessage;
            QVERIFY2(window2.openSessionFile(sessionPath, &errorMessage), qPrintable(errorMessage));
            auto* treeView2 = window2.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
            QVERIFY(treeView2 != nullptr);
            auto* treeModel2 = qobject_cast<AnalysisTreeModel*>(treeView2->model());
            QVERIFY(treeModel2 != nullptr);

            // User immediately manually selects box[0] before batch 1 finishes
            QModelIndex box0Index = findIndexByName(*treeModel2, QStringLiteral("box[0]"));
            QVERIFY(box0Index.isValid());
            treeView2->setCurrentIndex(box0Index);

            // Manual user selection must have cleared the pending restored path (P2-45)
            QVERIFY(!window2.pendingSelectedAnalysisPathForTesting().has_value());

            // Wait for all batches to finish
            QTRY_VERIFY(treeModel2->rowCount() >= 2);
            // Current index must NOT have been stolen by the pending path
            QCOMPARE(treeModel2->data(treeView2->currentIndex(), Qt::DisplayRole).toString(), QStringLiteral("box[0]"));
        }
    }

    void childFormatNavigationDoesNotApplyRootPendingPath() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString mp4Path = QStringLiteral(STREAMVIEW_SOURCE_DIR "/tests/fixtures/mp4_p5j4_two_tracks.mp4");
        const QString sessionPath = directory.filePath(QStringLiteral("pending_child.svsession"));

        {
            MainWindow window1;
            QString errorMessage;
            QVERIFY2(window1.openMediaSource(mp4Path, &errorMessage), qPrintable(errorMessage));
            auto* treeView = window1.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
            QVERIFY(treeView != nullptr);
            auto* treeModel = qobject_cast<AnalysisTreeModel*>(treeView->model());
            QVERIFY(treeModel != nullptr);
            QModelIndex moovBoxIndex;
            QTRY_VERIFY((moovBoxIndex = findIndexByName(*treeModel, QStringLiteral("box[1]"))).isValid());
            treeView->setCurrentIndex(moovBoxIndex);

            window1.setSaveFileDialogHandlerForTesting([&](QWidget*, const QString&, const QString&) {
                return sessionPath;
            });
            QVERIFY(window1.saveSession());
            QVERIFY(QFile::exists(sessionPath));
        }

        {
            MainWindow window2;
            QString errorMessage;
            QVERIFY2(window2.openSessionFile(sessionPath, &errorMessage), qPrintable(errorMessage));
            auto* tableView = window2.findChild<QTableView*>(QStringLiteral("timelineTableView"));
            QVERIFY(tableView != nullptr);
            QTRY_VERIFY(tableView->model()->rowCount() > 0);

            // Double click sample row 0 to navigate into child format
            const QModelIndex sample0Index = tableView->model()->index(0, 0);
            Q_EMIT tableView->doubleClicked(sample0Index);

            auto* backButton = window2.findChild<QToolButton*>(QStringLiteral("navigationBackButton"));
            QVERIFY(backButton != nullptr);
            QTRY_VERIFY(backButton->isEnabled());

            // Root pending path must NOT be applied to the child tree (P2-43)
            auto* treeView2 = window2.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
            QVERIFY(treeView2 != nullptr);
            auto* treeModel2 = qobject_cast<AnalysisTreeModel*>(treeView2->model());
            QVERIFY(treeModel2 != nullptr);
            if (treeView2->currentIndex().isValid()) {
                QVERIFY(treeModel2->data(treeView2->currentIndex(), Qt::DisplayRole).toString() != QStringLiteral("box[1]"));
            }
        }
    }
};

QTEST_MAIN(MainWindowTest)

#include "main_window_test.moc"
