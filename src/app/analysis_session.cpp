#include "analysis_session.h"

#include <streamview/core/version.h>
#include <streamview/rules/analysis_cache.h>
#include <streamview/rules/analysis_cache_payload.h>
#include <streamview/rules/language_version.h>

#include <chrono>
#include <utility>

namespace streamview::app {

static_assert(core::SourcePager::pageSizeBytes() ==
              rules::h264AnnexBDetectionProbeSizeBytes());

namespace {

struct SessionCacheSetup final {
    std::unique_ptr<rules::AnalysisCacheOwner> owner;
    AnalysisSessionCacheStatus status = AnalysisSessionCacheStatus::Disabled;
    QString errorMessage;
};

[[nodiscard]] SessionCacheSetup setupSessionCache(
    const core::RandomAccessSource& source,
    const rules::RuleEntryPointIdentity& ruleIdentity,
    AnalysisSessionCacheOptions cacheOptions,
    std::optional<core::SourceFingerprint> verifiedFingerprint) {
    if (!cacheOptions.enabled()) {
        return {};
    }

    SessionCacheSetup result;
    result.status = AnalysisSessionCacheStatus::Failed;
    if (!verifiedFingerprint) {
        const auto* fileSource = dynamic_cast<const core::FileSource*>(&source);
        if (fileSource == nullptr) {
            result.errorMessage =
                QStringLiteral("Only local file sessions can use the persistent cache");
            return result;
        }
        core::SourceFingerprintResult fingerprint = fileSource->fingerprint();
        if (!fingerprint.succeeded()) {
            result.errorMessage = fingerprint.errorMessage.isEmpty()
                                      ? QStringLiteral("Unable to fingerprint cache source")
                                      : std::move(fingerprint.errorMessage);
            return result;
        }
        verifiedFingerprint = std::move(fingerprint.fingerprint);
    }

    auto cacheNamespace = rules::AnalysisCacheNamespace::create(
        *verifiedFingerprint, ruleIdentity, {}, &result.errorMessage);
    if (!cacheNamespace) {
        return result;
    }
    auto started = rules::AnalysisCacheOwner::start(
        cacheOptions.databasePath, std::move(*cacheNamespace), cacheOptions.ownerOptions);
    if (!started.succeeded()) {
        result.errorMessage = std::move(started.errorMessage);
        return result;
    }
    result.owner = std::move(started.owner);
    result.status = AnalysisSessionCacheStatus::Active;
    return result;
}

} // namespace

AnalysisSession::AnalysisSession(std::unique_ptr<core::RandomAccessSource> source,
                                 QString sourcePath,
                                 core::SourcePage initialPage,
                                 rules::H264AnnexBDetectionResult formatDetection,
                                 rules::AacAdtsDetectionResult aacFormatDetection,
                                 std::variant<rules::H264AnnexBAnalyzer, rules::AacAdtsAnalyzer> analyzer,
                                 SessionUserState userState,
                                 std::unique_ptr<rules::AnalysisCacheOwner> cacheOwner,
                                 AnalysisSessionCacheStatus cacheStatus,
                                 QString cacheErrorMessage)
    : source_(std::move(source)), sourcePath_(std::move(sourcePath)),
      initialPage_(std::move(initialPage)), formatDetection_(std::move(formatDetection)),
      aacFormatDetection_(std::move(aacFormatDetection)),
      analyzer_(std::move(analyzer)), userState_(std::move(userState)),
      cacheOwner_(std::move(cacheOwner)), cacheStatus_(cacheStatus),
      cacheErrorMessage_(std::move(cacheErrorMessage)) {}

std::unique_ptr<AnalysisSession> AnalysisSession::openFile(const QString& path,
                                                           QString* errorMessage) {
    return openFile(path, {}, errorMessage);
}

std::unique_ptr<AnalysisSession>
AnalysisSession::openFile(const QString& path,
                          AnalysisSessionCacheOptions cacheOptions,
                          QString* errorMessage) {
    auto source = core::FileSource::open(path, errorMessage);
    if (!source) {
        return nullptr;
    }
    return createPrepared(std::move(source), path, nullptr, {}, std::move(cacheOptions),
                          std::nullopt, errorMessage);
}

std::unique_ptr<AnalysisSession>
AnalysisSession::create(std::unique_ptr<core::RandomAccessSource> source,
                        QString* errorMessage) {
    return createPrepared(std::move(source), {}, nullptr, {}, {}, std::nullopt,
                          errorMessage);
}

std::unique_ptr<AnalysisSession>
AnalysisSession::createPrepared(std::unique_ptr<core::RandomAccessSource> source,
                                QString sourcePath,
                                const rules::RuleCatalogLookupResult* resolvedRule,
                                SessionUserState userState,
                                AnalysisSessionCacheOptions cacheOptions,
                                std::optional<core::SourceFingerprint> verifiedFingerprint,
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

    rules::H264AnnexBDetectionResult formatDetection;
    rules::AacAdtsDetectionResult aacFormatDetection;
    std::optional<std::variant<rules::H264AnnexBAnalyzer, rules::AacAdtsAnalyzer>> analyzerVariant;
    QString analyzerError;

    if (resolvedRule != nullptr) {
        if (resolvedRule->package->manifest().packageId == QStringLiteral("org.streamview.aac") ||
            (resolvedRule->entryPoint &&
             resolvedRule->entryPoint->format == QStringLiteral("audio.aac.adts"))) {
            aacFormatDetection =
                rules::detectAacAdtsCandidate(initialPage.bytes, source->sizeBytes());
            auto aacAnalyzer =
                rules::AacAdtsAnalyzer::create(*source, *resolvedRule, &analyzerError);
            if (!aacAnalyzer) {
                if (errorMessage != nullptr) {
                    *errorMessage = analyzerError;
                }
                return nullptr;
            }
            analyzerVariant.emplace(std::move(*aacAnalyzer));
        } else {
            formatDetection =
                rules::detectH264AnnexBCandidate(initialPage.bytes, source->sizeBytes());
            auto h264Analyzer =
                rules::H264AnnexBAnalyzer::create(*source, *resolvedRule, &analyzerError);
            if (!h264Analyzer) {
                if (errorMessage != nullptr) {
                    *errorMessage = analyzerError;
                }
                return nullptr;
            }
            analyzerVariant.emplace(std::move(*h264Analyzer));
        }
    } else {
        formatDetection =
            rules::detectH264AnnexBCandidate(initialPage.bytes, source->sizeBytes());
        aacFormatDetection =
            rules::detectAacAdtsCandidate(initialPage.bytes, source->sizeBytes());

        const auto aacConf = aacFormatDetection.candidate
                                 ? std::optional(aacFormatDetection.candidate->confidence)
                                 : std::nullopt;
        const auto h264Conf = formatDetection.candidate
                                  ? std::optional(formatDetection.candidate->confidence)
                                  : std::nullopt;

        bool chooseAac = false;
        if (aacConf == rules::AacAdtsDetectionConfidence::Strong) {
            chooseAac = true;
        } else if (aacConf == rules::AacAdtsDetectionConfidence::Probable &&
                   h264Conf != rules::H264AnnexBDetectionConfidence::Strong &&
                   h264Conf != rules::H264AnnexBDetectionConfidence::Probable) {
            chooseAac = true;
        }

        if (chooseAac) {
            auto aacAnalyzer = rules::AacAdtsAnalyzer::create(*source, &analyzerError);
            if (!aacAnalyzer) {
                if (errorMessage != nullptr) {
                    *errorMessage = analyzerError;
                }
                return nullptr;
            }
            analyzerVariant.emplace(std::move(*aacAnalyzer));
        } else {
            auto h264Analyzer = rules::H264AnnexBAnalyzer::create(*source, &analyzerError);
            if (!h264Analyzer) {
                if (errorMessage != nullptr) {
                    *errorMessage = analyzerError;
                }
                return nullptr;
            }
            analyzerVariant.emplace(std::move(*h264Analyzer));
        }
    }

    const auto& ruleIdent = std::visit(
        [](const auto& a) -> const rules::RuleEntryPointIdentity& { return a.ruleIdentity(); },
        *analyzerVariant);
    SessionCacheSetup cacheSetup =
        setupSessionCache(*source, ruleIdent, std::move(cacheOptions),
                          std::move(verifiedFingerprint));

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return std::unique_ptr<AnalysisSession>(
        new AnalysisSession(std::move(source), std::move(sourcePath), std::move(initialPage),
                            std::move(formatDetection), std::move(aacFormatDetection),
                            std::move(*analyzerVariant), std::move(userState),
                            std::move(cacheSetup.owner), cacheSetup.status,
                            std::move(cacheSetup.errorMessage)));
}

AnalysisSessionRestoreResult
AnalysisSession::restoreSession(const QString& sessionPath,
                                const rules::RulePackageCatalog& catalog) {
    return restoreSession(sessionPath, catalog, {});
}

AnalysisSessionRestoreResult AnalysisSession::restoreSession(
    const QString& sessionPath,
    const rules::RulePackageCatalog& catalog,
    AnalysisSessionCacheOptions cacheOptions) {
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
    core::SourceFingerprintResult fingerprint = source->fingerprint();
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
                                  document.userState(), std::move(cacheOptions),
                                  std::move(fingerprint.fingerprint), &errorMessage);
    if (!session) {
        return {AnalysisSessionRestoreStatus::AnalyzerError,
                {}, std::nullopt, resolved.status, std::move(errorMessage)};
    }
    return {AnalysisSessionRestoreStatus::Restored, std::move(session), std::nullopt,
            resolved.status, {}};
}

