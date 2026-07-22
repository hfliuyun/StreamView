#include "analysis_session.h"

#include <utility>

namespace streamview::app {

AnalysisSession::AnalysisSession(std::unique_ptr<core::RandomAccessSource> source,
                                 core::SourcePage initialPage,
                                 rules::H264AnnexBAnalyzer analyzer)
    : source_(std::move(source)), initialPage_(std::move(initialPage)),
      analyzer_(std::move(analyzer)) {}

std::unique_ptr<AnalysisSession> AnalysisSession::openFile(const QString& path,
                                                           QString* errorMessage) {
    auto source = core::FileSource::open(path, errorMessage);
    if (!source) {
        return nullptr;
    }
    return create(std::move(source), errorMessage);
}

std::unique_ptr<AnalysisSession>
AnalysisSession::create(std::unique_ptr<core::RandomAccessSource> source,
                        QString* errorMessage) {
    if (!source) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("No media source was provided");
        }
        return nullptr;
    }

    core::SourcePage initialPage;
    initialPage.status = core::SourcePageStatus::EndOfSource;
    const core::SourcePager pager(*source);
    if (pager.pageCount() > 0) {
        initialPage = pager.loadPage(0);
    }
    if (!initialPage.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = initialPage.errorMessage;
        }
        return nullptr;
    }

    QString analyzerError;
    auto analyzer = rules::H264AnnexBAnalyzer::create(*source, &analyzerError);
    if (!analyzer) {
        if (errorMessage != nullptr) {
            *errorMessage = analyzerError;
        }
        return nullptr;
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return std::unique_ptr<AnalysisSession>(
        new AnalysisSession(std::move(source), std::move(initialPage), std::move(*analyzer)));
}

rules::H264AnnexBAnalysisBatch AnalysisSession::analyzeBatch(std::size_t maximumRecords) {
    return analyzer_.analyzeBatch(maximumRecords);
}

} // namespace streamview::app
