#include <streamview/core/source_pager.h>

#include <algorithm>
#include <limits>
#include <span>

namespace streamview::core {

quint64 SourcePager::pageCount() const noexcept {
    const quint64 sourceSize = source_->sizeBytes();
    return (sourceSize / pageSizeBytes()) + ((sourceSize % pageSizeBytes()) != 0U ? 1U : 0U);
}

SourcePage SourcePager::loadPage(quint64 pageIndex) const {
    SourcePage page;
    page.pageIndex = pageIndex;

    if (pageIndex > std::numeric_limits<quint64>::max() / pageSizeBytes()) {
        page.errorMessage = QStringLiteral("Page index exceeds the source coordinate range");
        return page;
    }

    page.byteOffset = pageIndex * pageSizeBytes();
    const quint64 sourceSize = source_->sizeBytes();
    if (page.byteOffset >= sourceSize) {
        page.status = SourcePageStatus::EndOfSource;
        return page;
    }

    const quint64 available = sourceSize - page.byteOffset;
    const quint64 requested = std::min(pageSizeBytes(), available);
    page.bytes.resize(static_cast<std::size_t>(requested));

    const auto readResult = source_->readAt(page.byteOffset, std::span(page.bytes));
    if (readResult.bytesRead > page.bytes.size()) {
        page.bytes.clear();
        page.errorMessage = QStringLiteral("Source reported more bytes than the page request");
        return page;
    }
    page.bytes.resize(readResult.bytesRead);

    if (readResult.status == SourceReadStatus::Error) {
        page.errorMessage = readResult.errorMessage.isEmpty()
                                ? QStringLiteral("Source page read failed")
                                : readResult.errorMessage;
        return page;
    }

    if (readResult.status == SourceReadStatus::Complete && readResult.bytesRead != requested) {
        page.errorMessage = QStringLiteral("Source completed a page read without all bytes");
        return page;
    }

    const bool reachedDeclaredEnd = page.byteOffset + readResult.bytesRead >= sourceSize;
    if (readResult.status == SourceReadStatus::EndOfSource && !reachedDeclaredEnd) {
        page.errorMessage = QStringLiteral("Source ended before the declared source end");
        return page;
    }
    if (readResult.status == SourceReadStatus::EndOfSource || reachedDeclaredEnd) {
        page.status = SourcePageStatus::EndOfSource;
    } else {
        page.status = SourcePageStatus::Ready;
    }
    return page;
}

} // namespace streamview::core