void AnalysisSession::enableCache(AnalysisSessionCacheOptions cacheOptions) {
    if (!cacheOptions.enabled() || cacheStatus_ == AnalysisSessionCacheStatus::Active) {
        return;
    }
    if (analysisStarted_) {
        disableCache(QStringLiteral(
            "Analysis cache cannot be enabled after analysis has started"));
        return;
    }

    cacheOwner_.reset();
    pendingCacheWrites_.clear();
    SessionCacheSetup setup =
        setupSessionCache(*source_, ruleIdentity(), std::move(cacheOptions), std::nullopt);
    cacheOwner_ = std::move(setup.owner);
    cacheStatus_ = setup.status;
    cacheErrorMessage_ = std::move(setup.errorMessage);
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
    analysisStarted_ = true;
    pollCacheWrites();
    if (std::holds_alternative<rules::H264AnnexBAnalyzer>(analyzer_)) {
        auto& h264 = std::get<rules::H264AnnexBAnalyzer>(analyzer_);
        rules::H264AnnexBAnalysisBatch batch =
            h264.analyzeBatch(maximumRecords, maximumInspectedPositions);
        publishCachePages(batch);
        return batch;
    } else {
        auto& aac = std::get<rules::AacAdtsAnalyzer>(analyzer_);
        auto aacBatch = aac.analyzeBatch(maximumRecords, maximumInspectedPositions);
        rules::H264AnnexBAnalysisBatch batch;
        batch.status = static_cast<rules::H264AnnexBAnalysisStatus>(aacBatch.status);
        batch.nalUnitNodes = aacBatch.frameNodes;
        batch.errorMessage = aacBatch.errorMessage;
        publishCachePages(batch);
        return batch;
    }
}

