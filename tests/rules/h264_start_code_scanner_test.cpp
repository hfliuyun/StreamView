#include <streamview/rules/h264_start_code_scanner.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <vector>

using streamview::core::CancellationSource;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::rules::H264StartCodeScanner;
using streamview::rules::StartCodeScanStatus;

namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

class MemorySource final : public RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> data) : data_(std::move(data)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= data_.size()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const std::size_t count = std::min(destination.size(), data_.size() - offset);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(count),
                    destination.begin());
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

private:
    std::vector<std::byte> data_;
};

} // namespace

class H264StartCodeScannerTest final : public QObject {
    Q_OBJECT

private slots:
    void publishesThreeAndFourBytePrefixesInBatches() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0xAA,
                                   0x00, 0x00, 0x00, 0x01, 0x41}));
        H264StartCodeScanner scanner(source);

        const auto first = scanner.scanBatch(1);
        QCOMPARE(first.status, StartCodeScanStatus::InProgress);
        QCOMPARE(first.records.size(), std::size_t(1));
        QCOMPARE(first.records.front().startCodeOffset, quint64(0));
        QCOMPARE(first.records.front().startCodeLength, quint8(3));
        QCOMPARE(first.records.front().nalUnitOffset, quint64(3));
        QCOMPARE(first.records.front().nalUnitLength, quint64(2));
        QVERIFY(first.records.front().startCode.has_value());
        QVERIFY(first.records.front().nalUnit.has_value());

        const auto second = scanner.scanBatch(1);
        QCOMPARE(second.status, StartCodeScanStatus::Complete);
        QCOMPARE(second.records.size(), std::size_t(1));
        QCOMPARE(second.records.front().startCodeOffset, quint64(5));
        QCOMPARE(second.records.front().startCodeLength, quint8(4));
        QCOMPARE(second.records.front().nalUnitOffset, quint64(9));
        QCOMPARE(second.records.front().nalUnitLength, quint64(1));
        QVERIFY(scanner.finished());

        const auto finished = scanner.scanBatch();
        QCOMPARE(finished.status, StartCodeScanStatus::Complete);
        QVERIFY(finished.records.empty());
    }

    void detectsPrefixAcrossReadWindowBoundary() {
        std::vector<std::byte> data(65534, std::byte{0});
        data.back() = static_cast<std::byte>(0xFF);
        const auto suffix = bytes({0x00, 0x00, 0x01, 0x65});
        data.insert(data.end(), suffix.begin(), suffix.end());
        MemorySource source(std::move(data));
        H264StartCodeScanner scanner(source);

        const auto result = scanner.scanBatch();
        QCOMPARE(result.status, StartCodeScanStatus::Complete);
        QCOMPARE(result.records.size(), std::size_t(1));
        QCOMPARE(result.records.front().startCodeOffset, quint64(65534));
        QCOMPARE(result.records.front().startCodeLength, quint8(3));
        QCOMPARE(result.records.front().nalUnitLength, quint64(1));
    }

    void completesWhenNoPrefixExists() {
        MemorySource source(bytes({0x12, 0x00, 0x00, 0x02, 0x34}));
        H264StartCodeScanner scanner(source);

        const auto result = scanner.scanBatch();
        QCOMPARE(result.status, StartCodeScanStatus::Complete);
        QVERIFY(result.records.empty());
    }

    void observesCancellationBeforeReading() {
        MemorySource source(std::vector<std::byte>(4096, std::byte{0}));
        CancellationSource cancellation;
        QVERIFY(cancellation.requestCancellation());
        H264StartCodeScanner scanner(source, cancellation.token());

        const auto result = scanner.scanBatch();
        QCOMPARE(result.status, StartCodeScanStatus::Cancelled);
        QVERIFY(result.records.empty());
        QCOMPARE(scanner.cursor(), quint64(0));
    }

    void rejectsZeroBatchSizeWithoutAdvancing() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65}));
        H264StartCodeScanner scanner(source);

        const auto result = scanner.scanBatch(0);
        QCOMPARE(result.status, StartCodeScanStatus::InvalidBatchSize);
        QCOMPARE(scanner.cursor(), quint64(0));
        QVERIFY(!result.errorMessage.isEmpty());
    }
};

QTEST_GUILESS_MAIN(H264StartCodeScannerTest)

#include "h264_start_code_scanner_test.moc"
