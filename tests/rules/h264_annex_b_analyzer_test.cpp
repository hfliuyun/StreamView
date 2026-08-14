#include <streamview/core/source_pager.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/h264_annex_b_analyzer.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <vector>

using streamview::core::AnalysisNodeKind;
using streamview::core::CancellationSource;
using streamview::core::MaterializationState;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;
using streamview::core::SourcePager;
using streamview::rules::DslParser;
using streamview::rules::H264AnnexBAnalysisBatch;
using streamview::rules::H264AnnexBAnalysisStatus;
using streamview::rules::H264AnnexBAnalyzer;
using streamview::rules::H264EbspRbspMapLimits;
using streamview::rules::h264AnnexBRuleSource;

namespace {

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

[[nodiscard]] std::vector<bool> unsignedExpGolombBits(quint64 value) {
    const quint64 codeNum = value + 1;
    std::size_t codeBits = 0;
    for (quint64 remaining = codeNum; remaining != 0; remaining >>= 1) {
        ++codeBits;
    }
    std::vector<bool> result(codeBits - 1, false);
    for (std::size_t index = codeBits; index != 0; --index) {
        result.push_back(((codeNum >> (index - 1)) & 1U) != 0);
    }
    return result;
}

void appendFixedBits(std::vector<bool>& bits, quint64 value, std::size_t width) {
    for (std::size_t remaining = width; remaining != 0; --remaining) {
        bits.push_back(((value >> (remaining - 1)) & 1U) != 0);
    }
}

void appendUnsignedExpGolomb(std::vector<bool>& bits, quint64 value) {
    const auto encoded = unsignedExpGolombBits(value);
    bits.insert(bits.end(), encoded.begin(), encoded.end());
}

void appendSignedExpGolomb(std::vector<bool>& bits, qint64 value) {
    const quint64 codeNum = value <= 0 ? static_cast<quint64>(-value) * 2
                                      : static_cast<quint64>(value) * 2 - 1;
    appendUnsignedExpGolomb(bits, codeNum);
}

[[nodiscard]] std::vector<std::byte> packAnnexBNal(unsigned int header,
                                                   std::vector<bool> rbspBits,
                                                   bool appendTrailingBits = true) {
    if (appendTrailingBits) {
        rbspBits.push_back(true);
        while (rbspBits.size() % 8 != 0) {
            rbspBits.push_back(false);
        }
    }
    Q_ASSERT(rbspBits.size() % 8 == 0);

    auto result = bytes({0x00, 0x00, 0x01, header});
    for (std::size_t offset = 0; offset < rbspBits.size(); offset += 8) {
        unsigned int value = 0;
        for (std::size_t bit = 0; bit < 8; ++bit) {
            value = (value << 1U) |
                    static_cast<unsigned int>(rbspBits.at(offset + bit));
        }
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

void appendNal(std::vector<std::byte>& stream, const std::vector<std::byte>& nal) {
    stream.insert(stream.end(), nal.begin(), nal.end());
}

void appendPocSequenceParameterSetPrefix(std::vector<bool>& bits, quint64 pocType) {
    appendFixedBits(bits, 66, 8);
    appendFixedBits(bits, 0, 8);
    appendFixedBits(bits, 30, 8);
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, pocType);
}

void appendPocSequenceParameterSetSuffix(std::vector<bool>& bits,
                                         bool frameMbsOnly = true) {
    appendUnsignedExpGolomb(bits, 1);
    appendFixedBits(bits, 0, 1);
    appendUnsignedExpGolomb(bits, 19);
    appendUnsignedExpGolomb(bits, 14);
    appendFixedBits(bits, frameMbsOnly ? 1 : 0, 1);
    if (!frameMbsOnly) {
        appendFixedBits(bits, 0, 1);
    }
    appendFixedBits(bits, 1, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
}

[[nodiscard]] std::vector<std::byte> sequenceParameterSetForProfile(
    quint64 profileIdc,
    quint64 sequenceParameterSetId = 0,
    quint64 log2MaxFrameNumMinus4 = 0) {
    std::vector<bool> bits;
    appendFixedBits(bits, profileIdc, 8);
    appendFixedBits(bits, 0, 8);
    appendFixedBits(bits, profileIdc == 100 ? 31 : 30, 8);
    appendUnsignedExpGolomb(bits, sequenceParameterSetId);
    if (profileIdc == 100) {
        appendUnsignedExpGolomb(bits, 1);
        appendUnsignedExpGolomb(bits, 0);
        appendUnsignedExpGolomb(bits, 0);
        appendFixedBits(bits, 0, 1);
        appendFixedBits(bits, 0, 1);
    }
    appendUnsignedExpGolomb(bits, log2MaxFrameNumMinus4);
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 0);
    appendPocSequenceParameterSetSuffix(bits);
    return packAnnexBNal(0x67, std::move(bits));
}

[[nodiscard]] std::vector<std::byte> pictureOrderCountSequenceParameterSet(
    quint64 pocType,
    bool deltaAlwaysZero = false,
    qint64 nonReferenceOffset = 0,
    qint64 topToBottomOffset = 0,
    quint64 cycleCount = 0,
    std::initializer_list<qint64> cycleOffsets = {},
    bool frameMbsOnly = true) {
    std::vector<bool> bits;
    appendPocSequenceParameterSetPrefix(bits, pocType);
    if (pocType == 0) {
        appendUnsignedExpGolomb(bits, 0);
    } else if (pocType == 1) {
        appendFixedBits(bits, deltaAlwaysZero ? 1 : 0, 1);
        appendSignedExpGolomb(bits, nonReferenceOffset);
        appendSignedExpGolomb(bits, topToBottomOffset);
        appendUnsignedExpGolomb(bits, cycleCount);
        for (const qint64 offset : cycleOffsets) {
            appendSignedExpGolomb(bits, offset);
        }
    }
    appendPocSequenceParameterSetSuffix(bits, frameMbsOnly);
    return packAnnexBNal(0x67, std::move(bits));
}

void appendPictureParameterSetBase(std::vector<bool>& bits,
                                   bool bottomDeltaPresent,
                                   quint64 pictureParameterSetId = 0,
                                   quint64 sequenceParameterSetId = 0,
                                   bool deblockingControlPresent = false) {
    appendUnsignedExpGolomb(bits, pictureParameterSetId);
    appendUnsignedExpGolomb(bits, sequenceParameterSetId);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, bottomDeltaPresent ? 1 : 0, 1);
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 0);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 2);
    appendSignedExpGolomb(bits, 0);
    appendSignedExpGolomb(bits, 0);
    appendSignedExpGolomb(bits, 0);
    appendFixedBits(bits, deblockingControlPresent ? 1 : 0, 1);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
}

[[nodiscard]] std::vector<std::byte> pictureParameterSet(
    bool bottomDeltaPresent,
    quint64 pictureParameterSetId = 0,
    quint64 sequenceParameterSetId = 0) {
    std::vector<bool> bits;
    appendPictureParameterSetBase(
        bits, bottomDeltaPresent, pictureParameterSetId, sequenceParameterSetId);
    return packAnnexBNal(0x68, std::move(bits));
}

[[nodiscard]] std::vector<std::byte> pictureParameterSetWithDeblockingControl() {
    std::vector<bool> bits;
    appendPictureParameterSetBase(bits, false, 0, 0, true);
    return packAnnexBNal(0x68, std::move(bits));
}

[[nodiscard]] std::vector<std::byte> deblockingOffsetSlice(bool idr,
                                                           qint64 alphaOffset,
                                                           qint64 betaOffset) {
    std::vector<bool> bits;
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 2);
    appendUnsignedExpGolomb(bits, 0);
    appendFixedBits(bits, 0, 4);
    if (idr) {
        appendUnsignedExpGolomb(bits, 0);
    }
    appendFixedBits(bits, 3, 4);
    if (idr) {
        appendFixedBits(bits, 0, 1);
        appendFixedBits(bits, 0, 1);
    }
    appendSignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 0);
    appendSignedExpGolomb(bits, alphaOffset);
    appendSignedExpGolomb(bits, betaOffset);
    return packAnnexBNal(idr ? 0x65 : 0x01, std::move(bits));
}

[[nodiscard]] std::vector<std::byte> pictureParameterSetWithExtension(
    bool transform8x8,
    qint64 secondChromaQpOffset,
    bool scalingMatrixPresent = false,
    quint64 pictureParameterSetId = 0,
    quint64 sequenceParameterSetId = 0) {
    std::vector<bool> bits;
    appendPictureParameterSetBase(
        bits, false, pictureParameterSetId, sequenceParameterSetId);
    appendFixedBits(bits, transform8x8 ? 1 : 0, 1);
    appendFixedBits(bits, scalingMatrixPresent ? 1 : 0, 1);
    appendSignedExpGolomb(bits, secondChromaQpOffset);
    return packAnnexBNal(0x68, std::move(bits));
}

[[nodiscard]] std::vector<std::byte> pictureParameterSetWithoutSecondOffset() {
    std::vector<bool> bits;
    appendPictureParameterSetBase(bits, false, 7, 0);
    appendFixedBits(bits, 0, 1);
    appendFixedBits(bits, 0, 1);
    Q_ASSERT(bits.size() % 8 == 0);
    return packAnnexBNal(0x68, std::move(bits), false);
}

[[nodiscard]] std::vector<std::byte> pictureOrderCountSlice(
    bool idr,
    quint64 pocType,
    std::initializer_list<qint64> typeOneDeltas = {},
    bool fieldPic = false,
    quint64 idrPictureId = 0,
    quint64 frameNum = 0,
    quint64 frameNumBits = 4,
    quint64 pictureParameterSetId = 0) {
    std::vector<bool> bits;
    appendUnsignedExpGolomb(bits, 0);
    appendUnsignedExpGolomb(bits, 2);
    appendUnsignedExpGolomb(bits, pictureParameterSetId);
    appendFixedBits(bits, frameNum, frameNumBits);
    if (fieldPic) {
        appendFixedBits(bits, 1, 1);
        appendFixedBits(bits, 0, 1);
    }
    if (idr) {
        appendUnsignedExpGolomb(bits, idrPictureId);
    }
    if (pocType == 0) {
        appendFixedBits(bits, 3, 4);
    } else if (pocType == 1) {
        for (const qint64 delta : typeOneDeltas) {
            appendSignedExpGolomb(bits, delta);
        }
    }
    if (idr) {
        appendFixedBits(bits, 0, 1);
        appendFixedBits(bits, 0, 1);
    }
    appendSignedExpGolomb(bits, 0);
    return packAnnexBNal(idr ? 0x65 : 0x01, std::move(bits));
}

void appendFfCoded(std::vector<bool>& bits, quint64 value) {
    while (value >= 0xff) {
        appendFixedBits(bits, 0xff, 8);
        value -= 0xff;
    }
    appendFixedBits(bits, value, 8);
}

struct SeiMessagePayload {
    quint64 payloadType = 0;
    std::vector<quint8> payloadBytes;
};

[[nodiscard]] std::vector<std::byte> packSeiNal(
    const std::vector<SeiMessagePayload>& messages,
    bool appendTrailingBits = true) {
    std::vector<bool> bits;
    for (const auto& msg : messages) {
        appendFfCoded(bits, msg.payloadType);
        appendFfCoded(bits, msg.payloadBytes.size());
        for (const quint8 byteVal : msg.payloadBytes) {
            appendFixedBits(bits, byteVal, 8);
        }
    }
    return packAnnexBNal(0x06, std::move(bits), appendTrailingBits);
}

[[nodiscard]] std::vector<std::byte> replaceCodewordBeforeNextNal(
    std::vector<std::byte> data,
    std::size_t sourceBitOffset,
    std::size_t oldBitLength,
    quint64 value) {
    const auto prefix = bytes({0x00, 0x00, 0x01});
    const auto nextPrefix = std::search(
        data.begin() + static_cast<std::ptrdiff_t>((sourceBitOffset + oldBitLength + 7) / 8),
        data.end(),
        prefix.begin(),
        prefix.end());
    Q_ASSERT(nextPrefix != data.end());
    const std::size_t nextPrefixBit =
        static_cast<std::size_t>(std::distance(data.begin(), nextPrefix)) * 8;

    std::vector<bool> bits;
    bits.reserve(data.size() * 8);
    for (const std::byte byte : data) {
        const unsigned int valueByte = std::to_integer<unsigned int>(byte);
        for (int bit = 7; bit >= 0; --bit) {
            bits.push_back(((valueByte >> bit) & 1U) != 0);
        }
    }
    const std::vector<bool> replacement = unsignedExpGolombBits(value);
    Q_ASSERT(replacement.size() >= oldBitLength);
    bits.erase(bits.begin() + static_cast<std::ptrdiff_t>(sourceBitOffset),
               bits.begin() + static_cast<std::ptrdiff_t>(sourceBitOffset + oldBitLength));
    bits.insert(bits.begin() + static_cast<std::ptrdiff_t>(sourceBitOffset),
                replacement.begin(),
                replacement.end());
    const std::size_t growth = replacement.size() - oldBitLength;
    bits.erase(bits.begin() + static_cast<std::ptrdiff_t>(nextPrefixBit),
               bits.begin() + static_cast<std::ptrdiff_t>(nextPrefixBit + growth));
    Q_ASSERT(bits.size() == data.size() * 8);

    for (std::size_t byteIndex = 0; byteIndex < data.size(); ++byteIndex) {
        unsigned int valueByte = 0;
        for (std::size_t bit = 0; bit < 8; ++bit) {
            valueByte = (valueByte << 1U) |
                        static_cast<unsigned int>(bits.at(byteIndex * 8 + bit));
        }
        data.at(byteIndex) = static_cast<std::byte>(valueByte);
    }
    return data;
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

class FailAfterFirstReadSource final : public RandomAccessSource {
public:
    [[nodiscard]] quint64 sizeBytes() const noexcept override { return 4; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("failing-memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (readCount_ != 0) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected read failure")};
        }
        if (destination.empty()) {
            return {SourceReadStatus::Complete, 0, {}};
        }
        ++readCount_;
        const auto data = bytes({0x00, 0x00, 0x01, 0x65});
        if (byteOffset >= data.size()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto offset = static_cast<std::size_t>(byteOffset);
        const std::size_t count = std::min(destination.size(), data.size() - offset);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(count),
                    destination.begin());
        return {SourceReadStatus::Complete, count, {}};
    }

private:
    mutable std::size_t readCount_ = 0;
};

class FailAtOffsetSource final : public RandomAccessSource {
public:
    FailAtOffsetSource(std::vector<std::byte> data, quint64 failingOffset)
        : data_(std::move(data)), failingOffset_(failingOffset) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("offset-failing-memory");
    }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (byteOffset == failingOffset_) {
            return {SourceReadStatus::Error, 0, QStringLiteral("injected payload read failure")};
        }
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
    quint64 failingOffset_ = 0;
};

class CancellingSource final : public RandomAccessSource {
public:
    explicit CancellingSource(CancellationSource& cancellation)
        : data_(2048, std::byte{0xFF}), cancellation_(&cancellation) {
        const auto first = bytes({0x00, 0x00, 0x01, 0x65});
        std::copy(first.begin(), first.end(), data_.begin());
        const auto second = bytes({0x00, 0x00, 0x01, 0x4c});
        std::copy(second.begin(), second.end(), data_.begin() + 10);
    }

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("cancelling-memory"); }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (!cancellationRequested_) {
            cancellationRequested_ = true;
            (void)cancellation_->requestCancellation();
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
    CancellationSource* cancellation_ = nullptr;
    mutable bool cancellationRequested_ = false;
};

class ArmedCancellingSource final : public RandomAccessSource {
public:
    explicit ArmedCancellingSource(std::vector<std::byte> data) : data_(std::move(data)) {}

    void arm(quint64 byteOffset, CancellationSource& cancellation) noexcept {
        cancellationOffset_ = byteOffset;
        cancellation_ = &cancellation;
        armed_ = true;
    }

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return static_cast<quint64>(data_.size());
    }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("armed-cancelling-memory");
    }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (armed_ && byteOffset == cancellationOffset_) {
            armed_ = false;
            (void)cancellation_->requestCancellation();
        }
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
    mutable CancellationSource* cancellation_ = nullptr;
    mutable quint64 cancellationOffset_ = 0;
    mutable bool armed_ = false;
};

class HundredGigabyteSparseSource final : public RandomAccessSource {
public:
    HundredGigabyteSparseSource() : prefix_(1040, std::byte{0xFF}) {
        const auto put = [this](std::size_t offset,
                                std::initializer_list<unsigned int> values) {
            const auto valueBytes = bytes(values);
            std::copy(valueBytes.begin(),
                      valueBytes.end(),
                      prefix_.begin() + static_cast<std::ptrdiff_t>(offset));
        };
        put(0, {0x00, 0x00, 0x01, 0x65, 0x12});
        put(10, {0x00, 0x00, 0x01, 0x6E});
        put(1030, {0x00, 0x00, 0x01, 0x0c});
    }

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return 100ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    [[nodiscard]] QString identity() const override {
        return QStringLiteral("hundred-gigabyte-sparse");
    }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (byteOffset >= sizeBytes()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto count = static_cast<std::size_t>(std::min<quint64>(
            sizeBytes() - byteOffset, static_cast<quint64>(destination.size())));
        maximumRequestSize_ = std::max(maximumRequestSize_, count);
        highestReadEnd_ = std::max(highestReadEnd_, byteOffset + static_cast<quint64>(count));
        for (std::size_t index = 0; index < count; ++index) {
            const quint64 absolute = byteOffset + static_cast<quint64>(index);
            destination[index] = absolute < static_cast<quint64>(prefix_.size())
                                     ? prefix_.at(static_cast<std::size_t>(absolute))
                                     : std::byte{0xFF};
        }
        return {count == destination.size() ? SourceReadStatus::Complete
                                            : SourceReadStatus::EndOfSource,
                count,
                {}};
    }

    [[nodiscard]] std::size_t maximumRequestSize() const noexcept {
        return maximumRequestSize_;
    }
    [[nodiscard]] quint64 highestReadEnd() const noexcept { return highestReadEnd_; }

private:
    std::vector<std::byte> prefix_;
    mutable std::size_t maximumRequestSize_ = 0;
    mutable quint64 highestReadEnd_ = 0;
};

} // namespace

