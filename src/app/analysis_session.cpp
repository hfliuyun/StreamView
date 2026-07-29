#include "analysis_session.h"

#include <streamview/core/version.h>
#include <streamview/rules/language_version.h>

#include <utility>

namespace streamview::app {

static_assert(core::SourcePager::pageSizeBytes() ==
              rules::h264AnnexBDetectionProbeSizeBytes());

AnalysisSession::AnalysisSession(std::unique_ptr<core::RandomAccessSource> source,
                                 QString sourcePath,
                                 core::SourcePage initialPage,
                                 rules::H264AnnexBDetectionResult formatDetection,
                                 rules::H264AnnexBAnalyzer analyzer,
                                 SessionUserState userState)
    : source_(std::move(source)), sourcePath_(std::move(sourcePath)),
      initialPage_(std::move(initialPage)), formatDetection_(std::move(formatDetection)),
      analyzer_(std::move(analyzer)), userState_(std::move(userState)) {}

std::unique_ptr<AnalysisSession> AnalysisSession::openFile(const QString& path,
                                                           QString* errorMessage) {
    auto source = core::FileSource::open(path, errorMessage);
    if (!source) {
        return nullptr;
    }
    return createPrepared(std::move(source), path, nullptr, {}, errorMessage);
}

std::unique_ptr<AnalysisSession>
AnalysisSession::create(std::unique_ptr<core::RandomAccessSource> source,
                        QString* errorMessage) {
    return createPrepared(std::move(source), {}, nullptr, {}, errorMessage);
}

std::unique_ptr<AnalysisSession>
AnalysisSession::createPrepared(std::unique_ptr<core::RandomAccessSource> source,
                                QString sourcePath,
                                const rules::RuleCatalogLookupResult* resolvedRule,
                                SessionUserState userState,
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

    auto formatDetection =
        rules::detectH264AnnexBCandidate(initialPage.bytes, source->sizeBytes());

    QString analyzerError;
    auto analyzer = resolvedRule == nullptr
                        ? rules::H264AnnexBAnalyzer::create(*source, &analyzerError)
                        : rules::H264AnnexBAnalyzer::create(*source, *resolvedRule,
                                                           &analyzerError);
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
        new AnalysisSession(std::move(source), std::move(sourcePath), std::move(initialPage),
                            std::move(formatDetection), std::move(*analyzer),
                            std::move(userState)));
}

AnalysisSessionRestoreResult
AnalysisSession::restoreSession(const QString& sessionPath,
                                const rules::RulePackageCatalog& catalog) {
    SessionDocumentLoadResult loaded = SessionDocument::load(sessionPath);
    if (!loaded.succeeded()) {
        return {AnalysisSessionRestoreStatus::SessionDocumentError,
                {}, loaded.status, std::nullopt, std::move(loaded.errorMessage)};
    }
    SessionDocument document = std::move(*loaded.document);

    QString errorMessage;
    auto source = core::FileSource::open(document.sourcePath(), &errorMessage);
    if (!source) {
        return {AnalysisSessionRestoreStatus::SourceOpenError,
                {}, std::nullopt, std::nullopt, std::move(errorMessage)};
    }
    const core::SourceFingerprintResult fingerprint = source->fingerprint();
    if (!fingerprint.succeeded()) {
        return {AnalysisSessionRestoreStatus::SourceFingerprintError,
                {}, std::nullopt, std::nullopt,
                fingerprint.errorMessage.isEmpty()
                    ? QStringLiteral("Unable to fingerprint the saved session source")
                    : fingerprint.errorMessage};
    }
    if (*fingerprint.fingerprint != document.sourceFingerprint()) {
        return {AnalysisSessionRestoreStatus::SourceFingerprintMismatch,
                {}, std::nullopt, std::nullopt,
                QStringLiteral("Saved session source fingerprint does not match the opened file")};
    }

    const rules::RuleEntryPointIdentity& requestedRule = document.ruleIdentity();
    rules::RuleCatalogLookupResult resolved = catalog.resolve(
        requestedRule.packageIdentity(), requestedRule.entryPointId(),
        rules::languageVersion(), core::version());
    if (!resolved.succeeded()) {
        return {AnalysisSessionRestoreStatus::RuleLookupError,
                {}, std::nullopt, resolved.status, std::move(resolved.errorMessage)};
    }

    auto session = createPrepared(std::move(source), document.sourcePath(), &resolved,
                                  document.userState(), &errorMessage);
    if (!session) {
        return {AnalysisSessionRestoreStatus::AnalyzerError,
                {}, std::nullopt, resolved.status, std::move(errorMessage)};
    }
    return {AnalysisSessionRestoreStatus::Restored, std::move(session), std::nullopt,
            resolved.status, {}};
}

bool AnalysisSession::saveSession(const QString& sessionPath,
                                  const SessionUserState& userState,
                                  QString* errorMessage) const {
    const auto* fileSource = dynamic_cast<const core::FileSource*>(source_.get());
    if (fileSource == nullptr || sourcePath_.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "Only a local file analysis session can be saved persistently");
        }
        return false;
    }
    core::SourceFingerprintResult fingerprint = fileSource->fingerprint();
    if (!fingerprint.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = fingerprint.errorMessage.isEmpty()
                                ? QStringLiteral("Unable to fingerprint the analysis source")
                                : std::move(fingerprint.errorMessage);
        }
        return false;
    }
    auto document = SessionDocument::create(sourcePath_, identity(),
                                            std::move(*fingerprint.fingerprint),
                                            ruleIdentity(), userState, errorMessage);
    return document && document->save(sessionPath, errorMessage);
}

rules::H264AnnexBAnalysisBatch AnalysisSession::analyzeBatch(
    std::size_t maximumRecords, quint64 maximumInspectedPositions) {
    return analyzer_.analyzeBatch(maximumRecords, maximumInspectedPositions);
}

} // namespace streamview::app