void AnalysisSession::pollCacheWrites() {
    for (auto iterator = pendingCacheWrites_.begin(); iterator != pendingCacheWrites_.end();) {
        if (iterator->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++iterator;
            continue;
        }
        rules::AnalysisCacheOwnerWriteResult completed = iterator->get();
        iterator = pendingCacheWrites_.erase(iterator);
        if (!completed.succeeded() && cacheStatus_ == AnalysisSessionCacheStatus::Active) {
            disableCache(completed.errorMessage.isEmpty()
                             ? QStringLiteral("Analysis cache write failed")
                             : std::move(completed.errorMessage));
        }
    }
}

void AnalysisSession::disableCache(QString errorMessage) {
    cacheStatus_ = AnalysisSessionCacheStatus::Failed;
    cacheErrorMessage_ = errorMessage.isEmpty() ? QStringLiteral("Analysis cache is unavailable")
                                                : std::move(errorMessage);
}

void AnalysisSession::acceptCacheWrite(
    rules::AnalysisCacheOwnerWriteSubmission submission) {
    if (!submission.accepted()) {
        disableCache(submission.errorMessage);
        return;
    }
    pendingCacheWrites_.push_back(std::move(submission.completion));
}

void AnalysisSession::publishCachePages(const rules::H264AnnexBAnalysisBatch& batch) {
    if (cacheStatus_ != AnalysisSessionCacheStatus::Active || !cacheOwner_) {
        return;
    }
    if (batch.progressiveIndexUpdate) {
        rules::H264ProgressiveIndexCachePage page;
        page.key = {core::PagedCachePageKind::ProgressiveIndex, 0,
                    nextProgressiveCachePageIndex_};
        page.firstRecordIndex = batch.progressiveIndexUpdate->firstRecordIndex;
        page.indexedThroughByteOffset =
            batch.progressiveIndexUpdate->indexedThroughByteOffset;
        page.endOfSource = batch.progressiveIndexUpdate->endOfSource;
        page.records = batch.progressiveIndexUpdate->records;
        auto submitted = cacheOwner_->writeProgressiveIndex({std::move(page)});
        if (submitted.accepted()) {
            ++nextProgressiveCachePageIndex_;
        }
        acceptCacheWrite(std::move(submitted));
    }
    if (cacheStatus_ != AnalysisSessionCacheStatus::Active || !finished() ||
        materializedCacheSubmitted_) {
        return;
    }

    materializedCacheSubmitted_ = true;
    rules::MaterializedResultCacheExportResult exported =
        rules::exportMaterializedResultCachePages(tree(), 0);
    if (!exported.succeeded()) {
        disableCache(exported.errorMessage);
        return;
    }
    acceptCacheWrite(cacheOwner_->writeMaterializedResult(std::move(exported.pages)));
}

} // namespace streamview::app
