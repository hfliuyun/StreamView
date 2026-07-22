#include <streamview/core/source_pager.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>

using streamview::core::RandomAccessSource;
using streamview::core::SourcePageStatus;
using streamview::core::SourcePager;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;

namespace {

class RecordingSource final : public RandomAccessSource {
public:
    explicit RecordingSource(quint64 sizeBytes) : sizeBytes_(sizeBytes) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return sizeBytes_; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("recording-source"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        ++readCount_;
        lastOffset_ = byteOffset;
        lastRequestSize_ = destination.size();

        if (byteOffset >= sizeBytes_) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }

        const auto available = sizeBytes_ - byteOffset;
        const auto count = static_cast<std::size_t>(
            std::min<quint64>(available, static_cast<quint64>(destination.size())));
        for (std::size_t index = 0; index < count; ++index) {
            destination[index] = std::byte((byteOffset + index) & 0xFFU);
        }

        const auto status = count == destination.size() ? SourceReadStatus::Complete
                                                        : SourceReadStatus::EndOfSource;
        return {status, count, {}};
    }

    [[nodiscard]] std::size_t readCount() const noexcept { return readCount_; }
    [[nodiscard]] quint64 lastOffset() const noexcept { return lastOffset_; }
    [[nodiscard]] std::size_t lastRequestSize() const noexcept { return lastRequestSize_; }

private:
    quint64 sizeBytes_ = 0;
    mutable std::size_t readCount_ = 0;
    mutable quint64 lastOffset_ = 0;
    mutable std::size_t lastRequestSize_ = 0;
};

class FailingSource final : public RandomAccessSource {
public:
    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return SourcePager::pageSizeBytes();
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("failing-source"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64, std::span<std::byte> destination) const override {
        destination[0] = std::byte{0xAA};
        destination[1] = std::byte{0xBB};
        return {SourceReadStatus::Error, 2, QStringLiteral("simulated read failure")};
    }
};

class PrematureEndSource final : public RandomAccessSource {
public:
    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return SourcePager::pageSizeBytes() * 2U;
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("premature-end"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64, std::span<std::byte> destination) const override {
        destination[0] = std::byte{0x11};
        destination[1] = std::byte{0x22};
        return {SourceReadStatus::EndOfSource, 2, {}};
    }
};

} // namespace

class SourcePagerTest final : public QObject {
    Q_OBJECT

private slots:
    void loadsOnePageFromAHundredGigabyteSource() {
        constexpr quint64 sourceSize = 100ULL * 1024ULL * 1024ULL * 1024ULL;
        RecordingSource source(sourceSize);
        SourcePager pager(source);
        constexpr quint64 pageIndex = 123456;

        const auto page = pager.loadPage(pageIndex);

        QCOMPARE(page.status, SourcePageStatus::Ready);
        QCOMPARE(page.pageIndex, pageIndex);
        QCOMPARE(page.byteOffset, pageIndex * SourcePager::pageSizeBytes());
        QCOMPARE(page.bytes.size(), static_cast<std::size_t>(SourcePager::pageSizeBytes()));
        QCOMPARE(source.readCount(), std::size_t{1});
        QCOMPARE(source.lastOffset(), page.byteOffset);
        QCOMPARE(source.lastRequestSize(), page.bytes.size());
    }

    void loadsOnlyTheShortFinalPage() {
        RecordingSource source(SourcePager::pageSizeBytes() + 3U);
        SourcePager pager(source);

        const auto page = pager.loadPage(1);

        QCOMPARE(page.status, SourcePageStatus::EndOfSource);
        QCOMPARE(page.byteOffset, SourcePager::pageSizeBytes());
        QCOMPARE(page.bytes.size(), std::size_t{3});
        QCOMPARE(std::to_integer<unsigned int>(page.bytes.at(0)), 0U);
        QCOMPARE(std::to_integer<unsigned int>(page.bytes.at(2)), 2U);
        QCOMPARE(source.lastRequestSize(), std::size_t{3});
    }

    void marksAnExactFullFinalPageAsTheEnd() {
        RecordingSource source(SourcePager::pageSizeBytes());
        SourcePager pager(source);

        const auto page = pager.loadPage(0);

        QCOMPARE(page.status, SourcePageStatus::EndOfSource);
        QCOMPARE(page.bytes.size(), static_cast<std::size_t>(SourcePager::pageSizeBytes()));
    }

    void doesNotReadPastTheLastPage() {
        RecordingSource source(4);
        SourcePager pager(source);

        const auto page = pager.loadPage(1);

        QCOMPARE(page.status, SourcePageStatus::EndOfSource);
        QVERIFY(page.bytes.empty());
        QCOMPARE(source.readCount(), std::size_t{0});
    }

    void keepsOnlyReportedBytesWhenTheSourceFails() {
        FailingSource source;
        SourcePager pager(source);

        const auto page = pager.loadPage(0);

        QCOMPARE(page.status, SourcePageStatus::Error);
        QCOMPARE(page.bytes.size(), std::size_t{2});
        QCOMPARE(std::to_integer<unsigned int>(page.bytes.at(0)), 0xAAU);
        QCOMPARE(std::to_integer<unsigned int>(page.bytes.at(1)), 0xBBU);
        QCOMPARE(page.errorMessage, QStringLiteral("simulated read failure"));
    }

    void rejectsAnUnexpectedEndBeforeTheDeclaredSourceSize() {
        PrematureEndSource source;
        SourcePager pager(source);

        const auto page = pager.loadPage(0);

        QCOMPARE(page.status, SourcePageStatus::Error);
        QCOMPARE(page.bytes.size(), std::size_t{2});
        QVERIFY(page.errorMessage.contains(QStringLiteral("declared source end")));
    }

    void rejectsAPageIndexThatOverflowsSourceCoordinates() {
        RecordingSource source(std::numeric_limits<quint64>::max());
        SourcePager pager(source);
        const quint64 pageIndex =
            (std::numeric_limits<quint64>::max() / SourcePager::pageSizeBytes()) + 1U;

        const auto page = pager.loadPage(pageIndex);

        QCOMPARE(page.status, SourcePageStatus::Error);
        QVERIFY(page.bytes.empty());
        QVERIFY(page.errorMessage.contains(QStringLiteral("coordinate")));
        QCOMPARE(source.readCount(), std::size_t{0});
    }
};

QTEST_GUILESS_MAIN(SourcePagerTest)

#include "source_pager_test.moc"
