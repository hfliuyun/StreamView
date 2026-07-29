#include <streamview/rules/analysis_cache_payload.h>

#include <QByteArray>
#include <QMetaType>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <ranges>
#include <span>
#include <utility>

namespace streamview::rules {

namespace {

constexpr std::array<std::byte, 8> progressiveMagic{
    std::byte{'S'}, std::byte{'V'}, std::byte{'P'}, std::byte{'I'},
    std::byte{'D'}, std::byte{'X'}, std::byte{0},   std::byte{0},
};
constexpr std::array<std::byte, 8> materializedMagic{
    std::byte{'S'}, std::byte{'V'}, std::byte{'M'}, std::byte{'A'},
    std::byte{'T'}, std::byte{'R'}, std::byte{0},   std::byte{0},
};
constexpr std::size_t progressiveHeaderSize = 56;
constexpr std::size_t progressiveRecordSize = 48;
constexpr std::size_t materializedHeaderSize = 40;
constexpr std::size_t maximumTextBytes = 32U * 1024U;
constexpr std::size_t maximumLocationSpans = 1024;
constexpr std::size_t maximumNodeDiagnostics = 256;
constexpr std::size_t maximumMaterializedNodes = 1024;
constexpr quint32 progressiveEndOfSourceFlag = 1U;
constexpr quint32 nodeLocationFlag = 1U;
constexpr quint32 nodeSpecificationFlag = 2U;
constexpr quint32 diagnosticLocationFlag = 1U;

static_assert(AnalysisCachePageBodyCodec::progressiveIndexFormatVersion() ==
              AnalysisCacheVersions::current().progressiveIndexPayload);
static_assert(AnalysisCachePageBodyCodec::materializedResultFormatVersion() ==
              AnalysisCacheVersions::current().materializedResultPayload);

enum class StoredValueKind : quint32 {
    None = 0,
    Boolean = 1,
    Unsigned = 2,
    Signed = 3,
    String = 4,
};

class Writer final {
public:
    void append32(quint32 value) {
        if (!reserve(4U)) {
            return;
        }
        for (int shift : {24, 16, 8, 0}) {
            bytes_.push_back(
                static_cast<std::byte>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
        }
    }

    void append64(quint64 value) {
        if (!reserve(8U)) {
            return;
        }
        for (int shift : {56, 48, 40, 32, 24, 16, 8, 0}) {
            bytes_.push_back(
                static_cast<std::byte>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
        }
    }

    void append(std::span<const std::byte> bytes) {
        if (!reserve(bytes.size())) {
            return;
        }
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] bool appendString(const QString& text) {
        const QByteArray utf8 = text.toUtf8();
        if (static_cast<std::size_t>(utf8.size()) > maximumTextBytes ||
            QString::fromUtf8(utf8) != text ||
            !reserve(4U + static_cast<std::size_t>(utf8.size()))) {
            return false;
        }
        append32(static_cast<quint32>(utf8.size()));
        append(std::as_bytes(std::span(utf8.constData(), static_cast<std::size_t>(utf8.size()))));
        return true;
    }

    [[nodiscard]] std::vector<std::byte> take() && { return std::move(bytes_); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }

private:
    [[nodiscard]] bool reserve(std::size_t byteCount) {
        if (byteCount > AnalysisCachePayloadEnvelope::maximumPayloadBytes() - bytes_.size()) {
            overflowed_ = true;
            return false;
        }
        return true;
    }

    std::vector<std::byte> bytes_;
    bool overflowed_ = false;
};

class Reader final {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool read32(quint32* value) {
        if (remaining() < 4U) {
            return false;
        }
        *value = 0;
        for (std::size_t index = 0; index < 4U; ++index) {
            *value = (*value << 8U) | std::to_integer<quint32>(bytes_[offset_ + index]);
        }
        offset_ += 4U;
        return true;
    }

    [[nodiscard]] bool read64(quint64* value) {
        if (remaining() < 8U) {
            return false;
        }
        *value = 0;
        for (std::size_t index = 0; index < 8U; ++index) {
            *value = (*value << 8U) | std::to_integer<quint64>(bytes_[offset_ + index]);
        }
        offset_ += 8U;
        return true;
    }

    [[nodiscard]] bool readMagic(std::span<const std::byte> magic) {
        if (remaining() < magic.size() ||
            !std::equal(magic.begin(), magic.end(), bytes_.begin() +
                                                        static_cast<std::ptrdiff_t>(offset_))) {
            return false;
        }
        offset_ += magic.size();
        return true;
    }

    [[nodiscard]] bool readString(QString* text) {
        quint32 length = 0;
        if (!read32(&length) || length > maximumTextBytes || remaining() < length) {
            return false;
        }
        const QByteArray utf8(reinterpret_cast<const char*>(bytes_.data() + offset_),
                              static_cast<qsizetype>(length));
        const QString decoded = QString::fromUtf8(utf8);
        if (decoded.toUtf8() != utf8) {
            return false;
        }
        *text = decoded;
        offset_ += length;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] bool atEnd() const noexcept { return offset_ == bytes_.size(); }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

[[nodiscard]] bool validKey(const core::PagedCachePageKey& key,
                            core::PagedCachePageKind expectedKind) {
    constexpr quint64 sqliteMaximum = static_cast<quint64>(std::numeric_limits<qint64>::max());
    return key.kind == expectedKind && key.streamId <= sqliteMaximum &&
           key.pageIndex <= sqliteMaximum;
}

[[nodiscard]] bool addWouldOverflow(quint64 left, quint64 right) {
    return right > std::numeric_limits<quint64>::max() - left;
}

[[nodiscard]] bool matchesByteSpan(const std::optional<core::SourceSpan>& span,
                                   quint64 offset, quint64 length) {
    if (length == 0) {
        return !span.has_value();
    }
    if (!span || offset > std::numeric_limits<quint64>::max() / 8U ||
        length > std::numeric_limits<quint64>::max() / 8U) {
        return false;
    }
    return span->start().absoluteBitOffset() == offset * 8U &&
           span->bitLength() == length * 8U;
}

[[nodiscard]] bool validateIndexRecord(const H264StartCodeRecord& record,
                                       quint64 indexedThroughByteOffset,
                                       std::optional<quint64> previousEnd,
                                       quint64* recordEnd) {
    if ((record.startCodeLength != 3U && record.startCodeLength != 4U) ||
        addWouldOverflow(record.startCodeOffset, record.startCodeLength) ||
        record.nalUnitOffset != record.startCodeOffset + record.startCodeLength ||
        addWouldOverflow(record.nalUnitOffset, record.nalUnitLength) ||
        record.trailingZero8BitsOffset != record.nalUnitOffset + record.nalUnitLength ||
        addWouldOverflow(record.trailingZero8BitsOffset, record.trailingZero8BitsLength)) {
        return false;
    }
    *recordEnd = record.trailingZero8BitsOffset + record.trailingZero8BitsLength;
    return (!previousEnd || record.startCodeOffset >= *previousEnd) &&
           *recordEnd <= indexedThroughByteOffset &&
           matchesByteSpan(record.startCode, record.startCodeOffset, record.startCodeLength) &&
           matchesByteSpan(record.nalUnit, record.nalUnitOffset, record.nalUnitLength) &&
           matchesByteSpan(record.trailingZero8Bits, record.trailingZero8BitsOffset,
                           record.trailingZero8BitsLength);
}

[[nodiscard]] std::optional<H264StartCodeRecord>
makeIndexRecord(quint64 startCodeOffset, quint32 startCodeLength, quint64 nalUnitOffset,
                quint64 nalUnitLength, quint64 trailingOffset, quint64 trailingLength,
                quint64 indexedThroughByteOffset, std::optional<quint64> previousEnd) {
    if (startCodeLength > std::numeric_limits<quint8>::max()) {
        return std::nullopt;
    }
    H264StartCodeRecord record;
    record.startCodeOffset = startCodeOffset;
    record.startCodeLength = static_cast<quint8>(startCodeLength);
    record.nalUnitOffset = nalUnitOffset;
    record.nalUnitLength = nalUnitLength;
    record.trailingZero8BitsOffset = trailingOffset;
    record.trailingZero8BitsLength = trailingLength;
    if (startCodeOffset <= std::numeric_limits<quint64>::max() / 8U) {
        record.startCode = core::SourceSpan::create(
            core::SourceBitAddress(startCodeOffset * 8U),
            static_cast<quint64>(startCodeLength) * 8U);
    }
    if (nalUnitLength != 0 && nalUnitOffset <= std::numeric_limits<quint64>::max() / 8U &&
        nalUnitLength <= std::numeric_limits<quint64>::max() / 8U) {
        record.nalUnit = core::SourceSpan::create(core::SourceBitAddress(nalUnitOffset * 8U),
                                                 nalUnitLength * 8U);
    }
    if (trailingLength != 0 && trailingOffset <= std::numeric_limits<quint64>::max() / 8U &&
        trailingLength <= std::numeric_limits<quint64>::max() / 8U) {
        record.trailingZero8Bits = core::SourceSpan::create(
            core::SourceBitAddress(trailingOffset * 8U), trailingLength * 8U);
    }
    quint64 end = 0;
    return validateIndexRecord(record, indexedThroughByteOffset, previousEnd, &end)
               ? std::optional<H264StartCodeRecord>(std::move(record))
               : std::nullopt;
}

[[nodiscard]] std::optional<quint32> nodeKindCode(core::AnalysisNodeKind kind) {
    const quint32 code = static_cast<quint32>(kind) + 1U;
    return code >= 1U && code <= 6U ? std::optional<quint32>(code) : std::nullopt;
}

[[nodiscard]] std::optional<core::AnalysisNodeKind> nodeKind(quint32 code) {
    return code >= 1U && code <= 6U
               ? std::optional<core::AnalysisNodeKind>(
                     static_cast<core::AnalysisNodeKind>(code - 1U))
               : std::nullopt;
}

[[nodiscard]] std::optional<quint32> stateCode(core::MaterializationState state) {
    const quint32 code = static_cast<quint32>(state) + 1U;
    if (code < 1U || code > 7U || state == core::MaterializationState::Indexing ||
        state == core::MaterializationState::WaitingDependency) {
        return std::nullopt;
    }
    return code;
}

[[nodiscard]] std::optional<core::MaterializationState> stateFromCode(quint32 code) {
    if (code < 1U || code > 7U) {
        return std::nullopt;
    }
    const auto state = static_cast<core::MaterializationState>(code - 1U);
    return state == core::MaterializationState::Indexing ||
                   state == core::MaterializationState::WaitingDependency
               ? std::nullopt
               : std::optional<core::MaterializationState>(state);
}

[[nodiscard]] bool isTransientStateCode(quint32 code) {
    return code == static_cast<quint32>(core::MaterializationState::Indexing) + 1U ||
           code == static_cast<quint32>(core::MaterializationState::WaitingDependency) + 1U;
}

[[nodiscard]] std::optional<quint32> diagnosticCode(core::DiagnosticCode code) {
    const quint32 value = static_cast<quint32>(code) + 1U;
    return value >= 1U && value <= 7U ? std::optional<quint32>(value) : std::nullopt;
}

[[nodiscard]] std::optional<core::DiagnosticCode> diagnosticFromCode(quint32 code) {
    return code >= 1U && code <= 7U
               ? std::optional<core::DiagnosticCode>(
                     static_cast<core::DiagnosticCode>(code - 1U))
               : std::nullopt;
}

[[nodiscard]] std::optional<quint32> severityCode(core::DiagnosticSeverity severity) {
    const quint32 value = static_cast<quint32>(severity) + 1U;
    return value >= 1U && value <= 3U ? std::optional<quint32>(value) : std::nullopt;
}

[[nodiscard]] std::optional<core::DiagnosticSeverity> severityFromCode(quint32 code) {
    return code >= 1U && code <= 3U
               ? std::optional<core::DiagnosticSeverity>(
                     static_cast<core::DiagnosticSeverity>(code - 1U))
               : std::nullopt;
}

[[nodiscard]] std::optional<StoredValueKind> storedValueKind(const QVariant& value) {
    if (!value.isValid()) {
        return StoredValueKind::None;
    }
    switch (value.typeId()) {
    case QMetaType::Bool:
        return StoredValueKind::Boolean;
    case QMetaType::ULongLong:
        return StoredValueKind::Unsigned;
    case QMetaType::LongLong:
        return StoredValueKind::Signed;
    case QMetaType::QString:
        return StoredValueKind::String;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool isUnicodeScalarText(const QString& text) {
    return QString::fromUtf8(text.toUtf8()) == text;
}

[[nodiscard]] bool validLocation(const core::FieldLocation& location) {
    return core::FieldLocation::create(location.logicalRange(), location.sourceSpans()).has_value();
}

[[nodiscard]] bool appendLocation(Writer* writer, const core::FieldLocation& location) {
    if (!validLocation(location) || location.sourceSpans().size() > maximumLocationSpans) {
        return false;
    }
    writer->append64(location.logicalRange().start().viewId().value());
    writer->append64(location.logicalRange().start().bitOffset());
    writer->append64(location.logicalRange().bitLength());
    writer->append32(static_cast<quint32>(location.sourceSpans().size()));
    writer->append32(0U);
    for (const core::SourceSpan& span : location.sourceSpans()) {
        writer->append64(span.start().absoluteBitOffset());
        writer->append64(span.bitLength());
    }
    return true;
}

[[nodiscard]] std::optional<core::FieldLocation> readLocation(Reader* reader) {
    quint64 viewId = 0;
    quint64 logicalStart = 0;
    quint64 logicalLength = 0;
    quint32 spanCount = 0;
    quint32 reserved = 0;
    if (!reader->read64(&viewId) || !reader->read64(&logicalStart) ||
        !reader->read64(&logicalLength) || !reader->read32(&spanCount) ||
        !reader->read32(&reserved) || reserved != 0U || spanCount > maximumLocationSpans) {
        return std::nullopt;
    }
    const auto range = core::LogicalRange::create(
        core::LogicalBitAddress(core::LogicalViewId(viewId), logicalStart), logicalLength);
    if (!range) {
        return std::nullopt;
    }
    std::vector<core::SourceSpan> spans;
    spans.reserve(spanCount);
    for (quint32 index = 0; index < spanCount; ++index) {
        quint64 start = 0;
        quint64 length = 0;
        if (!reader->read64(&start) || !reader->read64(&length)) {
            return std::nullopt;
        }
        auto span = core::SourceSpan::create(core::SourceBitAddress(start), length);
        if (!span) {
            return std::nullopt;
        }
        spans.push_back(*span);
    }
    return core::FieldLocation::create(*range, std::move(spans));
}

[[nodiscard]] bool validNodeTopology(const MaterializedResultCacheNode& node) {
    if (node.id.value() == 0 || node.spec.name.isEmpty() || !stateCode(node.spec.state) ||
        !isUnicodeScalarText(node.spec.name) ||
        !isUnicodeScalarText(node.spec.metadata.typeName) ||
        !isUnicodeScalarText(node.spec.metadata.description) ||
        (node.spec.value.typeId() == QMetaType::QString &&
         !isUnicodeScalarText(node.spec.value.toString())) ||
        (node.spec.kind == core::AnalysisNodeKind::Root &&
         (node.id.value() != 1U || node.parentId.has_value())) ||
        (node.spec.kind != core::AnalysisNodeKind::Root &&
         (!node.parentId || node.parentId->value() == 0 ||
          node.parentId->value() >= node.id.value())) ||
        (node.spec.kind == core::AnalysisNodeKind::SyntaxField && !node.spec.location) ||
        (node.spec.kind == core::AnalysisNodeKind::ComputedField && node.spec.location) ||
        (node.spec.location && !validLocation(*node.spec.location)) ||
        node.diagnostics.size() > maximumNodeDiagnostics) {
        return false;
    }
    if (node.spec.metadata.specification &&
        (node.spec.metadata.specification->standard.isEmpty() ||
         node.spec.metadata.specification->clause.isEmpty() ||
         !isUnicodeScalarText(node.spec.metadata.specification->standard) ||
         !isUnicodeScalarText(node.spec.metadata.specification->clause))) {
        return false;
    }
    return std::ranges::all_of(node.diagnostics, [](const core::ParseDiagnostic& diagnostic) {
        return diagnosticCode(diagnostic.code).has_value() &&
               severityCode(diagnostic.severity).has_value() && !diagnostic.message.isEmpty() &&
               isUnicodeScalarText(diagnostic.message) &&
               isUnicodeScalarText(diagnostic.fieldPath) &&
               (!diagnostic.location || validLocation(*diagnostic.location));
    });
}

[[nodiscard]] bool appendValue(Writer* writer, StoredValueKind kind, const QVariant& value) {
    switch (kind) {
    case StoredValueKind::None:
        return true;
    case StoredValueKind::Boolean:
        writer->append64(value.toBool() ? 1U : 0U);
        return true;
    case StoredValueKind::Unsigned:
        writer->append64(value.toULongLong());
        return true;
    case StoredValueKind::Signed:
        writer->append64(std::bit_cast<quint64>(value.toLongLong()));
        return true;
    case StoredValueKind::String:
        return writer->appendString(value.toString());
    }
    return false;
}

[[nodiscard]] bool readValue(Reader* reader, StoredValueKind kind, QVariant* value) {
    quint64 stored = 0;
    switch (kind) {
    case StoredValueKind::None:
        *value = {};
        return true;
    case StoredValueKind::Boolean:
        if (!reader->read64(&stored) || stored > 1U) {
            return false;
        }
        *value = QVariant::fromValue(stored != 0U);
        return true;
    case StoredValueKind::Unsigned:
        if (!reader->read64(&stored)) {
            return false;
        }
        *value = QVariant::fromValue<qulonglong>(stored);
        return true;
    case StoredValueKind::Signed:
        if (!reader->read64(&stored)) {
            return false;
        }
        *value = QVariant::fromValue<qlonglong>(std::bit_cast<qint64>(stored));
        return true;
    case StoredValueKind::String: {
        QString text;
        if (!reader->readString(&text)) {
            return false;
        }
        *value = text;
        return true;
    }
    }
    return false;
}

[[nodiscard]] AnalysisCacheBodyEncodeResult invalidEncode(AnalysisCacheBodyEncodeStatus status,
                                                          QString message) {
    return {status, {}, std::move(message)};
}

[[nodiscard]] H264ProgressiveIndexCacheDecodeResult invalidProgressiveDecode(
    AnalysisCacheBodyDecodeStatus status, QString message) {
    return {status, std::nullopt, std::move(message)};
}

[[nodiscard]] MaterializedResultCacheDecodeResult invalidMaterializedDecode(
    AnalysisCacheBodyDecodeStatus status, QString message) {
    return {status, std::nullopt, std::move(message)};
}

} // namespace

AnalysisCacheBodyEncodeResult AnalysisCachePageBodyCodec::encodeProgressiveIndex(
    const H264ProgressiveIndexCachePage& page) {
    const std::size_t maximumRecords =
        (AnalysisCachePayloadEnvelope::maximumPayloadBytes() - progressiveHeaderSize) /
        progressiveRecordSize;
    if (page.records.size() > maximumRecords ||
        page.records.size() > std::numeric_limits<quint32>::max()) {
        return invalidEncode(AnalysisCacheBodyEncodeStatus::PayloadTooLarge,
                             QStringLiteral("Progressive-index body exceeds one cache page"));
    }
    if (!validKey(page.key, core::PagedCachePageKind::ProgressiveIndex) ||
        page.firstRecordIndex >
            std::numeric_limits<quint64>::max() - page.records.size()) {
        return invalidEncode(AnalysisCacheBodyEncodeStatus::InvalidArgument,
                             QStringLiteral("Progressive-index cache page metadata is invalid"));
    }

    std::optional<quint64> previousEnd;
    for (const H264StartCodeRecord& record : page.records) {
        quint64 end = 0;
        if (!validateIndexRecord(record, page.indexedThroughByteOffset, previousEnd, &end)) {
            return invalidEncode(AnalysisCacheBodyEncodeStatus::InvalidArgument,
                                 QStringLiteral("Progressive-index cache record is invalid"));
        }
        previousEnd = end;
    }

    Writer writer;
    writer.append(progressiveMagic);
    writer.append32(progressiveIndexFormatVersion());
    writer.append32(page.endOfSource ? progressiveEndOfSourceFlag : 0U);
    writer.append64(page.key.streamId);
    writer.append64(page.key.pageIndex);
    writer.append64(page.firstRecordIndex);
    writer.append64(page.indexedThroughByteOffset);
    writer.append32(static_cast<quint32>(page.records.size()));
    writer.append32(0U);
    for (const H264StartCodeRecord& record : page.records) {
        writer.append64(record.startCodeOffset);
        writer.append32(record.startCodeLength);
        writer.append32(0U);
        writer.append64(record.nalUnitOffset);
        writer.append64(record.nalUnitLength);
        writer.append64(record.trailingZero8BitsOffset);
        writer.append64(record.trailingZero8BitsLength);
    }
    return {AnalysisCacheBodyEncodeStatus::Encoded, std::move(writer).take(), {}};
}

H264ProgressiveIndexCacheDecodeResult AnalysisCachePageBodyCodec::decodeProgressiveIndex(
    const core::PagedCachePageKey& expectedKey, std::span<const std::byte> bytes) {
    if (!validKey(expectedKey, core::PagedCachePageKind::ProgressiveIndex) ||
        bytes.size() < progressiveHeaderSize ||
        bytes.size() > AnalysisCachePayloadEnvelope::maximumPayloadBytes()) {
        return invalidProgressiveDecode(AnalysisCacheBodyDecodeStatus::InvalidBody,
                                        QStringLiteral("Progressive-index body size is invalid"));
    }
    Reader reader(bytes);
    quint32 version = 0;
    quint32 flags = 0;
    quint64 streamId = 0;
    quint64 pageIndex = 0;
    quint64 firstRecordIndex = 0;
    quint64 indexedThrough = 0;
    quint32 recordCount = 0;
    quint32 reserved = 0;
    if (!reader.readMagic(progressiveMagic) || !reader.read32(&version)) {
        return invalidProgressiveDecode(AnalysisCacheBodyDecodeStatus::InvalidBody,
                                        QStringLiteral("Progressive-index body header is invalid"));
    }
    if (version != progressiveIndexFormatVersion()) {
        return invalidProgressiveDecode(
            AnalysisCacheBodyDecodeStatus::UnsupportedVersion,
            QStringLiteral("Progressive-index body version is unsupported"));
    }
    if (!reader.read32(&flags) || !reader.read64(&streamId) || !reader.read64(&pageIndex) ||
        !reader.read64(&firstRecordIndex) || !reader.read64(&indexedThrough) ||
        !reader.read32(&recordCount) || !reader.read32(&reserved) ||
        (flags & ~progressiveEndOfSourceFlag) != 0U || reserved != 0U ||
        recordCount > (AnalysisCachePayloadEnvelope::maximumPayloadBytes() -
                       progressiveHeaderSize) /
                          progressiveRecordSize ||
        reader.remaining() != static_cast<std::size_t>(recordCount) * progressiveRecordSize ||
        firstRecordIndex > std::numeric_limits<quint64>::max() - recordCount) {
        return invalidProgressiveDecode(
            AnalysisCacheBodyDecodeStatus::InvalidBody,
            QStringLiteral("Progressive-index body framing is invalid"));
    }
    if (streamId != expectedKey.streamId || pageIndex != expectedKey.pageIndex) {
        return invalidProgressiveDecode(
            AnalysisCacheBodyDecodeStatus::PageKeyMismatch,
            QStringLiteral("Progressive-index body page key does not match"));
    }

    H264ProgressiveIndexCachePage page;
    page.key = expectedKey;
    page.firstRecordIndex = firstRecordIndex;
    page.indexedThroughByteOffset = indexedThrough;
    page.endOfSource = (flags & progressiveEndOfSourceFlag) != 0U;
    page.records.reserve(recordCount);
    std::optional<quint64> previousEnd;
    for (quint32 index = 0; index < recordCount; ++index) {
        quint64 startOffset = 0;
        quint32 startLength = 0;
        quint32 recordReserved = 0;
        quint64 nalOffset = 0;
        quint64 nalLength = 0;
        quint64 trailingOffset = 0;
        quint64 trailingLength = 0;
        if (!reader.read64(&startOffset) || !reader.read32(&startLength) ||
            !reader.read32(&recordReserved) || recordReserved != 0U ||
            !reader.read64(&nalOffset) || !reader.read64(&nalLength) ||
            !reader.read64(&trailingOffset) || !reader.read64(&trailingLength)) {
            return invalidProgressiveDecode(
                AnalysisCacheBodyDecodeStatus::InvalidBody,
                QStringLiteral("Progressive-index cache record is truncated"));
        }
        auto record = makeIndexRecord(startOffset, startLength, nalOffset, nalLength,
                                      trailingOffset, trailingLength, indexedThrough,
                                      previousEnd);
        if (!record) {
            return invalidProgressiveDecode(
                AnalysisCacheBodyDecodeStatus::InvalidBody,
                QStringLiteral("Progressive-index cache record is invalid"));
        }
        previousEnd = record->trailingZero8BitsOffset + record->trailingZero8BitsLength;
        page.records.push_back(std::move(*record));
    }
    if (!reader.atEnd()) {
        return invalidProgressiveDecode(
            AnalysisCacheBodyDecodeStatus::InvalidBody,
            QStringLiteral("Progressive-index body has trailing bytes"));
    }
    return {AnalysisCacheBodyDecodeStatus::Decoded, std::move(page), {}};
}

AnalysisCacheBodyEncodeResult AnalysisCachePageBodyCodec::encodeMaterializedResult(
    const MaterializedResultCachePage& page) {
    if (page.nodes.size() > maximumMaterializedNodes ||
        page.nodes.size() > std::numeric_limits<quint32>::max()) {
        return invalidEncode(AnalysisCacheBodyEncodeStatus::PayloadTooLarge,
                             QStringLiteral("Materialized-result body exceeds one cache page"));
    }
    if (!validKey(page.key, core::PagedCachePageKind::MaterializedResult) || page.nodes.empty()) {
        return invalidEncode(AnalysisCacheBodyEncodeStatus::InvalidArgument,
                             QStringLiteral("Materialized-result cache page metadata is invalid"));
    }
    for (std::size_t index = 0; index < page.nodes.size(); ++index) {
        const MaterializedResultCacheNode& node = page.nodes[index];
        if (!nodeKindCode(node.spec.kind) || !storedValueKind(node.spec.value) ||
            (!stateCode(node.spec.state) &&
             node.spec.state != core::MaterializationState::Indexing &&
             node.spec.state != core::MaterializationState::WaitingDependency) ||
            !std::ranges::all_of(node.diagnostics, [](const core::ParseDiagnostic& diagnostic) {
                return diagnosticCode(diagnostic.code).has_value() &&
                       severityCode(diagnostic.severity).has_value();
            })) {
            return invalidEncode(
                AnalysisCacheBodyEncodeStatus::UnsupportedValue,
                QStringLiteral("Materialized-result node contains an unsupported value"));
        }
        if (!validNodeTopology(node) ||
            (index != 0U && node.id.value() != page.nodes[index - 1U].id.value() + 1U)) {
            return invalidEncode(AnalysisCacheBodyEncodeStatus::InvalidArgument,
                                 QStringLiteral("Materialized-result cache node is invalid"));
        }
    }

    Writer writer;
    writer.append(materializedMagic);
    writer.append32(materializedResultFormatVersion());
    writer.append32(0U);
    writer.append64(page.key.streamId);
    writer.append64(page.key.pageIndex);
    writer.append32(static_cast<quint32>(page.nodes.size()));
    writer.append32(0U);
    for (const MaterializedResultCacheNode& node : page.nodes) {
        const auto kind = nodeKindCode(node.spec.kind);
        const auto state = stateCode(node.spec.state);
        const auto valueKind = storedValueKind(node.spec.value);
        if (!kind || !state || !valueKind) {
            return invalidEncode(AnalysisCacheBodyEncodeStatus::UnsupportedValue,
                                 QStringLiteral("Materialized-result node type is unsupported"));
        }
        quint32 flags = node.spec.location ? nodeLocationFlag : 0U;
        flags |= node.spec.metadata.specification ? nodeSpecificationFlag : 0U;
        writer.append64(node.id.value());
        writer.append64(node.parentId ? node.parentId->value() : 0U);
        writer.append32(*kind);
        writer.append32(*state);
        writer.append32(static_cast<quint32>(*valueKind));
        writer.append32(flags);
        if (!writer.appendString(node.spec.name) ||
            !writer.appendString(node.spec.metadata.typeName) ||
            !writer.appendString(node.spec.metadata.description)) {
            return invalidEncode(AnalysisCacheBodyEncodeStatus::PayloadTooLarge,
                                 QStringLiteral("Materialized-result node text is too large"));
        }
        if (node.spec.metadata.specification &&
            (!writer.appendString(node.spec.metadata.specification->standard) ||
             !writer.appendString(node.spec.metadata.specification->clause))) {
            return invalidEncode(AnalysisCacheBodyEncodeStatus::PayloadTooLarge,
                                 QStringLiteral("Materialized-result specification is too large"));
        }
        if (!appendValue(&writer, *valueKind, node.spec.value)) {
            return invalidEncode(AnalysisCacheBodyEncodeStatus::PayloadTooLarge,
                                 QStringLiteral("Materialized-result value is too large"));
        }
        if (node.spec.location && !appendLocation(&writer, *node.spec.location)) {
            return invalidEncode(AnalysisCacheBodyEncodeStatus::InvalidArgument,
                                 QStringLiteral("Materialized-result node location is invalid"));
        }
        writer.append32(static_cast<quint32>(node.diagnostics.size()));
        for (const core::ParseDiagnostic& diagnostic : node.diagnostics) {
            const auto code = diagnosticCode(diagnostic.code);
            const auto severity = severityCode(diagnostic.severity);
            if (!code || !severity) {
                return invalidEncode(
                    AnalysisCacheBodyEncodeStatus::UnsupportedValue,
                    QStringLiteral("Materialized-result diagnostic is unsupported"));
            }
            writer.append32(*code);
            writer.append32(*severity);
            writer.append32(diagnostic.location ? diagnosticLocationFlag : 0U);
            writer.append32(0U);
            if (!writer.appendString(diagnostic.message) ||
                !writer.appendString(diagnostic.fieldPath)) {
                return invalidEncode(
                    AnalysisCacheBodyEncodeStatus::PayloadTooLarge,
                    QStringLiteral("Materialized-result diagnostic text is too large"));
            }
            if (diagnostic.location && !appendLocation(&writer, *diagnostic.location)) {
                return invalidEncode(
                    AnalysisCacheBodyEncodeStatus::InvalidArgument,
                    QStringLiteral("Materialized-result diagnostic location is invalid"));
            }
        }
        if (writer.overflowed()) {
            return invalidEncode(AnalysisCacheBodyEncodeStatus::PayloadTooLarge,
                                 QStringLiteral("Materialized-result body exceeds one cache page"));
        }
    }
    return {AnalysisCacheBodyEncodeStatus::Encoded, std::move(writer).take(), {}};
}

MaterializedResultCacheDecodeResult AnalysisCachePageBodyCodec::decodeMaterializedResult(
    const core::PagedCachePageKey& expectedKey, std::span<const std::byte> bytes) {
    if (!validKey(expectedKey, core::PagedCachePageKind::MaterializedResult) ||
        bytes.size() < materializedHeaderSize ||
        bytes.size() > AnalysisCachePayloadEnvelope::maximumPayloadBytes()) {
        return invalidMaterializedDecode(
            AnalysisCacheBodyDecodeStatus::InvalidBody,
            QStringLiteral("Materialized-result body size is invalid"));
    }
    Reader reader(bytes);
    quint32 version = 0;
    quint32 headerReserved = 0;
    quint64 streamId = 0;
    quint64 pageIndex = 0;
    quint32 nodeCount = 0;
    quint32 countReserved = 0;
    if (!reader.readMagic(materializedMagic) || !reader.read32(&version)) {
        return invalidMaterializedDecode(
            AnalysisCacheBodyDecodeStatus::InvalidBody,
            QStringLiteral("Materialized-result body header is invalid"));
    }
    if (version != materializedResultFormatVersion()) {
        return invalidMaterializedDecode(
            AnalysisCacheBodyDecodeStatus::UnsupportedVersion,
            QStringLiteral("Materialized-result body version is unsupported"));
    }
    if (!reader.read32(&headerReserved) || !reader.read64(&streamId) ||
        !reader.read64(&pageIndex) || !reader.read32(&nodeCount) ||
        !reader.read32(&countReserved) || headerReserved != 0U || countReserved != 0U ||
        nodeCount == 0U || nodeCount > maximumMaterializedNodes) {
        return invalidMaterializedDecode(
            AnalysisCacheBodyDecodeStatus::InvalidBody,
            QStringLiteral("Materialized-result body framing is invalid"));
    }
    if (streamId != expectedKey.streamId || pageIndex != expectedKey.pageIndex) {
        return invalidMaterializedDecode(
            AnalysisCacheBodyDecodeStatus::PageKeyMismatch,
            QStringLiteral("Materialized-result body page key does not match"));
    }

    MaterializedResultCachePage page;
    page.key = expectedKey;
    page.nodes.reserve(nodeCount);
    for (quint32 index = 0; index < nodeCount; ++index) {
        quint64 nodeId = 0;
        quint64 parentId = 0;
        quint32 kindValue = 0;
        quint32 stateValue = 0;
        quint32 valueKindValue = 0;
        quint32 flags = 0;
        if (!reader.read64(&nodeId) || !reader.read64(&parentId) ||
            !reader.read32(&kindValue) || !reader.read32(&stateValue) ||
            !reader.read32(&valueKindValue) || !reader.read32(&flags) ||
            (flags & ~(nodeLocationFlag | nodeSpecificationFlag)) != 0U) {
            return invalidMaterializedDecode(
                AnalysisCacheBodyDecodeStatus::InvalidBody,
                QStringLiteral("Materialized-result node header is invalid"));
        }
        const auto kind = nodeKind(kindValue);
        if (!kind || stateValue < 1U || stateValue > 7U ||
            valueKindValue > static_cast<quint32>(StoredValueKind::String)) {
            return invalidMaterializedDecode(
                AnalysisCacheBodyDecodeStatus::UnsupportedValue,
                QStringLiteral("Materialized-result node type is unsupported"));
        }
        if (isTransientStateCode(stateValue)) {
            return invalidMaterializedDecode(
                AnalysisCacheBodyDecodeStatus::InvalidBody,
                QStringLiteral("Materialized-result node state is transient"));
        }
        const auto state = stateFromCode(stateValue);
        if (!state) {
            return invalidMaterializedDecode(
                AnalysisCacheBodyDecodeStatus::UnsupportedValue,
                QStringLiteral("Materialized-result node state is unsupported"));
        }
        MaterializedResultCacheNode node;
        node.id = core::AnalysisNodeId(nodeId);
        if (parentId != 0U) {
            node.parentId = core::AnalysisNodeId(parentId);
        }
        node.spec.kind = *kind;
        node.spec.state = *state;
        if (!reader.readString(&node.spec.name) ||
            !reader.readString(&node.spec.metadata.typeName) ||
            !reader.readString(&node.spec.metadata.description)) {
            return invalidMaterializedDecode(
                AnalysisCacheBodyDecodeStatus::InvalidBody,
                QStringLiteral("Materialized-result node text is invalid"));
        }
        if ((flags & nodeSpecificationFlag) != 0U) {
            core::AnalysisSpecification specification;
            if (!reader.readString(&specification.standard) ||
                !reader.readString(&specification.clause)) {
                return invalidMaterializedDecode(
                    AnalysisCacheBodyDecodeStatus::InvalidBody,
                    QStringLiteral("Materialized-result specification is invalid"));
            }
            node.spec.metadata.specification = std::move(specification);
        }
        if (!readValue(&reader, static_cast<StoredValueKind>(valueKindValue),
                       &node.spec.value)) {
            return invalidMaterializedDecode(
                AnalysisCacheBodyDecodeStatus::InvalidBody,
                QStringLiteral("Materialized-result node value is invalid"));
        }
        if ((flags & nodeLocationFlag) != 0U) {
            node.spec.location = readLocation(&reader);
            if (!node.spec.location) {
                return invalidMaterializedDecode(
                    AnalysisCacheBodyDecodeStatus::InvalidBody,
                    QStringLiteral("Materialized-result node location is invalid"));
            }
        }
        quint32 diagnosticCount = 0;
        if (!reader.read32(&diagnosticCount) ||
            diagnosticCount > maximumNodeDiagnostics) {
            return invalidMaterializedDecode(
                AnalysisCacheBodyDecodeStatus::InvalidBody,
                QStringLiteral("Materialized-result diagnostic count is invalid"));
        }
        node.diagnostics.reserve(diagnosticCount);
        for (quint32 diagnosticIndex = 0; diagnosticIndex < diagnosticCount;
             ++diagnosticIndex) {
            quint32 codeValue = 0;
            quint32 severityValue = 0;
            quint32 diagnosticFlags = 0;
            quint32 diagnosticReserved = 0;
            if (!reader.read32(&codeValue) || !reader.read32(&severityValue) ||
                !reader.read32(&diagnosticFlags) || !reader.read32(&diagnosticReserved) ||
                (diagnosticFlags & ~diagnosticLocationFlag) != 0U ||
                diagnosticReserved != 0U) {
                return invalidMaterializedDecode(
                    AnalysisCacheBodyDecodeStatus::InvalidBody,
                    QStringLiteral("Materialized-result diagnostic header is invalid"));
            }
            const auto code = diagnosticFromCode(codeValue);
            const auto severity = severityFromCode(severityValue);
            if (!code || !severity) {
                return invalidMaterializedDecode(
                    AnalysisCacheBodyDecodeStatus::UnsupportedValue,
                    QStringLiteral("Materialized-result diagnostic type is unsupported"));
            }
            core::ParseDiagnostic diagnostic;
            diagnostic.code = *code;
            diagnostic.severity = *severity;
            if (!reader.readString(&diagnostic.message) ||
                !reader.readString(&diagnostic.fieldPath)) {
                return invalidMaterializedDecode(
                    AnalysisCacheBodyDecodeStatus::InvalidBody,
                    QStringLiteral("Materialized-result diagnostic text is invalid"));
            }
            if ((diagnosticFlags & diagnosticLocationFlag) != 0U) {
                diagnostic.location = readLocation(&reader);
                if (!diagnostic.location) {
                    return invalidMaterializedDecode(
                        AnalysisCacheBodyDecodeStatus::InvalidBody,
                        QStringLiteral("Materialized-result diagnostic location is invalid"));
                }
            }
            node.diagnostics.push_back(std::move(diagnostic));
        }
        if (!validNodeTopology(node) ||
            (!page.nodes.empty() && node.id.value() != page.nodes.back().id.value() + 1U)) {
            return invalidMaterializedDecode(
                AnalysisCacheBodyDecodeStatus::InvalidBody,
                QStringLiteral("Materialized-result node topology is invalid"));
        }
        page.nodes.push_back(std::move(node));
    }
    if (!reader.atEnd()) {
        return invalidMaterializedDecode(
            AnalysisCacheBodyDecodeStatus::InvalidBody,
            QStringLiteral("Materialized-result body has trailing bytes"));
    }
    return {AnalysisCacheBodyDecodeStatus::Decoded, std::move(page), {}};
}

} // namespace streamview::rules
