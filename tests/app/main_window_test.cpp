#include "main_window.h"
#include "raw_data_model.h"
#include "raw_data_view.h"

#include <QFile>
#include <QLabel>
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

#include <optional>

using streamview::app::AnalysisSessionCacheOptions;
using streamview::app::MainWindow;
using streamview::app::RawDataModel;
using streamview::app::RawDataView;
using streamview::app::RawDisplayMode;

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

    void bidirectionalCoordinateSelectionPreservesDisjointSpansInActiveTree() {
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

        QVERIFY(backButton != nullptr);
        QVERIFY(breadcrumbLabel != nullptr);
        QVERIFY(treeView != nullptr);
        QVERIFY(inspector != nullptr);
        QVERIFY(rawView != nullptr);

        // Test 900x600
        window.resize(900, 600);
        window.show();
        QTest::qWait(50);
        QVERIFY(backButton->isVisible());
        QVERIFY(breadcrumbLabel->isVisible());
        QVERIFY(treeView->isVisible());
        QVERIFY(inspector->isVisible());
        QVERIFY(rawView->isVisible());

        // Test 1280x800
        window.resize(1280, 800);
        QTest::qWait(50);
        QVERIFY(backButton->isVisible());
        QVERIFY(breadcrumbLabel->isVisible());
        QVERIFY(treeView->isVisible());
        QVERIFY(inspector->isVisible());
        QVERIFY(rawView->isVisible());
    }
};

QTEST_MAIN(MainWindowTest)

#include "main_window_test.moc"
