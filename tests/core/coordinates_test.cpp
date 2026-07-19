#include <streamview/core/coordinates.h>

#include <QTest>

#include <limits>
#include <vector>

using streamview::core::LogicalBitAddress;
using streamview::core::LogicalRange;
using streamview::core::LogicalViewId;
using streamview::core::SourceBitAddress;
using streamview::core::SourceMapping;
using streamview::core::SourceSpan;

class CoordinatesTest final : public QObject {
    Q_OBJECT

private slots:
    void convertsByteAndBitAddresses() {
        const auto address = SourceBitAddress::fromByteAndBit(2, 3);
        QVERIFY(address.has_value());
        QCOMPARE(address->absoluteBitOffset(), quint64{19});
        QCOMPARE(address->byteOffset(), quint64{2});
        QCOMPARE(address->bitOffsetInByte(), quint8{3});
    }

    void rejectsInvalidAndOverflowingAddresses() {
        QVERIFY(!SourceBitAddress::fromByteAndBit(0, 8).has_value());

        constexpr quint64 max = std::numeric_limits<quint64>::max();
        const auto last = SourceBitAddress::fromByteAndBit(max / 8, 7);
        QVERIFY(last.has_value());
        QCOMPARE(last->absoluteBitOffset(), max);
        QVERIFY(!SourceBitAddress::fromByteAndBit((max / 8) + 1, 0).has_value());
    }

    void rejectsOverflowingRanges() {
        constexpr quint64 max = std::numeric_limits<quint64>::max();
        QVERIFY(SourceSpan::create(SourceBitAddress(max), 0).has_value());
        QVERIFY(!SourceSpan::create(SourceBitAddress(max), 1).has_value());

        const LogicalBitAddress logicalStart(LogicalViewId(1), max);
        QVERIFY(LogicalRange::create(logicalStart, 0).has_value());
        QVERIFY(!LogicalRange::create(logicalStart, 1).has_value());
    }

    void resolvesRangesAcrossExcludedSourceBits() {
        const auto first = SourceSpan::create(SourceBitAddress(0), 16);
        const auto second = SourceSpan::create(SourceBitAddress(24), 16);
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());

        const LogicalViewId viewId(7);
        const auto mapping = SourceMapping::create(viewId, {*first, *second});
        QVERIFY(mapping.has_value());
        QCOMPARE(mapping->logicalBitLength(), quint64{32});

        const auto range = LogicalRange::create(LogicalBitAddress(viewId, 8), 16);
        QVERIFY(range.has_value());
        const auto location = mapping->locate(*range);
        QVERIFY(location.has_value());
        QCOMPARE(location->sourceSpans().size(), std::size_t{2});
        QCOMPARE(location->sourceSpans().at(0).start().absoluteBitOffset(), quint64{8});
        QCOMPARE(location->sourceSpans().at(0).bitLength(), quint64{8});
        QCOMPARE(location->sourceSpans().at(1).start().absoluteBitOffset(), quint64{24});
        QCOMPARE(location->sourceSpans().at(1).bitLength(), quint64{8});
    }

    void coalescesAdjacentSpans() {
        const auto first = SourceSpan::create(SourceBitAddress(8), 8);
        const auto second = SourceSpan::create(SourceBitAddress(16), 8);
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());

        const auto mapping = SourceMapping::create(LogicalViewId(3), {*first, *second});
        QVERIFY(mapping.has_value());
        QCOMPARE(mapping->sourceSpans().size(), std::size_t{1});
        QCOMPARE(mapping->sourceSpans().front().start().absoluteBitOffset(), quint64{8});
        QCOMPARE(mapping->sourceSpans().front().bitLength(), quint64{16});
    }

    void rejectsInvalidMappingsAndRanges() {
        const auto first = SourceSpan::create(SourceBitAddress(0), 8);
        const auto overlap = SourceSpan::create(SourceBitAddress(7), 8);
        const auto earlier = SourceSpan::create(SourceBitAddress(0), 8);
        const auto later = SourceSpan::create(SourceBitAddress(16), 8);
        const auto empty = SourceSpan::create(SourceBitAddress(32), 0);
        QVERIFY(first.has_value());
        QVERIFY(overlap.has_value());
        QVERIFY(earlier.has_value());
        QVERIFY(later.has_value());
        QVERIFY(empty.has_value());

        QVERIFY(!SourceMapping::create(LogicalViewId(1), {*first, *overlap}).has_value());
        QVERIFY(!SourceMapping::create(LogicalViewId(1), {*later, *earlier}).has_value());
        QVERIFY(!SourceMapping::create(LogicalViewId(1), {*empty}).has_value());

        const auto mapping = SourceMapping::create(LogicalViewId(1), {*first});
        QVERIFY(mapping.has_value());
        const auto wrongView =
            LogicalRange::create(LogicalBitAddress(LogicalViewId(2), 0), 1);
        const auto outside =
            LogicalRange::create(LogicalBitAddress(LogicalViewId(1), 7), 2);
        QVERIFY(wrongView.has_value());
        QVERIFY(outside.has_value());
        QVERIFY(!mapping->locate(*wrongView).has_value());
        QVERIFY(!mapping->locate(*outside).has_value());
    }
};

QTEST_GUILESS_MAIN(CoordinatesTest)

#include "coordinates_test.moc"
