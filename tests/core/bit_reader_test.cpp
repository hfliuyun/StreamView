#include <streamview/core/bit_reader.h>

#include <QByteArray>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

using streamview::core::BitReadStatus;
using streamview::core::BitReader;
using streamview::core::RandomAccessSource;
using streamview::core::SourceBitAddress;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourceSpan;

namespace {

class ByteArraySource final : public RandomAccessSource {
public:
    explicit ByteArraySource(QByteArray bytes) : bytes_(std::move(bytes)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(bytes_.size());
    }

    [[nodiscard]] QString identity() const override { return QStringLiteral("test-memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (failReads_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected read error")};
        }
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= static_cast<quint64>(bytes_.size())) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }

        const std::size_t available =
            static_cast<std::size_t>(bytes_.size()) - static_cast<std::size_t>(byteOffset);
        const std::size_t count = std::min(available, destination.size());
        std::memcpy(destination.data(),
                    bytes_.constData() + static_cast<qsizetype>(byteOffset),
                    count);
        if (incompleteSuccess_ && count > 0) {
            return {SourceReadStatus::Complete, count - 1, {}};
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

    void setFailReads(bool failReads) noexcept { failReads_ = failReads; }
    void setIncompleteSuccess(bool incompleteSuccess) noexcept {
        incompleteSuccess_ = incompleteSuccess;
    }

private:
    QByteArray bytes_;
    bool failReads_ = false;
    bool incompleteSuccess_ = false;
};

} // namespace

class BitReaderTest final : public QObject {
    Q_OBJECT

private slots:
    void readsMostSignificantBitsFirst() {
        ByteArraySource source(QByteArray::fromHex("b26c"));
        const auto range = SourceSpan::create(SourceBitAddress(0), 16);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        const auto first = reader.readBits(3);
        QVERIFY(first.complete());
        QCOMPARE(first.value, quint64{0b101});
        QCOMPARE(first.bitCount, quint8{3});

        const auto second = reader.readBits(5);
        QVERIFY(second.complete());
        QCOMPARE(second.value, quint64{0b10010});

        const auto third = reader.readBits(8);
        QVERIFY(third.complete());
        QCOMPARE(third.value, quint64{0x6c});
        QCOMPARE(reader.position(), quint64{16});
        QCOMPARE(reader.remainingBits(), quint64{0});
    }

    void readsAnUnaligned64BitValue() {
        ByteArraySource source(QByteArray::fromHex("0123456789abcdef80"));
        const auto range = SourceSpan::create(SourceBitAddress(4), 64);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        const auto result = reader.readBits(64);
        QVERIFY(result.complete());
        QCOMPARE(result.value, quint64{0x123456789abcdef8ULL});
        QCOMPARE(reader.position(), quint64{64});
    }

    void doesNotAdvanceOnInvalidOrBoundedReads() {
        ByteArraySource source(QByteArray::fromHex("ff"));
        const auto range = SourceSpan::create(SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        QVERIFY(reader.readBits(0).status == BitReadStatus::InvalidBitCount);
        QVERIFY(reader.readBits(65).status == BitReadStatus::InvalidBitCount);
        QVERIFY(reader.readBits(9).status == BitReadStatus::EndOfRange);
        QCOMPARE(reader.position(), quint64{0});
        QVERIFY(!reader.seek(9));
        QVERIFY(reader.seek(8));
        QCOMPARE(reader.position(), quint64{8});
    }

    void doesNotAdvanceWhenTheSourceIsTruncated() {
        ByteArraySource source(QByteArray::fromHex("aa"));
        const auto range = SourceSpan::create(SourceBitAddress(0), 16);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        const auto result = reader.readBits(12);
        QVERIFY(result.status == BitReadStatus::EndOfSource);
        QCOMPARE(reader.position(), quint64{0});
    }

    void doesNotAdvanceOnSourceErrors() {
        ByteArraySource source(QByteArray::fromHex("aa"));
        source.setFailReads(true);
        const auto range = SourceSpan::create(SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        const auto result = reader.readBits(1);
        QVERIFY(result.status == BitReadStatus::SourceError);
        QCOMPARE(result.errorMessage, QStringLiteral("injected read error"));
        QCOMPARE(reader.position(), quint64{0});
    }

    void rejectsInconsistentSuccessfulReads() {
        ByteArraySource source(QByteArray::fromHex("aa"));
        source.setIncompleteSuccess(true);
        const auto range = SourceSpan::create(SourceBitAddress(0), 8);
        QVERIFY(range.has_value());
        BitReader reader(source, *range);

        const auto result = reader.readBits(1);
        QVERIFY(result.status == BitReadStatus::SourceError);
        QCOMPARE(reader.position(), quint64{0});
    }
};

QTEST_GUILESS_MAIN(BitReaderTest)

#include "bit_reader_test.moc"