class H264AnnexBAnalyzerTest final : public QObject {
    Q_OBJECT

private slots:
    void loadsBundledRule() {
        QString errorMessage;
        const QString source = h264AnnexBRuleSource(&errorMessage);

        QVERIFY2(!source.isEmpty(), qPrintable(errorMessage));
        const auto parsed = DslParser::parse(source);
        QVERIFY(parsed.succeeded());
        QCOMPARE(parsed.program.structs.size(), std::size_t(7));
        QCOMPARE(parsed.program.structs.at(0).name, QStringLiteral("NalUnitHeader"));
        QCOMPARE(parsed.program.structs.at(0).items.size(), std::size_t(4));
        QCOMPARE(parsed.program.structs.at(0).items.back().kind,
                 streamview::rules::DslStructItemKind::Assertion);
        QCOMPARE(parsed.program.structs.at(0).items.back().assertion.anchorFieldName,
                 QStringLiteral("nal_ref_idc"));
        QCOMPARE(parsed.program.structs.at(1).name,
                 QStringLiteral("AccessUnitDelimiterRbsp"));
        QCOMPARE(parsed.program.structs.at(1).items.size(), std::size_t(2));
        QCOMPARE(parsed.program.structs.at(1).items.at(1).kind,
                 streamview::rules::DslStructItemKind::RbspTrailingBits);
        QCOMPARE(parsed.program.structs.at(2).name,
                 QStringLiteral("SequenceParameterSetRbsp"));
        QCOMPARE(parsed.program.structs.at(3).name,
                 QStringLiteral("PictureParameterSetRbsp"));
        QCOMPARE(parsed.program.structs.at(3).items.back().kind,
                 streamview::rules::DslStructItemKind::RbspTrailingBits);
        QCOMPARE(parsed.program.structs.at(4).name,
                 QStringLiteral("IdrSliceLayerWithoutPartitioningRbsp"));
        QCOMPARE(parsed.program.structs.at(4).items.back().kind,
                 streamview::rules::DslStructItemKind::CompressedPayload);
        QCOMPARE(parsed.program.structs.at(5).name,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp"));
        QCOMPARE(parsed.program.structs.at(5).items.back().kind,
                 streamview::rules::DslStructItemKind::CompressedPayload);
        QCOMPARE(parsed.program.structs.at(6).name,
                 QStringLiteral("SeiRbsp"));
        QCOMPARE(parsed.program.structs.at(6).items.back().kind,
                 streamview::rules::DslStructItemKind::RbspTrailingBits);
        QCOMPARE(parsed.program.scans.size(), std::size_t(1));
        QCOMPARE(parsed.program.entry.targetName, QStringLiteral("nal_units"));
        QVERIFY(parsed.program.payloadDispatch.has_value());
        const auto& dispatch = *parsed.program.payloadDispatch;
        QCOMPARE(dispatch.viewKind, QStringLiteral("rbsp"));
        QCOMPARE(dispatch.sequenceName, QStringLiteral("nal_units"));
        QCOMPARE(dispatch.controllerFieldName, QStringLiteral("nal_unit_type"));
        QCOMPARE(dispatch.cases.size(), std::size_t(8));
        QCOMPARE(dispatch.cases.at(0).value, quint64(1));
        QCOMPARE(dispatch.cases.at(0).kind,
                 streamview::rules::DslPayloadCaseKind::Structure);
        QCOMPARE(dispatch.cases.at(0).targetName,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp"));
        QCOMPARE(dispatch.cases.at(2).value, quint64(6));
        QCOMPARE(dispatch.cases.at(2).kind,
                 streamview::rules::DslPayloadCaseKind::Structure);
        QCOMPARE(dispatch.cases.at(2).targetName,
                 QStringLiteral("SeiRbsp"));
    }

    void decodesTheSupportedBaselineSequenceParameterSetPayload() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->children().size(), std::size_t(1));
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->name(), QStringLiteral("SequenceParameterSetRbsp"));
        QCOMPARE(sps->state(), MaterializationState::Materialized);
        QCOMPARE(sps->metadata().specification->clause, QStringLiteral("7.3.2.1.1"));

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto profile = fieldNamed(QStringLiteral("profile_idc"));
        const auto level = fieldNamed(QStringLiteral("level_idc"));
        const auto spsId = fieldNamed(QStringLiteral("seq_parameter_set_id"));
        const auto picOrderType = fieldNamed(QStringLiteral("pic_order_cnt_type"));
        const auto width = fieldNamed(QStringLiteral("pic_width_in_mbs_minus1"));
        const auto height = fieldNamed(QStringLiteral("pic_height_in_map_units_minus1"));
        QVERIFY(profile.has_value());
        QVERIFY(level.has_value());
        QVERIFY(spsId.has_value());
        QVERIFY(picOrderType.has_value());
        QVERIFY(width.has_value());
        QVERIFY(height.has_value());
        QCOMPARE(profile->value().toULongLong(), quint64(66));
        QCOMPARE(level->value().toULongLong(), quint64(30));
        QCOMPARE(spsId->value().toULongLong(), quint64(0));
        QCOMPARE(picOrderType->value().toULongLong(), quint64(0));
        QCOMPARE(width->value().toULongLong(), quint64(19));
        QCOMPARE(height->value().toULongLong(), quint64(14));
        QCOMPARE(profile->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));

        const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(stop.has_value());
        QCOMPARE(stop->name(), QStringLiteral("rbsp_stop_one_bit"));
        QCOMPARE(stop->value().toULongLong(), quint64(1));
    }

    void decodesTheSupportedHighSequenceParameterSetSubset() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x64, 0x00, 0x1f, 0xac, 0xe8, 0x14, 0x1f, 0x90}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto profile = fieldNamed(QStringLiteral("profile_idc"));
        const auto chroma = fieldNamed(QStringLiteral("chroma_format_idc"));
        const auto depth = fieldNamed(QStringLiteral("bit_depth_luma_minus8"));
        const auto scaling = fieldNamed(QStringLiteral("seq_scaling_matrix_present_flag"));
        QVERIFY(profile.has_value());
        QVERIFY(chroma.has_value());
        QVERIFY(depth.has_value());
        QVERIFY(scaling.has_value());
        QCOMPARE(profile->value().toULongLong(), quint64(100));
        QCOMPARE(chroma->value().toULongLong(), quint64(1));
        QCOMPARE(depth->value().toULongLong(), quint64(0));
        QCOMPARE(scaling->value().toULongLong(), quint64(0));
    }

    void decodesPictureOrderCountTypeOneSequenceParameterSetCycle() {
        MemorySource source(pictureOrderCountSequenceParameterSet(
            1, false, -1, 1, 1, {-2}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Materialized);
        QVERIFY(sps->diagnostics().empty());

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                 : analyzer->tree().node(*found);
        };
        const auto type = fieldNamed(QStringLiteral("pic_order_cnt_type"));
        const auto alwaysZero =
            fieldNamed(QStringLiteral("delta_pic_order_always_zero_flag"));
        const auto nonReference = fieldNamed(QStringLiteral("offset_for_non_ref_pic"));
        const auto topToBottom =
            fieldNamed(QStringLiteral("offset_for_top_to_bottom_field"));
        const auto count =
            fieldNamed(QStringLiteral("num_ref_frames_in_pic_order_cnt_cycle"));
        const auto cycleOffset = fieldNamed(QStringLiteral("offset_for_ref_frame[0]"));
        const auto effectiveLsb = fieldNamed(
            QStringLiteral("effective_log2_max_pic_order_cnt_lsb_minus4"));
        const auto effectiveAlwaysZero = fieldNamed(
            QStringLiteral("effective_delta_pic_order_always_zero_flag"));
        QVERIFY(type.has_value());
        QVERIFY(alwaysZero.has_value());
        QVERIFY(nonReference.has_value());
        QVERIFY(topToBottom.has_value());
        QVERIFY(count.has_value());
        QVERIFY(cycleOffset.has_value());
        QVERIFY(effectiveLsb.has_value());
        QVERIFY(effectiveAlwaysZero.has_value());
        QVERIFY(!fieldNamed(QStringLiteral("log2_max_pic_order_cnt_lsb_minus4"))
                     .has_value());
        QCOMPARE(type->value().toULongLong(), quint64(1));
        QCOMPARE(type->metadata().typeName, QStringLiteral("PicOrderCntType"));
        QCOMPARE(alwaysZero->value().toULongLong(), quint64(0));
        QCOMPARE(nonReference->value().toLongLong(), qlonglong(-1));
        QCOMPARE(topToBottom->value().toLongLong(), qlonglong(1));
        QCOMPARE(count->value().toULongLong(), quint64(1));
        QCOMPARE(cycleOffset->value().toLongLong(), qlonglong(-2));
        QCOMPARE(effectiveLsb->value().toULongLong(), quint64(0));
        QCOMPARE(effectiveAlwaysZero->value().toULongLong(), quint64(0));
        QVERIFY(!effectiveLsb->location().has_value());
        QVERIFY(!effectiveAlwaysZero->location().has_value());

        const std::vector sourceFields{
            std::pair{type, std::pair{quint64(58), quint64(3)}},
            std::pair{alwaysZero, std::pair{quint64(61), quint64(1)}},
            std::pair{nonReference, std::pair{quint64(62), quint64(3)}},
            std::pair{topToBottom, std::pair{quint64(65), quint64(3)}},
            std::pair{count, std::pair{quint64(68), quint64(3)}},
            std::pair{cycleOffset, std::pair{quint64(71), quint64(5)}},
        };
        for (const auto& [field, expected] : sourceFields) {
            QCOMPARE(field->location()->sourceSpans().front().start().absoluteBitOffset(),
                     expected.first);
            QCOMPARE(field->location()->logicalRange().bitLength(), expected.second);
        }
    }

    void decodesTwoPictureOrderCountTypeOneDeltasInAnIdrSlice() {
        const auto sps = pictureOrderCountSequenceParameterSet(1, false);
        const auto pps = pictureParameterSet(true);
        const auto sliceNal = pictureOrderCountSlice(true, 1, {-1, 1});
        std::vector<std::byte> stream;
        appendNal(stream, sps);
        appendNal(stream, pps);
        appendNal(stream, sliceNal);
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x0a}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const auto count = fieldNamed(QStringLiteral("delta_pic_order_cnt_count"));
        const auto first = fieldNamed(QStringLiteral("delta_pic_order_cnt[0]"));
        const auto second = fieldNamed(QStringLiteral("delta_pic_order_cnt[1]"));
        QVERIFY(count.has_value());
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QVERIFY(!fieldNamed(QStringLiteral("pic_order_cnt_lsb")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("has_delta_pic_order_cnt_bottom")).has_value());
        QCOMPARE(count->value().toULongLong(), quint64(2));
        QVERIFY(!count->location().has_value());
        QCOMPARE(first->value().toLongLong(), qlonglong(-1));
        QCOMPARE(second->value().toLongLong(), qlonglong(1));
        const quint64 sliceRbspStart = static_cast<quint64>(sps.size() + pps.size() + 4) * 8;
        QCOMPARE(first->location()->sourceSpans().front().start().absoluteBitOffset(),
                 sliceRbspStart + 10);
        QCOMPARE(first->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(second->location()->sourceSpans().front().start().absoluteBitOffset(),
                 sliceRbspStart + 13);
        QCOMPARE(second->location()->logicalRange().bitLength(), quint64(3));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesOnePictureOrderCountTypeOneDeltaInANonIdrSlice() {
        const auto sps = pictureOrderCountSequenceParameterSet(1, false);
        const auto pps = pictureParameterSet(false);
        std::vector<std::byte> stream;
        appendNal(stream, sps);
        appendNal(stream, pps);
        appendNal(stream, pictureOrderCountSlice(false, 1, {2}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const auto count = fieldNamed(QStringLiteral("delta_pic_order_cnt_count"));
        const auto delta = fieldNamed(QStringLiteral("delta_pic_order_cnt[0]"));
        QVERIFY(count.has_value());
        QVERIFY(delta.has_value());
        QCOMPARE(count->value().toULongLong(), quint64(1));
        QCOMPARE(delta->value().toLongLong(), qlonglong(2));
        QVERIFY(!fieldNamed(QStringLiteral("delta_pic_order_cnt[1]")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("pic_order_cnt_lsb")).has_value());
        QVERIFY(slice->diagnostics().empty());
    }

    void suppressesSecondPictureOrderCountDeltaForATypeOneFieldPicture() {
        const auto sps = pictureOrderCountSequenceParameterSet(
            1, false, 0, 0, 0, {}, false);
        const auto pps = pictureParameterSet(true);
        std::vector<std::byte> stream;
        appendNal(stream, sps);
        appendNal(stream, pps);
        appendNal(stream, pictureOrderCountSlice(true, 1, {2}, true));
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x0a}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));

        const auto spsNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(spsNal.has_value());
        const auto spsRbsp = analyzer->tree().node(spsNal->children().at(2));
        QVERIFY(spsRbsp.has_value());
        const auto spsNode = analyzer->tree().node(spsRbsp->children().front());
        QVERIFY(spsNode.has_value());
        const auto spsFieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                spsNode->children().begin(), spsNode->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == spsNode->children().end() ? std::nullopt
                                                      : analyzer->tree().node(*found);
        };
        const auto frameMbsOnly = spsFieldNamed(QStringLiteral("frame_mbs_only_flag"));
        QVERIFY(frameMbsOnly.has_value());
        QCOMPARE(frameMbsOnly->value().toULongLong(), quint64(0));
        QVERIFY(!spsFieldNamed(QStringLiteral("offset_for_ref_frame[0]")).has_value());

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());
        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                    : analyzer->tree().node(*found);
        };
        const auto fieldPicFlag = fieldNamed(QStringLiteral("field_pic_flag"));
        const auto bottomFieldFlag = fieldNamed(QStringLiteral("bottom_field_flag"));
        const auto count = fieldNamed(QStringLiteral("delta_pic_order_cnt_count"));
        const auto delta = fieldNamed(QStringLiteral("delta_pic_order_cnt[0]"));
        QVERIFY(fieldPicFlag.has_value());
        QVERIFY(bottomFieldFlag.has_value());
        QVERIFY(count.has_value());
        QVERIFY(delta.has_value());
        QCOMPARE(fieldPicFlag->value().toULongLong(), quint64(1));
        QCOMPARE(bottomFieldFlag->value().toULongLong(), quint64(0));
        QCOMPARE(count->value().toULongLong(), quint64(1));
        QVERIFY(!count->location().has_value());
        QCOMPARE(delta->value().toLongLong(), qlonglong(2));
        QVERIFY(!fieldNamed(QStringLiteral("delta_pic_order_cnt[1]")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("pic_order_cnt_lsb")).has_value());

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void omitsTypeOneSliceDeltasWhenTheSequenceInfersZero() {
        const auto sps = pictureOrderCountSequenceParameterSet(1, true);
        const auto pps = pictureParameterSet(true);
        std::vector<std::byte> stream;
        appendNal(stream, sps);
        appendNal(stream, pps);
        appendNal(stream, pictureOrderCountSlice(true, 1));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        for (const auto childId : slice->children()) {
            const auto child = analyzer->tree().node(childId);
            QVERIFY(child.has_value());
            QVERIFY(!child->name().startsWith(QStringLiteral("delta_pic_order_cnt")));
            QVERIFY(child->name() != QStringLiteral("pic_order_cnt_lsb"));
        }
        QVERIFY(slice->diagnostics().empty());
    }

    void omitsPictureOrderSyntaxFromTypeTwoIdrAndNonIdrSlices() {
        const auto sps = pictureOrderCountSequenceParameterSet(2);
        const auto pps = pictureParameterSet(true);
        std::vector<std::byte> stream;
        appendNal(stream, sps);
        appendNal(stream, pps);
        appendNal(stream, pictureOrderCountSlice(true, 2));
        appendNal(stream, pictureOrderCountSlice(false, 2));
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x0a}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(5));
        const auto spsNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(spsNal.has_value());
        const auto spsRbsp = analyzer->tree().node(spsNal->children().at(2));
        QVERIFY(spsRbsp.has_value());
        const auto spsNode = analyzer->tree().node(spsRbsp->children().front());
        QVERIFY(spsNode.has_value());
        QCOMPARE(spsNode->state(), MaterializationState::Materialized);
        for (const auto childId : spsNode->children()) {
            const auto child = analyzer->tree().node(childId);
            QVERIFY(child.has_value());
            QVERIFY(child->name() != QStringLiteral("log2_max_pic_order_cnt_lsb_minus4"));
            QVERIFY(child->name() != QStringLiteral("delta_pic_order_always_zero_flag"));
            QVERIFY(child->name() != QStringLiteral("offset_for_non_ref_pic"));
            QVERIFY(child->name() != QStringLiteral("offset_for_top_to_bottom_field"));
            QVERIFY(child->name() !=
                    QStringLiteral("num_ref_frames_in_pic_order_cnt_cycle"));
        }

        for (const std::size_t nalIndex : {std::size_t(2), std::size_t(3)}) {
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(nalIndex));
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto slice = analyzer->tree().node(rbsp->children().front());
            QVERIFY(slice.has_value());
            QCOMPARE(slice->state(), MaterializationState::Materialized);
            QVERIFY(slice->diagnostics().empty());
            for (const auto childId : slice->children()) {
                const auto child = analyzer->tree().node(childId);
                QVERIFY(child.has_value());
                QVERIFY(child->name() != QStringLiteral("pic_order_cnt_lsb"));
                QVERIFY(child->name() != QStringLiteral("has_delta_pic_order_cnt_bottom"));
                QVERIFY(!child->name().startsWith(QStringLiteral("delta_pic_order_cnt")));
            }
        }

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void warnsOnOutOfRangeSequenceParameterSetFrameNumberWrap() {
        // log2_max_frame_num_minus4 carries ue(13), one above the 7.4.2.1.1 maximum,
        // which shifts every following field six bits later without invalidating them.
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0x8e, 0xd0, 0x28, 0x3f, 0x20}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Materialized);
        QVERIFY(sps->diagnostics().empty());

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto frameNum = fieldNamed(QStringLiteral("log2_max_frame_num_minus4"));
        QVERIFY(frameNum.has_value());
        QCOMPARE(frameNum->state(), MaterializationState::Materialized);
        QCOMPARE(frameNum->value().toULongLong(), quint64(13));
        QCOMPARE(frameNum->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = frameNum->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Field value is above its @range maximum"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("SequenceParameterSetRbsp.log2_max_frame_num_minus4"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(57));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(7));

        // Every field after the violation keeps its conformant value and stays clean.
        const auto lsb = fieldNamed(QStringLiteral("log2_max_pic_order_cnt_lsb_minus4"));
        const auto width = fieldNamed(QStringLiteral("pic_width_in_mbs_minus1"));
        const auto height = fieldNamed(QStringLiteral("pic_height_in_map_units_minus1"));
        const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(lsb.has_value());
        QVERIFY(width.has_value());
        QVERIFY(height.has_value());
        QVERIFY(stop.has_value());
        QCOMPARE(lsb->value().toULongLong(), quint64(0));
        QVERIFY(lsb->diagnostics().empty());
        QCOMPARE(width->value().toULongLong(), quint64(19));
        QCOMPARE(height->value().toULongLong(), quint64(14));
        QCOMPARE(stop->value().toULongLong(), quint64(1));
    }

    void decodesTheMinimalVideoUsabilityInformationCore() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xd0, 0x04}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto vuiPresent = fieldNamed(QStringLiteral("vui_parameters_present_flag"));
        const auto aspectPresent = fieldNamed(QStringLiteral("aspect_ratio_info_present_flag"));
        const auto nalHrd = fieldNamed(QStringLiteral("nal_hrd_parameters_present_flag"));
        const auto vclHrd = fieldNamed(QStringLiteral("vcl_hrd_parameters_present_flag"));
        const auto picStruct = fieldNamed(QStringLiteral("pic_struct_present_flag"));
        const auto restriction = fieldNamed(QStringLiteral("bitstream_restriction_flag"));
        const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(vuiPresent.has_value());
        QVERIFY(aspectPresent.has_value());
        QVERIFY(nalHrd.has_value());
        QVERIFY(vclHrd.has_value());
        QVERIFY(picStruct.has_value());
        QVERIFY(restriction.has_value());
        QVERIFY(stop.has_value());
        QCOMPARE(vuiPresent->value().toULongLong(), quint64(1));
        QCOMPARE(aspectPresent->value().toULongLong(), quint64(0));
        QCOMPARE(nalHrd->value().toULongLong(), quint64(0));
        QCOMPARE(vclHrd->value().toULongLong(), quint64(0));
        QCOMPARE(picStruct->value().toULongLong(), quint64(0));
        QCOMPARE(restriction->value().toULongLong(), quint64(0));
        QCOMPARE(stop->value().toULongLong(), quint64(1));
        QVERIFY(!fieldNamed(QStringLiteral("aspect_ratio_idc")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("motion_vectors_over_pic_boundaries_flag"))
                     .has_value());
    }

    void decodesTheSupportedVideoUsabilityInformationBranches() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xdf,
            0xf8, 0x00, 0x20, 0x00, 0x1f, 0xb8, 0x08, 0x08, 0x0d, 0x92, 0x00,
            0x00, 0x07, 0xd2, 0x00, 0x01, 0xd4, 0xc1, 0x3b, 0x41, 0x10, 0x83,
            0x2c,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const std::vector expectedUnsigned{
            std::pair{QStringLiteral("aspect_ratio_info_present_flag"), quint64(1)},
            std::pair{QStringLiteral("aspect_ratio_idc"), quint64(255)},
            std::pair{QStringLiteral("sar_width"), quint64(4)},
            std::pair{QStringLiteral("sar_height"), quint64(3)},
            std::pair{QStringLiteral("overscan_info_present_flag"), quint64(1)},
            std::pair{QStringLiteral("overscan_appropriate_flag"), quint64(1)},
            std::pair{QStringLiteral("video_signal_type_present_flag"), quint64(1)},
            std::pair{QStringLiteral("video_format"), quint64(5)},
            std::pair{QStringLiteral("video_full_range_flag"), quint64(1)},
            std::pair{QStringLiteral("colour_description_present_flag"), quint64(1)},
            std::pair{QStringLiteral("colour_primaries"), quint64(1)},
            std::pair{QStringLiteral("transfer_characteristics"), quint64(1)},
            std::pair{QStringLiteral("matrix_coefficients"), quint64(1)},
            std::pair{QStringLiteral("chroma_loc_info_present_flag"), quint64(1)},
            std::pair{QStringLiteral("chroma_sample_loc_type_top_field"), quint64(2)},
            std::pair{QStringLiteral("chroma_sample_loc_type_bottom_field"), quint64(3)},
            std::pair{QStringLiteral("timing_info_present_flag"), quint64(1)},
            std::pair{QStringLiteral("num_units_in_tick"), quint64(1001)},
            std::pair{QStringLiteral("time_scale"), quint64(60000)},
            std::pair{QStringLiteral("fixed_frame_rate_flag"), quint64(1)},
            std::pair{QStringLiteral("nal_hrd_parameters_present_flag"), quint64(0)},
            std::pair{QStringLiteral("vcl_hrd_parameters_present_flag"), quint64(0)},
            std::pair{QStringLiteral("pic_struct_present_flag"), quint64(1)},
            std::pair{QStringLiteral("bitstream_restriction_flag"), quint64(1)},
            std::pair{QStringLiteral("motion_vectors_over_pic_boundaries_flag"), quint64(1)},
            std::pair{QStringLiteral("max_bytes_per_pic_denom"), quint64(2)},
            std::pair{QStringLiteral("max_bits_per_mb_denom"), quint64(1)},
            std::pair{QStringLiteral("log2_max_mv_length_horizontal"), quint64(16)},
            std::pair{QStringLiteral("log2_max_mv_length_vertical"), quint64(15)},
            std::pair{QStringLiteral("max_num_reorder_frames"), quint64(2)},
            std::pair{QStringLiteral("max_dec_frame_buffering"), quint64(4)},
            std::pair{QStringLiteral("rbsp_stop_one_bit"), quint64(1)},
        };
        for (const auto& [name, expected] : expectedUnsigned) {
            const auto field = fieldNamed(name);
            QVERIFY2(field.has_value(), qPrintable(name));
            QCOMPARE(field->value().toULongLong(), expected);
            QVERIFY(field->diagnostics().empty());
        }
        const auto aspect = fieldNamed(QStringLiteral("aspect_ratio_idc"));
        const auto chroma = fieldNamed(QStringLiteral("chroma_sample_loc_type_top_field"));
        QVERIFY(aspect.has_value());
        QVERIFY(chroma.has_value());
        QCOMPARE(aspect->metadata().specification->clause, QStringLiteral("E.1.1"));
        QCOMPARE(chroma->metadata().specification->clause, QStringLiteral("E.2.1"));
        QCOMPARE(aspect->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(85));
        QCOMPARE(aspect->location()->sourceSpans().front().bitLength(), quint64(8));
        QVERIFY(!fieldNamed(QStringLiteral("low_delay_hrd_flag")).has_value());
    }

    void decodesTheSupportedHypotheticalReferenceDecoderBranches() {
        struct HrdCase {
            std::vector<unsigned int> values;
            std::vector<std::pair<QString, quint64>> expectedFields;
            std::vector<QString> absentFields;
            quint64 lowDelayHrdFlag;
        };
        const std::vector cases{
            HrdCase{
                {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a,
                 0x0f, 0xd0, 0x51, 0xa5, 0x59, 0x17, 0xb5, 0x68, 0xd0},
                {
                    {QStringLiteral("nal_hrd_cpb_cnt_minus1"), quint64(1)},
                    {QStringLiteral("nal_hrd_bit_rate_scale"), quint64(3)},
                    {QStringLiteral("nal_hrd_cpb_size_scale"), quint64(4)},
                    {QStringLiteral("nal_hrd_cpb_count"), quint64(2)},
                    {QStringLiteral("nal_hrd_bit_rate_value_minus1[0]"), quint64(0)},
                    {QStringLiteral("nal_hrd_cpb_size_value_minus1[0]"), quint64(1)},
                    {QStringLiteral("nal_hrd_cbr_flag[0]"), quint64(1)},
                    {QStringLiteral("nal_hrd_bit_rate_value_minus1[1]"), quint64(2)},
                    {QStringLiteral("nal_hrd_cpb_size_value_minus1[1]"), quint64(3)},
                    {QStringLiteral("nal_hrd_cbr_flag[1]"), quint64(0)},
                    {QStringLiteral("nal_hrd_initial_cpb_removal_delay_length_minus1"),
                     quint64(23)},
                    {QStringLiteral("nal_hrd_cpb_removal_delay_length_minus1"), quint64(22)},
                    {QStringLiteral("nal_hrd_dpb_output_delay_length_minus1"), quint64(21)},
                    {QStringLiteral("nal_hrd_time_offset_length"), quint64(20)},
                },
                {QStringLiteral("vcl_hrd_cpb_cnt_minus1")},
                quint64(1),
            },
            HrdCase{
                {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a,
                 0x0f, 0xd0, 0x31, 0x22, 0x9a, 0xa5, 0xb1, 0xa2},
                {
                    {QStringLiteral("vcl_hrd_cpb_cnt_minus1"), quint64(0)},
                    {QStringLiteral("vcl_hrd_bit_rate_scale"), quint64(1)},
                    {QStringLiteral("vcl_hrd_cpb_size_scale"), quint64(2)},
                    {QStringLiteral("vcl_hrd_cpb_count"), quint64(1)},
                    {QStringLiteral("vcl_hrd_bit_rate_value_minus1[0]"), quint64(4)},
                    {QStringLiteral("vcl_hrd_cpb_size_value_minus1[0]"), quint64(5)},
                    {QStringLiteral("vcl_hrd_cbr_flag[0]"), quint64(1)},
                    {QStringLiteral("vcl_hrd_initial_cpb_removal_delay_length_minus1"),
                     quint64(10)},
                    {QStringLiteral("vcl_hrd_cpb_removal_delay_length_minus1"), quint64(11)},
                    {QStringLiteral("vcl_hrd_dpb_output_delay_length_minus1"), quint64(12)},
                    {QStringLiteral("vcl_hrd_time_offset_length"), quint64(13)},
                },
                {QStringLiteral("nal_hrd_cpb_cnt_minus1")},
                quint64(0),
            },
            HrdCase{
                {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f,
                 0xd0, 0x64, 0xa9, 0x8e, 0x84, 0xaa, 0x99, 0xc8, 0x59, 0x8e,
                 0x5b, 0x1a, 0xe9},
                {
                    {QStringLiteral("nal_hrd_cpb_cnt_minus1"), quint64(0)},
                    {QStringLiteral("nal_hrd_bit_rate_scale"), quint64(2)},
                    {QStringLiteral("nal_hrd_cpb_size_scale"), quint64(5)},
                    {QStringLiteral("nal_hrd_bit_rate_value_minus1[0]"), quint64(1)},
                    {QStringLiteral("nal_hrd_cpb_size_value_minus1[0]"), quint64(2)},
                    {QStringLiteral("vcl_hrd_cpb_cnt_minus1"), quint64(1)},
                    {QStringLiteral("vcl_hrd_bit_rate_scale"), quint64(6)},
                    {QStringLiteral("vcl_hrd_cpb_size_scale"), quint64(7)},
                    {QStringLiteral("vcl_hrd_bit_rate_value_minus1[1]"), quint64(5)},
                    {QStringLiteral("vcl_hrd_cpb_size_value_minus1[1]"), quint64(6)},
                    {QStringLiteral("vcl_hrd_initial_cpb_removal_delay_length_minus1"),
                     quint64(11)},
                    {QStringLiteral("vcl_hrd_time_offset_length"), quint64(14)},
                },
                {},
                quint64(1),
            },
        };

        for (const auto& hrdCase : cases) {
            std::vector<std::byte> sourceBytes;
            sourceBytes.reserve(hrdCase.values.size());
            for (const unsigned int value : hrdCase.values) {
                sourceBytes.push_back(static_cast<std::byte>(value));
            }
            MemorySource source(std::move(sourceBytes));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto sps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(sps.has_value());
            QCOMPARE(sps->state(), MaterializationState::Materialized);
            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    sps->children().begin(), sps->children().end(), [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == sps->children().end() ? std::nullopt
                                                    : analyzer->tree().node(*found);
            };
            for (const auto& [name, expected] : hrdCase.expectedFields) {
                const auto field = fieldNamed(name);
                QVERIFY2(field.has_value(), qPrintable(name));
                QCOMPARE(field->value().toULongLong(), expected);
                QVERIFY(field->diagnostics().empty());
            }
            for (const auto& name : hrdCase.absentFields) {
                QVERIFY2(!fieldNamed(name).has_value(), qPrintable(name));
            }
            const auto anyHrd = fieldNamed(QStringLiteral("hrd_parameters_present"));
            const auto lowDelay = fieldNamed(QStringLiteral("low_delay_hrd_flag"));
            const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
            QVERIFY(anyHrd.has_value());
            QVERIFY(lowDelay.has_value());
            QVERIFY(stop.has_value());
            QCOMPARE(anyHrd->value().toBool(), true);
            QVERIFY(!anyHrd->location().has_value());
            QCOMPARE(lowDelay->value().toULongLong(), hrdCase.lowDelayHrdFlag);
            QCOMPARE(stop->value().toULongLong(), quint64(1));
        }

        MemorySource metadataSource(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a,
            0x0f, 0xd0, 0x51, 0xa5, 0x59, 0x17, 0xb5, 0x68, 0xd0,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(metadataSource, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));
        const auto batch = analyzer->analyzeBatch();
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        const auto count = std::find_if(
            sps->children().begin(), sps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("nal_hrd_cpb_cnt_minus1");
            });
        QVERIFY(count != sps->children().end());
        const auto countNode = analyzer->tree().node(*count);
        QVERIFY(countNode.has_value());
        QCOMPARE(countNode->metadata().specification->clause, QStringLiteral("E.2.2"));
        QCOMPARE(countNode->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(90));
    }

    void warnsOnOutOfRangeVideoUsabilityInformationValues() {
        struct RangeCase {
            std::vector<unsigned int> values;
            QString fieldName;
            quint64 value;
            quint64 bitLength;
        };
        const std::vector cases{
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd1, 0x3c, 0x10},
                      QStringLiteral("chroma_sample_loc_type_top_field"), quint64(6), quint64(5)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd1, 0x9c, 0x10},
                      QStringLiteral("chroma_sample_loc_type_bottom_field"),
                      quint64(6), quint64(5)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd0, 0x0c, 0x25, 0xf8},
                      QStringLiteral("max_bytes_per_pic_denom"), quint64(17), quint64(9)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd0, 0x0e, 0x12, 0xf8},
                      QStringLiteral("max_bits_per_mb_denom"), quint64(17), quint64(9)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd0, 0x0f, 0x09, 0x78},
                      QStringLiteral("log2_max_mv_length_horizontal"), quint64(17), quint64(9)},
            RangeCase{{0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                       0xf4, 0x0a, 0x0f, 0xd0, 0x0f, 0x84, 0xb8},
                      QStringLiteral("log2_max_mv_length_vertical"), quint64(17), quint64(9)},
        };

        for (const auto& rangeCase : cases) {
            std::vector<std::byte> sourceBytes;
            sourceBytes.reserve(rangeCase.values.size());
            for (const unsigned int value : rangeCase.values) {
                sourceBytes.push_back(static_cast<std::byte>(value));
            }
            MemorySource source(std::move(sourceBytes));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto sps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(sps.has_value());
            QCOMPARE(sps->state(), MaterializationState::Materialized);
            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    sps->children().begin(), sps->children().end(), [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == sps->children().end() ? std::nullopt
                                                    : analyzer->tree().node(*found);
            };
            const auto field = fieldNamed(rangeCase.fieldName);
            QVERIFY2(field.has_value(), qPrintable(rangeCase.fieldName));
            QCOMPARE(field->value().toULongLong(), rangeCase.value);
            QCOMPARE(field->diagnostics().size(), std::size_t(1));
            const auto& diagnostic = field->diagnostics().front();
            QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
            QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
            QCOMPARE(diagnostic.fieldPath,
                     QStringLiteral("SequenceParameterSetRbsp.") + rangeCase.fieldName);
            QVERIFY(diagnostic.location.has_value());
            QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(),
                     rangeCase.bitLength);
            const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
            QVERIFY(stop.has_value());
            QCOMPARE(stop->value().toULongLong(), quint64(1));
        }
    }

    void rejectsOutOfRangeHypotheticalReferenceDecoderScheduleCountAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                   0xf4, 0x0a, 0x0f, 0xd0, 0x41, 0x08, 0x07,
                                   0x00, 0x00, 0x01, 0x0c}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Invalid);
        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == sps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto count = fieldNamed(QStringLiteral("nal_hrd_cpb_cnt_minus1"));
        const auto scale = fieldNamed(QStringLiteral("nal_hrd_bit_rate_scale"));
        const auto computedCount = fieldNamed(QStringLiteral("nal_hrd_cpb_count"));
        QVERIFY(count.has_value());
        QVERIFY(scale.has_value());
        QVERIFY(computedCount.has_value());
        QCOMPARE(count->value().toULongLong(), quint64(32));
        QCOMPARE(computedCount->value().toULongLong(), quint64(33));
        QCOMPARE(count->diagnostics().size(), std::size_t(1));
        const auto& warning = count->diagnostics().front();
        QCOMPARE(warning.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(warning.severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(warning.fieldPath,
                 QStringLiteral("SequenceParameterSetRbsp.nal_hrd_cpb_cnt_minus1"));
        QVERIFY(warning.location.has_value());
        QCOMPARE(warning.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(90));
        QCOMPARE(warning.location->sourceSpans().front().bitLength(), quint64(11));
        QCOMPARE(sps->diagnostics().size(), std::size_t(1));
        const auto& error = sps->diagnostics().front();
        QCOMPARE(error.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(error.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(error.fieldPath,
                 QStringLiteral("SequenceParameterSetRbsp.nal_hrd_cpb_count"));
        QVERIFY(!error.location.has_value());
        QVERIFY(!fieldNamed(QStringLiteral("nal_hrd_bit_rate_value_minus1[0]")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("vcl_hrd_parameters_present_flag")).has_value());

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsTruncatedSequenceParameterSetVuiAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xd8,
                                   0x00, 0x00, 0x01, 0x0c}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Invalid);
        const auto vui = std::find_if(
            sps->children().begin(), sps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("vui_parameters_present_flag");
            });
        QVERIFY(vui != sps->children().end());
        const auto vuiNode = analyzer->tree().node(*vui);
        QVERIFY(vuiNode.has_value());
        QCOMPARE(vuiNode->value().toULongLong(), quint64(1));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsReservedSequenceParameterSetPictureOrderCountTypeAndContinues() {
        std::vector<std::byte> stream;
        appendNal(stream, pictureOrderCountSequenceParameterSet(3));
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x0a}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Invalid);
        const auto picOrder = std::find_if(
            sps->children().begin(), sps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("pic_order_cnt_type");
            });
        QVERIFY(picOrder != sps->children().end());
        const auto picOrderNode = analyzer->tree().node(*picOrder);
        QVERIFY(picOrderNode.has_value());
        QCOMPARE(picOrderNode->value().toULongLong(), quint64(3));
        QCOMPARE(picOrderNode->metadata().typeName, QStringLiteral("PicOrderCntType"));
        QCOMPARE(picOrderNode->location()->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(58));
        QCOMPARE(picOrderNode->location()->logicalRange().bitLength(), quint64(5));
        QCOMPARE(sps->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = sps->diagnostics().front();
        QCOMPARE(diagnostic.code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("SequenceParameterSetRbsp.pic_order_cnt_type"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(58));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(5));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsPictureOrderCountCycleAboveTheDeclaredBoundAndContinues() {
        std::vector<std::byte> stream;
        appendNal(stream,
                  pictureOrderCountSequenceParameterSet(1, false, 0, 0, 256));
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x0a}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Invalid);
        const auto count = std::find_if(
            sps->children().begin(), sps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() ==
                                   QStringLiteral(
                                       "num_ref_frames_in_pic_order_cnt_cycle");
            });
        QVERIFY(count != sps->children().end());
        const auto countNode = analyzer->tree().node(*count);
        QVERIFY(countNode.has_value());
        QCOMPARE(countNode->value().toULongLong(), quint64(256));
        QCOMPARE(countNode->location()->sourceSpans().front().start()
                     .absoluteBitOffset(),
                 quint64(64));
        QCOMPARE(countNode->location()->logicalRange().bitLength(), quint64(17));
        QCOMPARE(countNode->diagnostics().size(), std::size_t(1));
        QCOMPARE(countNode->diagnostics().front().severity,
                 streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(sps->diagnostics().size(), std::size_t(1));
        QCOMPARE(sps->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QVERIFY(std::none_of(
            sps->children().begin(), sps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node &&
                       node->name().startsWith(QStringLiteral("offset_for_ref_frame["));
            }));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void reportsTruncatedPictureOrderCountCycleOffsetAndContinues() {
        std::vector<bool> bits;
        appendPocSequenceParameterSetPrefix(bits, 1);
        appendFixedBits(bits, 0, 1);
        appendSignedExpGolomb(bits, 0);
        appendSignedExpGolomb(bits, 0);
        appendUnsignedExpGolomb(bits, 1);
        while (bits.size() % 8 != 0) {
            bits.push_back(false);
        }
        std::vector<std::byte> stream;
        appendNal(stream, packAnnexBNal(0x67, std::move(bits), false));
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x0a}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sps.has_value());
        QCOMPARE(sps->state(), MaterializationState::Invalid);
        QCOMPARE(sps->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = sps->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Unable to read complete Exp-Golomb codeword"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("SequenceParameterSetRbsp.offset_for_ref_frame[0]"));
        QVERIFY(!std::any_of(
            sps->children().begin(), sps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("offset_for_ref_frame[0]");
            }));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsInvalidSequenceParameterSetProfileAndReservedBits() {
        const auto rejectedField = [](std::initializer_list<unsigned int> payload,
                                      const QString& expectedName) {
            std::vector<unsigned int> values{0x00, 0x00, 0x01, 0x67};
            values.insert(values.end(), payload.begin(), payload.end());
            return std::pair<std::vector<unsigned int>, QString>{std::move(values), expectedName};
        };
        const std::vector cases{
            rejectedField({0x65}, QStringLiteral("profile_idc")),
            rejectedField({0x42, 0x01}, QStringLiteral("reserved_zero_2bits")),
        };

        for (const auto& [values, expectedName] : cases) {
            std::vector<std::byte> sourceBytes;
            sourceBytes.reserve(values.size());
            for (const unsigned int value : values) {
                sourceBytes.push_back(static_cast<std::byte>(value));
            }
            MemorySource source(std::move(sourceBytes));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Invalid);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto sps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(sps.has_value());
            QCOMPARE(sps->state(), MaterializationState::Invalid);
            const auto field = std::find_if(
                sps->children().begin(), sps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == expectedName;
                });
            QVERIFY(field != sps->children().end());
        }
    }

    void decodesTheSupportedPictureParameterSetBaseSyntax() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                                   0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->children().size(), std::size_t(1));
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->name(), QStringLiteral("PictureParameterSetRbsp"));
        QCOMPARE(pps->state(), MaterializationState::Materialized);
        QCOMPARE(pps->metadata().specification->clause, QStringLiteral("7.3.2.2"));
        QCOMPARE(pps->children().size(), std::size_t(24));

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                pps->children().begin(), pps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == pps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto ppsId = fieldNamed(QStringLiteral("pic_parameter_set_id"));
        const auto spsId = fieldNamed(QStringLiteral("seq_parameter_set_id"));
        const auto sliceGroups = fieldNamed(QStringLiteral("num_slice_groups_minus1"));
        const auto weightedBipred = fieldNamed(QStringLiteral("weighted_bipred_idc"));
        const auto qp = fieldNamed(QStringLiteral("pic_init_qp_minus26"));
        const auto deblocking =
            fieldNamed(QStringLiteral("deblocking_filter_control_present_flag"));
        const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(ppsId.has_value());
        QVERIFY(spsId.has_value());
        QVERIFY(sliceGroups.has_value());
        QVERIFY(weightedBipred.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(deblocking.has_value());
        QVERIFY(stop.has_value());
        QCOMPARE(ppsId->value().toULongLong(), quint64(0));
        QCOMPARE(ppsId->metadata().specification->clause, QStringLiteral("7.4.2.2"));
        QVERIFY(ppsId->metadata().description.contains(QStringLiteral("0 to 255")));
        QCOMPARE(spsId->value().toULongLong(), quint64(0));
        QCOMPARE(sliceGroups->value().toULongLong(), quint64(0));
        QCOMPARE(weightedBipred->value().toULongLong(), quint64(0));
        QCOMPARE(weightedBipred->metadata().typeName, QStringLiteral("WeightedBipredIdc"));
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QCOMPARE(deblocking->value().toULongLong(), quint64(1));
        QCOMPARE(stop->value().toULongLong(), quint64(1));
        QCOMPARE(ppsId->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(120));
        QCOMPARE(ppsId->location()->sourceSpans().front().bitLength(), quint64(1));
    }

    void decodesNonDefaultPictureParameterSetValues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0x7d, 0x02, 0x83, 0xf2,
                                   0x00, 0x00, 0x01, 0x68,
                                   0x23, 0xed, 0x66, 0x8a, 0xe0}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                pps->children().begin(), pps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == pps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        QCOMPARE(fieldNamed(QStringLiteral("pic_parameter_set_id"))->value().toULongLong(),
                 quint64(3));
        QCOMPARE(fieldNamed(QStringLiteral("seq_parameter_set_id"))->value().toULongLong(),
                 quint64(2));
        QCOMPARE(fieldNamed(QStringLiteral("entropy_coding_mode_flag"))->value().toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("bottom_field_pic_order_in_frame_present_flag"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("num_ref_idx_l0_default_active_minus1"))
                     ->value()
                     .toULongLong(),
                 quint64(2));
        QCOMPARE(fieldNamed(QStringLiteral("num_ref_idx_l1_default_active_minus1"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("weighted_bipred_idc"))->value().toULongLong(),
                 quint64(2));
        QCOMPARE(fieldNamed(QStringLiteral("pic_init_qp_minus26"))->value().toLongLong(),
                 qlonglong(-1));
        QCOMPARE(fieldNamed(QStringLiteral("pic_init_qs_minus26"))->value().toLongLong(),
                 qlonglong(1));
        QCOMPARE(fieldNamed(QStringLiteral("chroma_qp_index_offset"))->value().toLongLong(),
                 qlonglong(-2));
        QCOMPARE(fieldNamed(QStringLiteral("constrained_intra_pred_flag"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("redundant_pic_cnt_present_flag"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
    }

    void decodesHighProfilePictureParameterSetWithoutExtension() {
        const auto spsNal = sequenceParameterSetForProfile(100);
        std::vector<std::byte> stream;
        appendNal(stream, spsNal);
        appendNal(stream, pictureParameterSet(false));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Materialized);
        QVERIFY(pps->diagnostics().empty());

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                pps->children().begin(), pps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == pps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto hasExtension = fieldNamed(QStringLiteral("has_pps_extension"));
        const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(hasExtension.has_value());
        QVERIFY(stop.has_value());
        QCOMPARE(hasExtension->value().toBool(), false);
        QVERIFY(!hasExtension->location().has_value());
        QVERIFY(!fieldNamed(QStringLiteral("transform_8x8_mode_flag")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("pic_scaling_matrix_present_flag")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("second_chroma_qp_index_offset")).has_value());
        QCOMPARE(stop->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(spsNal.size() + 4) * 8 + 16);
    }

    void decodesBoundedHighProfilePictureParameterSetExtensions() {
        struct ExtensionCase final {
            bool transform8x8 = false;
            qint64 secondChromaOffset = 0;
            quint64 offsetBitLength = 0;
        };
        const std::vector<ExtensionCase> cases{
            {false, 0, 1},
            {true, 3, 5},
            {false, -2, 5},
        };

        for (const auto& testCase : cases) {
            const auto spsNal = sequenceParameterSetForProfile(100);
            std::vector<std::byte> stream;
            appendNal(stream, spsNal);
            appendNal(stream,
                      pictureParameterSetWithExtension(
                          testCase.transform8x8, testCase.secondChromaOffset));
            MemorySource source(std::move(stream));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto pps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(pps.has_value());
            QCOMPARE(pps->state(), MaterializationState::Materialized);
            QVERIFY(pps->diagnostics().empty());

            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    pps->children().begin(), pps->children().end(), [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == pps->children().end() ? std::nullopt
                                                    : analyzer->tree().node(*found);
            };
            const auto hasExtension = fieldNamed(QStringLiteral("has_pps_extension"));
            const auto transform = fieldNamed(QStringLiteral("transform_8x8_mode_flag"));
            const auto scaling =
                fieldNamed(QStringLiteral("pic_scaling_matrix_present_flag"));
            const auto secondOffset =
                fieldNamed(QStringLiteral("second_chroma_qp_index_offset"));
            const auto stop = fieldNamed(QStringLiteral("rbsp_stop_one_bit"));
            QVERIFY(hasExtension.has_value());
            QVERIFY(transform.has_value());
            QVERIFY(scaling.has_value());
            QVERIFY(secondOffset.has_value());
            QVERIFY(stop.has_value());
            QCOMPARE(hasExtension->value().toBool(), true);
            QVERIFY(!hasExtension->location().has_value());
            QCOMPARE(transform->value().toULongLong(),
                     quint64(testCase.transform8x8 ? 1 : 0));
            QCOMPARE(scaling->value().toULongLong(), quint64(0));
            QCOMPARE(secondOffset->value().toLongLong(),
                     qlonglong(testCase.secondChromaOffset));

            const quint64 ppsRbspStart = quint64(spsNal.size() + 4) * 8;
            QCOMPARE(transform->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     ppsRbspStart + 16);
            QCOMPARE(transform->location()->logicalRange().bitLength(), quint64(1));
            QCOMPARE(transform->location()->sourceSpans().front().bitLength(),
                     quint64(1));
            QCOMPARE(scaling->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     ppsRbspStart + 17);
            QCOMPARE(scaling->location()->logicalRange().bitLength(), quint64(1));
            QCOMPARE(scaling->location()->sourceSpans().front().bitLength(), quint64(1));
            QCOMPARE(secondOffset->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     ppsRbspStart + 18);
            QCOMPARE(secondOffset->location()->logicalRange().bitLength(),
                     testCase.offsetBitLength);
            QCOMPARE(secondOffset->location()->sourceSpans().front().bitLength(),
                     testCase.offsetBitLength);
            QCOMPARE(stop->location()->sourceSpans().front().start().absoluteBitOffset(),
                     ppsRbspStart + 18 + testCase.offsetBitLength);
            QCOMPARE(stop->location()->sourceSpans().front().bitLength(), quint64(1));
        }
    }

    void rejectsHighProfilePictureScalingMatrixAndContinues() {
        const auto spsNal = sequenceParameterSetForProfile(100);
        std::vector<std::byte> stream;
        appendNal(stream, spsNal);
        appendNal(stream, pictureParameterSetWithExtension(true, 0, true));
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Invalid);
        QCOMPARE(pps->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = pps->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral(
                     "PictureParameterSetRbsp.pic_scaling_matrix_present_flag"));
        QVERIFY(diagnostic.location.has_value());
        const quint64 ppsRbspStart = quint64(spsNal.size() + 4) * 8;
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 ppsRbspStart + 17);
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(1));
        QVERIFY(!std::any_of(
            pps->children().begin(), pps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node &&
                       node->name() == QStringLiteral("second_chroma_qp_index_offset");
            }));
        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsPictureParameterSetExtensionsForNonHighProfilesAndContinues() {
        for (const quint64 profileIdc : {quint64(66), quint64(77), quint64(88)}) {
            const auto spsNal = sequenceParameterSetForProfile(profileIdc);
            std::vector<std::byte> stream;
            appendNal(stream, spsNal);
            appendNal(stream, pictureParameterSetWithExtension(false, 0));
            appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
            MemorySource source(std::move(stream));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(1));
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Invalid);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto pps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(pps.has_value());
            QCOMPARE(pps->state(), MaterializationState::Invalid);
            QCOMPARE(pps->diagnostics().size(), std::size_t(1));
            const auto& diagnostic = pps->diagnostics().front();
            QCOMPARE(diagnostic.code,
                     streamview::core::DiagnosticCode::InvalidSyntax);
            QCOMPARE(diagnostic.message,
                     QStringLiteral("Assertion condition is false"));
            QCOMPARE(diagnostic.fieldPath,
                     QStringLiteral("PictureParameterSetRbsp.seq_parameter_set_id"));
            QVERIFY(diagnostic.location.has_value());
            QCOMPARE(diagnostic.location->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     quint64(spsNal.size() + 4) * 8 + 1);
            QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(1));
            QVERIFY(!std::any_of(
                pps->children().begin(), pps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node &&
                           node->name() == QStringLiteral("transform_8x8_mode_flag");
                }));
            const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(followingNal.has_value());
            QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        }
    }

    void rejectsTruncatedSecondChromaOffsetAndContinues() {
        const auto spsNal = sequenceParameterSetForProfile(100);
        std::vector<std::byte> stream;
        appendNal(stream, spsNal);
        appendNal(stream, pictureParameterSetWithoutSecondOffset());
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Invalid);
        QCOMPARE(pps->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = pps->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Unable to read complete Exp-Golomb codeword"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral(
                     "PictureParameterSetRbsp.second_chroma_qp_index_offset"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->logicalRange().bitLength(), quint64(0));
        QVERIFY(diagnostic.location->sourceSpans().empty());
        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsPpsExtensionWithoutSequenceParameterSet() {
        std::vector<std::byte> stream;
        appendNal(stream, pictureParameterSetWithExtension(false, 0));
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto ppsNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(ppsNal.has_value());
        QCOMPARE(ppsNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(ppsNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::WaitingDependency);
        QCOMPARE(pps->diagnostics().size(), std::size_t(1));
        QCOMPARE(pps->diagnostics().front().code,
                 streamview::core::DiagnosticCode::DependencyUnavailable);
        QCOMPARE(pps->diagnostics().front().fieldPath,
                 QStringLiteral("PictureParameterSetRbsp.seq_parameter_set_id"));
        const auto hasExtension = std::find_if(
            pps->children().begin(), pps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("has_pps_extension");
            });
        QVERIFY(hasExtension != pps->children().end());
        QCOMPARE(analyzer->tree().node(*hasExtension)->value().toBool(), true);
        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void doesNotUseFutureSequenceParameterSetForPpsExtension() {
        std::vector<std::byte> stream;
        appendNal(stream, pictureParameterSetWithExtension(false, 0));
        appendNal(stream, sequenceParameterSetForProfile(100));
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
        const auto ppsNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(ppsNal.has_value());
        QCOMPARE(ppsNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(ppsNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::WaitingDependency);
        QCOMPARE(pps->diagnostics().size(), std::size_t(1));
        QCOMPARE(pps->diagnostics().front().code,
                 streamview::core::DiagnosticCode::DependencyUnavailable);
        QCOMPARE(pps->diagnostics().front().fieldPath,
                 QStringLiteral("PictureParameterSetRbsp.seq_parameter_set_id"));
        const auto futureSps = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(futureSps.has_value());
        QCOMPARE(futureSps->state(), MaterializationState::Materialized);
        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void failedSpsRedefinitionDoesNotHideHighProfileForPpsExtension() {
        auto malformedSps = sequenceParameterSetForProfile(100);
        malformedSps.at(4) = std::byte{0x65};
        std::vector<std::byte> stream;
        appendNal(stream, sequenceParameterSetForProfile(100));
        appendNal(stream, malformedSps);
        appendNal(stream, pictureParameterSetWithExtension(true, -2));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
        const auto malformedNal = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(malformedNal.has_value());
        QCOMPARE(malformedNal->state(), MaterializationState::Invalid);
        const auto ppsNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(ppsNal.has_value());
        QCOMPARE(ppsNal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(ppsNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Materialized);
        QVERIFY(pps->diagnostics().empty());
        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                pps->children().begin(), pps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == pps->children().end() ? std::nullopt
                                                : analyzer->tree().node(*found);
        };
        const auto hasExtension = fieldNamed(QStringLiteral("has_pps_extension"));
        const auto transform = fieldNamed(QStringLiteral("transform_8x8_mode_flag"));
        const auto offset = fieldNamed(QStringLiteral("second_chroma_qp_index_offset"));
        QVERIFY(hasExtension.has_value());
        QVERIFY(transform.has_value());
        QVERIFY(offset.has_value());
        QCOMPARE(hasExtension->value().toBool(), true);
        QCOMPARE(transform->value().toULongLong(), quint64(1));
        QCOMPARE(offset->value().toLongLong(), qlonglong(-2));
    }

    void rejectsStalePpsAfterSequenceParameterSetRedefinition() {
        std::vector<std::byte> stream;
        appendNal(stream, sequenceParameterSetForProfile(100));
        appendNal(stream, pictureParameterSetWithExtension(false, 0));
        appendNal(stream, sequenceParameterSetForProfile(66));
        appendNal(stream, pictureOrderCountSlice(true, 0));
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(5));
        const auto sliceNal = analyzer->tree().node(batch.nalUnitNodes.at(3));
        QVERIFY(sliceNal.has_value());
        QCOMPARE(sliceNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(sliceNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::WaitingDependency);
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        QCOMPARE(slice->diagnostics().front().code,
                 streamview::core::DiagnosticCode::DependencyUnavailable);
        QCOMPARE(slice->diagnostics().front().fieldPath,
                 QStringLiteral(
                     "IdrSliceLayerWithoutPartitioningRbsp.pic_parameter_set_id"));
        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void selectsContextGenerationsByStreamPositionAcrossSpsPpsRedefinitions() {
        std::vector<std::byte> stream;
        // SPS id 0 (gen 0): Baseline profile 66, log2_max_frame_num_minus4 = 0 (frame_num width = 4 bits)
        appendNal(stream, sequenceParameterSetForProfile(66, 0, 0));
        // PPS id 0 (gen 0): references SPS id 0
        appendNal(stream, pictureParameterSet(false, 0, 0));
        // Slice A: IDR slice referencing PPS id 0, frameNum = 0, frameNumBits = 4
        appendNal(stream, pictureOrderCountSlice(true, 0, {}, false, 0, 0, 4, 0));
        // SPS id 0 (gen 1): Baseline profile 66, log2_max_frame_num_minus4 = 2 (frame_num width = 6 bits)
        appendNal(stream, sequenceParameterSetForProfile(66, 0, 2));
        // PPS id 0 (gen 1): references SPS id 0
        appendNal(stream, pictureParameterSet(false, 0, 0));
        // Slice B: IDR slice referencing PPS id 0, frameNum = 0, frameNumBits = 6
        appendNal(stream, pictureOrderCountSlice(true, 0, {}, false, 1, 0, 6, 0));
        // AUD trailing NAL
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));

        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(7));

        const QStringList expectedSliceChildren = {
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("idr_pic_id"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("no_output_of_prior_pics_flag"),
            QStringLiteral("long_term_reference_flag"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };

        const auto childNamesOf = [&](const auto& node) {
            QStringList names;
            for (const auto childId : node.children()) {
                if (const auto child = analyzer->tree().node(childId)) {
                    names.append(child->name());
                }
            }
            return names;
        };

        const auto fieldNamed = [&](const auto& parentNode, const QString& name) {
            const auto found = std::find_if(
                parentNode.children().begin(), parentNode.children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == parentNode.children().end() ? std::nullopt
                                                        : analyzer->tree().node(*found);
        };

        // Check NAL 0: SPS gen 0
        const auto sps0Nal = analyzer->tree().node(batch.nalUnitNodes.at(0));
        QVERIFY(sps0Nal.has_value());
        QCOMPARE(sps0Nal->state(), MaterializationState::Materialized);

        // Check NAL 1: PPS gen 0
        const auto pps0Nal = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(pps0Nal.has_value());
        QCOMPARE(pps0Nal->state(), MaterializationState::Materialized);

        // Check NAL 2: Slice A (bound to gen 0)
        const auto sliceANal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(sliceANal.has_value());
        QCOMPARE(sliceANal->state(), MaterializationState::Materialized);
        const auto rbspA = analyzer->tree().node(sliceANal->children().at(2));
        QVERIFY(rbspA.has_value());
        const auto sliceA = analyzer->tree().node(rbspA->children().front());
        QVERIFY(sliceA.has_value());
        QCOMPARE(sliceA->state(), MaterializationState::Materialized);
        QVERIFY(sliceA->diagnostics().empty());
        QCOMPARE(childNamesOf(*sliceA), expectedSliceChildren);
        const auto frameNumA = fieldNamed(*sliceA, QStringLiteral("frame_num"));
        QVERIFY(frameNumA.has_value());
        QCOMPARE(frameNumA->location()->logicalRange().bitLength(), quint64(4));

        // Check NAL 3: SPS gen 1
        const auto sps1Nal = analyzer->tree().node(batch.nalUnitNodes.at(3));
        QVERIFY(sps1Nal.has_value());
        QCOMPARE(sps1Nal->state(), MaterializationState::Materialized);

        // Check NAL 4: PPS gen 1
        const auto pps1Nal = analyzer->tree().node(batch.nalUnitNodes.at(4));
        QVERIFY(pps1Nal.has_value());
        QCOMPARE(pps1Nal->state(), MaterializationState::Materialized);

        // Check NAL 5: Slice B (bound to gen 1)
        const auto sliceBNal = analyzer->tree().node(batch.nalUnitNodes.at(5));
        QVERIFY(sliceBNal.has_value());
        QCOMPARE(sliceBNal->state(), MaterializationState::Materialized);
        const auto rbspB = analyzer->tree().node(sliceBNal->children().at(2));
        QVERIFY(rbspB.has_value());
        const auto sliceB = analyzer->tree().node(rbspB->children().front());
        QVERIFY(sliceB.has_value());
        QCOMPARE(sliceB->state(), MaterializationState::Materialized);
        QVERIFY(sliceB->diagnostics().empty());
        QCOMPARE(childNamesOf(*sliceB), expectedSliceChildren);
        const auto frameNumB = fieldNamed(*sliceB, QStringLiteral("frame_num"));
        QVERIFY(frameNumB.has_value());
        QCOMPARE(frameNumB->location()->logicalRange().bitLength(), quint64(6));

        // Check NAL 6: AUD
        const auto audNal = analyzer->tree().node(batch.nalUnitNodes.at(6));
        QVERIFY(audNal.has_value());
        QCOMPARE(audNal->state(), MaterializationState::Materialized);
    }

    void failedSpsRedefinitionPreservesPriorGenerationForSubsequentSlices() {
        std::vector<std::byte> stream;
        // SPS id 0 (gen 0): valid Baseline profile 66, log2_max_frame_num_minus4 = 0 (4-bit frame_num)
        appendNal(stream, sequenceParameterSetForProfile(66, 0, 0));
        // PPS id 0 (gen 0): references SPS id 0
        appendNal(stream, pictureParameterSet(false, 0, 0));
        // Slice A: IDR slice referencing PPS id 0, frame_num length = 4 bits
        appendNal(stream, pictureOrderCountSlice(true, 0, {}, false, 0, 0, 4, 0));
        // Malformed SPS id 0 (failed redefinition attempt): reserved profile 99, same seq_parameter_set_id = 0, log2_max_frame_num_minus4 = 2
        appendNal(stream, sequenceParameterSetForProfile(99, 0, 2));
        // PPS id 0 re-sent: references SPS id 0, still successfully binds to prior valid gen 0
        appendNal(stream, pictureParameterSet(false, 0, 0));
        // Slice B: IDR slice referencing PPS id 0 after failed redefinition; should still bind gen 0 (4-bit frame_num)
        appendNal(stream, pictureOrderCountSlice(true, 0, {}, false, 1, 0, 4, 0));
        // AUD trailing NAL
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));

        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(7));

        const QStringList expectedSliceChildren = {
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("idr_pic_id"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("no_output_of_prior_pics_flag"),
            QStringLiteral("long_term_reference_flag"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };

        const auto childNamesOf = [&](const auto& node) {
            QStringList names;
            for (const auto childId : node.children()) {
                if (const auto child = analyzer->tree().node(childId)) {
                    names.append(child->name());
                }
            }
            return names;
        };

        const auto fieldNamed = [&](const auto& parentNode, const QString& name) {
            const auto found = std::find_if(
                parentNode.children().begin(), parentNode.children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == parentNode.children().end() ? std::nullopt
                                                        : analyzer->tree().node(*found);
        };

        // NAL 0: SPS 0 gen 0 (Materialized)
        const auto sps0Nal = analyzer->tree().node(batch.nalUnitNodes.at(0));
        QVERIFY(sps0Nal.has_value());
        QCOMPARE(sps0Nal->state(), MaterializationState::Materialized);

        // NAL 1: PPS 0 gen 0 (Materialized)
        const auto pps0Nal = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(pps0Nal.has_value());
        QCOMPARE(pps0Nal->state(), MaterializationState::Materialized);

        // NAL 2: Slice A (Materialized, 4 bits frame_num, bound to gen 0)
        const auto sliceANal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(sliceANal.has_value());
        QCOMPARE(sliceANal->state(), MaterializationState::Materialized);
        const auto rbspA = analyzer->tree().node(sliceANal->children().at(2));
        QVERIFY(rbspA.has_value());
        const auto sliceA = analyzer->tree().node(rbspA->children().front());
        QVERIFY(sliceA.has_value());
        QCOMPARE(sliceA->state(), MaterializationState::Materialized);
        QVERIFY(sliceA->diagnostics().empty());
        QCOMPARE(childNamesOf(*sliceA), expectedSliceChildren);
        const auto frameNumA = fieldNamed(*sliceA, QStringLiteral("frame_num"));
        QVERIFY(frameNumA.has_value());
        QCOMPARE(frameNumA->location()->logicalRange().bitLength(), quint64(4));

        // NAL 3: Malformed SPS 0 (Invalid)
        const auto malformedSpsNal = analyzer->tree().node(batch.nalUnitNodes.at(3));
        QVERIFY(malformedSpsNal.has_value());
        QCOMPARE(malformedSpsNal->state(), MaterializationState::Invalid);

        // NAL 4: PPS 0 re-sent (Materialized, still successfully resolves prior gen 0 of SPS 0)
        const auto pps0PostNal = analyzer->tree().node(batch.nalUnitNodes.at(4));
        QVERIFY(pps0PostNal.has_value());
        QCOMPARE(pps0PostNal->state(), MaterializationState::Materialized);
        QVERIFY(pps0PostNal->diagnostics().empty());

        // NAL 5: Slice B (Materialized, 4 bits frame_num, still bound to gen 0 because failed redefinition published no new generation)
        const auto sliceBNal = analyzer->tree().node(batch.nalUnitNodes.at(5));
        QVERIFY(sliceBNal.has_value());
        QCOMPARE(sliceBNal->state(), MaterializationState::Materialized);
        const auto rbspB = analyzer->tree().node(sliceBNal->children().at(2));
        QVERIFY(rbspB.has_value());
        const auto sliceB = analyzer->tree().node(rbspB->children().front());
        QVERIFY(sliceB.has_value());
        QCOMPARE(sliceB->state(), MaterializationState::Materialized);
        QVERIFY(sliceB->diagnostics().empty());
        QCOMPARE(childNamesOf(*sliceB), expectedSliceChildren);
        const auto frameNumB = fieldNamed(*sliceB, QStringLiteral("frame_num"));
        QVERIFY(frameNumB.has_value());
        QCOMPARE(frameNumB->location()->logicalRange().bitLength(), quint64(4));

        // NAL 6: AUD (Materialized)
        const auto audNal = analyzer->tree().node(batch.nalUnitNodes.at(6));
        QVERIFY(audNal.has_value());
        QCOMPARE(audNal->state(), MaterializationState::Materialized);
    }

    void invalidParameterSetDefinitionDoesNotPublishOrFallBack() {
        std::vector<std::byte> stream;
        // SPS id 0: valid Baseline profile 66
        appendNal(stream, sequenceParameterSetForProfile(66, 0));
        // PPS id 0: references SPS id 0
        appendNal(stream, pictureParameterSet(false, 0, 0));
        // Slice A: IDR slice referencing PPS id 0
        appendNal(stream, pictureOrderCountSlice(true, 0, {}, false, 0, 0, 4, 0));
        // Malformed SPS id 1: invalid reserved profile 99
        appendNal(stream, sequenceParameterSetForProfile(99, 1));
        // PPS id 1 with extension: references invalid SPS id 1, triggering SPS profile dependency check
        appendNal(stream, pictureParameterSetWithExtension(true, 0, false, 1, 1));
        // Slice B: IDR slice referencing PPS id 1
        appendNal(stream, pictureOrderCountSlice(true, 0, {}, false, 1, 0, 4, 1));
        // AUD trailing NAL
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));

        MemorySource source(std::move(stream));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(7));

        const QStringList expectedSliceChildren = {
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("idr_pic_id"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("no_output_of_prior_pics_flag"),
            QStringLiteral("long_term_reference_flag"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };

        const auto childNamesOf = [&](const auto& node) {
            QStringList names;
            for (const auto childId : node.children()) {
                if (const auto child = analyzer->tree().node(childId)) {
                    names.append(child->name());
                }
            }
            return names;
        };

        // Slice A (NAL 2): fully valid and materialized
        const auto sliceANal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(sliceANal.has_value());
        QCOMPARE(sliceANal->state(), MaterializationState::Materialized);
        const auto rbspA = analyzer->tree().node(sliceANal->children().at(2));
        QVERIFY(rbspA.has_value());
        const auto sliceA = analyzer->tree().node(rbspA->children().front());
        QVERIFY(sliceA.has_value());
        QCOMPARE(sliceA->state(), MaterializationState::Materialized);
        QVERIFY(sliceA->diagnostics().empty());
        QCOMPARE(childNamesOf(*sliceA), expectedSliceChildren);

        // Malformed SPS (NAL 3): Invalid
        const auto malformedSpsNal = analyzer->tree().node(batch.nalUnitNodes.at(3));
        QVERIFY(malformedSpsNal.has_value());
        QCOMPARE(malformedSpsNal->state(), MaterializationState::Invalid);

        // PPS 1 (NAL 4): Invalid due to DependencyUnavailable on SPS 1
        const auto pps1Nal = analyzer->tree().node(batch.nalUnitNodes.at(4));
        QVERIFY(pps1Nal.has_value());
        QCOMPARE(pps1Nal->state(), MaterializationState::Invalid);
        const auto rbspPps1 = analyzer->tree().node(pps1Nal->children().at(2));
        QVERIFY(rbspPps1.has_value());
        const auto pps1 = analyzer->tree().node(rbspPps1->children().front());
        QVERIFY(pps1.has_value());
        QCOMPARE(pps1->state(), MaterializationState::WaitingDependency);
        QCOMPARE(pps1->diagnostics().size(), std::size_t(1));
        QCOMPARE(pps1->diagnostics().front().code,
                 streamview::core::DiagnosticCode::DependencyUnavailable);

        // Slice B (NAL 5): Invalid due to DependencyUnavailable on PPS 1
        const auto sliceBNal = analyzer->tree().node(batch.nalUnitNodes.at(5));
        QVERIFY(sliceBNal.has_value());
        QCOMPARE(sliceBNal->state(), MaterializationState::Invalid);
        const auto rbspB = analyzer->tree().node(sliceBNal->children().at(2));
        QVERIFY(rbspB.has_value());
        const auto sliceB = analyzer->tree().node(rbspB->children().front());
        QVERIFY(sliceB.has_value());
        QCOMPARE(sliceB->state(), MaterializationState::WaitingDependency);
        QCOMPARE(sliceB->diagnostics().size(), std::size_t(1));
        QCOMPARE(sliceB->diagnostics().front().code,
                 streamview::core::DiagnosticCode::DependencyUnavailable);

        // Following AUD NAL (NAL 6): Materialized
        const auto audNal = analyzer->tree().node(batch.nalUnitNodes.at(6));
        QVERIFY(audNal.has_value());
        QCOMPARE(audNal->state(), MaterializationState::Materialized);
    }

    void decodesTheBoundedIdrSliceHeaderAndKeepsSliceDataOpaque() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x65, 0xba, 0xcc, 0xd5,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->children().size(), std::size_t(1));
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->name(), QStringLiteral("IdrSliceLayerWithoutPartitioningRbsp"));
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->metadata().specification->clause, QStringLiteral("7.3.2.8, 7.3.3"));

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const std::vector expectedUnsigned{
            std::pair{QStringLiteral("first_mb_in_slice"), quint64(0)},
            std::pair{QStringLiteral("slice_type"), quint64(2)},
            std::pair{QStringLiteral("pic_parameter_set_id"), quint64(0)},
            std::pair{QStringLiteral("frame_num"), quint64(5)},
            std::pair{QStringLiteral("idr_pic_id"), quint64(0)},
            std::pair{QStringLiteral("pic_order_cnt_lsb"), quint64(3)},
            std::pair{QStringLiteral("no_output_of_prior_pics_flag"), quint64(0)},
            std::pair{QStringLiteral("long_term_reference_flag"), quint64(0)},
        };
        for (const auto& [name, expected] : expectedUnsigned) {
            const auto field = fieldNamed(name);
            QVERIFY2(field.has_value(), qPrintable(name));
            QCOMPARE(field->value().toULongLong(), expected);
        }
        const auto qp = fieldNamed(QStringLiteral("slice_qp_delta"));
        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QVERIFY(!fieldNamed(QStringLiteral("delta_pic_order_cnt_bottom")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("redundant_pic_cnt")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("disable_deblocking_filter_idc")).has_value());
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->state(), MaterializationState::Materialized);
        QCOMPARE(payload->metadata().specification->clause, QStringLiteral("7.3.2.10"));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(7));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(payload->location()->sourceSpans().front().bitLength(), quint64(7));
        QCOMPARE(fieldNamed(QStringLiteral("frame_num"))
                     ->location()
                     ->logicalRange()
                     .bitLength(),
                 quint64(4));
        QCOMPARE(fieldNamed(QStringLiteral("pic_order_cnt_lsb"))
                     ->location()
                     ->logicalRange()
                     .bitLength(),
                 quint64(4));
    }

    void validatesIdrPictureIdentifierRangeWithoutMovingFollowingFields() {
        for (const quint64 identifier : {quint64(65535), quint64(65536)}) {
            const auto spsNal = sequenceParameterSetForProfile(66);
            const auto ppsNal = pictureParameterSet(false);
            std::vector<std::byte> stream;
            appendNal(stream, spsNal);
            appendNal(stream, ppsNal);
            appendNal(stream, pictureOrderCountSlice(true, 0, {}, false, identifier));
            appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
            MemorySource source(std::move(stream));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto slice = analyzer->tree().node(rbsp->children().front());
            QVERIFY(slice.has_value());
            QCOMPARE(slice->state(), MaterializationState::Materialized);

            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    slice->children().begin(), slice->children().end(), [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == slice->children().end() ? std::nullopt
                                                       : analyzer->tree().node(*found);
            };
            const auto idrPictureId = fieldNamed(QStringLiteral("idr_pic_id"));
            const auto pictureOrder = fieldNamed(QStringLiteral("pic_order_cnt_lsb"));
            const auto noOutput =
                fieldNamed(QStringLiteral("no_output_of_prior_pics_flag"));
            const auto qp = fieldNamed(QStringLiteral("slice_qp_delta"));
            const auto payload = fieldNamed(QStringLiteral("slice_data"));
            QVERIFY(idrPictureId.has_value());
            QVERIFY(pictureOrder.has_value());
            QVERIFY(noOutput.has_value());
            QVERIFY(qp.has_value());
            QVERIFY(payload.has_value());
            QCOMPARE(idrPictureId->value().toULongLong(), identifier);
            QCOMPARE(idrPictureId->metadata().specification->clause,
                     QStringLiteral("7.3.3, 7.4.3"));

            const quint64 sliceRbspStart = quint64(spsNal.size() + ppsNal.size() + 4) * 8;
            QCOMPARE(idrPictureId->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     sliceRbspStart + 9);
            QCOMPARE(idrPictureId->location()->logicalRange().bitLength(), quint64(33));
            QCOMPARE(idrPictureId->location()->sourceSpans().front().bitLength(),
                     quint64(33));
            QCOMPARE(pictureOrder->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     sliceRbspStart + 42);
            QCOMPARE(noOutput->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     sliceRbspStart + 46);
            QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                     sliceRbspStart + 48);
            QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                     sliceRbspStart + 49);
            QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(7));

            if (identifier == 65535) {
                QVERIFY(idrPictureId->diagnostics().empty());
            } else {
                QCOMPARE(idrPictureId->diagnostics().size(), std::size_t(1));
                const auto& diagnostic = idrPictureId->diagnostics().front();
                QCOMPARE(diagnostic.code,
                         streamview::core::DiagnosticCode::InvalidSyntax);
                QCOMPARE(diagnostic.severity,
                         streamview::core::DiagnosticSeverity::Warning);
                QCOMPARE(diagnostic.message,
                         QStringLiteral("Field value is above its @range maximum"));
                QCOMPARE(diagnostic.fieldPath,
                         QStringLiteral(
                             "IdrSliceLayerWithoutPartitioningRbsp.idr_pic_id"));
                QVERIFY(diagnostic.location.has_value());
                QCOMPARE(diagnostic.location->sourceSpans().front().start()
                             .absoluteBitOffset(),
                         sliceRbspStart + 9);
                QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(),
                         quint64(33));
            }

            const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(followingNal.has_value());
            QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        }
    }

    void validatesIdrFrameNumberWithoutMovingFollowingFields() {
        for (const quint64 frameNumber : {quint64(0), quint64(1)}) {
            const auto spsNal = sequenceParameterSetForProfile(66);
            const auto ppsNal = pictureParameterSet(false);
            std::vector<std::byte> stream;
            appendNal(stream, spsNal);
            appendNal(stream, ppsNal);
            appendNal(stream,
                      pictureOrderCountSlice(true, 0, {}, false, 0, frameNumber));
            appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
            MemorySource source(std::move(stream));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto slice = analyzer->tree().node(rbsp->children().front());
            QVERIFY(slice.has_value());
            QCOMPARE(slice->state(), MaterializationState::Materialized);

            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    slice->children().begin(), slice->children().end(), [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == slice->children().end() ? std::nullopt
                                                       : analyzer->tree().node(*found);
            };
            const auto frameNum = fieldNamed(QStringLiteral("frame_num"));
            const auto idrPictureId = fieldNamed(QStringLiteral("idr_pic_id"));
            const auto pictureOrder = fieldNamed(QStringLiteral("pic_order_cnt_lsb"));
            const auto noOutput =
                fieldNamed(QStringLiteral("no_output_of_prior_pics_flag"));
            const auto qp = fieldNamed(QStringLiteral("slice_qp_delta"));
            const auto payload = fieldNamed(QStringLiteral("slice_data"));
            QVERIFY(frameNum.has_value());
            QVERIFY(idrPictureId.has_value());
            QVERIFY(pictureOrder.has_value());
            QVERIFY(noOutput.has_value());
            QVERIFY(qp.has_value());
            QVERIFY(payload.has_value());
            QCOMPARE(frameNum->value().toULongLong(), frameNumber);

            const quint64 sliceRbspStart = quint64(spsNal.size() + ppsNal.size() + 4) * 8;
            QCOMPARE(frameNum->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     sliceRbspStart + 5);
            QCOMPARE(frameNum->location()->logicalRange().bitLength(), quint64(4));
            QCOMPARE(frameNum->location()->sourceSpans().front().bitLength(), quint64(4));
            QCOMPARE(idrPictureId->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     sliceRbspStart + 9);
            QCOMPARE(pictureOrder->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     sliceRbspStart + 10);
            QCOMPARE(noOutput->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     sliceRbspStart + 14);
            QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                     sliceRbspStart + 16);
            QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                     sliceRbspStart + 17);
            QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(7));

            if (frameNumber == 0) {
                QVERIFY(frameNum->diagnostics().empty());
            } else {
                QCOMPARE(frameNum->diagnostics().size(), std::size_t(1));
                const auto& diagnostic = frameNum->diagnostics().front();
                QCOMPARE(diagnostic.code,
                         streamview::core::DiagnosticCode::InvalidSyntax);
                QCOMPARE(diagnostic.severity,
                         streamview::core::DiagnosticSeverity::Warning);
                QCOMPARE(diagnostic.message,
                         QStringLiteral("Field value is above its @range maximum"));
                QCOMPARE(diagnostic.fieldPath,
                         QStringLiteral(
                             "IdrSliceLayerWithoutPartitioningRbsp.frame_num"));
                QVERIFY(diagnostic.location.has_value());
                QCOMPARE(diagnostic.location->sourceSpans().front().start()
                             .absoluteBitOffset(),
                         sliceRbspStart + 5);
                QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(),
                         quint64(4));
            }

            const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(followingNal.has_value());
            QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        }
    }

    void decodesTheBoundedNonIdrAllISliceHeaderAndKeepsSliceDataOpaque() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xee, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xba, 0x9e, 0xaa,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->children().size(), std::size_t(1));
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->name(),
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp"));
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->metadata().specification->clause,
                 QStringLiteral("7.3.2.8, 7.3.3"));
        QCOMPARE(slice->children().size(), std::size_t(12));

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const std::vector expectedUnsigned{
            std::pair{QStringLiteral("first_mb_in_slice"), quint64(0)},
            std::pair{QStringLiteral("slice_type"), quint64(2)},
            std::pair{QStringLiteral("pic_parameter_set_id"), quint64(0)},
            std::pair{QStringLiteral("frame_num"), quint64(5)},
            std::pair{QStringLiteral("pic_order_cnt_lsb"), quint64(3)},
        };
        for (const auto& [name, expected] : expectedUnsigned) {
            const auto field = fieldNamed(name);
            QVERIFY2(field.has_value(), qPrintable(name));
            QCOMPARE(field->value().toULongLong(), expected);
        }
        const auto sliceType = fieldNamed(QStringLiteral("slice_type"));
        const auto isPSlice = fieldNamed(QStringLiteral("is_p_slice"));
        const auto qp = fieldNamed(QStringLiteral("slice_qp_delta"));
        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(sliceType.has_value());
        QVERIFY(isPSlice.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(sliceType->metadata().typeName,
                 QStringLiteral("NonIdrSliceType"));
        QCOMPARE(isPSlice->kind(), AnalysisNodeKind::ComputedField);
        QCOMPARE(isPSlice->value().toBool(), false);
        QVERIFY(!isPSlice->location().has_value());
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QVERIFY(!fieldNamed(QStringLiteral("idr_pic_id")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("no_output_of_prior_pics_flag")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("long_term_reference_flag")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("delta_pic_order_cnt_bottom")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("redundant_pic_cnt")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("cabac_init_idc")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("disable_deblocking_filter_idc")).has_value());
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->state(), MaterializationState::Materialized);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(190));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(10));
        QCOMPARE(fieldNamed(QStringLiteral("frame_num"))
                     ->location()
                     ->logicalRange()
                     .bitLength(),
                 quint64(4));
        QCOMPARE(fieldNamed(QStringLiteral("pic_order_cnt_lsb"))
                     ->location()
                     ->logicalRange()
                     .bitLength(),
                 quint64(4));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesTheBoundedNonIdrPSliceTypeZeroAndKeepsSliceDataOpaque() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x66, 0xaa,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->name(), QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp"));
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(14));

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("num_ref_idx_active_override_flag"),
            QStringLiteral("ref_pic_list_modification_flag_l0"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const auto sliceType = fieldNamed(QStringLiteral("slice_type"));
        const auto isPSlice = fieldNamed(QStringLiteral("is_p_slice"));
        const auto ppsId = fieldNamed(QStringLiteral("pic_parameter_set_id"));
        const auto frameNum = fieldNamed(QStringLiteral("frame_num"));
        const auto poc = fieldNamed(QStringLiteral("pic_order_cnt_lsb"));
        const auto overrideFlag =
            fieldNamed(QStringLiteral("num_ref_idx_active_override_flag"));
        const auto modificationFlag =
            fieldNamed(QStringLiteral("ref_pic_list_modification_flag_l0"));
        const auto qp = fieldNamed(QStringLiteral("slice_qp_delta"));
        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(sliceType.has_value());
        QVERIFY(isPSlice.has_value());
        QVERIFY(ppsId.has_value());
        QVERIFY(frameNum.has_value());
        QVERIFY(poc.has_value());
        QVERIFY(overrideFlag.has_value());
        QVERIFY(modificationFlag.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QVERIFY(!fieldNamed(QStringLiteral("num_ref_idx_l0_active_minus1")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("cabac_init_idc")).has_value());
        QCOMPARE(sliceType->value().toULongLong(), quint64(0));
        QCOMPARE(sliceType->metadata().typeName, QStringLiteral("NonIdrSliceType"));
        QCOMPARE(isPSlice->kind(), AnalysisNodeKind::ComputedField);
        QCOMPARE(isPSlice->value().toBool(), true);
        QVERIFY(!isPSlice->location().has_value());
        QCOMPARE(ppsId->value().toULongLong(), quint64(0));
        QCOMPARE(frameNum->value().toULongLong(), quint64(5));
        QCOMPARE(poc->value().toULongLong(), quint64(3));
        QCOMPARE(overrideFlag->value().toULongLong(), quint64(0));
        QCOMPARE(modificationFlag->value().toULongLong(), quint64(0));
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QCOMPARE(sliceType->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(177));
        QCOMPARE(sliceType->location()->logicalRange().bitLength(), quint64(1));
        QCOMPARE(ppsId->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(178));
        QCOMPARE(frameNum->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(179));
        QCOMPARE(frameNum->location()->logicalRange().bitLength(), quint64(4));
        QCOMPARE(poc->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(183));
        QCOMPARE(poc->location()->logicalRange().bitLength(), quint64(4));
        QCOMPARE(overrideFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(187));
        QCOMPARE(modificationFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(188));
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(189));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->state(), MaterializationState::Materialized);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(190));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(10));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesTheEquivalentNonIdrPSliceTypeFive() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xee, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0x9a, 0xa6, 0x75,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->name(), QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp"));
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(15));

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const auto sliceType = fieldNamed(QStringLiteral("slice_type"));
        const auto isPSlice = fieldNamed(QStringLiteral("is_p_slice"));
        const auto ppsId = fieldNamed(QStringLiteral("pic_parameter_set_id"));
        const auto overrideFlag =
            fieldNamed(QStringLiteral("num_ref_idx_active_override_flag"));
        const auto modificationFlag =
            fieldNamed(QStringLiteral("ref_pic_list_modification_flag_l0"));
        const auto cabacInitIdc = fieldNamed(QStringLiteral("cabac_init_idc"));
        const auto qp = fieldNamed(QStringLiteral("slice_qp_delta"));
        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(sliceType.has_value());
        QVERIFY(isPSlice.has_value());
        QVERIFY(ppsId.has_value());
        QVERIFY(overrideFlag.has_value());
        QVERIFY(modificationFlag.has_value());
        QVERIFY(cabacInitIdc.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QVERIFY(!fieldNamed(QStringLiteral("num_ref_idx_l0_active_minus1")).has_value());
        QCOMPARE(sliceType->value().toULongLong(), quint64(5));
        QCOMPARE(sliceType->metadata().typeName, QStringLiteral("NonIdrSliceType"));
        QCOMPARE(isPSlice->value().toBool(), true);
        QCOMPARE(sliceType->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(177));
        QCOMPARE(sliceType->location()->logicalRange().bitLength(), quint64(5));
        QCOMPARE(ppsId->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(182));
        QCOMPARE(overrideFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(191));
        QCOMPARE(modificationFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(192));
        QCOMPARE(cabacInitIdc->value().toULongLong(), quint64(0));
        QCOMPARE(cabacInitIdc->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(cabacInitIdc->location()->logicalRange().bitLength(), quint64(1));
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(194));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(195));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(5));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesNonIdrPSliceReferenceIndexOverrideAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x76, 0xaa,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(15));

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("num_ref_idx_active_override_flag"),
            QStringLiteral("num_ref_idx_l0_active_minus1"),
            QStringLiteral("ref_pic_list_modification_flag_l0"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto overrideFlag = analyzer->tree().node(slice->children().at(9));
        const auto overrideCount = analyzer->tree().node(slice->children().at(10));
        const auto modificationFlag = analyzer->tree().node(slice->children().at(11));
        const auto qp = analyzer->tree().node(slice->children().at(13));
        const auto payload = analyzer->tree().node(slice->children().at(14));
        QVERIFY(overrideFlag.has_value());
        QVERIFY(overrideCount.has_value());
        QVERIFY(modificationFlag.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(overrideFlag->value().toULongLong(), quint64(1));
        QCOMPARE(overrideCount->value().toULongLong(), quint64(2));
        QCOMPARE(modificationFlag->value().toULongLong(), quint64(0));
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QCOMPARE(overrideFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(187));
        QCOMPARE(overrideFlag->location()->logicalRange().bitLength(), quint64(1));
        QCOMPARE(overrideCount->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(188));
        QCOMPARE(overrideCount->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(modificationFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(191));
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(192));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(7));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesEquivalentTypeFiveReferenceIndexOverride() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0x9a, 0xa7, 0x6a,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(15));
        const auto sliceType = analyzer->tree().node(slice->children().at(1));
        const auto overrideFlag = analyzer->tree().node(slice->children().at(9));
        const auto overrideCount = analyzer->tree().node(slice->children().at(10));
        const auto modificationFlag = analyzer->tree().node(slice->children().at(11));
        const auto qp = analyzer->tree().node(slice->children().at(13));
        const auto payload = analyzer->tree().node(slice->children().at(14));
        QVERIFY(sliceType.has_value());
        QVERIFY(overrideFlag.has_value());
        QVERIFY(overrideCount.has_value());
        QVERIFY(modificationFlag.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(sliceType->value().toULongLong(), quint64(5));
        QCOMPARE(overrideFlag->value().toULongLong(), quint64(1));
        QCOMPARE(overrideCount->name(), QStringLiteral("num_ref_idx_l0_active_minus1"));
        QCOMPARE(overrideCount->value().toULongLong(), quint64(2));
        QCOMPARE(modificationFlag->value().toULongLong(), quint64(0));
        QCOMPARE(overrideFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(191));
        QCOMPARE(overrideCount->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(192));
        QCOMPARE(overrideCount->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(modificationFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(195));
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(196));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(197));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(3));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void warnsOnOutOfRangeReferenceIndexOverrideWithoutMovingPayloadBoundary() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x70, 0x42, 0xaa,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(15));
        const auto overrideCount = analyzer->tree().node(slice->children().at(10));
        const auto modificationFlag = analyzer->tree().node(slice->children().at(11));
        const auto qp = analyzer->tree().node(slice->children().at(13));
        const auto payload = analyzer->tree().node(slice->children().at(14));
        QVERIFY(overrideCount.has_value());
        QVERIFY(modificationFlag.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(overrideCount->value().toULongLong(), quint64(32));
        QCOMPARE(overrideCount->state(), MaterializationState::Materialized);
        QCOMPARE(overrideCount->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = overrideCount->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Field value is above its @range maximum"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp."
                                "num_ref_idx_l0_active_minus1"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(188));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(11));
        QCOMPARE(modificationFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(199));
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(200));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(201));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(7));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsTruncatedReferenceIndexOverrideAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x70,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);
        QCOMPARE(slice->children().size(), std::size_t(10));
        const auto overrideFlag = analyzer->tree().node(slice->children().back());
        QVERIFY(overrideFlag.has_value());
        QCOMPARE(overrideFlag->name(), QStringLiteral("num_ref_idx_active_override_flag"));
        QCOMPARE(overrideFlag->value().toULongLong(), quint64(1));
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp."
                                "num_ref_idx_l0_active_minus1"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(188));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(4));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        const auto followingHeader = analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        const auto followingType = analyzer->tree().node(followingHeader->children().at(2));
        QVERIFY(followingType.has_value());
        QCOMPARE(followingType->name(), QStringLiteral("nal_unit_type"));
        QCOMPARE(followingType->value().toULongLong(), quint64(10));
    }

    void decodesTerminatingReferenceListModificationAfterReferenceIndexOverride() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x77, 0x25,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(17));
        const auto overrideCount = analyzer->tree().node(slice->children().at(10));
        const auto modificationFlag = analyzer->tree().node(slice->children().at(11));
        const auto operation = analyzer->tree().node(slice->children().at(12));
        const auto usesAbsDiff = analyzer->tree().node(slice->children().at(13));
        const auto qp = analyzer->tree().node(slice->children().at(15));
        const auto payload = analyzer->tree().node(slice->children().at(16));
        QVERIFY(overrideCount.has_value());
        QVERIFY(modificationFlag.has_value());
        QVERIFY(operation.has_value());
        QVERIFY(usesAbsDiff.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(overrideCount->name(), QStringLiteral("num_ref_idx_l0_active_minus1"));
        QCOMPARE(overrideCount->value().toULongLong(), quint64(2));
        QCOMPARE(modificationFlag->name(),
                 QStringLiteral("ref_pic_list_modification_flag_l0"));
        QCOMPARE(modificationFlag->value().toULongLong(), quint64(1));
        QCOMPARE(operation->name(), QStringLiteral("modification_of_pic_nums_idc[0]"));
        QCOMPARE(operation->value().toULongLong(), quint64(3));
        QCOMPARE(operation->metadata().typeName, QStringLiteral("ModificationOfPicNumsIdc"));
        QCOMPARE(usesAbsDiff->name(), QStringLiteral("uses_abs_diff_pic_num[0]"));
        QCOMPARE(usesAbsDiff->kind(), AnalysisNodeKind::ComputedField);
        QCOMPARE(usesAbsDiff->value().toBool(), false);
        QVERIFY(!usesAbsDiff->location().has_value());
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QVERIFY(slice->diagnostics().empty());
        QCOMPARE(modificationFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(191));
        QCOMPARE(operation->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(192));
        QCOMPARE(operation->location()->logicalRange().bitLength(), quint64(5));
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(197));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(198));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(2));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        const auto followingHeader = analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        const auto followingType = analyzer->tree().node(followingHeader->children().at(2));
        QVERIFY(followingType.has_value());
        QCOMPARE(followingType->name(), QStringLiteral("nal_unit_type"));
        QCOMPARE(followingType->value().toULongLong(), quint64(10));
    }

    void decodesAllReferenceListModificationOperationsAndKeepsPayloadOpaque() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x6d, 0xab, 0x21, 0x35, 0x40,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(25));

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("num_ref_idx_active_override_flag"),
            QStringLiteral("ref_pic_list_modification_flag_l0"),
            QStringLiteral("modification_of_pic_nums_idc[0]"),
            QStringLiteral("uses_abs_diff_pic_num[0]"),
            QStringLiteral("abs_diff_pic_num_minus1[0]"),
            QStringLiteral("modification_of_pic_nums_idc[1]"),
            QStringLiteral("uses_abs_diff_pic_num[1]"),
            QStringLiteral("abs_diff_pic_num_minus1[1]"),
            QStringLiteral("modification_of_pic_nums_idc[2]"),
            QStringLiteral("uses_abs_diff_pic_num[2]"),
            QStringLiteral("long_term_pic_num[2]"),
            QStringLiteral("modification_of_pic_nums_idc[3]"),
            QStringLiteral("uses_abs_diff_pic_num[3]"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto modificationFlag = analyzer->tree().node(slice->children().at(10));
        const auto operation0 = analyzer->tree().node(slice->children().at(11));
        const auto usesAbsDiff0 = analyzer->tree().node(slice->children().at(12));
        const auto absDiff0 = analyzer->tree().node(slice->children().at(13));
        const auto operation1 = analyzer->tree().node(slice->children().at(14));
        const auto usesAbsDiff1 = analyzer->tree().node(slice->children().at(15));
        const auto absDiff1 = analyzer->tree().node(slice->children().at(16));
        const auto operation2 = analyzer->tree().node(slice->children().at(17));
        const auto usesAbsDiff2 = analyzer->tree().node(slice->children().at(18));
        const auto longTerm2 = analyzer->tree().node(slice->children().at(19));
        const auto operation3 = analyzer->tree().node(slice->children().at(20));
        const auto usesAbsDiff3 = analyzer->tree().node(slice->children().at(21));
        const auto qp = analyzer->tree().node(slice->children().at(23));
        const auto payload = analyzer->tree().node(slice->children().at(24));
        QVERIFY(modificationFlag.has_value());
        QVERIFY(operation0.has_value());
        QVERIFY(usesAbsDiff0.has_value());
        QVERIFY(absDiff0.has_value());
        QVERIFY(operation1.has_value());
        QVERIFY(usesAbsDiff1.has_value());
        QVERIFY(absDiff1.has_value());
        QVERIFY(operation2.has_value());
        QVERIFY(usesAbsDiff2.has_value());
        QVERIFY(longTerm2.has_value());
        QVERIFY(operation3.has_value());
        QVERIFY(usesAbsDiff3.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(modificationFlag->value().toULongLong(), quint64(1));
        QCOMPARE(operation0->value().toULongLong(), quint64(0));
        QCOMPARE(usesAbsDiff0->value().toBool(), true);
        QCOMPARE(absDiff0->value().toULongLong(), quint64(2));
        QCOMPARE(operation1->value().toULongLong(), quint64(1));
        QCOMPARE(usesAbsDiff1->value().toBool(), true);
        QCOMPARE(absDiff1->value().toULongLong(), quint64(0));
        QCOMPARE(operation2->value().toULongLong(), quint64(2));
        QCOMPARE(usesAbsDiff2->value().toBool(), false);
        QCOMPARE(longTerm2->value().toULongLong(), quint64(3));
        QCOMPARE(operation3->value().toULongLong(), quint64(3));
        QCOMPARE(usesAbsDiff3->value().toBool(), false);
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        for (const auto& computed : {usesAbsDiff0, usesAbsDiff1, usesAbsDiff2, usesAbsDiff3}) {
            QCOMPARE(computed->kind(), AnalysisNodeKind::ComputedField);
            QVERIFY(!computed->location().has_value());
        }
        for (const auto& operation : {operation0, operation1, operation2, operation3}) {
            QCOMPARE(operation->metadata().typeName,
                     QStringLiteral("ModificationOfPicNumsIdc"));
        }
        QCOMPARE(modificationFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(188));
        QCOMPARE(operation0->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(189));
        QCOMPARE(operation0->location()->logicalRange().bitLength(), quint64(1));
        QCOMPARE(absDiff0->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(190));
        QCOMPARE(absDiff0->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(operation1->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(operation1->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(absDiff1->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(196));
        QCOMPARE(operation2->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(197));
        QCOMPARE(longTerm2->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(200));
        QCOMPARE(longTerm2->location()->logicalRange().bitLength(), quint64(5));
        QCOMPARE(operation3->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(205));
        QCOMPARE(operation3->location()->logicalRange().bitLength(), quint64(5));
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(210));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(211));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(13));
        QVERIFY(slice->diagnostics().empty());

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        const auto followingHeader = analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        const auto followingType = analyzer->tree().node(followingHeader->children().at(2));
        QVERIFY(followingType.has_value());
        QCOMPARE(followingType->value().toULongLong(), quint64(10));
    }

    void decodesEquivalentTypeFiveReferenceListTerminator() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0x9a, 0xa6, 0x93,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(16));
        const auto sliceType = analyzer->tree().node(slice->children().at(1));
        const auto modificationFlag = analyzer->tree().node(slice->children().at(10));
        const auto operation = analyzer->tree().node(slice->children().at(11));
        const auto usesAbsDiff = analyzer->tree().node(slice->children().at(12));
        const auto qp = analyzer->tree().node(slice->children().at(14));
        const auto payload = analyzer->tree().node(slice->children().at(15));
        QVERIFY(sliceType.has_value());
        QVERIFY(modificationFlag.has_value());
        QVERIFY(operation.has_value());
        QVERIFY(usesAbsDiff.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(sliceType->value().toULongLong(), quint64(5));
        QCOMPARE(modificationFlag->value().toULongLong(), quint64(1));
        QCOMPARE(operation->name(), QStringLiteral("modification_of_pic_nums_idc[0]"));
        QCOMPARE(operation->value().toULongLong(), quint64(3));
        QCOMPARE(usesAbsDiff->value().toBool(), false);
        QCOMPARE(modificationFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(192));
        QCOMPARE(operation->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(operation->location()->logicalRange().bitLength(), quint64(5));
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(198));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(199));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(1));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsUnknownReferenceListModificationOperationAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x69, 0x49, 0xaa,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);
        QCOMPARE(slice->children().size(), std::size_t(12));
        const auto operation = analyzer->tree().node(slice->children().back());
        QVERIFY(operation.has_value());
        QCOMPARE(operation->name(), QStringLiteral("modification_of_pic_nums_idc[0]"));
        QCOMPARE(operation->value().toULongLong(), quint64(4));
        QCOMPARE(operation->metadata().typeName, QStringLiteral("ModificationOfPicNumsIdc"));
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Field value is not declared by its enum type"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp."
                                "modification_of_pic_nums_idc[0]"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(189));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(5));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        const auto followingHeader = analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        const auto followingType = analyzer->tree().node(followingHeader->children().at(2));
        QVERIFY(followingType.has_value());
        QCOMPARE(followingType->value().toULongLong(), quint64(10));
    }

    void rejectsTruncatedReferenceListModificationOperationAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x68,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);
        QCOMPARE(slice->children().size(), std::size_t(11));
        const auto modificationFlag = analyzer->tree().node(slice->children().back());
        QVERIFY(modificationFlag.has_value());
        QCOMPARE(modificationFlag->name(),
                 QStringLiteral("ref_pic_list_modification_flag_l0"));
        QCOMPARE(modificationFlag->value().toULongLong(), quint64(1));
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp."
                                "modification_of_pic_nums_idc[0]"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(189));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(3));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        const auto followingHeader = analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        const auto followingType = analyzer->tree().node(followingHeader->children().at(2));
        QVERIFY(followingType.has_value());
        QCOMPARE(followingType->value().toULongLong(), quint64(10));
    }

    void rejectsTruncatedReferenceListModificationOperandAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x6c,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);
        QCOMPARE(slice->children().size(), std::size_t(13));
        const auto operation = analyzer->tree().node(slice->children().at(11));
        const auto usesAbsDiff = analyzer->tree().node(slice->children().at(12));
        QVERIFY(operation.has_value());
        QVERIFY(usesAbsDiff.has_value());
        QCOMPARE(operation->name(), QStringLiteral("modification_of_pic_nums_idc[0]"));
        QCOMPARE(operation->value().toULongLong(), quint64(0));
        QCOMPARE(usesAbsDiff->name(), QStringLiteral("uses_abs_diff_pic_num[0]"));
        QCOMPARE(usesAbsDiff->value().toBool(), true);
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp."
                                "abs_diff_pic_num_minus1[0]"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(190));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(2));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        const auto followingHeader = analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        const auto followingType = analyzer->tree().node(followingHeader->children().at(2));
        QVERIFY(followingType.has_value());
        QCOMPARE(followingType->value().toULongLong(), quint64(10));
    }

    void rejectsReferenceListModificationWithoutTerminatorAndContinues() {
        std::vector<std::byte> sourceBytes = bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x6b,
        });
        sourceBytes.insert(sourceBytes.end(), 31, std::byte{0xbb});
        for (const unsigned int value : {0xb8U, 0x00U, 0x00U, 0x01U, 0x0aU}) {
            sourceBytes.push_back(static_cast<std::byte>(value));
        }
        MemorySource source(std::move(sourceBytes));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);
        QCOMPARE(slice->children().size(), std::size_t(203));
        const auto finalOperation = analyzer->tree().node(slice->children().at(200));
        const auto finalComputed = analyzer->tree().node(slice->children().at(201));
        const auto finalOperand = analyzer->tree().node(slice->children().at(202));
        QVERIFY(finalOperation.has_value());
        QVERIFY(finalComputed.has_value());
        QVERIFY(finalOperand.has_value());
        QCOMPARE(finalOperation->name(),
                 QStringLiteral("modification_of_pic_nums_idc[63]"));
        QCOMPARE(finalOperation->value().toULongLong(), quint64(2));
        QCOMPARE(finalComputed->name(), QStringLiteral("uses_abs_diff_pic_num[63]"));
        QCOMPARE(finalComputed->value().toBool(), false);
        QCOMPARE(finalOperand->name(), QStringLiteral("long_term_pic_num[63]"));
        QCOMPARE(finalOperand->value().toULongLong(), quint64(0));
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Sentinel repeat did not terminate within its declared maximum"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp."
                                "modification_of_pic_nums_idc[63]"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(441));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(3));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        const auto followingHeader = analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        const auto followingType = analyzer->tree().node(followingHeader->children().at(2));
        QVERIFY(followingType.has_value());
        QCOMPARE(followingType->value().toULongLong(), quint64(10));
    }

    void decodesWeightedPredictionTableWithImportedDefaultCountInNonIdrPSlice() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xcf, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x67, 0x4e, 0x42, 0x98, 0xf0, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("num_ref_idx_active_override_flag"),
            QStringLiteral("ref_pic_list_modification_flag_l0"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("luma_log2_weight_denom"),
            QStringLiteral("chroma_log2_weight_denom"),
            QStringLiteral("effective_l0_count"),
            QStringLiteral("luma_weight_l0_flag[0]"),
            QStringLiteral("luma_weight_l0[0]"),
            QStringLiteral("luma_offset_l0[0]"),
            QStringLiteral("chroma_weight_l0_flag[0]"),
            QStringLiteral("chroma_weight_l0_cb[0]"),
            QStringLiteral("chroma_offset_l0_cb[0]"),
            QStringLiteral("chroma_weight_l0_cr[0]"),
            QStringLiteral("chroma_offset_l0_cr[0]"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        QCOMPARE(slice->children().size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };

        // The slice header omits num_ref_idx_l0_active_minus1, so optional_value()
        // falls back to the imported PPS default of 0, yielding a single entry.
        QCOMPARE(fieldNamed(QStringLiteral("num_ref_idx_active_override_flag"))
                     ->value()
                     .toULongLong(),
                 quint64(0));
        QCOMPARE(
            fieldNamed(QStringLiteral("uses_explicit_weighting"))->value().toBool(),
            true);
        QCOMPARE(fieldNamed(QStringLiteral("effective_l0_count"))->value().toULongLong(),
                 quint64(1));
        QCOMPARE(
            fieldNamed(QStringLiteral("luma_log2_weight_denom"))->value().toULongLong(),
            quint64(0));
        QCOMPARE(fieldNamed(QStringLiteral("luma_weight_l0[0]"))->value().toLongLong(),
                 qlonglong(1));
        QCOMPARE(fieldNamed(QStringLiteral("luma_offset_l0[0]"))->value().toLongLong(),
                 qlonglong(-1));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_weight_l0_cb[0]"))->value().toLongLong(),
            qlonglong(2));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_offset_l0_cb[0]"))->value().toLongLong(),
            qlonglong(-2));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_weight_l0_cr[0]"))->value().toLongLong(),
            qlonglong(3));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_offset_l0_cr[0]"))->value().toLongLong(),
            qlonglong(-3));

        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(payload.has_value());
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(220));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesWeightedPredictionTableWithOverriddenCountInNonIdrPSlice() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xcf, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x74, 0x4e, 0x22, 0x52, 0x6e, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("num_ref_idx_active_override_flag"),
            QStringLiteral("num_ref_idx_l0_active_minus1"),
            QStringLiteral("ref_pic_list_modification_flag_l0"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("luma_log2_weight_denom"),
            QStringLiteral("chroma_log2_weight_denom"),
            QStringLiteral("effective_l0_count"),
            QStringLiteral("luma_weight_l0_flag[0]"),
            QStringLiteral("luma_weight_l0[0]"),
            QStringLiteral("luma_offset_l0[0]"),
            QStringLiteral("chroma_weight_l0_flag[0]"),
            QStringLiteral("luma_weight_l0_flag[1]"),
            QStringLiteral("chroma_weight_l0_flag[1]"),
            QStringLiteral("chroma_weight_l0_cb[1]"),
            QStringLiteral("chroma_offset_l0_cb[1]"),
            QStringLiteral("chroma_weight_l0_cr[1]"),
            QStringLiteral("chroma_offset_l0_cr[1]"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        QCOMPARE(slice->children().size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };

        // The slice header declares num_ref_idx_l0_active_minus1, so optional_value()
        // selects it over the imported PPS default and the loop runs twice. Each entry
        // carries an independent pair of luma and chroma flags.
        QCOMPARE(fieldNamed(QStringLiteral("num_ref_idx_l0_active_minus1"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("effective_l0_count"))->value().toULongLong(),
                 quint64(2));
        QCOMPARE(
            fieldNamed(QStringLiteral("luma_log2_weight_denom"))->value().toULongLong(),
            quint64(1));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_log2_weight_denom"))->value().toULongLong(),
            quint64(2));
        QCOMPARE(fieldNamed(QStringLiteral("luma_weight_l0[0]"))->value().toLongLong(),
                 qlonglong(4));
        QCOMPARE(fieldNamed(QStringLiteral("luma_offset_l0[0]"))->value().toLongLong(),
                 qlonglong(0));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_weight_l0_flag[0]"))->value().toULongLong(),
            quint64(0));
        QCOMPARE(
            fieldNamed(QStringLiteral("luma_weight_l0_flag[1]"))->value().toULongLong(),
            quint64(0));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_weight_l0_cb[1]"))->value().toLongLong(),
            qlonglong(1));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_offset_l0_cr[1]"))->value().toLongLong(),
            qlonglong(-1));
    }

    void decodesCabacInitializationIdentifiersAndContinues() {
        struct CabacCase {
            quint64 value = 0;
            std::vector<std::byte> data;
            quint64 codewordLength = 0;
            quint64 qpOffset = 0;
            quint64 payloadOffset = 0;
            quint64 payloadLength = 0;
        };
        const std::vector<CabacCase> cases{
            {0,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xee, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x01, 0xea, 0x67,
                 0x00, 0x00, 0x01, 0x0a,
             }),
             1,
             190,
             191,
             1},
            {1,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xee, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x01, 0xea, 0x62, 0xd5,
                 0x00, 0x00, 0x01, 0x0a,
             }),
             3,
             192,
             193,
             7},
            {2,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xee, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x01, 0xea, 0x63, 0xd5,
                 0x00, 0x00, 0x01, 0x0a,
             }),
             3,
             192,
             193,
             7},
        };

        for (const auto& testCase : cases) {
            MemorySource source(testCase.data);
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto slice = analyzer->tree().node(rbsp->children().front());
            QVERIFY(slice.has_value());
            QCOMPARE(slice->state(), MaterializationState::Materialized);
            QCOMPARE(slice->children().size(), std::size_t(15));

            const auto cabacInitIdc = analyzer->tree().node(slice->children().at(12));
            const auto qp = analyzer->tree().node(slice->children().at(13));
            const auto payload = analyzer->tree().node(slice->children().at(14));
            QVERIFY(cabacInitIdc.has_value());
            QVERIFY(qp.has_value());
            QVERIFY(payload.has_value());
            QCOMPARE(cabacInitIdc->name(), QStringLiteral("cabac_init_idc"));
            QCOMPARE(cabacInitIdc->value().toULongLong(), testCase.value);
            QCOMPARE(cabacInitIdc->metadata().specification->clause,
                     QStringLiteral("7.3.3, 7.4.3"));
            QCOMPARE(cabacInitIdc->location()
                         ->sourceSpans()
                         .front()
                         .start()
                         .absoluteBitOffset(),
                     quint64(189));
            QCOMPARE(cabacInitIdc->location()->logicalRange().bitLength(),
                     testCase.codewordLength);
            QVERIFY(cabacInitIdc->diagnostics().empty());
            QCOMPARE(qp->name(), QStringLiteral("slice_qp_delta"));
            QCOMPARE(qp->value().toLongLong(), qlonglong(0));
            QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                     testCase.qpOffset);
            QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
            QCOMPARE(payload->name(), QStringLiteral("slice_data"));
            QCOMPARE(payload->state(), MaterializationState::Materialized);
            QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                     testCase.payloadOffset);
            QCOMPARE(payload->location()->logicalRange().bitLength(),
                     testCase.payloadLength);

            const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(followingNal.has_value());
            QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        }
    }

    void warnsForOutOfRangeCabacInitializationIdentifierAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x61, 0x2a,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(16));
        const auto cabacInitIdc = analyzer->tree().node(slice->children().at(12));
        const auto qp = analyzer->tree().node(slice->children().at(13));
        const auto deblockingMode = analyzer->tree().node(slice->children().at(14));
        const auto payload = analyzer->tree().node(slice->children().at(15));
        QVERIFY(cabacInitIdc.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(deblockingMode.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(cabacInitIdc->name(), QStringLiteral("cabac_init_idc"));
        QCOMPARE(cabacInitIdc->value().toULongLong(), quint64(3));
        QCOMPARE(cabacInitIdc->state(), MaterializationState::Materialized);
        QCOMPARE(cabacInitIdc->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = cabacInitIdc->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Field value is above its @range maximum"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp.cabac_init_idc"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(189));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(5));
        QCOMPARE(cabacInitIdc->location()->logicalRange().bitLength(), quint64(5));
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(194));
        QCOMPARE(deblockingMode->name(), QStringLiteral("disable_deblocking_filter_idc"));
        QCOMPARE(deblockingMode->value().toULongLong(), quint64(1));
        QCOMPARE(deblockingMode->location()
                     ->sourceSpans()
                     .front()
                     .start()
                     .absoluteBitOffset(),
                 quint64(195));
        QCOMPARE(deblockingMode->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->name(), QStringLiteral("slice_data"));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(198));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(2));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsTruncatedCabacInitializationIdentifierAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xee, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x60,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);
        QCOMPARE(slice->children().size(), std::size_t(12));
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp.cabac_init_idc"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(189));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(3));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        const auto followingHeader = analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        const auto followingType = analyzer->tree().node(followingHeader->children().at(2));
        QVERIFY(followingType.has_value());
        QCOMPARE(followingType->value().toULongLong(), quint64(10));
    }

    void decodesTheBoundedNonIdrBSliceTypeOneAndKeepsSliceDataOpaque() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9c, 0x40, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(16));

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("direct_spatial_mv_pred_flag"),
            QStringLiteral("num_ref_idx_active_override_flag"),
            QStringLiteral("ref_pic_list_modification_flag_l0"),
            QStringLiteral("ref_pic_list_modification_flag_l1"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto sliceType = analyzer->tree().node(slice->children().at(1));
        const auto isPSlice = analyzer->tree().node(slice->children().at(2));
        const auto isBSlice = analyzer->tree().node(slice->children().at(3));
        const auto usesLists = analyzer->tree().node(slice->children().at(4));
        const auto directFlag = analyzer->tree().node(slice->children().at(9));
        const auto listOneFlag = analyzer->tree().node(slice->children().at(12));
        const auto payload = analyzer->tree().node(slice->children().at(15));
        QVERIFY(sliceType.has_value());
        QVERIFY(isPSlice.has_value());
        QVERIFY(isBSlice.has_value());
        QVERIFY(usesLists.has_value());
        QVERIFY(directFlag.has_value());
        QVERIFY(listOneFlag.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(sliceType->value().toULongLong(), quint64(1));
        QCOMPARE(sliceType->metadata().typeName, QStringLiteral("NonIdrSliceType"));
        QCOMPARE(isPSlice->value().toBool(), false);
        QCOMPARE(isBSlice->kind(), AnalysisNodeKind::ComputedField);
        QCOMPARE(isBSlice->value().toBool(), true);
        QVERIFY(!isBSlice->location().has_value());
        QCOMPARE(usesLists->kind(), AnalysisNodeKind::ComputedField);
        QCOMPARE(usesLists->value().toBool(), true);
        QVERIFY(!usesLists->location().has_value());
        QCOMPARE(directFlag->value().toULongLong(), quint64(1));
        QCOMPARE(directFlag->metadata().specification->clause, QStringLiteral("7.3.3"));
        QCOMPARE(directFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(189));
        QCOMPARE(directFlag->location()->logicalRange().bitLength(), quint64(1));
        QCOMPARE(listOneFlag->value().toULongLong(), quint64(0));
        QCOMPARE(listOneFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(192));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->state(), MaterializationState::Materialized);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(194));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(14));
        QVERIFY(slice->diagnostics().empty());

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesTheEquivalentNonIdrBSliceTypeSix() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0x9e, 0xa7, 0x10, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(16));

        const auto sliceType = analyzer->tree().node(slice->children().at(1));
        const auto isBSlice = analyzer->tree().node(slice->children().at(3));
        const auto directFlag = analyzer->tree().node(slice->children().at(9));
        const auto payload = analyzer->tree().node(slice->children().at(15));
        QVERIFY(sliceType.has_value());
        QVERIFY(isBSlice.has_value());
        QVERIFY(directFlag.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(sliceType->value().toULongLong(), quint64(6));
        QCOMPARE(sliceType->location()->logicalRange().bitLength(), quint64(5));
        QCOMPARE(isBSlice->value().toBool(), true);
        QCOMPARE(directFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(191));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(196));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(12));
        QVERIFY(slice->diagnostics().empty());
    }

    void decodesTemporalDirectPredictionInNonIdrBSlice() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x98, 0x40, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(16));

        const auto directFlag = analyzer->tree().node(slice->children().at(9));
        const auto payload = analyzer->tree().node(slice->children().at(15));
        QVERIFY(directFlag.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(directFlag->name(), QStringLiteral("direct_spatial_mv_pred_flag"));
        QCOMPARE(directFlag->value().toULongLong(), quint64(0));
        QCOMPARE(directFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(189));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(194));
        QVERIFY(slice->diagnostics().empty());
    }

    void decodesNonIdrBSliceReferenceIndexOverridesForBothLists() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9e, 0xd1, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(18));

        const auto overrideFlag = analyzer->tree().node(slice->children().at(10));
        const auto listZeroCount = analyzer->tree().node(slice->children().at(11));
        const auto listOneCount = analyzer->tree().node(slice->children().at(12));
        const auto listZeroFlag = analyzer->tree().node(slice->children().at(13));
        const auto listOneFlag = analyzer->tree().node(slice->children().at(14));
        const auto payload = analyzer->tree().node(slice->children().at(17));
        QVERIFY(overrideFlag.has_value());
        QVERIFY(listZeroCount.has_value());
        QVERIFY(listOneCount.has_value());
        QVERIFY(listZeroFlag.has_value());
        QVERIFY(listOneFlag.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(overrideFlag->value().toULongLong(), quint64(1));
        QCOMPARE(listZeroCount->name(), QStringLiteral("num_ref_idx_l0_active_minus1"));
        QCOMPARE(listZeroCount->value().toULongLong(), quint64(2));
        QCOMPARE(listZeroCount->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(191));
        QCOMPARE(listZeroCount->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(listOneCount->name(), QStringLiteral("num_ref_idx_l1_active_minus1"));
        QCOMPARE(listOneCount->value().toULongLong(), quint64(1));
        QCOMPARE(listOneCount->metadata().specification->clause,
                 QStringLiteral("7.3.3, 7.4.3"));
        QCOMPARE(listOneCount->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(194));
        QCOMPARE(listOneCount->location()->logicalRange().bitLength(), quint64(3));
        QVERIFY(listOneCount->diagnostics().empty());
        QCOMPARE(listZeroFlag->value().toULongLong(), quint64(0));
        QCOMPARE(listOneFlag->value().toULongLong(), quint64(0));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(200));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(8));
        QVERIFY(slice->diagnostics().empty());
    }

    void decodesNonIdrBSliceReferenceListModificationLoop() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9d, 0x90, 0x88, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(21));

        const auto listZeroFlag = analyzer->tree().node(slice->children().at(11));
        const auto operation0 = analyzer->tree().node(slice->children().at(12));
        const auto usesAbsDiff0 = analyzer->tree().node(slice->children().at(13));
        const auto absDiff0 = analyzer->tree().node(slice->children().at(14));
        const auto operation1 = analyzer->tree().node(slice->children().at(15));
        const auto usesAbsDiff1 = analyzer->tree().node(slice->children().at(16));
        const auto listOneFlag = analyzer->tree().node(slice->children().at(17));
        const auto payload = analyzer->tree().node(slice->children().at(20));
        QVERIFY(listZeroFlag.has_value());
        QVERIFY(operation0.has_value());
        QVERIFY(usesAbsDiff0.has_value());
        QVERIFY(absDiff0.has_value());
        QVERIFY(operation1.has_value());
        QVERIFY(usesAbsDiff1.has_value());
        QVERIFY(listOneFlag.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(listZeroFlag->value().toULongLong(), quint64(1));
        QCOMPARE(operation0->name(), QStringLiteral("modification_of_pic_nums_idc[0]"));
        QCOMPARE(operation0->value().toULongLong(), quint64(0));
        QCOMPARE(usesAbsDiff0->value().toBool(), true);
        QCOMPARE(absDiff0->name(), QStringLiteral("abs_diff_pic_num_minus1[0]"));
        QCOMPARE(absDiff0->value().toULongLong(), quint64(3));
        QCOMPARE(operation1->name(), QStringLiteral("modification_of_pic_nums_idc[1]"));
        QCOMPARE(operation1->value().toULongLong(), quint64(3));
        QCOMPARE(usesAbsDiff1->value().toBool(), false);
        QCOMPARE(listOneFlag->name(),
                 QStringLiteral("ref_pic_list_modification_flag_l1"));
        QCOMPARE(listOneFlag->value().toULongLong(), quint64(0));
        QCOMPARE(listOneFlag->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(203));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(205));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(11));
        QVERIFY(slice->diagnostics().empty());
    }

    void decodesTerminatingListOneModificationInNonIdrBSlice() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9c, 0x92, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(18));

        const auto listOneFlag = analyzer->tree().node(slice->children().at(12));
        const auto operation = analyzer->tree().node(slice->children().at(13));
        const auto usesAbsDiff = analyzer->tree().node(slice->children().at(14));
        const auto qp = analyzer->tree().node(slice->children().at(16));
        const auto payload = analyzer->tree().node(slice->children().at(17));
        QVERIFY(listOneFlag.has_value());
        QVERIFY(operation.has_value());
        QVERIFY(usesAbsDiff.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(listOneFlag->name(),
                 QStringLiteral("ref_pic_list_modification_flag_l1"));
        QCOMPARE(listOneFlag->value().toULongLong(), quint64(1));
        QCOMPARE(operation->name(),
                 QStringLiteral("modification_of_pic_nums_idc_l1[0]"));
        QCOMPARE(operation->value().toULongLong(), quint64(3));
        QCOMPARE(operation->metadata().typeName,
                 QStringLiteral("ModificationOfPicNumsIdc"));
        QCOMPARE(operation->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(operation->location()->logicalRange().bitLength(), quint64(5));
        QCOMPARE(usesAbsDiff->name(), QStringLiteral("uses_abs_diff_pic_num_l1[0]"));
        QCOMPARE(usesAbsDiff->kind(), AnalysisNodeKind::ComputedField);
        QCOMPARE(usesAbsDiff->value().toBool(), false);
        QVERIFY(!usesAbsDiff->location().has_value());
        QCOMPARE(qp->name(), QStringLiteral("slice_qp_delta"));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(199));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(9));
        QVERIFY(slice->diagnostics().empty());

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesAllListOneModificationOperationsAndKeepsPayloadOpaque() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9c, 0xc8, 0x9b, 0x44, 0x80, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(27));

        const std::vector<std::pair<QString, quint64>> expectedLoop{
            {QStringLiteral("modification_of_pic_nums_idc_l1[0]"), quint64(0)},
            {QStringLiteral("abs_diff_pic_num_minus1_l1[0]"), quint64(3)},
            {QStringLiteral("modification_of_pic_nums_idc_l1[1]"), quint64(1)},
            {QStringLiteral("abs_diff_pic_num_minus1_l1[1]"), quint64(2)},
            {QStringLiteral("modification_of_pic_nums_idc_l1[2]"), quint64(2)},
            {QStringLiteral("long_term_pic_num_l1[2]"), quint64(1)},
            {QStringLiteral("modification_of_pic_nums_idc_l1[3]"), quint64(3)},
        };
        const std::vector<std::size_t> loopIndices{13, 15, 16, 18, 19, 21, 22};
        for (std::size_t entry = 0; entry < expectedLoop.size(); ++entry) {
            const auto node =
                analyzer->tree().node(slice->children().at(loopIndices.at(entry)));
            QVERIFY2(node.has_value(), qPrintable(expectedLoop.at(entry).first));
            QCOMPARE(node->name(), expectedLoop.at(entry).first);
            QCOMPARE(node->value().toULongLong(), expectedLoop.at(entry).second);
        }

        const auto firstComputed = analyzer->tree().node(slice->children().at(14));
        const auto lastComputed = analyzer->tree().node(slice->children().at(23));
        const auto qp = analyzer->tree().node(slice->children().at(25));
        const auto payload = analyzer->tree().node(slice->children().at(26));
        QVERIFY(firstComputed.has_value());
        QVERIFY(lastComputed.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(firstComputed->name(), QStringLiteral("uses_abs_diff_pic_num_l1[0]"));
        QCOMPARE(firstComputed->value().toBool(), true);
        QCOMPARE(lastComputed->name(), QStringLiteral("uses_abs_diff_pic_num_l1[3]"));
        QCOMPARE(lastComputed->value().toBool(), false);
        QCOMPARE(qp->name(), QStringLiteral("slice_qp_delta"));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(217));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(15));
        QVERIFY(slice->diagnostics().empty());
    }

    void decodesIndependentListZeroAndListOneModificationLoops() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9d, 0xa2, 0x59, 0x49, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(26));

        const auto listZeroFlag = analyzer->tree().node(slice->children().at(11));
        const auto listZeroOp = analyzer->tree().node(slice->children().at(12));
        const auto listZeroOperand = analyzer->tree().node(slice->children().at(14));
        const auto listZeroTerm = analyzer->tree().node(slice->children().at(15));
        const auto listOneFlag = analyzer->tree().node(slice->children().at(17));
        const auto listOneOp = analyzer->tree().node(slice->children().at(18));
        const auto listOneOperand = analyzer->tree().node(slice->children().at(20));
        const auto listOneTerm = analyzer->tree().node(slice->children().at(21));
        const auto payload = analyzer->tree().node(slice->children().at(25));
        QVERIFY(listZeroFlag.has_value());
        QVERIFY(listZeroOp.has_value());
        QVERIFY(listZeroOperand.has_value());
        QVERIFY(listZeroTerm.has_value());
        QVERIFY(listOneFlag.has_value());
        QVERIFY(listOneOp.has_value());
        QVERIFY(listOneOperand.has_value());
        QVERIFY(listOneTerm.has_value());
        QVERIFY(payload.has_value());

        QCOMPARE(listZeroFlag->name(),
                 QStringLiteral("ref_pic_list_modification_flag_l0"));
        QCOMPARE(listZeroOp->name(), QStringLiteral("modification_of_pic_nums_idc[0]"));
        QCOMPARE(listZeroOp->value().toULongLong(), quint64(0));
        QCOMPARE(listZeroOperand->name(), QStringLiteral("abs_diff_pic_num_minus1[0]"));
        QCOMPARE(listZeroOperand->value().toULongLong(), quint64(1));
        QCOMPARE(listZeroTerm->name(), QStringLiteral("modification_of_pic_nums_idc[1]"));
        QCOMPARE(listZeroTerm->value().toULongLong(), quint64(3));

        QCOMPARE(listOneFlag->name(),
                 QStringLiteral("ref_pic_list_modification_flag_l1"));
        QCOMPARE(listOneOp->name(),
                 QStringLiteral("modification_of_pic_nums_idc_l1[0]"));
        QCOMPARE(listOneOp->value().toULongLong(), quint64(2));
        QCOMPARE(listOneOperand->name(), QStringLiteral("long_term_pic_num_l1[0]"));
        QCOMPARE(listOneOperand->value().toULongLong(), quint64(4));
        QCOMPARE(listOneOperand->location()
                     ->sourceSpans()
                     .front()
                     .start()
                     .absoluteBitOffset(),
                 quint64(205));
        QCOMPARE(listOneTerm->name(),
                 QStringLiteral("modification_of_pic_nums_idc_l1[1]"));
        QCOMPARE(listOneTerm->value().toULongLong(), quint64(3));

        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(216));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(8));
        QVERIFY(slice->diagnostics().empty());
    }

    void rejectsUnknownListOneModificationOperationAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9c, 0x96, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);
        QCOMPARE(slice->children().size(), std::size_t(14));

        const auto operation = analyzer->tree().node(slice->children().back());
        QVERIFY(operation.has_value());
        QCOMPARE(operation->name(),
                 QStringLiteral("modification_of_pic_nums_idc_l1[0]"));
        QCOMPARE(operation->value().toULongLong(), quint64(4));
        QCOMPARE(operation->metadata().typeName,
                 QStringLiteral("ModificationOfPicNumsIdc"));
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Field value is not declared by its enum type"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp."
                                "modification_of_pic_nums_idc_l1[0]"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(5));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        const auto followingHeader = analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        const auto followingType = analyzer->tree().node(followingHeader->children().at(2));
        QVERIFY(followingType.has_value());
        QCOMPARE(followingType->value().toULongLong(), quint64(10));
    }

    void decodesExplicitWeightedBipredictionTableForBothReferenceLists() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x78, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x98, 0x35, 0x4e, 0x49, 0x4c, 0x40, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("direct_spatial_mv_pred_flag"),
            QStringLiteral("num_ref_idx_active_override_flag"),
            QStringLiteral("ref_pic_list_modification_flag_l0"),
            QStringLiteral("ref_pic_list_modification_flag_l1"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("luma_log2_weight_denom"),
            QStringLiteral("chroma_log2_weight_denom"),
            QStringLiteral("effective_l0_count"),
            QStringLiteral("luma_weight_l0_flag[0]"),
            QStringLiteral("luma_weight_l0[0]"),
            QStringLiteral("luma_offset_l0[0]"),
            QStringLiteral("chroma_weight_l0_flag[0]"),
            QStringLiteral("chroma_weight_l0_cb[0]"),
            QStringLiteral("chroma_offset_l0_cb[0]"),
            QStringLiteral("chroma_weight_l0_cr[0]"),
            QStringLiteral("chroma_offset_l0_cr[0]"),
            QStringLiteral("effective_l1_count"),
            QStringLiteral("luma_weight_l1_flag[0]"),
            QStringLiteral("chroma_weight_l1_flag[0]"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        QCOMPARE(slice->children().size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };

        // weighted_bipred_idc == 1 selects the explicit table for a B slice, so both
        // list 0 and list 1 are traversed. Neither count is overridden in the header,
        // so both optional_value() leaves fall back to their imported PPS defaults.
        QCOMPARE(fieldNamed(QStringLiteral("is_b_slice"))->value().toBool(), true);
        QCOMPARE(
            fieldNamed(QStringLiteral("uses_explicit_weighting"))->value().toBool(),
            true);
        QCOMPARE(fieldNamed(QStringLiteral("effective_l0_count"))->value().toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("effective_l1_count"))->value().toULongLong(),
                 quint64(1));
        QCOMPARE(
            fieldNamed(QStringLiteral("luma_log2_weight_denom"))->value().toULongLong(),
            quint64(2));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_log2_weight_denom"))->value().toULongLong(),
            quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("luma_weight_l0[0]"))->value().toLongLong(),
                 qlonglong(1));
        QCOMPARE(fieldNamed(QStringLiteral("luma_offset_l0[0]"))->value().toLongLong(),
                 qlonglong(-1));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_weight_l0_cr[0]"))->value().toLongLong(),
            qlonglong(-2));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_offset_l0_cr[0]"))->value().toLongLong(),
            qlonglong(3));
        // Both list 1 flags are clear, so the entry contributes no weight codewords.
        QCOMPARE(
            fieldNamed(QStringLiteral("luma_weight_l1_flag[0]"))->value().toULongLong(),
            quint64(0));
        QCOMPARE(
            fieldNamed(QStringLiteral("chroma_weight_l1_flag[0]"))->value().toULongLong(),
            quint64(0));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesExplicitWeightedBipredictionTableWithOverriddenCounts() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x78, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9a, 0xa6, 0x48, 0x97, 0x40, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("direct_spatial_mv_pred_flag"),
            QStringLiteral("num_ref_idx_active_override_flag"),
            QStringLiteral("num_ref_idx_l0_active_minus1"),
            QStringLiteral("num_ref_idx_l1_active_minus1"),
            QStringLiteral("ref_pic_list_modification_flag_l0"),
            QStringLiteral("ref_pic_list_modification_flag_l1"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("luma_log2_weight_denom"),
            QStringLiteral("chroma_log2_weight_denom"),
            QStringLiteral("effective_l0_count"),
            QStringLiteral("luma_weight_l0_flag[0]"),
            QStringLiteral("chroma_weight_l0_flag[0]"),
            QStringLiteral("luma_weight_l0_flag[1]"),
            QStringLiteral("luma_weight_l0[1]"),
            QStringLiteral("luma_offset_l0[1]"),
            QStringLiteral("chroma_weight_l0_flag[1]"),
            QStringLiteral("effective_l1_count"),
            QStringLiteral("luma_weight_l1_flag[0]"),
            QStringLiteral("luma_weight_l1[0]"),
            QStringLiteral("luma_offset_l1[0]"),
            QStringLiteral("chroma_weight_l1_flag[0]"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        QCOMPARE(slice->children().size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };

        // Both counts are declared in the slice header, so each optional_value() leaf
        // selects its declared field instead of the imported PPS default. The two
        // lists therefore run to different lengths.
        QCOMPARE(fieldNamed(QStringLiteral("num_ref_idx_l0_active_minus1"))
                     ->value()
                     .toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("num_ref_idx_l1_active_minus1"))
                     ->value()
                     .toULongLong(),
                 quint64(0));
        QCOMPARE(fieldNamed(QStringLiteral("effective_l0_count"))->value().toULongLong(),
                 quint64(2));
        QCOMPARE(fieldNamed(QStringLiteral("effective_l1_count"))->value().toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("luma_weight_l0[1]"))->value().toLongLong(),
                 qlonglong(2));
        QCOMPARE(fieldNamed(QStringLiteral("luma_offset_l0[1]"))->value().toLongLong(),
                 qlonglong(1));
        QCOMPARE(fieldNamed(QStringLiteral("luma_weight_l1[0]"))->value().toLongLong(),
                 qlonglong(-1));
        QCOMPARE(fieldNamed(QStringLiteral("luma_offset_l1[0]"))->value().toLongLong(),
                 qlonglong(0));
    }

    void reportsTruncatedExplicitWeightedPredictionTable() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xcf, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x67, 0x40,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto truncatedNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(truncatedNal.has_value());
        const auto rbsp = analyzer->tree().node(truncatedNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());

        // The stream ends between luma_weight_l0[0] and luma_offset_l0[0], so the
        // table stops mid-entry and the partial prefix stays materialized.
        const auto lastChild = analyzer->tree().node(slice->children().back());
        QVERIFY(lastChild.has_value());
        QCOMPARE(lastChild->name(), QStringLiteral("luma_weight_l0[0]"));
        QCOMPARE(lastChild->value().toLongLong(), qlonglong(1));

        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Unable to read complete Exp-Golomb codeword"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral(
                     "NonIdrSliceLayerWithoutPartitioningRbsp.luma_offset_l0[0]"));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesTheBottomFieldPictureNonIdrSliceHeader() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0x24,
            0x00, 0x00, 0x01, 0x68, 0xde, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x61, 0xeb, 0x98, 0x40, 0x80,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("is_p_slice"),
            QStringLiteral("is_b_slice"),
            QStringLiteral("uses_reference_lists"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("field_pic_flag"),
            QStringLiteral("bottom_field_flag"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("num_ref_idx_active_override_flag"),
            QStringLiteral("ref_pic_list_modification_flag_l0"),
            QStringLiteral("uses_explicit_weighting"),
            QStringLiteral("adaptive_ref_pic_marking_mode_flag"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        QCOMPARE(slice->children().size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };

        QCOMPARE(fieldNamed(QStringLiteral("frame_num"))->value().toULongLong(),
                 quint64(5));
        QCOMPARE(fieldNamed(QStringLiteral("field_pic_flag"))->value().toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("bottom_field_flag"))->value().toULongLong(),
                 quint64(1));
        QCOMPARE(fieldNamed(QStringLiteral("pic_order_cnt_lsb"))->value().toULongLong(),
                 quint64(3));
        // The PPS sets bottom_field_pic_order_in_frame_present_flag, but a field
        // picture carries no bottom-field delta, so the computed guard is false.
        QCOMPARE(fieldNamed(QStringLiteral("has_delta_pic_order_cnt_bottom"))
                     ->value()
                     .toBool(),
                 false);
        QVERIFY(!fieldNamed(QStringLiteral("delta_pic_order_cnt_bottom")).has_value());
    }

    void decodesTheMbaffFrameNonIdrSliceHeaderWithBottomFieldPictureOrderDelta() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0x64,
            0x00, 0x00, 0x01, 0x68, 0xde, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x61, 0xea, 0x36, 0x10, 0x80,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };

        // An MBAFF stream clears frame_mbs_only_flag and then codes a frame, so
        // field_pic_flag is present and zero while bottom_field_flag is absent.
        QCOMPARE(fieldNamed(QStringLiteral("field_pic_flag"))->value().toULongLong(),
                 quint64(0));
        QVERIFY(!fieldNamed(QStringLiteral("bottom_field_flag")).has_value());
        QCOMPARE(fieldNamed(QStringLiteral("has_delta_pic_order_cnt_bottom"))
                     ->value()
                     .toBool(),
                 true);
        const auto delta = fieldNamed(QStringLiteral("delta_pic_order_cnt_bottom"));
        QVERIFY(delta.has_value());
        QCOMPARE(delta->value().toLongLong(), qlonglong(-1));
    }

    void decodesTheTopFieldPictureIdrSliceHeader() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0x24,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x65, 0x88, 0xad, 0x31, 0x00, 0x80,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->name(),
                 QStringLiteral("IdrSliceLayerWithoutPartitioningRbsp"));
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());

        const std::vector expectedNames{
            QStringLiteral("first_mb_in_slice"),
            QStringLiteral("slice_type"),
            QStringLiteral("pic_parameter_set_id"),
            QStringLiteral("frame_num"),
            QStringLiteral("field_pic_flag"),
            QStringLiteral("bottom_field_flag"),
            QStringLiteral("idr_pic_id"),
            QStringLiteral("pic_order_cnt_lsb"),
            QStringLiteral("has_delta_pic_order_cnt_bottom"),
            QStringLiteral("no_output_of_prior_pics_flag"),
            QStringLiteral("long_term_reference_flag"),
            QStringLiteral("slice_qp_delta"),
            QStringLiteral("slice_data"),
        };
        QCOMPARE(slice->children().size(), expectedNames.size());
        for (std::size_t index = 0; index < expectedNames.size(); ++index) {
            const auto child = analyzer->tree().node(slice->children().at(index));
            QVERIFY(child.has_value());
            QCOMPARE(child->name(), expectedNames.at(index));
        }

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };

        QCOMPARE(fieldNamed(QStringLiteral("field_pic_flag"))->value().toULongLong(),
                 quint64(1));
        // A top field clears bottom_field_flag; the field is still present.
        QCOMPARE(fieldNamed(QStringLiteral("bottom_field_flag"))->value().toULongLong(),
                 quint64(0));
        QCOMPARE(fieldNamed(QStringLiteral("idr_pic_id"))->value().toULongLong(),
                 quint64(0));
        QCOMPARE(fieldNamed(QStringLiteral("pic_order_cnt_lsb"))->value().toULongLong(),
                 quint64(3));
    }

    void reportsTruncationBetweenFieldPictureFlagAndBottomFieldFlag() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0x89, 0xd0, 0x28, 0x3c, 0x90,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x61, 0xe0, 0x0b,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());

        // A twelve-bit frame_num places field_pic_flag on the last bit of the
        // payload, so bottom_field_flag has no source left to read.
        const auto lastChild = analyzer->tree().node(slice->children().back());
        QVERIFY(lastChild.has_value());
        QCOMPARE(lastChild->name(), QStringLiteral("field_pic_flag"));
        QCOMPARE(lastChild->value().toULongLong(), quint64(1));

        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::TruncatedSource);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Unable to read complete syntax field"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral(
                     "NonIdrSliceLayerWithoutPartitioningRbsp.bottom_field_flag"));
    }

    void omitsFieldPictureFlagsForAProgressiveSequence() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x61, 0xea, 0x61, 0x00, 0x80,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QVERIFY(slice->diagnostics().empty());

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };

        // frame_mbs_only_flag is one, so neither field flag is read and
        // optional_value() supplies the clause 7.4.3 inference of zero.
        QVERIFY(!fieldNamed(QStringLiteral("field_pic_flag")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("bottom_field_flag")).has_value());
        const auto guard = fieldNamed(QStringLiteral("has_delta_pic_order_cnt_bottom"));
        QVERIFY(guard.has_value());
        QCOMPARE(guard->value().toBool(), false);
        QVERIFY(!fieldNamed(QStringLiteral("delta_pic_order_cnt_bottom")).has_value());
        QCOMPARE(fieldNamed(QStringLiteral("frame_num"))->value().toULongLong(),
                 quint64(5));
    }

    void acceptsImplicitWeightedBipredictionInNonIdrBSlice() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0xb8, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9c, 0x40, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(16));

        const auto payload = analyzer->tree().node(slice->children().at(15));
        QVERIFY(payload.has_value());
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(194));
        QVERIFY(slice->diagnostics().empty());
    }

    void decodesCabacInitializationIdentifierForNonIdrBSlice() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xee, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xaa, 0x9c, 0x60, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(17));

        const auto listOneFlag = analyzer->tree().node(slice->children().at(12));
        const auto cabacInitIdc = analyzer->tree().node(slice->children().at(14));
        const auto qp = analyzer->tree().node(slice->children().at(15));
        const auto payload = analyzer->tree().node(slice->children().at(16));
        QVERIFY(listOneFlag.has_value());
        QVERIFY(cabacInitIdc.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(listOneFlag->name(),
                 QStringLiteral("ref_pic_list_modification_flag_l1"));
        QCOMPARE(cabacInitIdc->name(), QStringLiteral("cabac_init_idc"));
        QCOMPARE(cabacInitIdc->value().toULongLong(), quint64(0));
        QCOMPARE(cabacInitIdc->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(cabacInitIdc->location()->logicalRange().bitLength(), quint64(1));
        QVERIFY(cabacInitIdc->diagnostics().empty());
        QCOMPARE(qp->name(), QStringLiteral("slice_qp_delta"));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(195));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(13));
        QVERIFY(slice->diagnostics().empty());
    }

    void decodesSlidingWindowMarkingForAReferenceNonIdrSlice() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x41, 0xea, 0x62, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                    : analyzer->tree().node(*found);
        };
        const auto markingMode =
            fieldNamed(QStringLiteral("adaptive_ref_pic_marking_mode_flag"));
        QVERIFY(markingMode.has_value());
        QCOMPARE(markingMode->value().toULongLong(), quint64(0));
        QCOMPARE(markingMode->metadata().specification->clause,
                 QStringLiteral("7.3.3.3"));
        QCOMPARE(markingMode->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(189));
        QCOMPARE(markingMode->location()->logicalRange().bitLength(), quint64(1));
        QVERIFY(
            !fieldNamed(QStringLiteral("memory_management_control_operation[0]"))
                 .has_value());

        const auto qp = fieldNamed(QStringLiteral("slice_qp_delta"));
        QVERIFY(qp.has_value());
        QCOMPARE(qp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(190));
        QVERIFY(slice->diagnostics().empty());

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void omitsMarkingFieldsForANonReferenceNonIdrSlice() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x01, 0xea, 0x64, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);

        for (const auto childId : slice->children()) {
            const auto child = analyzer->tree().node(childId);
            QVERIFY(child.has_value());
            QVERIFY(child->name() !=
                    QStringLiteral("adaptive_ref_pic_marking_mode_flag"));
            QVERIFY(child->name() !=
                    QStringLiteral("memory_management_control_operation[0]"));
        }
        QVERIFY(slice->diagnostics().empty());
    }

    void decodesEachAdaptiveMarkingOperandSetAndContinues() {
        struct MarkingCase final {
            QString operandName;
            quint64 operation = 0;
            quint64 operandValue = 0;
            std::vector<std::byte> data;
            quint64 operationOffset = 0;
            quint64 operationLength = 0;
            quint64 operandOffset = 0;
            quint64 operandLength = 0;
            quint64 terminatorOffset = 0;
        };
        const std::vector<MarkingCase> cases{
            {QStringLiteral("difference_of_pic_nums_minus1[0]"),
             1,
             3,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x41, 0xea, 0x65, 0x13, 0xd5,
                 0x00, 0x00, 0x01, 0x0a,
             }),
             190, 3, 193, 5, 198},
            {QStringLiteral("long_term_pic_num_mmco[0]"),
             2,
             1,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf6, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x41, 0xea, 0x65, 0xac, 0xd5,
                 0x00, 0x00, 0x01, 0x0a,
             }),
             190, 3, 193, 3, 196},
            {QStringLiteral("max_long_term_frame_idx_plus1[0]"),
             4,
             2,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf6, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x41, 0xea, 0x64, 0xaf, 0xd5,
                 0x00, 0x00, 0x01, 0x0a,
             }),
             190, 5, 195, 3, 198},
            {QStringLiteral("long_term_frame_idx[0]"),
             6,
             1,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf6, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x41, 0xea, 0x64, 0xeb, 0xd5,
                 0x00, 0x00, 0x01, 0x0a,
             }),
             190, 5, 195, 3, 198},
        };

        for (const auto& testCase : cases) {
            MemorySource source(testCase.data);
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto slice = analyzer->tree().node(rbsp->children().front());
            QVERIFY(slice.has_value());
            QVERIFY2(slice->state() == MaterializationState::Materialized,
                     qPrintable(testCase.operandName));

            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    slice->children().begin(),
                    slice->children().end(),
                    [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == slice->children().end() ? std::nullopt
                                                        : analyzer->tree().node(*found);
            };

            const auto operation =
                fieldNamed(QStringLiteral("memory_management_control_operation[0]"));
            QVERIFY2(operation.has_value(), qPrintable(testCase.operandName));
            QCOMPARE(operation->value().toULongLong(), testCase.operation);
            QCOMPARE(operation->metadata().typeName,
                     QStringLiteral("MemoryManagementControlOperation"));
            QCOMPARE(operation->metadata().specification->clause,
                     QStringLiteral("7.3.3.3, 7.4.3.3"));
            QCOMPARE(operation->location()->sourceSpans().front().start().absoluteBitOffset(),
                     testCase.operationOffset);
            QCOMPARE(operation->location()->logicalRange().bitLength(),
                     testCase.operationLength);

            const auto operand = fieldNamed(testCase.operandName);
            QVERIFY2(operand.has_value(), qPrintable(testCase.operandName));
            QCOMPARE(operand->value().toULongLong(), testCase.operandValue);
            QCOMPARE(operand->location()->sourceSpans().front().start().absoluteBitOffset(),
                     testCase.operandOffset);
            QCOMPARE(operand->location()->logicalRange().bitLength(),
                     testCase.operandLength);
            QVERIFY(operand->diagnostics().empty());

            const auto terminator =
                fieldNamed(QStringLiteral("memory_management_control_operation[1]"));
            QVERIFY2(terminator.has_value(), qPrintable(testCase.operandName));
            QCOMPARE(terminator->value().toULongLong(), quint64(0));
            QCOMPARE(terminator->location()->sourceSpans().front().start().absoluteBitOffset(),
                     testCase.terminatorOffset);

            QVERIFY(slice->diagnostics().empty());
            const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(followingNal.has_value());
            QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        }
    }

    void rejectsAdaptiveMarkingOperandsBeyondDerivedBoundsAndContinues() {
        struct InvalidCase final {
            QString fieldName;
            quint64 operation = 0;
            quint64 operandOffset = 0;
            quint64 operandLength = 0;
            std::vector<std::byte> data;
        };
        const std::vector<InvalidCase> cases{
            {QStringLiteral("difference_of_pic_nums_minus1[0]"),
             1,
             193,
             9,
             replaceCodewordBeforeNextNal(
                 bytes({
                     0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                     0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
                     0x00, 0x00, 0x01, 0x41, 0xea, 0x65, 0x13, 0xd5,
                     0x00, 0x00, 0x01, 0x0a,
                 }),
                 193,
                 5,
                 16)},
            {QStringLiteral("difference_of_pic_nums_minus1[0]"),
             3,
             195,
             9,
             replaceCodewordBeforeNextNal(
                 replaceCodewordBeforeNextNal(
                     bytes({
                         0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a,
                         0x0f, 0xc8,
                         0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
                         0x00, 0x00, 0x01, 0x41, 0xea, 0x65, 0x13, 0xd5,
                         0x00, 0x00, 0x01, 0x0a,
                     }),
                     190,
                     3,
                     3),
                 195,
                 5,
                 16)},
            {QStringLiteral("long_term_pic_num_mmco[0]"),
             2,
             193,
             3,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x41, 0xea, 0x65, 0xac, 0xd5,
                 0x00, 0x00, 0x01, 0x0a,
             })},
            {QStringLiteral("max_long_term_frame_idx_plus1[0]"),
             4,
             195,
             3,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x41, 0xea, 0x64, 0xaf, 0xd5,
                 0x00, 0x00, 0x01, 0x0a,
             })},
            {QStringLiteral("long_term_frame_idx[0]"),
             6,
             195,
             3,
             bytes({
                 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
                 0x00, 0x00, 0x01, 0x41, 0xea, 0x64, 0xeb, 0xd5,
                 0x00, 0x00, 0x01, 0x0a,
             })},
        };

        for (const auto& testCase : cases) {
            MemorySource source(testCase.data);
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
            QVERIFY(invalidNal.has_value());
            QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
            const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto slice = analyzer->tree().node(rbsp->children().front());
            QVERIFY(slice.has_value());
            QCOMPARE(slice->state(), MaterializationState::Invalid);

            const auto operation = std::find_if(
                slice->children().begin(),
                slice->children().end(),
                [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node &&
                           node->name() == QStringLiteral(
                                               "memory_management_control_operation[0]");
                });
            QVERIFY(operation != slice->children().end());
            const auto operationNode = analyzer->tree().node(*operation);
            QVERIFY(operationNode.has_value());
            QCOMPARE(operationNode->value().toULongLong(), testCase.operation);

            const auto operand = std::find_if(
                slice->children().begin(),
                slice->children().end(),
                [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == testCase.fieldName;
                });
            QVERIFY(operand != slice->children().end());
            const auto operandNode = analyzer->tree().node(*operand);
            QVERIFY(operandNode.has_value());
            QCOMPARE(operandNode->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     testCase.operandOffset);
            QCOMPARE(operandNode->location()->logicalRange().bitLength(),
                     testCase.operandLength);

            QCOMPARE(slice->diagnostics().size(), std::size_t(1));
            const auto& diagnostic = slice->diagnostics().front();
            QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
            QCOMPARE(diagnostic.message, QStringLiteral("Assertion condition is false"));
            QCOMPARE(diagnostic.fieldPath,
                     QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp.") +
                         testCase.fieldName);
            QVERIFY(diagnostic.location.has_value());
            QCOMPARE(diagnostic.location->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     testCase.operandOffset);
            QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(),
                     testCase.operandLength);

            const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(followingNal.has_value());
            QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        }
    }

    void derivesTheAdaptiveMarkingBoundFromAFieldPicture() {
        const auto base = bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0x24,
            0x00, 0x00, 0x01, 0x68, 0xde, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x61, 0xeb, 0x99, 0x5c, 0x80,
            0x00, 0x00, 0x01, 0x0a,
        });
        struct FieldCase final {
            quint64 operandValue = 0;
            MaterializationState expectedState = MaterializationState::Materialized;
            quint64 operandLength = 0;
        };
        const std::vector<FieldCase> cases{
            {16, MaterializationState::Materialized, 9},
            {32, MaterializationState::Invalid, 11},
        };

        for (const auto& testCase : cases) {
            MemorySource source(
                replaceCodewordBeforeNextNal(base, 195, 1, testCase.operandValue));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
            QVERIFY(nal.has_value());
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto slice = analyzer->tree().node(rbsp->children().front());
            QVERIFY(slice.has_value());
            QCOMPARE(slice->state(), testCase.expectedState);

            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    slice->children().begin(),
                    slice->children().end(),
                    [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == slice->children().end() ? std::nullopt
                                                        : analyzer->tree().node(*found);
            };
            const auto fieldPicFlag = fieldNamed(QStringLiteral("field_pic_flag"));
            QVERIFY(fieldPicFlag.has_value());
            QCOMPARE(fieldPicFlag->value().toULongLong(), quint64(1));
            const auto operand =
                fieldNamed(QStringLiteral("difference_of_pic_nums_minus1[0]"));
            QVERIFY(operand.has_value());
            QCOMPARE(operand->value().toULongLong(), testCase.operandValue);
            QCOMPARE(operand->location()->sourceSpans().front().start()
                         .absoluteBitOffset(),
                     quint64(195));
            QCOMPARE(operand->location()->logicalRange().bitLength(),
                     testCase.operandLength);

            if (testCase.expectedState == MaterializationState::Materialized) {
                QVERIFY(slice->diagnostics().empty());
            } else {
                QCOMPARE(slice->diagnostics().size(), std::size_t(1));
                QCOMPARE(slice->diagnostics().front().fieldPath,
                         QStringLiteral(
                             "NonIdrSliceLayerWithoutPartitioningRbsp."
                             "difference_of_pic_nums_minus1[0]"));
            }
            const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(followingNal.has_value());
            QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        }
    }

    void rejectsReservedMarkingOperationAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x41, 0xea, 0x64, 0x40, 0xd5,
            0x00, 0x00, 0x01, 0x0a,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);

        const auto operation = analyzer->tree().node(slice->children().back());
        QVERIFY(operation.has_value());
        QCOMPARE(operation->name(),
                 QStringLiteral("memory_management_control_operation[0]"));
        QCOMPARE(operation->value().toULongLong(), quint64(7));
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Field value is not declared by its enum type"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("NonIdrSliceLayerWithoutPartitioningRbsp."
                                "memory_management_control_operation[0]"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(190));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(7));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesAllPpsControlledIdrSliceFieldsAndSkipsDisabledFilterOffsets() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xde, 0x3d, 0x80,
            0x00, 0x00, 0x01, 0x65, 0xba, 0xcd, 0xb6, 0xaa,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(14));

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const auto delta = fieldNamed(QStringLiteral("delta_pic_order_cnt_bottom"));
        const auto redundant = fieldNamed(QStringLiteral("redundant_pic_cnt"));
        const auto noOutput = fieldNamed(QStringLiteral("no_output_of_prior_pics_flag"));
        const auto longTerm = fieldNamed(QStringLiteral("long_term_reference_flag"));
        const auto qp = fieldNamed(QStringLiteral("slice_qp_delta"));
        const auto disable = fieldNamed(QStringLiteral("disable_deblocking_filter_idc"));
        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(delta.has_value());
        QVERIFY(redundant.has_value());
        QVERIFY(noOutput.has_value());
        QVERIFY(longTerm.has_value());
        QVERIFY(qp.has_value());
        QVERIFY(disable.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(delta->value().toLongLong(), qlonglong(-1));
        QCOMPARE(redundant->value().toULongLong(), quint64(2));
        QCOMPARE(noOutput->value().toULongLong(), quint64(0));
        QCOMPARE(longTerm->value().toULongLong(), quint64(1));
        QCOMPARE(qp->value().toLongLong(), qlonglong(0));
        QCOMPARE(disable->value().toULongLong(), quint64(1));
        QVERIFY(!fieldNamed(QStringLiteral("slice_alpha_c0_offset_div2")).has_value());
        QVERIFY(!fieldNamed(QStringLiteral("slice_beta_offset_div2")).has_value());
        QCOMPARE(delta->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(190));
        QCOMPARE(delta->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(redundant->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(redundant->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(disable->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(199));
        QCOMPARE(disable->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(202));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(6));
    }

    void warnsOnOutOfRangeRedundantPictureCountWithoutMovingPayloadBoundary() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xde, 0x3d, 0x80,
            0x00, 0x00, 0x01, 0x65, 0xba, 0xce, 0x02, 0x04, 0xaa, 0xaa,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const auto redundant = fieldNamed(QStringLiteral("redundant_pic_cnt"));
        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(redundant.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(redundant->value().toULongLong(), quint64(128));
        QCOMPARE(redundant->state(), MaterializationState::Materialized);
        QCOMPARE(redundant->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = redundant->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Field value is above its @range maximum"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("IdrSliceLayerWithoutPartitioningRbsp.redundant_pic_cnt"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(191));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(15));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(212));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(12));
    }

    void decodesTheEquivalentAllISliceTypeSeven() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
            0x00, 0x00, 0x01, 0x65, 0x88, 0xac, 0xcd, 0x55,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                     : analyzer->tree().node(*found);
        };
        const auto sliceType = fieldNamed(QStringLiteral("slice_type"));
        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(sliceType.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(sliceType->value().toULongLong(), quint64(7));
        QCOMPARE(sliceType->location()->logicalRange().bitLength(), quint64(7));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(11));
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(197));
    }

    void rejectsUnsupportedIdrSliceTypeThreeAndContinuesScanning() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x65, 0x90,
            0x00, 0x00, 0x01, 0x09, 0x50,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("IdrSliceLayerWithoutPartitioningRbsp.slice_type"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(33));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(5));
        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsIdrSliceWithoutPriorPictureParameterSetAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0xba, 0xcc, 0xd5,
                                   0x00, 0x00, 0x01, 0x09, 0x50}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto idrNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(idrNal.has_value());
        QCOMPARE(idrNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(idrNal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->children().size(), std::size_t(3));
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        QCOMPARE(slice->diagnostics().front().code,
                 streamview::core::DiagnosticCode::DependencyUnavailable);
        QCOMPARE(slice->diagnostics().front().fieldPath,
                 QStringLiteral("IdrSliceLayerWithoutPartitioningRbsp.pic_parameter_set_id"));
        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void decodesIdrSliceDeblockingOffsetsAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
            0x00, 0x00, 0x01, 0x65, 0xba, 0xcc, 0xd5,
            0x00, 0x00, 0x01, 0x09, 0x50,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto idrNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(idrNal.has_value());
        QCOMPARE(idrNal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(idrNal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);
        QCOMPARE(slice->children().size(), std::size_t(14));
        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const auto disable = fieldNamed(QStringLiteral("disable_deblocking_filter_idc"));
        const auto alpha = fieldNamed(QStringLiteral("slice_alpha_c0_offset_div2"));
        const auto beta = fieldNamed(QStringLiteral("slice_beta_offset_div2"));
        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(disable.has_value());
        QVERIFY(alpha.has_value());
        QVERIFY(beta.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(disable->value().toULongLong(), quint64(0));
        QCOMPARE(alpha->value().toLongLong(), qlonglong(1));
        QCOMPARE(beta->value().toLongLong(), qlonglong(0));
        QCOMPARE(disable->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(disable->location()->logicalRange().bitLength(), quint64(1));
        QCOMPARE(alpha->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(194));
        QCOMPARE(alpha->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(beta->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(197));
        QCOMPARE(beta->location()->logicalRange().bitLength(), quint64(1));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(198));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(2));
        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void validatesSliceDeblockingOffsetsWithoutMovingFollowingFields() {
        struct Probe final {
            std::size_t diagnosticCount = 0;
            QString message;
            QString fieldPath;
            quint64 diagnosticStart = 0;
            quint64 diagnosticLength = 0;
            quint64 sliceRbspStart = 0;
            quint64 alphaStart = 0;
            quint64 alphaLength = 0;
            quint64 betaStart = 0;
            quint64 betaLength = 0;
            quint64 payloadStart = 0;
            quint64 payloadLength = 0;
            qlonglong alphaValue = 0;
            qlonglong betaValue = 0;
        };

        const auto probe = [](bool idr,
                              const QString& offending,
                              qint64 alphaOffset,
                              qint64 betaOffset,
                              Probe& out) {
            const auto spsNal = sequenceParameterSetForProfile(66);
            const auto ppsNal = pictureParameterSetWithDeblockingControl();
            std::vector<std::byte> stream;
            appendNal(stream, spsNal);
            appendNal(stream, ppsNal);
            appendNal(stream, deblockingOffsetSlice(idr, alphaOffset, betaOffset));
            appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
            MemorySource source(std::move(stream));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(2));
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto slice = analyzer->tree().node(rbsp->children().front());
            QVERIFY(slice.has_value());
            QCOMPARE(slice->state(), MaterializationState::Materialized);

            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    slice->children().begin(), slice->children().end(), [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == slice->children().end() ? std::nullopt
                                                        : analyzer->tree().node(*found);
            };
            const auto alpha = fieldNamed(QStringLiteral("slice_alpha_c0_offset_div2"));
            const auto beta = fieldNamed(QStringLiteral("slice_beta_offset_div2"));
            const auto payload = fieldNamed(QStringLiteral("slice_data"));
            QVERIFY(alpha.has_value());
            QVERIFY(beta.has_value());
            QVERIFY(payload.has_value());

            out.sliceRbspStart = quint64(spsNal.size() + ppsNal.size() + 4) * 8;
            out.alphaValue = alpha->value().toLongLong();
            out.betaValue = beta->value().toLongLong();
            out.alphaStart =
                alpha->location()->sourceSpans().front().start().absoluteBitOffset();
            out.alphaLength = alpha->location()->sourceSpans().front().bitLength();
            out.betaStart =
                beta->location()->sourceSpans().front().start().absoluteBitOffset();
            out.betaLength = beta->location()->sourceSpans().front().bitLength();
            out.payloadStart =
                payload->location()->sourceSpans().front().start().absoluteBitOffset();
            out.payloadLength = payload->location()->logicalRange().bitLength();
            QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);

            const auto sibling = offending == QStringLiteral("slice_alpha_c0_offset_div2")
                                     ? beta
                                     : alpha;
            QVERIFY(sibling->diagnostics().empty());

            const auto offender = fieldNamed(offending);
            QVERIFY(offender.has_value());
            out.diagnosticCount = offender->diagnostics().size();
            if (out.diagnosticCount == 1) {
                const auto& diagnostic = offender->diagnostics().front();
                QCOMPARE(diagnostic.code,
                         streamview::core::DiagnosticCode::InvalidSyntax);
                QCOMPARE(diagnostic.severity,
                         streamview::core::DiagnosticSeverity::Warning);
                out.message = diagnostic.message;
                out.fieldPath = diagnostic.fieldPath;
                QVERIFY(diagnostic.location.has_value());
                out.diagnosticStart = diagnostic.location->sourceSpans()
                                          .front()
                                          .start()
                                          .absoluteBitOffset();
                out.diagnosticLength =
                    diagnostic.location->sourceSpans().front().bitLength();
            }

            const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(followingNal.has_value());
            QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        };

        struct Case final {
            bool idr;
            const char* structure;
            const char* offending;
            qint64 legalAlpha;
            qint64 legalBeta;
            qint64 illegalAlpha;
            qint64 illegalBeta;
            const char* message;
        };
        const Case cases[] = {
            {true, "IdrSliceLayerWithoutPartitioningRbsp", "slice_alpha_c0_offset_div2",
             6, 0, 7, 0, "Field value is above its @range maximum"},
            {true, "IdrSliceLayerWithoutPartitioningRbsp", "slice_alpha_c0_offset_div2",
             -6, 0, -7, 0, "Field value is below its @range minimum"},
            {true, "IdrSliceLayerWithoutPartitioningRbsp", "slice_beta_offset_div2",
             0, 6, 0, 7, "Field value is above its @range maximum"},
            {true, "IdrSliceLayerWithoutPartitioningRbsp", "slice_beta_offset_div2",
             0, -6, 0, -7, "Field value is below its @range minimum"},
            {false, "NonIdrSliceLayerWithoutPartitioningRbsp",
             "slice_alpha_c0_offset_div2", -6, 0, -7, 0,
             "Field value is below its @range minimum"},
            {false, "NonIdrSliceLayerWithoutPartitioningRbsp", "slice_beta_offset_div2",
             0, 6, 0, 7, "Field value is above its @range maximum"},
        };

        for (const auto& testCase : cases) {
            const QString offending = QString::fromLatin1(testCase.offending);
            Probe legal;
            probe(testCase.idr, offending, testCase.legalAlpha, testCase.legalBeta, legal);
            if (QTest::currentTestFailed()) {
                return;
            }
            Probe illegal;
            probe(testCase.idr,
                  offending,
                  testCase.illegalAlpha,
                  testCase.illegalBeta,
                  illegal);
            if (QTest::currentTestFailed()) {
                return;
            }

            QCOMPARE(legal.diagnosticCount, std::size_t(0));
            QCOMPARE(legal.alphaValue, qlonglong(testCase.legalAlpha));
            QCOMPARE(legal.betaValue, qlonglong(testCase.legalBeta));
            QCOMPARE(illegal.diagnosticCount, std::size_t(1));
            QCOMPARE(illegal.alphaValue, qlonglong(testCase.illegalAlpha));
            QCOMPARE(illegal.betaValue, qlonglong(testCase.illegalBeta));
            QCOMPARE(illegal.message, QString::fromLatin1(testCase.message));
            QCOMPARE(illegal.fieldPath,
                     QString::fromLatin1(testCase.structure) + QLatin1Char('.') +
                         offending);

            const quint64 offenderStart =
                offending == QStringLiteral("slice_alpha_c0_offset_div2")
                    ? illegal.alphaStart
                    : illegal.betaStart;
            QCOMPARE(illegal.diagnosticStart, offenderStart);
            QCOMPARE(illegal.diagnosticLength, quint64(7));

            QCOMPARE(illegal.sliceRbspStart, legal.sliceRbspStart);
            QCOMPARE(illegal.alphaStart, legal.alphaStart);
            QCOMPARE(illegal.alphaLength, legal.alphaLength);
            QCOMPARE(illegal.betaStart, legal.betaStart);
            QCOMPARE(illegal.betaLength, legal.betaLength);
            QCOMPARE(illegal.payloadStart, legal.payloadStart);
            QCOMPARE(illegal.payloadLength, legal.payloadLength);

            const quint64 headerBits = testCase.idr ? 18 : 15;
            QCOMPARE(illegal.alphaStart, illegal.sliceRbspStart + headerBits);
            QCOMPARE(illegal.betaStart,
                     illegal.alphaStart + illegal.alphaLength);
            QCOMPARE(illegal.payloadStart, illegal.betaStart + illegal.betaLength);
        }
    }

    void decodesWithinSliceDeblockingModeAndOffsets() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
            0x00, 0x00, 0x01, 0x65, 0xba, 0xcc, 0xb4, 0xea,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const QString& name) {
            const auto found = std::find_if(
                slice->children().begin(), slice->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == slice->children().end() ? std::nullopt
                                                   : analyzer->tree().node(*found);
        };
        const auto disable = fieldNamed(QStringLiteral("disable_deblocking_filter_idc"));
        const auto alpha = fieldNamed(QStringLiteral("slice_alpha_c0_offset_div2"));
        const auto beta = fieldNamed(QStringLiteral("slice_beta_offset_div2"));
        const auto payload = fieldNamed(QStringLiteral("slice_data"));
        QVERIFY(disable.has_value());
        QVERIFY(alpha.has_value());
        QVERIFY(beta.has_value());
        QVERIFY(payload.has_value());
        QCOMPARE(disable->value().toULongLong(), quint64(2));
        QCOMPARE(alpha->value().toLongLong(), qlonglong(1));
        QCOMPARE(beta->value().toLongLong(), qlonglong(-1));
        QCOMPARE(disable->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(disable->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(alpha->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(196));
        QCOMPARE(alpha->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(beta->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(199));
        QCOMPARE(beta->location()->logicalRange().bitLength(), quint64(3));
        QCOMPARE(payload->kind(), AnalysisNodeKind::CompressedPayload);
        QCOMPARE(payload->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(202));
        QCOMPARE(payload->location()->logicalRange().bitLength(), quint64(6));
    }

    void rejectsReservedDeblockingFilterModeAndContinues() {
        MemorySource source(bytes({
            0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
            0x00, 0x00, 0x01, 0x65, 0xba, 0xcc, 0x92,
            0x00, 0x00, 0x01, 0x09, 0x50,
        }));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(4));
        const auto idrNal = analyzer->tree().node(batch.nalUnitNodes.at(2));
        QVERIFY(idrNal.has_value());
        QCOMPARE(idrNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(idrNal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        const auto slice = analyzer->tree().node(rbsp->children().front());
        QVERIFY(slice.has_value());
        QCOMPARE(slice->state(), MaterializationState::Invalid);
        QCOMPARE(slice->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = slice->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(diagnostic.message,
                 QStringLiteral("Field value is not declared by its enum type"));
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral(
                     "IdrSliceLayerWithoutPartitioningRbsp.disable_deblocking_filter_idc"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(193));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(5));
        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void warnsOnOutOfRangePictureParameterSetIdentifier() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x67,
                                   0x42, 0x00, 0x1e, 0xf4, 0x0a, 0x0f, 0xc8,
                                   0x00, 0x00, 0x01, 0x68,
                                   0x00, 0x80, 0xce, 0x3c, 0x80}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Materialized);
        const auto ppsId = analyzer->tree().node(pps->children().front());
        QVERIFY(ppsId.has_value());
        QCOMPARE(ppsId->name(), QStringLiteral("pic_parameter_set_id"));
        QCOMPARE(ppsId->value().toULongLong(), quint64(256));
        QCOMPARE(ppsId->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = ppsId->diagnostics().front();
        QCOMPARE(diagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(diagnostic.severity, streamview::core::DiagnosticSeverity::Warning);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("PictureParameterSetRbsp.pic_parameter_set_id"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(120));
        QCOMPARE(diagnostic.location->sourceSpans().front().bitLength(), quint64(17));
    }

    void validatesPictureParameterSetQpOffsetsWithoutMovingFollowingFields() {
        struct Probe final {
            std::size_t diagnosticCount = 0;
            QString message;
            QString fieldPath;
            quint64 diagnosticStart = 0;
            quint64 diagnosticLength = 0;
            quint64 ppsRbspStart = 0;
            quint64 targetStart = 0;
            quint64 targetLength = 0;
            quint64 followingFieldStart = 0;
            qlonglong targetValue = 0;
        };

        const auto probe = [](bool highProfile,
                              const QString& offending,
                              qint64 picInitQsMinus26,
                              qint64 chromaQpIndexOffset,
                              std::optional<qint64> secondChromaQpIndexOffset,
                              Probe& out) {
            const auto spsNal = sequenceParameterSetForProfile(highProfile ? 100 : 66);
            std::vector<bool> bits;
            appendUnsignedExpGolomb(bits, 0);
            appendUnsignedExpGolomb(bits, 0);
            appendFixedBits(bits, 0, 1);
            appendFixedBits(bits, 0, 1);
            appendUnsignedExpGolomb(bits, 0);
            appendUnsignedExpGolomb(bits, 0);
            appendUnsignedExpGolomb(bits, 0);
            appendFixedBits(bits, 0, 1);
            appendFixedBits(bits, 0, 2);
            appendSignedExpGolomb(bits, 0);
            appendSignedExpGolomb(bits, picInitQsMinus26);
            appendSignedExpGolomb(bits, chromaQpIndexOffset);
            appendFixedBits(bits, 0, 1);
            appendFixedBits(bits, 0, 1);
            appendFixedBits(bits, 0, 1);
            if (secondChromaQpIndexOffset.has_value()) {
                appendFixedBits(bits, 1, 1);
                appendFixedBits(bits, 0, 1);
                appendSignedExpGolomb(bits, *secondChromaQpIndexOffset);
            }
            const auto ppsNal = packAnnexBNal(0x68, std::move(bits));
            std::vector<std::byte> stream;
            appendNal(stream, spsNal);
            appendNal(stream, ppsNal);
            appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
            MemorySource source(std::move(stream));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            QCOMPARE(batch.nalUnitNodes.size(), std::size_t(3));
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.at(1));
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto pps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(pps.has_value());
            QCOMPARE(pps->state(), MaterializationState::Materialized);

            const auto fieldNamed = [&](const QString& name) {
                const auto found = std::find_if(
                    pps->children().begin(), pps->children().end(), [&](const auto id) {
                        const auto node = analyzer->tree().node(id);
                        return node && node->name() == name;
                    });
                return found == pps->children().end() ? std::nullopt
                                                      : analyzer->tree().node(*found);
            };

            const auto target = fieldNamed(offending);
            QVERIFY(target.has_value());
            QCOMPARE(target->metadata().specification->clause,
                     QStringLiteral("7.4.2.2"));

            const QString followingName =
                offending == QStringLiteral("second_chroma_qp_index_offset")
                    ? QStringLiteral("rbsp_stop_one_bit")
                    : QStringLiteral("deblocking_filter_control_present_flag");
            const auto following = fieldNamed(followingName);
            QVERIFY(following.has_value());

            out.ppsRbspStart = quint64(spsNal.size() + 4) * 8;
            out.targetValue = target->value().toLongLong();
            out.targetStart =
                target->location()->sourceSpans().front().start().absoluteBitOffset();
            out.targetLength =
                target->location()->sourceSpans().front().bitLength();
            out.followingFieldStart =
                following->location()->sourceSpans().front().start().absoluteBitOffset();

            if (offending == QStringLiteral("pic_init_qs_minus26")) {
                const auto sibling = fieldNamed(QStringLiteral("chroma_qp_index_offset"));
                QVERIFY(sibling.has_value());
                QVERIFY(sibling->diagnostics().empty());
            } else if (offending == QStringLiteral("chroma_qp_index_offset")) {
                const auto sibling = fieldNamed(QStringLiteral("pic_init_qs_minus26"));
                QVERIFY(sibling.has_value());
                QVERIFY(sibling->diagnostics().empty());
            }

            out.diagnosticCount = target->diagnostics().size();
            if (out.diagnosticCount == 1) {
                const auto& diagnostic = target->diagnostics().front();
                QCOMPARE(diagnostic.code,
                         streamview::core::DiagnosticCode::InvalidSyntax);
                QCOMPARE(diagnostic.severity,
                         streamview::core::DiagnosticSeverity::Warning);
                out.message = diagnostic.message;
                out.fieldPath = diagnostic.fieldPath;
                QVERIFY(diagnostic.location.has_value());
                out.diagnosticStart = diagnostic.location->sourceSpans()
                                          .front()
                                          .start()
                                          .absoluteBitOffset();
                out.diagnosticLength =
                    diagnostic.location->sourceSpans().front().bitLength();
            }

            const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
            QVERIFY(followingNal.has_value());
            QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        };

        struct Case final {
            bool highProfile;
            const char* offending;
            qint64 legalQs;
            qint64 legalChroma;
            std::optional<qint64> legalSecond;
            qint64 illegalQs;
            qint64 illegalChroma;
            std::optional<qint64> illegalSecond;
            const char* message;
            quint64 expectedBitLength;
        };

        const Case cases[] = {
            {false, "pic_init_qs_minus26", 25, 0, std::nullopt, 26, 0, std::nullopt,
             "Field value is above its @range maximum", 11},
            {false, "pic_init_qs_minus26", -26, 0, std::nullopt, -27, 0, std::nullopt,
             "Field value is below its @range minimum", 11},
            {false, "chroma_qp_index_offset", 0, 12, std::nullopt, 0, 13, std::nullopt,
             "Field value is above its @range maximum", 9},
            {false, "chroma_qp_index_offset", 0, -12, std::nullopt, 0, -13, std::nullopt,
             "Field value is below its @range minimum", 9},
            {true, "second_chroma_qp_index_offset", 0, 0, 12, 0, 0, 13,
             "Field value is above its @range maximum", 9},
            {true, "second_chroma_qp_index_offset", 0, 0, -12, 0, 0, -13,
             "Field value is below its @range minimum", 9},
        };

        for (const auto& testCase : cases) {
            const QString offending = QString::fromLatin1(testCase.offending);
            Probe legal;
            probe(testCase.highProfile,
                  offending,
                  testCase.legalQs,
                  testCase.legalChroma,
                  testCase.legalSecond,
                  legal);
            if (QTest::currentTestFailed()) {
                return;
            }
            Probe illegal;
            probe(testCase.highProfile,
                  offending,
                  testCase.illegalQs,
                  testCase.illegalChroma,
                  testCase.illegalSecond,
                  illegal);
            if (QTest::currentTestFailed()) {
                return;
            }

            const qint64 expectedLegalValue =
                offending == QStringLiteral("pic_init_qs_minus26")
                    ? testCase.legalQs
                    : (offending == QStringLiteral("chroma_qp_index_offset")
                           ? testCase.legalChroma
                           : *testCase.legalSecond);
            const qint64 expectedIllegalValue =
                offending == QStringLiteral("pic_init_qs_minus26")
                    ? testCase.illegalQs
                    : (offending == QStringLiteral("chroma_qp_index_offset")
                           ? testCase.illegalChroma
                           : *testCase.illegalSecond);

            QCOMPARE(legal.diagnosticCount, std::size_t(0));
            QCOMPARE(legal.targetValue, qlonglong(expectedLegalValue));
            QCOMPARE(illegal.diagnosticCount, std::size_t(1));
            QCOMPARE(illegal.targetValue, qlonglong(expectedIllegalValue));
            QCOMPARE(illegal.message, QString::fromLatin1(testCase.message));
            QCOMPARE(illegal.fieldPath,
                     QStringLiteral("PictureParameterSetRbsp.") + offending);

            QCOMPARE(illegal.diagnosticStart, illegal.targetStart);
            QCOMPARE(illegal.diagnosticLength, testCase.expectedBitLength);

            QCOMPARE(illegal.ppsRbspStart, legal.ppsRbspStart);
            QCOMPARE(illegal.targetStart, legal.targetStart);
            QCOMPARE(illegal.targetLength, legal.targetLength);
            QCOMPARE(illegal.followingFieldStart, legal.followingFieldStart);

            const quint64 baseOffsetBits =
                offending == QStringLiteral("pic_init_qs_minus26")
                    ? 11
                    : (offending == QStringLiteral("chroma_qp_index_offset") ? 12 : 18);
            QCOMPARE(illegal.targetStart, illegal.ppsRbspStart + baseOffsetBits);
        }
    }

    void rejectsPictureParameterSetWithoutPriorSequenceParameterSetAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
                                   0x00, 0x00, 0x01, 0x09, 0x50}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto ppsNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(ppsNal.has_value());
        QCOMPARE(ppsNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(ppsNal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Materialized);
        QCOMPARE(pps->diagnostics().size(), std::size_t(1));
        const auto& diagnostic = pps->diagnostics().front();
        QCOMPARE(diagnostic.code,
                 streamview::core::DiagnosticCode::DependencyUnavailable);
        QCOMPARE(diagnostic.fieldPath,
                 QStringLiteral("PictureParameterSetRbsp.seq_parameter_set_id"));
        QVERIFY(diagnostic.location.has_value());
        QCOMPARE(diagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(33));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsUnsupportedPictureParameterSetSliceGroupsAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x68, 0xc5,
                                   0x00, 0x00, 0x01, 0x0c}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(invalidNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto pps = analyzer->tree().node(rbsp->children().front());
        QVERIFY(pps.has_value());
        QCOMPARE(pps->state(), MaterializationState::Invalid);
        const auto sliceGroups = std::find_if(
            pps->children().begin(), pps->children().end(), [&](const auto id) {
                const auto node = analyzer->tree().node(id);
                return node && node->name() == QStringLiteral("num_slice_groups_minus1");
            });
        QVERIFY(sliceGroups != pps->children().end());
        QCOMPARE(analyzer->tree().node(*sliceGroups)->value().toULongLong(), quint64(1));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsReservedPictureParameterSetValues() {
        const std::vector cases{
            std::pair{std::vector<unsigned int>{0xce, 0xfc, 0x80},
                      QStringLiteral("weighted_bipred_idc")},
        };

        for (const auto& [payload, expectedName] : cases) {
            std::vector<std::byte> sourceBytes = bytes({0x00, 0x00, 0x01, 0x68});
            for (const unsigned int value : payload) {
                sourceBytes.push_back(static_cast<std::byte>(value));
            }
            MemorySource source(std::move(sourceBytes));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Invalid);
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            const auto pps = analyzer->tree().node(rbsp->children().front());
            QVERIFY(pps.has_value());
            QCOMPARE(pps->state(), MaterializationState::Invalid);
            const auto field = std::find_if(
                pps->children().begin(), pps->children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == expectedName;
                });
            QVERIFY(field != pps->children().end());
        }
    }

    void decodesTheAccessUnitDelimiterPayload() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09, 0x50}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        QCOMPARE(nal->children().size(), std::size_t(3));

        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->children().size(), std::size_t(1));

        const auto aud = analyzer->tree().node(rbsp->children().front());
        QVERIFY(aud.has_value());
        QCOMPARE(aud->name(), QStringLiteral("AccessUnitDelimiterRbsp"));
        QCOMPARE(aud->state(), MaterializationState::Materialized);
        QCOMPARE(aud->children().size(), std::size_t(6));
        QCOMPARE(aud->metadata().specification->clause, QStringLiteral("7.3.2.4"));

        const auto primaryPicType = analyzer->tree().node(aud->children().at(0));
        QVERIFY(primaryPicType.has_value());
        QCOMPARE(primaryPicType->name(), QStringLiteral("primary_pic_type"));
        QCOMPARE(primaryPicType->kind(), AnalysisNodeKind::SyntaxField);
        QCOMPARE(primaryPicType->value().toULongLong(), quint64(2));
        QCOMPARE(primaryPicType->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(primaryPicType->location()->sourceSpans().front().bitLength(), quint64(3));

        const auto stopBit = analyzer->tree().node(aud->children().at(1));
        QVERIFY(stopBit.has_value());
        QCOMPARE(stopBit->name(), QStringLiteral("rbsp_stop_one_bit"));
        QCOMPARE(stopBit->value().toULongLong(), quint64(1));
        QCOMPARE(stopBit->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(35));
        QCOMPARE(stopBit->metadata().specification->clause, QStringLiteral("7.3.2.11"));

        for (std::size_t index = 0; index < 4; ++index) {
            const auto alignment = analyzer->tree().node(aud->children().at(2 + index));
            QVERIFY(alignment.has_value());
            QCOMPARE(alignment->name(),
                     QStringLiteral("rbsp_alignment_zero_bit[%1]").arg(index));
            QCOMPARE(alignment->value().toULongLong(), quint64(0));
            QCOMPARE(
                alignment->location()->sourceSpans().front().start().absoluteBitOffset(),
                quint64(36 + index));
            QCOMPARE(alignment->location()->sourceSpans().front().bitLength(), quint64(1));
        }
    }

    void reportsAHeaderOnlyAccessUnitDelimiterAsTruncated() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->children().size(), std::size_t(3));

        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->diagnostics().size(), std::size_t(1));
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::TruncatedSource);
    }

    void materializesHeaderOnlyEndOfSequenceAndEndOfStream() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x0A, 0x00, 0x00, 0x01, 0x0B}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        for (const auto nodeId : batch.nalUnitNodes) {
            const auto nal = analyzer->tree().node(nodeId);
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            QCOMPARE(nal->children().size(), std::size_t(3));
            const auto rbsp = analyzer->tree().node(nal->children().at(2));
            QVERIFY(rbsp.has_value());
            QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
            QCOMPARE(rbsp->state(), MaterializationState::Materialized);
            QVERIFY(rbsp->children().empty());
            QVERIFY(rbsp->diagnostics().empty());
        }
    }

    void rejectsANonEmptyEndOfSequencePayload() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x0A, 0x80,
                                   0x00, 0x00, 0x01, 0x09, 0x50}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QVERIFY(rbsp->children().empty());
        QCOMPARE(rbsp->diagnostics().size(), std::size_t(1));
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QVERIFY(rbsp->diagnostics().front().message.contains(QStringLiteral("empty RBSP")));
        QCOMPARE(rbsp->location()->logicalRange().bitLength(), quint64(8));

        const auto following = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(following.has_value());
        QCOMPARE(following->state(), MaterializationState::Materialized);
        const auto followingRbsp = analyzer->tree().node(following->children().at(2));
        QVERIFY(followingRbsp.has_value());
        QCOMPARE(followingRbsp->children().size(), std::size_t(1));
    }

    void rejectsAnAccessUnitDelimiterWithAClearedStopBit() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09, 0x40}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->children().size(), std::size_t(1));

        const auto aud = analyzer->tree().node(rbsp->children().front());
        QVERIFY(aud.has_value());
        QCOMPARE(aud->state(), MaterializationState::Invalid);
        QCOMPARE(aud->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(aud->children().size(), std::size_t(2));
        const auto retained = analyzer->tree().node(aud->children().front());
        QVERIFY(retained.has_value());
        QCOMPARE(retained->name(), QStringLiteral("primary_pic_type"));
        QCOMPARE(retained->value().toULongLong(), quint64(2));
    }

    void rejectsNonzeroAccessUnitDelimiterPaddingAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09, 0x54,
                                   0x00, 0x00, 0x01, 0x0c}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto malformedNal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(malformedNal.has_value());
        QCOMPARE(malformedNal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(malformedNal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto aud = analyzer->tree().node(rbsp->children().front());
        QVERIFY(aud.has_value());
        QCOMPARE(aud->state(), MaterializationState::Invalid);
        QCOMPARE(aud->children().size(), std::size_t(4));
        const auto nonzeroPadding = analyzer->tree().node(aud->children().at(3));
        QVERIFY(nonzeroPadding.has_value());
        QCOMPARE(nonzeroPadding->name(), QStringLiteral("rbsp_alignment_zero_bit[1]"));
        QCOMPARE(nonzeroPadding->value().toULongLong(), quint64(1));

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
    }

    void rejectsAnAccessUnitDelimiterWithUndeclaredTrailingBits() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x09, 0x50, 0x55}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->diagnostics().size(), std::size_t(1));
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QVERIFY(rbsp->diagnostics().front().message.contains(QStringLiteral("8 undeclared")));

        const auto aud = analyzer->tree().node(rbsp->children().front());
        QVERIFY(aud.has_value());
        QCOMPARE(aud->state(), MaterializationState::Materialized);
        QCOMPARE(aud->children().size(), std::size_t(6));
    }

    void keepsUndispatchedNalUnitPayloadsUninterpreted() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x6c, 0xAA, 0x00, 0x00, 0x01, 0x0c}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));

        const auto withPayload = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(withPayload.has_value());
        QCOMPARE(withPayload->state(), MaterializationState::Materialized);
        QCOMPARE(withPayload->children().size(), std::size_t(3));
        const auto rbsp = analyzer->tree().node(withPayload->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QVERIFY(rbsp->children().empty());

        const auto headerOnly = analyzer->tree().node(batch.nalUnitNodes.back());
        QVERIFY(headerOnly.has_value());
        QCOMPARE(headerOnly->state(), MaterializationState::Materialized);
        QCOMPARE(headerOnly->children().size(), std::size_t(2));
    }

    void rejectsInvalidMapperLimitsBeforeAnalysis() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0x12}));
        H264EbspRbspMapLimits limits;
        limits.maximumMappingSegments = 0;
        QString errorMessage;

        const auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, std::nullopt, limits);

        QVERIFY(!analyzer.has_value());
        QVERIFY(errorMessage.contains(QStringLiteral("greater than zero")));
    }

    void materializesStartCodesAndNalHeadersInBatches() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x6c, 0xAA,
                                   0x00, 0x00, 0x00, 0x01, 0x4c}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto firstBatch = analyzer->analyzeBatch(1);
        QCOMPARE(firstBatch.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(firstBatch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(firstBatch.progressiveIndexUpdate.has_value());
        QCOMPARE(firstBatch.progressiveIndexUpdate->firstRecordIndex, quint64{0});
        QCOMPARE(firstBatch.progressiveIndexUpdate->records.size(), std::size_t{1});
        QCOMPARE(firstBatch.progressiveIndexUpdate->indexedThroughByteOffset, quint64{5});
        QVERIFY(!firstBatch.progressiveIndexUpdate->endOfSource);

        const auto firstNal = analyzer->tree().node(firstBatch.nalUnitNodes.front());
        QVERIFY(firstNal.has_value());
        QCOMPARE(firstNal->kind(), AnalysisNodeKind::Region);
        QCOMPARE(firstNal->state(), MaterializationState::Materialized);
        QCOMPARE(firstNal->children().size(), std::size_t(3));

        const auto firstStartCode = analyzer->tree().node(firstNal->children().at(0));
        QVERIFY(firstStartCode.has_value());
        QCOMPARE(firstStartCode->name(), QStringLiteral("start_code"));
        QVERIFY(firstStartCode->location().has_value());
        QCOMPARE(firstStartCode->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(0));
        QCOMPARE(firstStartCode->location()->sourceSpans().front().bitLength(), quint64(24));

        const auto firstHeader = analyzer->tree().node(firstNal->children().at(1));
        QVERIFY(firstHeader.has_value());
        QCOMPARE(firstHeader->name(), QStringLiteral("NalUnitHeader"));
        QCOMPARE(firstHeader->children().size(), std::size_t(3));
        const auto forbidden = analyzer->tree().node(firstHeader->children().at(0));
        const auto referenceIdc = analyzer->tree().node(firstHeader->children().at(1));
        const auto unitType = analyzer->tree().node(firstHeader->children().at(2));
        QVERIFY(forbidden.has_value());
        QVERIFY(referenceIdc.has_value());
        QVERIFY(unitType.has_value());
        QCOMPARE(forbidden->value().toULongLong(), quint64(0));
        QCOMPARE(referenceIdc->value().toULongLong(), quint64(3));
        QCOMPARE(unitType->value().toULongLong(), quint64(12));
        QVERIFY(forbidden->metadata().specification.has_value());
        QCOMPARE(forbidden->metadata().specification->standard,
                 QStringLiteral("ITU-T H.264"));
        QCOMPARE(forbidden->metadata().specification->clause, QStringLiteral("7.3.1"));
        QVERIFY(!forbidden->metadata().description.isEmpty());
        QVERIFY(!referenceIdc->metadata().description.isEmpty());
        QVERIFY(!unitType->metadata().description.isEmpty());
        QCOMPARE(forbidden->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(24));
        QCOMPARE(referenceIdc->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(25));
        QCOMPARE(unitType->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(27));

        const auto firstRbsp = analyzer->tree().node(firstNal->children().at(2));
        QVERIFY(firstRbsp.has_value());
        QCOMPARE(firstRbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(firstRbsp->state(), MaterializationState::Materialized);
        QVERIFY(firstRbsp->location().has_value());
        QCOMPARE(firstRbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(firstRbsp->location()->sourceSpans().front().bitLength(), quint64(8));

        const auto secondBatch = analyzer->analyzeBatch(1);
        QCOMPARE(secondBatch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(secondBatch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(secondBatch.progressiveIndexUpdate.has_value());
        QCOMPARE(secondBatch.progressiveIndexUpdate->firstRecordIndex, quint64{1});
        QCOMPARE(secondBatch.progressiveIndexUpdate->records.size(), std::size_t{1});
        QCOMPARE(secondBatch.progressiveIndexUpdate->indexedThroughByteOffset, quint64{10});
        QVERIFY(secondBatch.progressiveIndexUpdate->endOfSource);
        const auto secondNal = analyzer->tree().node(secondBatch.nalUnitNodes.front());
        QVERIFY(secondNal.has_value());
        const auto secondStartCode = analyzer->tree().node(secondNal->children().at(0));
        QVERIFY(secondStartCode.has_value());
        QVERIFY(secondStartCode->location().has_value());
        QCOMPARE(secondStartCode->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(40));
        QCOMPARE(secondStartCode->location()->sourceSpans().front().bitLength(), quint64(32));
        const auto secondHeader = analyzer->tree().node(secondNal->children().at(1));
        QVERIFY(secondHeader.has_value());
        const auto secondReferenceIdc = analyzer->tree().node(secondHeader->children().at(1));
        const auto secondUnitType = analyzer->tree().node(secondHeader->children().at(2));
        QCOMPARE(secondReferenceIdc->value().toULongLong(), quint64(2));
        QCOMPARE(secondUnitType->value().toULongLong(), quint64(12));

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
    }

    void mapsRbspPayloadAndPublishesExcludedBytes() {
        MemorySource source(bytes(
            {0x00, 0x00, 0x01, 0x6c, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);
        QCOMPARE(nal->children().size(), std::size_t(5));

        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->kind(), AnalysisNodeKind::Region);
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QVERIFY(rbsp->location().has_value());
        QCOMPARE(rbsp->location()->logicalRange().bitLength(), quint64(24));
        QCOMPARE(rbsp->location()->sourceSpans().size(), std::size_t(2));
        QCOMPARE(rbsp->location()->sourceSpans().at(0).start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().at(0).bitLength(), quint64(16));
        QCOMPARE(rbsp->location()->sourceSpans().at(1).start().absoluteBitOffset(),
                 quint64(56));
        QCOMPARE(rbsp->location()->sourceSpans().at(1).bitLength(), quint64(8));
        QCOMPARE(rbsp->metadata().specification->clause, QStringLiteral("7.3.1, 7.4.1"));

        const auto excluded = analyzer->tree().node(nal->children().at(3));
        QVERIFY(excluded.has_value());
        QCOMPARE(excluded->name(), QStringLiteral("emulation_prevention_three_byte[0]"));
        QCOMPARE(excluded->kind(), AnalysisNodeKind::Region);
        QCOMPARE(excluded->state(), MaterializationState::Materialized);
        QCOMPARE(excluded->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(48));
        QCOMPARE(excluded->location()->sourceSpans().front().bitLength(), quint64(8));
        const auto trailing = analyzer->tree().node(nal->children().at(4));
        QVERIFY(trailing.has_value());
        QCOMPARE(trailing->name(), QStringLiteral("trailing_zero_8bits"));
        QCOMPARE(trailing->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(64));
        QCOMPARE(trailing->location()->sourceSpans().front().bitLength(), quint64(16));
    }

    void publishesTrailingZerosAfterMappedPayload() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0x12, 0x00, 0x00}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->children().size(), std::size_t(4));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        const auto trailing = analyzer->tree().node(nal->children().at(3));
        QVERIFY(rbsp.has_value());
        QVERIFY(trailing.has_value());
        QCOMPARE(rbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(rbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().front().bitLength(), quint64(8));
        QCOMPARE(trailing->name(), QStringLiteral("trailing_zero_8bits"));
        QCOMPARE(trailing->state(), MaterializationState::Materialized);
        QCOMPARE(trailing->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(40));
        QCOMPARE(trailing->location()->sourceSpans().front().bitLength(), quint64(16));
    }

    void keepsEbspConformanceIssuesLocalAndContinues() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x6c, 0x00, 0x00,
                                   0x03, 0x04, 0x00, 0x00, 0x01, 0x0c}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        const auto firstNal = analyzer->tree().node(batch.nalUnitNodes.at(0));
        const auto secondNal = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(firstNal.has_value());
        QVERIFY(secondNal.has_value());
        QCOMPARE(firstNal->state(), MaterializationState::Invalid);
        QCOMPARE(firstNal->diagnostics().size(), std::size_t(1));
        QCOMPARE(firstNal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QVERIFY(firstNal->diagnostics().front().message.contains(QStringLiteral("00 00 03")));
        QCOMPARE(firstNal->diagnostics()
                     .front()
                     .location->sourceSpans()
                     .front()
                     .start()
                     .absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(firstNal->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(32));
        const auto rbsp = analyzer->tree().node(firstNal->children().at(2));
        const auto excluded = analyzer->tree().node(firstNal->children().at(3));
        QVERIFY(rbsp.has_value());
        QVERIFY(excluded.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(excluded->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(48));
        QCOMPARE(secondNal->state(), MaterializationState::Materialized);
        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
    }

    void boundsRbspMappingAcrossAnalysisBatches() {
        MemorySource source(
            bytes({0x00, 0x00, 0x01, 0x6c, 0x12, 0x34, 0x56, 0x78, 0x9A}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto first = analyzer->analyzeBatch(1, 64, 2);
        QCOMPARE(first.status, H264AnnexBAnalysisStatus::InProgress);
        QVERIFY(first.nalUnitNodes.empty());
        const auto rootWhilePending = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootWhilePending.has_value());
        QCOMPARE(rootWhilePending->children().size(), std::size_t(1));
        const auto pendingNal = analyzer->tree().node(rootWhilePending->children().front());
        QVERIFY(pendingNal.has_value());
        QCOMPARE(pendingNal->state(), MaterializationState::Indexing);
        QCOMPARE(pendingNal->children().size(), std::size_t(2));

        const auto second = analyzer->analyzeBatch(1, 64, 2);
        QCOMPARE(second.status, H264AnnexBAnalysisStatus::InProgress);
        QVERIFY(second.nalUnitNodes.empty());
        const auto third = analyzer->analyzeBatch(1, 64, 2);
        QCOMPARE(third.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(third.nalUnitNodes.size(), std::size_t(1));
        QCOMPARE(third.nalUnitNodes.front(), pendingNal->id());
        const auto completedNal = analyzer->tree().node(third.nalUnitNodes.front());
        QVERIFY(completedNal.has_value());
        QCOMPARE(completedNal->state(), MaterializationState::Materialized);
        QCOMPARE(completedNal->children().size(), std::size_t(3));
    }

    void cancellationRetainsTheCommittedRbspPrefix() {
        CancellationSource cancellation;
        MemorySource source(
            bytes({0x00, 0x00, 0x01, 0x65, 0x12, 0x34, 0x56, 0x78, 0x9A}));
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto first = analyzer->analyzeBatch(1, 64, 2);
        QCOMPARE(first.status, H264AnnexBAnalysisStatus::InProgress);
        QVERIFY(first.nalUnitNodes.empty());
        QVERIFY(cancellation.requestCancellation());

        const auto cancelled = analyzer->analyzeBatch(1, 64, 2);

        QCOMPARE(cancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(cancelled.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(cancelled.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Cancelled);
        QCOMPARE(nal->children().size(), std::size_t(3));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Cancelled);
        QVERIFY(rbsp->location().has_value());
        QCOMPARE(rbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().front().bitLength(), quint64(16));
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::Cancelled);
    }

    void leavesExtensionHeaderPayloadUninterpreted() {
        for (const unsigned int nalUnitType : {14U, 20U, 21U}) {
            MemorySource source(bytes({0x00,
                                       0x00,
                                       0x01,
                                       0x60U | nalUnitType,
                                       0x00,
                                       0x00,
                                       0x03,
                                       0x02,
                                       0x00,
                                       0x00}));
            QString errorMessage;
            auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
            QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

            const auto batch = analyzer->analyzeBatch();

            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
            QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
            const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
            QVERIFY(nal.has_value());
            QCOMPARE(nal->state(), MaterializationState::Materialized);
            QCOMPARE(nal->children().size(), std::size_t(3));
            const auto header = analyzer->tree().node(nal->children().at(1));
            const auto trailing = analyzer->tree().node(nal->children().at(2));
            QVERIFY(header.has_value());
            QVERIFY(trailing.has_value());
            const auto type = analyzer->tree().node(header->children().at(2));
            QVERIFY(type.has_value());
            QCOMPARE(type->value().toULongLong(), quint64(nalUnitType));
            QCOMPARE(trailing->name(), QStringLiteral("trailing_zero_8bits"));
            QCOMPARE(trailing->location()->sourceSpans().front().start().absoluteBitOffset(),
                     quint64(64));
            QCOMPARE(trailing->location()->sourceSpans().front().bitLength(), quint64(16));
        }
    }

    void exposesBoundedScanProgressAndRejectsInvalidWorkBudget() {
        MemorySource source(std::vector<std::byte>(20, std::byte{0x12}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto invalid = analyzer->analyzeBatch(1, 0);
        QCOMPARE(invalid.status, H264AnnexBAnalysisStatus::InvalidBatchSize);
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QVERIFY(!analyzer->finished());

        const auto invalidMappingBudget = analyzer->analyzeBatch(1, 8, 0);
        QCOMPARE(invalidMappingBudget.status, H264AnnexBAnalysisStatus::InvalidBatchSize);
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QVERIFY(!analyzer->finished());

        const auto first = analyzer->analyzeBatch(1, 8);
        QCOMPARE(first.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(analyzer->scanCursor(), quint64(8));
        QVERIFY(!first.progressiveIndexUpdate.has_value());
        const auto second = analyzer->analyzeBatch(1, 8);
        QCOMPARE(second.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(analyzer->scanCursor(), quint64(16));
        QVERIFY(!second.progressiveIndexUpdate.has_value());
        const auto third = analyzer->analyzeBatch(1, 8);
        QCOMPARE(third.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(analyzer->scanCursor(), quint64(20));
        QVERIFY(third.progressiveIndexUpdate.has_value());
        QCOMPARE(third.progressiveIndexUpdate->firstRecordIndex, quint64{0});
        QCOMPARE(third.progressiveIndexUpdate->indexedThroughByteOffset, quint64{20});
        QVERIFY(third.progressiveIndexUpdate->records.empty());
        QVERIFY(third.progressiveIndexUpdate->endOfSource);
    }

    void retainsTheInvalidForbiddenBitAsAPartialResult() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0xE5}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(analyzer->tree().hasPartialResults());

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->diagnostics().size(), std::size_t(1));
        QCOMPARE(nal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);

        const auto header = analyzer->tree().node(nal->children().at(1));
        QVERIFY(header.has_value());
        QCOMPARE(header->state(), MaterializationState::Invalid);
        QCOMPARE(header->children().size(), std::size_t(1));
        const auto forbidden = analyzer->tree().node(header->children().front());
        QVERIFY(forbidden.has_value());
        QCOMPARE(forbidden->value().toULongLong(), quint64(1));
        QCOMPARE(forbidden->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(24));

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
    }

    void rejectsAnIdrNalWithZeroReferencePriorityBeforePayloadMapping() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x05, 0xaa, 0xbb,
                                   0x00, 0x00, 0x01, 0x0a}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));
        QVERIFY(analyzer->tree().hasPartialResults());

        const auto invalidNal = analyzer->tree().node(batch.nalUnitNodes.at(0));
        QVERIFY(invalidNal.has_value());
        QCOMPARE(invalidNal->state(), MaterializationState::Invalid);
        QCOMPARE(invalidNal->children().size(), std::size_t(2));
        QCOMPARE(invalidNal->diagnostics().size(), std::size_t(1));
        const auto& nalDiagnostic = invalidNal->diagnostics().front();
        QCOMPARE(nalDiagnostic.code, streamview::core::DiagnosticCode::InvalidSyntax);
        QCOMPARE(nalDiagnostic.severity,
                 streamview::core::DiagnosticSeverity::Error);
        QCOMPARE(nalDiagnostic.message, QStringLiteral("Assertion condition is false"));
        QCOMPARE(nalDiagnostic.fieldPath,
                 QStringLiteral("NalUnitHeader.nal_ref_idc"));
        QVERIFY(nalDiagnostic.location.has_value());
        QCOMPARE(nalDiagnostic.location->sourceSpans().size(), std::size_t(1));
        QCOMPARE(nalDiagnostic.location->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(25));
        QCOMPARE(nalDiagnostic.location->sourceSpans().front().bitLength(), quint64(2));

        const auto invalidHeader =
            analyzer->tree().node(invalidNal->children().at(1));
        QVERIFY(invalidHeader.has_value());
        QCOMPARE(invalidHeader->name(), QStringLiteral("NalUnitHeader"));
        QCOMPARE(invalidHeader->state(), MaterializationState::Invalid);
        QCOMPARE(invalidHeader->children().size(), std::size_t(3));
        QCOMPARE(invalidHeader->diagnostics().size(), std::size_t(1));
        QCOMPARE(invalidHeader->diagnostics().front().fieldPath,
                 QStringLiteral("NalUnitHeader.nal_ref_idc"));
        for (const auto childId : invalidNal->children()) {
            const auto child = analyzer->tree().node(childId);
            QVERIFY(child.has_value());
            QVERIFY(child->name() != QStringLiteral("rbsp_payload"));
            QVERIFY(child->name() !=
                    QStringLiteral("IdrSliceLayerWithoutPartitioningRbsp"));
        }

        const auto followingNal = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(followingNal.has_value());
        QCOMPARE(followingNal->state(), MaterializationState::Materialized);
        QCOMPARE(followingNal->children().size(), std::size_t(3));
        const auto followingHeader =
            analyzer->tree().node(followingNal->children().at(1));
        QVERIFY(followingHeader.has_value());
        QCOMPARE(followingHeader->state(), MaterializationState::Materialized);
        QCOMPARE(followingHeader->children().size(), std::size_t(3));
        QCOMPARE(followingHeader->children().size(), invalidHeader->children().size());
        const auto followingRbsp =
            analyzer->tree().node(followingNal->children().at(2));
        QVERIFY(followingRbsp.has_value());
        QCOMPARE(followingRbsp->name(), QStringLiteral("rbsp_payload"));
        QCOMPARE(followingRbsp->state(), MaterializationState::Materialized);
    }

    void publishesAnEmptyFinalNalUnitAsTruncated() {
        MemorySource source(bytes({0x00, 0x00, 0x01}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(analyzer->tree().hasPartialResults());

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->children().size(), std::size_t(2));
        QCOMPARE(nal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::TruncatedSource);
        QVERIFY(nal->diagnostics().front().location.has_value());
        QCOMPARE(nal->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(24));

        const auto startCode = analyzer->tree().node(nal->children().at(0));
        const auto header = analyzer->tree().node(nal->children().at(1));
        QVERIFY(startCode.has_value());
        QVERIFY(header.has_value());
        QCOMPARE(startCode->location()->sourceSpans().front().bitLength(), quint64(24));
        QCOMPARE(header->name(), QStringLiteral("NalUnitHeader"));
        QCOMPARE(header->state(), MaterializationState::Invalid);
        QVERIFY(header->children().empty());
        QCOMPARE(header->diagnostics().front().code,
                 streamview::core::DiagnosticCode::TruncatedSource);

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
    }

    void retainsPublishedNodesWhenHeaderReadingFails() {
        FailAfterFirstReadSource source;
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::SourceError);
        QCOMPARE(batch.errorMessage, QStringLiteral("injected read failure"));
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(analyzer->tree().hasPartialResults());

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::SourceError);
        QVERIFY(nal->diagnostics().front().location.has_value());
        QCOMPARE(nal->diagnostics()
                     .front()
                     .location->sourceSpans()
                     .front()
                     .start()
                     .absoluteBitOffset(),
                 quint64(24));
        QCOMPARE(nal->diagnostics().front().location->sourceSpans().front().bitLength(),
                 quint64(1));

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
        QCOMPARE(root->diagnostics().front().code,
                 streamview::core::DiagnosticCode::SourceError);
    }

    void retainsMappedPrefixWhenPayloadReadingFails() {
        constexpr quint64 payloadLength = 65'537;
        std::vector<std::byte> data(static_cast<std::size_t>(4U + payloadLength),
                                    std::byte{0x12});
        data[0] = std::byte{0x00};
        data[1] = std::byte{0x00};
        data[2] = std::byte{0x01};
        data[3] = std::byte{0x65};
        FailAtOffsetSource source(std::move(data), 4U + 65'536U);
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        H264AnnexBAnalysisBatch terminalBatch;
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto batch = analyzer->analyzeBatch();
            if (batch.status == H264AnnexBAnalysisStatus::SourceError) {
                terminalBatch = std::move(batch);
                break;
            }
            QCOMPARE(batch.status, H264AnnexBAnalysisStatus::InProgress);
            QVERIFY(batch.nalUnitNodes.empty());
        }

        QCOMPARE(terminalBatch.status, H264AnnexBAnalysisStatus::SourceError);
        QCOMPARE(terminalBatch.errorMessage, QStringLiteral("injected payload read failure"));
        QCOMPARE(terminalBatch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(terminalBatch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->children().size(), std::size_t(3));
        const auto header = analyzer->tree().node(nal->children().at(1));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(header.has_value());
        QVERIFY(rbsp.has_value());
        QCOMPARE(header->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::SourceError);
        QVERIFY(rbsp->location().has_value());
        QCOMPARE(rbsp->location()->sourceSpans().size(), std::size_t(1));
        QCOMPARE(rbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().front().bitLength(), quint64(65'536U * 8U));
    }

    void reportsMapperResourceLimitsWithTheCommittedPrefix() {
        MemorySource source(
            bytes({0x00, 0x00, 0x01, 0x65, 0x00, 0x00, 0x03, 0x01}));
        H264EbspRbspMapLimits limits;
        limits.maximumMappingSegments = 1;
        limits.maximumExcludedSpans = 4;
        limits.maximumIssues = 4;
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, std::nullopt, limits);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();

        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::ResourceLimit);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Invalid);
        QCOMPARE(nal->diagnostics().front().code,
                 streamview::core::DiagnosticCode::ResourceLimit);
        QCOMPARE(nal->children().size(), std::size_t(4));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        const auto excluded = analyzer->tree().node(nal->children().at(3));
        QVERIFY(rbsp.has_value());
        QVERIFY(excluded.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Invalid);
        QCOMPARE(rbsp->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(32));
        QCOMPARE(rbsp->location()->sourceSpans().front().bitLength(), quint64(16));
        QCOMPARE(excluded->name(), QStringLiteral("emulation_prevention_three_byte[0]"));
        QCOMPARE(excluded->location()->sourceSpans().front().start().absoluteBitOffset(),
                 quint64(48));
        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
        QCOMPARE(root->diagnostics().front().code,
                 streamview::core::DiagnosticCode::ResourceLimit);
    }

    void cancellationKeepsTheLastPublishedBatch() {
        CancellationSource cancellation;
        CancellingSource source(cancellation);
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));
        QVERIFY(analyzer->finished());

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Cancelled);
        QCOMPARE(nal->children().size(), std::size_t(3));
        const auto header = analyzer->tree().node(nal->children().at(1));
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(header.has_value());
        QVERIFY(rbsp.has_value());
        QCOMPARE(header->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->state(), MaterializationState::Cancelled);
        QCOMPARE(rbsp->diagnostics().front().code,
                 streamview::core::DiagnosticCode::Cancelled);
        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Cancelled);
        QCOMPARE(root->diagnostics().front().code,
                 streamview::core::DiagnosticCode::Cancelled);
    }

    void resumesScannerCancellationWithoutReplayingNodes() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65, 0x12}));
        CancellationSource cancellation;
        QVERIFY(cancellation.requestCancellation());
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto cancelled = analyzer->analyzeBatch();
        QCOMPARE(cancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        QVERIFY(cancelled.nalUnitNodes.empty());
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QCOMPARE(analyzer->tree().nodeCount(), std::size_t(1));
        const auto cancelledRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(cancelledRoot.has_value());
        QCOMPARE(cancelledRoot->state(), MaterializationState::Cancelled);
        QCOMPARE(cancelledRoot->diagnostics().size(), std::size_t(1));

        QString resumeError = QStringLiteral("stale error");
        QVERIFY(analyzer->resumeAfterCancellation(std::nullopt, &resumeError));
        QVERIFY(resumeError.isEmpty());
        QVERIFY(!analyzer->finished());
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QCOMPARE(analyzer->tree().nodeCount(), std::size_t(1));
        const auto resumedRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(resumedRoot.has_value());
        QCOMPARE(resumedRoot->state(), MaterializationState::Indexing);
        QVERIFY(resumedRoot->diagnostics().empty());

        const auto resumed = analyzer->analyzeBatch();
        QCOMPARE(resumed.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(resumed.nalUnitNodes.size(), std::size_t(1));
        const auto nal = analyzer->tree().node(resumed.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->name(), QStringLiteral("nal_unit[0]"));
        const auto completedRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(completedRoot.has_value());
        QCOMPARE(completedRoot->state(), MaterializationState::Materialized);
        QVERIFY(completedRoot->diagnostics().empty());

        const auto replay = analyzer->analyzeBatch();
        QCOMPARE(replay.status, H264AnnexBAnalysisStatus::Complete);
        QVERIFY(replay.nalUnitNodes.empty());
    }

    void batchesCancelsAndResumesAHundredGigabyteSparseSource() {
        HundredGigabyteSparseSource source;
        CancellationSource cancellation;
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(
            source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto first = analyzer->analyzeBatch(1, 64, 64);
        QCOMPARE(first.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(first.nalUnitNodes.size(), std::size_t{1});
        const auto firstId = first.nalUnitNodes.front();
        QVERIFY(cancellation.requestCancellation());

        const auto cancelled = analyzer->analyzeBatch(1, 2048, 64);
        QCOMPARE(cancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        QVERIFY(cancelled.nalUnitNodes.empty());
        QVERIFY(analyzer->finished());
        const auto cancelledRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(cancelledRoot.has_value());
        QCOMPARE(cancelledRoot->children().size(), std::size_t{1});
        QCOMPARE(cancelledRoot->children().front(), firstId);

        QVERIFY(analyzer->resumeAfterCancellation());
        const auto resumed = analyzer->analyzeBatch(1, 64, 64);
        QCOMPARE(resumed.status, H264AnnexBAnalysisStatus::InProgress);
        QCOMPARE(resumed.nalUnitNodes.size(), std::size_t{1});
        QVERIFY(resumed.nalUnitNodes.front() != firstId);
        const auto resumedRoot = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(resumedRoot.has_value());
        QCOMPARE(resumedRoot->children().size(), std::size_t{2});
        QCOMPARE(resumedRoot->children().at(0), firstId);
        QCOMPARE(resumedRoot->children().at(1), resumed.nalUnitNodes.front());

        QVERIFY(source.maximumRequestSize() <= SourcePager::pageSizeBytes());
        QVERIFY(source.highestReadEnd() <= SourcePager::pageSizeBytes());
        QVERIFY(analyzer->scanCursor() < SourcePager::pageSizeBytes());
    }

    void resumesRepeatedMapperCancellationsWithStableAppendOnlyNodes() {
        std::vector<std::byte> data;
        const auto appendNal = [&data](quint8 header,
                                       std::size_t payloadLength,
                                       std::byte payloadByte) {
            const auto prefix = bytes({0x00, 0x00, 0x01});
            data.insert(data.end(), prefix.begin(), prefix.end());
            data.push_back(static_cast<std::byte>(header));
            data.insert(data.end(), payloadLength, payloadByte);
        };
        appendNal(0x65, 1025, std::byte{0x12});
        const quint64 secondStart = static_cast<quint64>(data.size());
        appendNal(0x4c, 1025, std::byte{0x34});
        appendNal(0x4c, 1, std::byte{0x56});

        ArmedCancellingSource source(std::move(data));
        CancellationSource initialCancellation;
        QVERIFY(initialCancellation.requestCancellation());
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(
            source, &errorMessage, initialCancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto initiallyCancelled = analyzer->analyzeBatch();
        QCOMPARE(initiallyCancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        QVERIFY(initiallyCancelled.nalUnitNodes.empty());

        CancellationSource firstMapperCancellation;
        source.arm(4, firstMapperCancellation);
        QVERIFY(analyzer->resumeAfterCancellation(firstMapperCancellation.token()));
        const auto firstCancelledNal = analyzer->analyzeBatch();
        QCOMPARE(firstCancelledNal.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(firstCancelledNal.nalUnitNodes.size(), std::size_t(1));
        const auto firstId = firstCancelledNal.nalUnitNodes.front();
        const auto firstNal = analyzer->tree().node(firstId);
        QVERIFY(firstNal.has_value());
        QCOMPARE(firstNal->name(), QStringLiteral("nal_unit[0]"));
        QCOMPARE(firstNal->state(), MaterializationState::Cancelled);

        const auto nodesBeforeSecondResume = analyzer->tree().nodeCount();
        CancellationSource secondMapperCancellation;
        source.arm(secondStart + 4U, secondMapperCancellation);
        QVERIFY(analyzer->resumeAfterCancellation(secondMapperCancellation.token()));
        QCOMPARE(analyzer->tree().nodeCount(), nodesBeforeSecondResume);
        const auto secondCancelledNal = analyzer->analyzeBatch();
        QCOMPARE(secondCancelledNal.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(secondCancelledNal.nalUnitNodes.size(), std::size_t(1));
        const auto secondId = secondCancelledNal.nalUnitNodes.front();
        QVERIFY(secondId != firstId);
        const auto secondNal = analyzer->tree().node(secondId);
        QVERIFY(secondNal.has_value());
        QCOMPARE(secondNal->name(), QStringLiteral("nal_unit[1]"));
        QCOMPARE(secondNal->state(), MaterializationState::Cancelled);

        const auto nodesBeforeFinalResume = analyzer->tree().nodeCount();
        QVERIFY(analyzer->resumeAfterCancellation());
        QCOMPARE(analyzer->tree().nodeCount(), nodesBeforeFinalResume);
        const auto completed = analyzer->analyzeBatch();
        QCOMPARE(completed.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(completed.nalUnitNodes.size(), std::size_t(1));
        const auto thirdId = completed.nalUnitNodes.front();
        QVERIFY(thirdId != firstId);
        QVERIFY(thirdId != secondId);
        const auto thirdNal = analyzer->tree().node(thirdId);
        QVERIFY(thirdNal.has_value());
        QCOMPARE(thirdNal->name(), QStringLiteral("nal_unit[2]"));
        QCOMPARE(thirdNal->state(), MaterializationState::Materialized);

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
        QVERIFY(root->diagnostics().empty());
        QCOMPARE(root->children().size(), std::size_t(3));
        QCOMPARE(root->children().at(0), firstId);
        QCOMPARE(root->children().at(1), secondId);
        QCOMPARE(root->children().at(2), thirdId);
        QVERIFY(analyzer->tree().hasPartialResults());
        QVERIFY(!analyzer->tree().isFullyMaterialized());
    }

    void rejectsRequestedReplacementTokenWithoutMutation() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65}));
        CancellationSource cancellation;
        QVERIFY(cancellation.requestCancellation());
        QString errorMessage;
        auto analyzer =
            H264AnnexBAnalyzer::create(source, &errorMessage, cancellation.token());
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto cancelled = analyzer->analyzeBatch();
        QCOMPARE(cancelled.status, H264AnnexBAnalysisStatus::Cancelled);
        const auto rootBefore = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootBefore.has_value());
        const auto nodeCount = analyzer->tree().nodeCount();
        const auto cursor = analyzer->scanCursor();

        CancellationSource replacement;
        QVERIFY(replacement.requestCancellation());
        QString resumeError;
        QVERIFY(!analyzer->resumeAfterCancellation(replacement.token(), &resumeError));
        QVERIFY(resumeError.contains(QStringLiteral("already requested")));
        QVERIFY(analyzer->finished());
        QCOMPARE(analyzer->tree().nodeCount(), nodeCount);
        QCOMPARE(analyzer->scanCursor(), cursor);
        const auto rootAfter = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootAfter.has_value());
        QCOMPARE(rootAfter->state(), rootBefore->state());
        QCOMPARE(rootAfter->diagnostics().size(), rootBefore->diagnostics().size());
        QCOMPARE(rootAfter->diagnostics().front().message,
                 rootBefore->diagnostics().front().message);

        const auto replay = analyzer->analyzeBatch();
        QCOMPARE(replay.status, H264AnnexBAnalysisStatus::Cancelled);
        QCOMPARE(replay.errorMessage, cancelled.errorMessage);
        QVERIFY(replay.nalUnitNodes.empty());
    }

    void rejectsNonCancelledAnalyzersWithoutMutation() {
        MemorySource source(bytes({0x00, 0x00, 0x01, 0x65}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        QString resumeError;
        QVERIFY(!analyzer->resumeAfterCancellation(std::nullopt, &resumeError));
        QVERIFY(resumeError.contains(QStringLiteral("cancelled")));
        QVERIFY(!analyzer->finished());
        QCOMPARE(analyzer->scanCursor(), quint64(0));
        QCOMPARE(analyzer->tree().nodeCount(), std::size_t(1));

        const auto completed = analyzer->analyzeBatch();
        QCOMPARE(completed.status, H264AnnexBAnalysisStatus::Complete);
        const auto rootBefore = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootBefore.has_value());
        const auto nodeCount = analyzer->tree().nodeCount();
        const auto cursor = analyzer->scanCursor();
        QVERIFY(!analyzer->resumeAfterCancellation());
        QCOMPARE(analyzer->tree().nodeCount(), nodeCount);
        QCOMPARE(analyzer->scanCursor(), cursor);
        const auto rootAfter = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(rootAfter.has_value());
        QCOMPARE(rootAfter->state(), rootBefore->state());
        QCOMPARE(rootAfter->diagnostics().size(), rootBefore->diagnostics().size());
        const auto replay = analyzer->analyzeBatch();
        QCOMPARE(replay.status, H264AnnexBAnalysisStatus::Complete);
        QVERIFY(replay.nalUnitNodes.empty());
    }

    void rejectsIrrecoverableFailureStatesWithoutMutation() {
        FailAfterFirstReadSource failingSource;
        QString errorMessage;
        auto sourceErrorAnalyzer = H264AnnexBAnalyzer::create(failingSource, &errorMessage);
        QVERIFY2(sourceErrorAnalyzer.has_value(), qPrintable(errorMessage));
        const auto sourceError = sourceErrorAnalyzer->analyzeBatch();
        QCOMPARE(sourceError.status, H264AnnexBAnalysisStatus::SourceError);
        const auto sourceErrorCount = sourceErrorAnalyzer->tree().nodeCount();
        const auto sourceErrorRoot =
            sourceErrorAnalyzer->tree().node(sourceErrorAnalyzer->tree().rootId());
        QVERIFY(sourceErrorRoot.has_value());
        QVERIFY(!sourceErrorAnalyzer->resumeAfterCancellation());
        QCOMPARE(sourceErrorAnalyzer->tree().nodeCount(), sourceErrorCount);
        QCOMPARE(sourceErrorAnalyzer->tree()
                     .node(sourceErrorAnalyzer->tree().rootId())
                     ->state(),
                 sourceErrorRoot->state());
        QCOMPARE(sourceErrorAnalyzer->analyzeBatch().status,
                 H264AnnexBAnalysisStatus::SourceError);

        MemorySource limitedSource(
            bytes({0x00, 0x00, 0x01, 0x65, 0x00, 0x00, 0x03, 0x01}));
        H264EbspRbspMapLimits limits;
        limits.maximumMappingSegments = 1;
        limits.maximumExcludedSpans = 4;
        limits.maximumIssues = 4;
        auto limitedAnalyzer =
            H264AnnexBAnalyzer::create(limitedSource, &errorMessage, std::nullopt, limits);
        QVERIFY2(limitedAnalyzer.has_value(), qPrintable(errorMessage));
        const auto limited = limitedAnalyzer->analyzeBatch();
        QCOMPARE(limited.status, H264AnnexBAnalysisStatus::ResourceLimit);
        const auto limitedCount = limitedAnalyzer->tree().nodeCount();
        const auto limitedRoot =
            limitedAnalyzer->tree().node(limitedAnalyzer->tree().rootId());
        QVERIFY(limitedRoot.has_value());
        QVERIFY(!limitedAnalyzer->resumeAfterCancellation());
        QCOMPARE(limitedAnalyzer->tree().nodeCount(), limitedCount);
        QCOMPARE(limitedAnalyzer->tree().node(limitedAnalyzer->tree().rootId())->state(),
                 limitedRoot->state());
        QCOMPARE(limitedAnalyzer->analyzeBatch().status,
                 H264AnnexBAnalysisStatus::ResourceLimit);
    }

    void decodesTheSupportedSeiPayloadWithSingleMessage() {
        const auto stream = packSeiNal({{5, {0x11, 0x22, 0x33, 0x44}}});
        MemorySource source(stream);
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        QCOMPARE(nal->state(), MaterializationState::Materialized);

        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        QCOMPARE(rbsp->state(), MaterializationState::Materialized);
        QCOMPARE(rbsp->children().size(), std::size_t(1));

        const auto sei = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sei.has_value());
        QCOMPARE(sei->name(), QStringLiteral("SeiRbsp"));
        QCOMPARE(sei->state(), MaterializationState::Materialized);
        QCOMPARE(sei->metadata().specification->clause, QStringLiteral("7.3.2.3"));

        const auto childNamesOf = [&](const auto& node) {
            QStringList names;
            for (const auto childId : node.children()) {
                if (const auto child = analyzer->tree().node(childId)) {
                    names.append(child->name());
                }
            }
            return names;
        };

        QStringList expectedChildren = {
            QStringLiteral("payload_type[0]"),
            QStringLiteral("payload_size[0]"),
            QStringLiteral("payload_data[0]"),
            QStringLiteral("rbsp_stop_one_bit"),
        };
        for (std::size_t index = 0; index < 7; ++index) {
            expectedChildren.append(
                QStringLiteral("rbsp_alignment_zero_bit[%1]").arg(index));
        }
        QCOMPARE(childNamesOf(*sei), expectedChildren);

        const auto fieldNamed = [&](const auto& parentNode, const QString& name) {
            const auto found = std::find_if(
                parentNode.children().begin(), parentNode.children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == parentNode.children().end() ? std::nullopt
                                                        : analyzer->tree().node(*found);
        };

        const auto payloadType = fieldNamed(*sei, QStringLiteral("payload_type[0]"));
        QVERIFY(payloadType.has_value());
        QCOMPARE(payloadType->value().toULongLong(), quint64(5));
        QCOMPARE(payloadType->location()->logicalRange().bitLength(), quint64(8));
        QCOMPARE(payloadType->metadata().specification->clause, QStringLiteral("7.3.2.3.1"));

        const auto payloadSize = fieldNamed(*sei, QStringLiteral("payload_size[0]"));
        QVERIFY(payloadSize.has_value());
        QCOMPARE(payloadSize->value().toULongLong(), quint64(4));
        QCOMPARE(payloadSize->location()->logicalRange().bitLength(), quint64(8));
        QCOMPARE(payloadSize->metadata().specification->clause, QStringLiteral("7.3.2.3.1"));

        const auto payloadData = fieldNamed(*sei, QStringLiteral("payload_data[0]"));
        QVERIFY(payloadData.has_value());
        QCOMPARE(payloadData->state(), MaterializationState::Lazy);
        QCOMPARE(payloadData->location()->logicalRange().bitLength(), quint64(32));
        QCOMPARE(payloadData->metadata().specification->clause, QStringLiteral("7.3.2.3.1"));

        const auto stopBit = fieldNamed(*sei, QStringLiteral("rbsp_stop_one_bit"));
        QVERIFY(stopBit.has_value());
        QCOMPARE(stopBit->value().toULongLong(), quint64(1));
        QCOMPARE(stopBit->location()->logicalRange().bitLength(), quint64(1));

        for (std::size_t index = 0; index < 7; ++index) {
            const auto alignBit = fieldNamed(
                *sei, QStringLiteral("rbsp_alignment_zero_bit[%1]").arg(index));
            QVERIFY(alignBit.has_value());
            QCOMPARE(alignBit->value().toULongLong(), quint64(0));
            QCOMPARE(alignBit->location()->logicalRange().bitLength(), quint64(1));
        }
    }

    void decodesMultipleSeiMessagesInSingleNalUnit() {
        const auto stream = packSeiNal({
            {5, {0xaa, 0xbb}},
            {6, {0x01, 0x02, 0x03}}
        });
        MemorySource source(stream);
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sei = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sei.has_value());
        QCOMPARE(sei->state(), MaterializationState::Materialized);

        const auto childNamesOf = [&](const auto& node) {
            QStringList names;
            for (const auto childId : node.children()) {
                if (const auto child = analyzer->tree().node(childId)) {
                    names.append(child->name());
                }
            }
            return names;
        };

        QStringList expectedChildren = {
            QStringLiteral("payload_type[0]"),
            QStringLiteral("payload_size[0]"),
            QStringLiteral("payload_data[0]"),
            QStringLiteral("payload_type[1]"),
            QStringLiteral("payload_size[1]"),
            QStringLiteral("payload_data[1]"),
            QStringLiteral("rbsp_stop_one_bit"),
        };
        for (std::size_t index = 0; index < 7; ++index) {
            expectedChildren.append(
                QStringLiteral("rbsp_alignment_zero_bit[%1]").arg(index));
        }
        QCOMPARE(childNamesOf(*sei), expectedChildren);

        const auto fieldNamed = [&](const auto& parentNode, const QString& name) {
            const auto found = std::find_if(
                parentNode.children().begin(), parentNode.children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == parentNode.children().end() ? std::nullopt
                                                        : analyzer->tree().node(*found);
        };

        const auto type0 = fieldNamed(*sei, QStringLiteral("payload_type[0]"));
        QVERIFY(type0.has_value());
        QCOMPARE(type0->value().toULongLong(), quint64(5));

        const auto size0 = fieldNamed(*sei, QStringLiteral("payload_size[0]"));
        QVERIFY(size0.has_value());
        QCOMPARE(size0->value().toULongLong(), quint64(2));

        const auto data0 = fieldNamed(*sei, QStringLiteral("payload_data[0]"));
        QVERIFY(data0.has_value());
        QCOMPARE(data0->location()->logicalRange().bitLength(), quint64(16));

        const auto type1 = fieldNamed(*sei, QStringLiteral("payload_type[1]"));
        QVERIFY(type1.has_value());
        QCOMPARE(type1->value().toULongLong(), quint64(6));

        const auto size1 = fieldNamed(*sei, QStringLiteral("payload_size[1]"));
        QVERIFY(size1.has_value());
        QCOMPARE(size1->value().toULongLong(), quint64(3));

        const auto data1 = fieldNamed(*sei, QStringLiteral("payload_data[1]"));
        QVERIFY(data1.has_value());
        QCOMPARE(data1->location()->logicalRange().bitLength(), quint64(24));
    }

    void decodesSeiMessageWithFfCodedPayloadTypeAndSize() {
        std::vector<quint8> payloadBytes(257, 0x42);
        const auto stream = packSeiNal({{256, payloadBytes}});
        MemorySource source(stream);
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sei = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sei.has_value());
        QCOMPARE(sei->state(), MaterializationState::Materialized);

        const auto fieldNamed = [&](const auto& parentNode, const QString& name) {
            const auto found = std::find_if(
                parentNode.children().begin(), parentNode.children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == parentNode.children().end() ? std::nullopt
                                                        : analyzer->tree().node(*found);
        };

        const auto payloadType = fieldNamed(*sei, QStringLiteral("payload_type[0]"));
        QVERIFY(payloadType.has_value());
        QCOMPARE(payloadType->value().toULongLong(), quint64(256));
        QCOMPARE(payloadType->location()->logicalRange().bitLength(), quint64(16));

        const auto payloadSize = fieldNamed(*sei, QStringLiteral("payload_size[0]"));
        QVERIFY(payloadSize.has_value());
        QCOMPARE(payloadSize->value().toULongLong(), quint64(257));
        QCOMPARE(payloadSize->location()->logicalRange().bitLength(), quint64(16));

        const auto payloadData = fieldNamed(*sei, QStringLiteral("payload_data[0]"));
        QVERIFY(payloadData.has_value());
        QCOMPARE(payloadData->location()->logicalRange().bitLength(), quint64(257 * 8));
    }

    void decodesSeiMessageWithZeroPayloadSize() {
        const auto stream = packSeiNal({{1, {}}});
        MemorySource source(stream);
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(1));

        const auto nal = analyzer->tree().node(batch.nalUnitNodes.front());
        QVERIFY(nal.has_value());
        const auto rbsp = analyzer->tree().node(nal->children().at(2));
        QVERIFY(rbsp.has_value());
        const auto sei = analyzer->tree().node(rbsp->children().front());
        QVERIFY(sei.has_value());
        QCOMPARE(sei->state(), MaterializationState::Materialized);

        const auto childNamesOf = [&](const auto& node) {
            QStringList names;
            for (const auto childId : node.children()) {
                if (const auto child = analyzer->tree().node(childId)) {
                    names.append(child->name());
                }
            }
            return names;
        };

        const QStringList expectedChildren = {
            QStringLiteral("payload_type[0]"),
            QStringLiteral("payload_size[0]"),
            QStringLiteral("payload_data[0]"),
            QStringLiteral("rbsp_stop_one_bit"),
            QStringLiteral("rbsp_alignment_zero_bit[0]"),
            QStringLiteral("rbsp_alignment_zero_bit[1]"),
            QStringLiteral("rbsp_alignment_zero_bit[2]"),
            QStringLiteral("rbsp_alignment_zero_bit[3]"),
            QStringLiteral("rbsp_alignment_zero_bit[4]"),
            QStringLiteral("rbsp_alignment_zero_bit[5]"),
            QStringLiteral("rbsp_alignment_zero_bit[6]"),
        };
        QCOMPARE(childNamesOf(*sei), expectedChildren);

        const auto fieldNamed = [&](const auto& parentNode, const QString& name) {
            const auto found = std::find_if(
                parentNode.children().begin(), parentNode.children().end(), [&](const auto id) {
                    const auto node = analyzer->tree().node(id);
                    return node && node->name() == name;
                });
            return found == parentNode.children().end() ? std::nullopt
                                                        : analyzer->tree().node(*found);
        };

        const auto payloadSize = fieldNamed(*sei, QStringLiteral("payload_size[0]"));
        QVERIFY(payloadSize.has_value());
        QCOMPARE(payloadSize->value().toULongLong(), quint64(0));
        QCOMPARE(payloadSize->location()->logicalRange().bitLength(), quint64(8));

        const auto payloadData = fieldNamed(*sei, QStringLiteral("payload_data[0]"));
        QVERIFY(payloadData.has_value());
        QCOMPARE(payloadData->location()->logicalRange().bitLength(), quint64(0));
    }

    void reportsTruncatedSeiPayloadAndContinues() {
        std::vector<bool> truncatedBits;
        appendFfCoded(truncatedBits, 5);
        appendFfCoded(truncatedBits, 20);
        appendFixedBits(truncatedBits, 0x11, 8);
        appendFixedBits(truncatedBits, 0x22, 8);
        auto stream = packAnnexBNal(0x06, std::move(truncatedBits), false);
        appendNal(stream, bytes({0x00, 0x00, 0x01, 0x09, 0x50}));

        MemorySource source(stream);
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QCOMPARE(batch.nalUnitNodes.size(), std::size_t(2));

        const auto seiNal = analyzer->tree().node(batch.nalUnitNodes.at(0));
        QVERIFY(seiNal.has_value());
        QCOMPARE(seiNal->state(), MaterializationState::Invalid);

        const auto audNal = analyzer->tree().node(batch.nalUnitNodes.at(1));
        QVERIFY(audNal.has_value());
        QCOMPARE(audNal->state(), MaterializationState::Materialized);
    }

    void reportsInputWithoutAStartCodeAsInvalid() {
        MemorySource source(bytes({0x12, 0x34, 0x56}));
        QString errorMessage;
        auto analyzer = H264AnnexBAnalyzer::create(source, &errorMessage);
        QVERIFY2(analyzer.has_value(), qPrintable(errorMessage));

        const auto batch = analyzer->analyzeBatch();
        QCOMPARE(batch.status, H264AnnexBAnalysisStatus::Complete);
        QVERIFY(batch.nalUnitNodes.empty());
        QVERIFY(analyzer->tree().hasPartialResults());

        const auto root = analyzer->tree().node(analyzer->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
        QCOMPARE(root->diagnostics().size(), std::size_t(1));
        QCOMPARE(root->diagnostics().front().code,
                 streamview::core::DiagnosticCode::InvalidSyntax);
        QVERIFY(root->diagnostics().front().message.contains(QStringLiteral("start code")));
    }
};

QTEST_GUILESS_MAIN(H264AnnexBAnalyzerTest)

#include "h264_annex_b_analyzer_test.moc"
