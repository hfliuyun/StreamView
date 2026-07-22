#include "raw_data_model.h"
#include "raw_data_view.h"

#include <streamview/core/coordinates.h>
#include <streamview/core/source.h>
#include <streamview/core/source_pager.h>

#include <QTest>
#include <QSignalSpy>
#include <QTableView>
#include <QImage>
#include <QToolButton>

#include <algorithm>
#include <cstddef>
#include <span>

using streamview::app::RawDataModel;
using streamview::app::RawDataView;
using streamview::core::RandomAccessSource;
using streamview::core::SourceBitAddress;
using streamview::core::SourcePager;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourceSpan;

namespace {

class RecordingSource final : public RandomAccessSource {
public:
    explicit RecordingSource(quint64 sizeBytes) : sizeBytes_(sizeBytes) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return sizeBytes_; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("raw-view"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        ++readCount_;
        const auto count = static_cast<std::size_t>(std::min<quint64>(
            sizeBytes_ - byteOffset, static_cast<quint64>(destination.size())));
        std::fill_n(destination.begin(), count, std::byte{0});
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

    [[nodiscard]] std::size_t readCount() const noexcept { return readCount_; }

private:
    quint64 sizeBytes_ = 0;
    mutable std::size_t readCount_ = 0;
};

} // namespace

class RawDataViewTest final : public QObject {
    Q_OBJECT

private slots:
    void revealsThePageContainingTheFirstSelectedSourceSpan() {
        RecordingSource source(SourcePager::pageSizeBytes() + 16U);
        RawDataView view;
        QVERIFY(view.setSource(&source));
        const quint64 selectedBit = (SourcePager::pageSizeBytes() + 2U) * 8U + 3U;
        const auto selection = SourceSpan::create(SourceBitAddress(selectedBit), 2);
        QVERIFY(selection.has_value());

        view.setSourceSelection({*selection});

        QCOMPARE(view.model()->pageIndex(), quint64{1});
        QCOMPARE(source.readCount(), std::size_t{2});
        QCOMPARE(view.model()
                     ->data(view.model()->index(0, RawDataModel::FirstByte + 2),
                            RawDataModel::SelectedBitsRole)
                     .toUInt(),
                 0x18U);
    }

    void emitsTheExactSourceBitClickedWithinAByteCell() {
        RecordingSource source(1);
        RawDataView view;
        view.resize(960, 240);
        view.show();
        QVERIFY(view.setSource(&source));
        QCoreApplication::processEvents();
        auto* table = view.findChild<QTableView*>(QStringLiteral("rawDataTable"));
        QVERIFY(table != nullptr);
        const QModelIndex firstByte =
            view.model()->index(0, RawDataModel::FirstByte);
        const QRect cell = table->visualRect(firstByte);
        QVERIFY(cell.isValid());
        constexpr int selectedBitInByte = 5;
        const QPoint clickPoint(
            cell.left() + ((selectedBitInByte * 2 + 1) * cell.width()) / 16,
            cell.center().y());
        QSignalSpy selectedSpy(&view, &RawDataView::sourceBitSelected);

        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, clickPoint);

        QCOMPARE(selectedSpy.count(), 1);
        QCOMPARE(selectedSpy.at(0).at(0).toULongLong(), quint64{5});
    }

    void paintsSelectedBitsAsIndependentCellSegments() {
        RecordingSource source(1);
        RawDataView view;
        view.resize(960, 240);
        view.show();
        QVERIFY(view.setSource(&source));
        QCoreApplication::processEvents();
        auto* table = view.findChild<QTableView*>(QStringLiteral("rawDataTable"));
        QVERIFY(table != nullptr);
        const auto selection = SourceSpan::create(SourceBitAddress(0), 1);
        QVERIFY(selection.has_value());
        view.setSourceSelection({*selection});

        const QStringList modeButtons = {
            QStringLiteral("hexModeButton"),
            QStringLiteral("binaryModeButton"),
            QStringLiteral("combinedModeButton"),
        };
        for (const QString& objectName : modeButtons) {
            auto* button = view.findChild<QToolButton*>(objectName);
            QVERIFY(button != nullptr);
            button->click();
            QCoreApplication::processEvents();

            const QRect cell =
                table->visualRect(view.model()->index(0, RawDataModel::FirstByte));
            QImage image(table->viewport()->size(), QImage::Format_ARGB32);
            image.fill(Qt::transparent);
            table->viewport()->render(&image);

            const QColor selected = image.pixelColor(cell.left() + cell.width() / 16,
                                                       cell.top() + 2);
            const QColor unselected = image.pixelColor(
                cell.left() + cell.width() * 15 / 16, cell.top() + 2);
            QVERIFY2(selected != unselected, qPrintable(objectName));
        }
    }
};

QTEST_MAIN(RawDataViewTest)

#include "raw_data_view_test.moc"
