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

[[nodiscard]] AnalysisSessionNavigationStatus navigationStatus(
    rules::RuleCatalogLookupStatus status) noexcept {
    switch (status) {
    case rules::RuleCatalogLookupStatus::MissingContent:
        return AnalysisSessionNavigationStatus::MissingContent;
    case rules::RuleCatalogLookupStatus::VersionConflict:
        return AnalysisSessionNavigationStatus::VersionConflict;
    case rules::RuleCatalogLookupStatus::IncompatibleLanguage:
        return AnalysisSessionNavigationStatus::IncompatibleLanguage;
    case rules::RuleCatalogLookupStatus::IncompatibleEngine:
        return AnalysisSessionNavigationStatus::IncompatibleEngine;
    case rules::RuleCatalogLookupStatus::Found:
    case rules::RuleCatalogLookupStatus::UnknownEntryPoint:
        return AnalysisSessionNavigationStatus::InvalidRulePackage;
    }
    return AnalysisSessionNavigationStatus::InvalidRulePackage;
}

[[nodiscard]] AnalysisSessionNavigationStatus navigationStatus(
    rules::RuleExecutionStatus status) noexcept {
    switch (status) {
    case rules::RuleExecutionStatus::Unsupported:
        return AnalysisSessionNavigationStatus::Unsupported;
    case rules::RuleExecutionStatus::TruncatedSource:
        return AnalysisSessionNavigationStatus::TruncatedSource;
    case rules::RuleExecutionStatus::InvalidSyntax:
        return AnalysisSessionNavigationStatus::InvalidSyntax;
    case rules::RuleExecutionStatus::DependencyUnavailable:
        return AnalysisSessionNavigationStatus::DependencyUnavailable;
    case rules::RuleExecutionStatus::SourceError:
        return AnalysisSessionNavigationStatus::SourceError;
    case rules::RuleExecutionStatus::Cancelled:
        return AnalysisSessionNavigationStatus::Cancelled;
    case rules::RuleExecutionStatus::ResourceLimit:
        return AnalysisSessionNavigationStatus::ResourceLimit;
    case rules::RuleExecutionStatus::Materialized:
    case rules::RuleExecutionStatus::InvalidDefinition:
        return AnalysisSessionNavigationStatus::InvalidDefinition;
    }
    return AnalysisSessionNavigationStatus::InvalidDefinition;
}

[[nodiscard]] AnalysisSessionNavigationStatus navigationStatus(
    rules::DslExecutionStatus status) noexcept {
    switch (status) {
    case rules::DslExecutionStatus::Unsupported:
        return AnalysisSessionNavigationStatus::Unsupported;
    case rules::DslExecutionStatus::TruncatedSource:
        return AnalysisSessionNavigationStatus::TruncatedSource;
    case rules::DslExecutionStatus::InvalidSyntax:
        return AnalysisSessionNavigationStatus::InvalidSyntax;
    case rules::DslExecutionStatus::DependencyUnavailable:
        return AnalysisSessionNavigationStatus::DependencyUnavailable;
    case rules::DslExecutionStatus::SourceError:
        return AnalysisSessionNavigationStatus::SourceError;
    case rules::DslExecutionStatus::Cancelled:
        return AnalysisSessionNavigationStatus::Cancelled;
    case rules::DslExecutionStatus::ResourceLimit:
        return AnalysisSessionNavigationStatus::ResourceLimit;
    case rules::DslExecutionStatus::Materialized:
    case rules::DslExecutionStatus::InvalidDefinition:
        return AnalysisSessionNavigationStatus::InvalidDefinition;
    }
    return AnalysisSessionNavigationStatus::InvalidDefinition;
}

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
                                 rules::Mp4DetectionResult mp4FormatDetection,
                                 rules::FormatSelection formatSelection,
                                 std::variant<rules::H264AnnexBAnalyzer, rules::AacAdtsAnalyzer, rules::Mp4IsobmffAnalyzer> analyzer,
                                 SessionUserState userState,
                                 std::unique_ptr<rules::AnalysisCacheOwner> cacheOwner,
                                 AnalysisSessionCacheStatus cacheStatus,
                                 QString cacheErrorMessage)
    : source_(std::move(source)), sourcePath_(std::move(sourcePath)),
      initialPage_(std::move(initialPage)), formatDetection_(std::move(formatDetection)),
      aacFormatDetection_(std::move(aacFormatDetection)),
      mp4FormatDetection_(std::move(mp4FormatDetection)),
      formatSelection_(std::move(formatSelection)),
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
    rules::Mp4DetectionResult mp4FormatDetection;
    std::optional<std::variant<rules::H264AnnexBAnalyzer, rules::AacAdtsAnalyzer, rules::Mp4IsobmffAnalyzer>> analyzerVariant;
    QString analyzerError;

    rules::FormatSelection formatSelection;
    if (resolvedRule != nullptr) {
        if (resolvedRule->package->manifest().packageId == QStringLiteral("org.streamview.aac") ||
            (resolvedRule->entryPoint &&
             resolvedRule->entryPoint->format == QStringLiteral("audio.aac.adts"))) {
            formatSelection.format = rules::DetectedFormat::AacAdts;
            formatSelection.reason = rules::DetectedFormatReason::AacFrameChain;
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
        } else if (resolvedRule->package->manifest().packageId == QStringLiteral("org.streamview.mp4") ||
                   (resolvedRule->entryPoint &&
                    (resolvedRule->entryPoint->format == QStringLiteral("video.mp4") ||
                     resolvedRule->entryPoint->format == QStringLiteral("video/mp4")))) {
            formatSelection.format = rules::DetectedFormat::Mp4Isobmff;
            formatSelection.reason = rules::DetectedFormatReason::Mp4StructuralTiling;
            mp4FormatDetection =
                rules::detectMp4Candidate(initialPage.bytes, source->sizeBytes());
            auto mp4Analyzer =
                rules::Mp4IsobmffAnalyzer::create(*source, *resolvedRule, &analyzerError);
            if (!mp4Analyzer) {
                if (errorMessage != nullptr) {
                    *errorMessage = analyzerError;
                }
                return nullptr;
            }
            analyzerVariant.emplace(std::move(*mp4Analyzer));
        } else {
            formatSelection.format = rules::DetectedFormat::H264AnnexB;
            formatSelection.reason = rules::DetectedFormatReason::H264AnchoredStartCodes;
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
        mp4FormatDetection =
            rules::detectMp4Candidate(initialPage.bytes, source->sizeBytes());

        formatSelection = rules::selectFormatFromDetection(
            mp4FormatDetection, aacFormatDetection, formatDetection);
        const bool chooseMp4 = formatSelection.format == rules::DetectedFormat::Mp4Isobmff;
        const bool chooseAac = formatSelection.format == rules::DetectedFormat::AacAdts;

        if (chooseMp4) {
            auto mp4Analyzer = rules::Mp4IsobmffAnalyzer::create(*source, &analyzerError);
            if (mp4Analyzer.has_value()) {
                analyzerVariant.emplace(std::move(*mp4Analyzer));
            } else {
                if (errorMessage != nullptr) {
                    *errorMessage = analyzerError.isEmpty()
                                        ? QStringLiteral("No installed package matches format: video/mp4")
                                        : analyzerError;
                }
                return nullptr;
            }
        } else if (chooseAac) {
            auto aacAnalyzer = rules::AacAdtsAnalyzer::create(*source, &analyzerError);
            if (aacAnalyzer.has_value()) {
                analyzerVariant.emplace(std::move(*aacAnalyzer));
            } else {
                // If AAC analyzer creation fails (e.g. no bundled rule package yet),
                // fall back cleanly to the existing H.264/unknown source path.
                auto h264Analyzer = rules::H264AnnexBAnalyzer::create(*source, &analyzerError);
                if (!h264Analyzer) {
                    if (errorMessage != nullptr) {
                        *errorMessage = analyzerError;
                    }
                    return nullptr;
                }
                analyzerVariant.emplace(std::move(*h264Analyzer));
            }
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
                            std::move(mp4FormatDetection), std::move(formatSelection),
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
    if (sourcePath_.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "Only a local file analysis session can be saved persistently");
        }
        return false;
    }
    const auto* fileSource = dynamic_cast<const core::FileSource*>(source_.get());
    if (fileSource == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Session persistence requires a file-backed source");
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

AnalysisBatchResult AnalysisSession::analyzeBatch(
    std::size_t maximumRecords,
    quint64 maximumInspectedPositions,
    quint64 maximumMappedBytes) {
    analysisStarted_ = true;
    pollCacheWrites();

    AnalysisBatchResult result;

    if (std::holds_alternative<rules::H264AnnexBAnalyzer>(analyzer_)) {
        auto& h264 = std::get<rules::H264AnnexBAnalyzer>(analyzer_);
        const auto batch =
            h264.analyzeBatch(maximumRecords, maximumInspectedPositions, maximumMappedBytes);
        switch (batch.status) {
        case rules::H264AnnexBAnalysisStatus::InProgress:
            result.status = AnalysisBatchStatus::InProgress;
            break;
        case rules::H264AnnexBAnalysisStatus::Complete:
            result.status = AnalysisBatchStatus::Complete;
            break;
        case rules::H264AnnexBAnalysisStatus::Cancelled:
            result.status = AnalysisBatchStatus::Cancelled;
            break;
        case rules::H264AnnexBAnalysisStatus::SourceError:
            result.status = AnalysisBatchStatus::SourceError;
            break;
        case rules::H264AnnexBAnalysisStatus::ResourceLimit:
            result.status = AnalysisBatchStatus::ResourceLimit;
            break;
        case rules::H264AnnexBAnalysisStatus::InvalidRule:
            result.status = AnalysisBatchStatus::InvalidRule;
            break;
        case rules::H264AnnexBAnalysisStatus::InvalidBatchSize:
            result.status = AnalysisBatchStatus::InvalidBatchSize;
            break;
        }
        result.topLevelNodes = batch.nalUnitNodes;
        result.errorMessage = batch.errorMessage;
        publishCachePages(batch);
    } else if (std::holds_alternative<rules::AacAdtsAnalyzer>(analyzer_)) {
        auto& aac = std::get<rules::AacAdtsAnalyzer>(analyzer_);
        const auto batch = aac.analyzeBatch(maximumRecords, maximumInspectedPositions);
        switch (batch.status) {
        case rules::AacAdtsAnalysisStatus::InProgress:
            result.status = AnalysisBatchStatus::InProgress;
            break;
        case rules::AacAdtsAnalysisStatus::Complete:
            result.status = AnalysisBatchStatus::Complete;
            break;
        case rules::AacAdtsAnalysisStatus::Cancelled:
            result.status = AnalysisBatchStatus::Cancelled;
            break;
        case rules::AacAdtsAnalysisStatus::SourceError:
            result.status = AnalysisBatchStatus::SourceError;
            break;
        case rules::AacAdtsAnalysisStatus::ResourceLimit:
            result.status = AnalysisBatchStatus::ResourceLimit;
            break;
        case rules::AacAdtsAnalysisStatus::InvalidRule:
            result.status = AnalysisBatchStatus::InvalidRule;
            break;
        case rules::AacAdtsAnalysisStatus::InvalidBatchSize:
            result.status = AnalysisBatchStatus::InvalidBatchSize;
            break;
        }
        result.topLevelNodes = batch.frameNodes;
        result.errorMessage = batch.errorMessage;
        if (cacheStatus_ == AnalysisSessionCacheStatus::Active && cacheOwner_ &&
            finished() && !materializedCacheSubmitted_) {
            materializedCacheSubmitted_ = true;
            rules::MaterializedResultCacheExportResult exported =
                rules::exportMaterializedResultCachePages(tree(), 0);
            if (!exported.succeeded()) {
                disableCache(exported.errorMessage);
            } else {
                acceptCacheWrite(cacheOwner_->writeMaterializedResult(std::move(exported.pages)));
            }
        }
    } else {
        auto& mp4 = std::get<rules::Mp4IsobmffAnalyzer>(analyzer_);
        const auto batch = mp4.analyzeBatch(maximumRecords, maximumInspectedPositions);
        switch (batch.status) {
        case rules::Mp4IsobmffAnalysisStatus::InProgress:
            result.status = AnalysisBatchStatus::InProgress;
            break;
        case rules::Mp4IsobmffAnalysisStatus::Complete:
            result.status = AnalysisBatchStatus::Complete;
            break;
        case rules::Mp4IsobmffAnalysisStatus::Cancelled:
            result.status = AnalysisBatchStatus::Cancelled;
            break;
        case rules::Mp4IsobmffAnalysisStatus::SourceError:
            result.status = AnalysisBatchStatus::SourceError;
            break;
        case rules::Mp4IsobmffAnalysisStatus::ResourceLimit:
            result.status = AnalysisBatchStatus::ResourceLimit;
            break;
        case rules::Mp4IsobmffAnalysisStatus::InvalidRule:
            result.status = AnalysisBatchStatus::InvalidRule;
            break;
        case rules::Mp4IsobmffAnalysisStatus::InvalidBatchSize:
            result.status = AnalysisBatchStatus::InvalidBatchSize;
            break;
        }
        result.topLevelNodes = batch.boxNodes;
        result.errorMessage = batch.errorMessage;
        if (cacheStatus_ == AnalysisSessionCacheStatus::Active && cacheOwner_ &&
            finished() && !materializedCacheSubmitted_) {
            materializedCacheSubmitted_ = true;
            rules::MaterializedResultCacheExportResult exported =
                rules::exportMaterializedResultCachePages(tree(), 0);
            if (!exported.succeeded()) {
                disableCache(exported.errorMessage);
            } else {
                acceptCacheWrite(cacheOwner_->writeMaterializedResult(std::move(exported.pages)));
            }
        }
    }

    lastBatchStatus_ = result.status;
    return result;
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

namespace {

[[nodiscard]] AnalysisSessionSampleStatus sampleStatus(
    rules::Mp4SampleTableExtractionStatus status) noexcept {
    switch (status) {
    case rules::Mp4SampleTableExtractionStatus::NoTracks:
        return AnalysisSessionSampleStatus::NoTracks;
    case rules::Mp4SampleTableExtractionStatus::IncompleteTrack:
        return AnalysisSessionSampleStatus::InconsistentTables;
    case rules::Mp4SampleTableExtractionStatus::UnsupportedTables:
        return AnalysisSessionSampleStatus::UnsupportedTables;
    case rules::Mp4SampleTableExtractionStatus::SourceError:
        return AnalysisSessionSampleStatus::SourceError;
    case rules::Mp4SampleTableExtractionStatus::ResourceLimit:
        return AnalysisSessionSampleStatus::ResourceLimit;
    case rules::Mp4SampleTableExtractionStatus::Cancelled:
        return AnalysisSessionSampleStatus::Cancelled;
    case rules::Mp4SampleTableExtractionStatus::Extracted:
        break;
    }
    return AnalysisSessionSampleStatus::InconsistentTables;
}

[[nodiscard]] AnalysisSessionSampleStatus sampleStatus(
    rules::Mp4SampleTableIndexStatus status) noexcept {
    switch (status) {
    case rules::Mp4SampleTableIndexStatus::InconsistentTables:
    case rules::Mp4SampleTableIndexStatus::ArithmeticOverflow:
        return AnalysisSessionSampleStatus::InconsistentTables;
    case rules::Mp4SampleTableIndexStatus::UnsupportedTables:
        return AnalysisSessionSampleStatus::UnsupportedTables;
    case rules::Mp4SampleTableIndexStatus::OutOfSourceRange:
    case rules::Mp4SampleTableIndexStatus::InvalidRequest:
        return AnalysisSessionSampleStatus::InvalidSampleRange;
    case rules::Mp4SampleTableIndexStatus::ResourceLimit:
        return AnalysisSessionSampleStatus::ResourceLimit;
    case rules::Mp4SampleTableIndexStatus::SourceError:
        return AnalysisSessionSampleStatus::SourceError;
    case rules::Mp4SampleTableIndexStatus::Cancelled:
        return AnalysisSessionSampleStatus::Cancelled;
    case rules::Mp4SampleTableIndexStatus::Built:
        break;
    }
    return AnalysisSessionSampleStatus::InconsistentTables;
}

[[nodiscard]] AnalysisSessionSampleStatus sampleStatus(
    rules::RuleCatalogLookupStatus status) noexcept {
    switch (status) {
    case rules::RuleCatalogLookupStatus::MissingContent:
    case rules::RuleCatalogLookupStatus::VersionConflict:
    case rules::RuleCatalogLookupStatus::IncompatibleLanguage:
    case rules::RuleCatalogLookupStatus::IncompatibleEngine:
        return AnalysisSessionSampleStatus::UnsupportedFormat;
    case rules::RuleCatalogLookupStatus::UnknownEntryPoint:
    case rules::RuleCatalogLookupStatus::Found:
        break;
    }
    return AnalysisSessionSampleStatus::InvalidRulePackage;
}

[[nodiscard]] AnalysisSessionSampleStatus sampleStatus(
    rules::SamplePayloadRunStatus status) noexcept {
    switch (status) {
    case rules::SamplePayloadRunStatus::UnsupportedFraming:
        return AnalysisSessionSampleStatus::UnsupportedTables;
    case rules::SamplePayloadRunStatus::DependencyUnavailable:
        return AnalysisSessionSampleStatus::DependencyUnavailable;
    case rules::SamplePayloadRunStatus::TruncatedSample:
    case rules::SamplePayloadRunStatus::InvalidUnitLength:
    case rules::SamplePayloadRunStatus::InvalidRequest:
        return AnalysisSessionSampleStatus::InvalidSampleRange;
    case rules::SamplePayloadRunStatus::SourceError:
        return AnalysisSessionSampleStatus::SourceError;
    case rules::SamplePayloadRunStatus::Cancelled:
        return AnalysisSessionSampleStatus::Cancelled;
    case rules::SamplePayloadRunStatus::ResourceLimit:
        return AnalysisSessionSampleStatus::ResourceLimit;
    case rules::SamplePayloadRunStatus::Executed:
        break;
    }
    return AnalysisSessionSampleStatus::InvalidSampleRange;
}

/// Maps the container's own terminal batch status onto a sample status, so a
/// caller learns why the tree is unusable instead of being told analysis has
/// merely not finished.
[[nodiscard]] AnalysisSessionSampleStatus containerFailureStatus(
    AnalysisBatchStatus batchStatus) noexcept {
    switch (batchStatus) {
    case AnalysisBatchStatus::SourceError:
        return AnalysisSessionSampleStatus::SourceError;
    case AnalysisBatchStatus::Cancelled:
        return AnalysisSessionSampleStatus::Cancelled;
    case AnalysisBatchStatus::ResourceLimit:
        return AnalysisSessionSampleStatus::ResourceLimit;
    case AnalysisBatchStatus::InvalidRule:
    case AnalysisBatchStatus::InvalidBatchSize:
        return AnalysisSessionSampleStatus::InvalidRulePackage;
    case AnalysisBatchStatus::InProgress:
    case AnalysisBatchStatus::Complete:
        break;
    }
    return AnalysisSessionSampleStatus::NotAnalyzed;
}

[[nodiscard]] QString containerFailureMessage(core::MaterializationState rootState) {
    if (rootState == core::MaterializationState::Cancelled) {
        return QStringLiteral("Container analysis was cancelled, so its track list is incomplete");
    }
    if (rootState == core::MaterializationState::Invalid) {
        return QStringLiteral(
            "Container analysis ended before the movie was complete, so its track list "
            "cannot be trusted");
    }
    return QStringLiteral("Analysis has not finished producing the container tree yet");
}

struct CompiledSampleEntry final {
    std::optional<rules::DslTypedProgram> program;
    AnalysisSessionSampleStatus status = AnalysisSessionSampleStatus::InvalidDefinition;
    QString errorMessage;
};

[[nodiscard]] CompiledSampleEntry compileSampleEntry(
    const rules::RulePackage& package, const rules::RulePackageEntryPoint& entryPoint) {
    CompiledSampleEntry compiled;
    const QByteArray* sourceBytes = package.fileContents(entryPoint.sourcePath);
    if (sourceBytes == nullptr) {
        compiled.status = AnalysisSessionSampleStatus::InvalidRulePackage;
        compiled.errorMessage =
            QStringLiteral("Rule file not found in package: %1").arg(entryPoint.sourcePath);
        return compiled;
    }
    const auto parsed = rules::DslParser::parse(QString::fromUtf8(*sourceBytes));
    if (!parsed.succeeded()) {
        compiled.errorMessage =
            QStringLiteral("Failed to parse rule file: %1").arg(entryPoint.sourcePath);
        return compiled;
    }
    auto compileResult = rules::DslCompiler::compileForTarget(parsed.program, entryPoint.target);
    if (!compileResult.succeeded() || !compileResult.program.has_value()) {
        compiled.errorMessage = QStringLiteral("Failed to compile rule file for target '%1'")
                                    .arg(entryPoint.target.value_or(QString()));
        return compiled;
    }
    if (compileResult.program->entry.kind != rules::DslEntryKind::Structure) {
        compiled.errorMessage = QStringLiteral("Compiled entry is not a structure entry");
        return compiled;
    }
    compiled.program = std::move(*compileResult.program);
    compiled.status = AnalysisSessionSampleStatus::Available;
    return compiled;
}

} // namespace

AnalysisSessionSampleStatus AnalysisSession::ensureSampleIndices(QString* errorMessage) {
    const auto fail = [errorMessage](AnalysisSessionSampleStatus status, QString message) {
        if (errorMessage != nullptr) {
            *errorMessage = std::move(message);
        }
        return status;
    };

    if (sampleIndicesBuilt_) {
        return trackIndices_.empty()
                   ? fail(AnalysisSessionSampleStatus::NoTracks,
                          QStringLiteral("The container declares no indexable track"))
                   : AnalysisSessionSampleStatus::Available;
    }

    if (!std::holds_alternative<rules::Mp4IsobmffAnalyzer>(analyzer_)) {
        return fail(AnalysisSessionSampleStatus::UnsupportedSource,
                    QStringLiteral("The active source is not a container carrying sample tables"));
    }
    if (!analysisStarted_) {
        return fail(AnalysisSessionSampleStatus::NotAnalyzed,
                    QStringLiteral("Analysis has not produced a container tree yet"));
    }

    const auto& mp4 = std::get<rules::Mp4IsobmffAnalyzer>(analyzer_);
    const auto& containerTree = mp4.tree();
    const auto root = containerTree.node(containerTree.rootId());
    if (!root) {
        return fail(AnalysisSessionSampleStatus::NotAnalyzed,
                    QStringLiteral("The container tree has no root"));
    }

    // The invariant is "index only a complete tree", and the root's state is what
    // expresses it: the analyzer materializes the root only on its Complete path,
    // while every terminal failure marks the root Invalid or Cancelled instead.
    // `finished()` cannot stand in for this, because it is equally true of an
    // abandoned analysis. Indexing a partial tree would be permanent: the
    // extractor skips a `trak` that is missing its `stbl` rather than failing, so
    // a truncated movie yields a short track list, and `sampleIndicesBuilt_`
    // latches with no invalidation path.
    if (root->state() != core::MaterializationState::Materialized) {
        return fail(containerFailureStatus(lastBatchStatus_),
                    containerFailureMessage(root->state()));
    }

    // The extractor consumes only `boxNodes`, and the analyzer publishes every
    // top-level box as a child of the tree root. Rebuilding the batch from those
    // children therefore reproduces the extractor's whole input without the
    // session having to retain analyzer batch state across calls.
    rules::Mp4IsobmffAnalysisBatch batch;
    batch.status = rules::Mp4IsobmffAnalysisStatus::Complete;
    batch.boxNodes = root->children();

    rules::Mp4SampleTableExtractionRequest extractionRequest;
    extractionRequest.sourceSizeBytes = source_->sizeBytes();

    auto extraction = rules::Mp4SampleTableExtractor::extract(mp4, batch, extractionRequest);
    if (!extraction.extracted()) {
        return fail(sampleStatus(extraction.status), std::move(extraction.errorMessage));
    }

    // Built into a local first, so a track that fails validation leaves the
    // session's cache untouched rather than half-populated.
    std::vector<rules::Mp4SampleTableIndex> indices;
    indices.reserve(extraction.tracks.size());
    for (auto& track : extraction.tracks) {
        auto built = rules::Mp4SampleTableIndex::build(std::move(track.tables),
                                                       std::move(track.readers));
        if (!built.succeeded()) {
            return fail(sampleStatus(built.status), std::move(built.errorMessage));
        }
        indices.push_back(std::move(*built.index));
    }

    trackIndices_ = std::move(indices);
    sampleIndicesBuilt_ = true;
    return AnalysisSessionSampleStatus::Available;
}

rules::Mp4SampleTableIndex* AnalysisSession::findTrackIndex(quint32 trackId) noexcept {
    for (auto& index : trackIndices_) {
        if (index.tables().trackId == trackId) {
            return &index;
        }
    }
    return nullptr;
}

AnalysisSessionTracksResult AnalysisSession::tracks() {
    AnalysisSessionTracksResult result;
    const auto status = ensureSampleIndices(&result.errorMessage);
    if (status != AnalysisSessionSampleStatus::Available) {
        result.status = status;
        return result;
    }

    result.tracks.reserve(trackIndices_.size());
    for (const auto& index : trackIndices_) {
        const auto& tables = index.tables();
        AnalysisSessionTrack track;
        track.trackId = tables.trackId;
        track.timescale = tables.timescale;
        track.sampleCount = index.sampleCount();
        // Taken from the first sample description the rules layer bound, never
        // derived here: which entry declares a format is the extractor's finding.
        if (!tables.sampleDescriptions.empty()) {
            track.targetFormat = tables.sampleDescriptions.front().targetFormat;
        }
        result.tracks.push_back(std::move(track));
    }
    result.status = AnalysisSessionSampleStatus::Available;
    result.errorMessage.clear();
    return result;
}

AnalysisSessionSamplePageResult
AnalysisSession::samplesForTrack(const AnalysisSessionSamplePageRequest& request) {
    AnalysisSessionSamplePageResult result;
    const auto status = ensureSampleIndices(&result.errorMessage);
    if (status != AnalysisSessionSampleStatus::Available) {
        result.status = status;
        return result;
    }

    auto* index = findTrackIndex(request.trackId);
    if (index == nullptr) {
        result.status = AnalysisSessionSampleStatus::TrackNotFound;
        result.errorMessage =
            QStringLiteral("Track %1 is not one of the indexed tracks").arg(request.trackId);
        return result;
    }

    rules::Mp4SamplePageRequest pageRequest;
    pageRequest.pageIndex = request.pageIndex;
    pageRequest.pageSize = request.pageSize;
    pageRequest.cancellation = request.cancellation;

    auto page = index->descriptorPage(pageRequest);
    if (!page.built()) {
        result.status = sampleStatus(page.status);
        result.errorMessage = std::move(page.errorMessage);
        return result;
    }

    result.status = AnalysisSessionSampleStatus::Available;
    result.descriptors = std::move(page.descriptors);
    result.firstSampleIndex = page.firstSampleIndex;
    result.sampleCount = index->sampleCount();
    result.errorMessage.clear();
    return result;
}

AnalysisSessionEnterSampleResult AnalysisSession::enterSample(
    quint32 trackId,
    quint64 sampleIndex,
    const rules::RulePackageCatalog& catalog,
    const rules::StructuralExecutionOptions& options) {
    AnalysisSessionEnterSampleResult result;

    const auto indexStatus = ensureSampleIndices(&result.errorMessage);
    if (indexStatus != AnalysisSessionSampleStatus::Available) {
        result.status = indexStatus;
        return result;
    }

    auto* index = findTrackIndex(trackId);
    if (index == nullptr) {
        result.status = AnalysisSessionSampleStatus::TrackNotFound;
        result.errorMessage =
            QStringLiteral("Track %1 is not one of the indexed tracks").arg(trackId);
        return result;
    }
    if (sampleIndex >= index->sampleCount()) {
        result.status = AnalysisSessionSampleStatus::SampleNotFound;
        result.errorMessage = QStringLiteral("Sample %1 is beyond the track's %2 samples")
                                  .arg(sampleIndex)
                                  .arg(index->sampleCount());
        return result;
    }

    rules::Mp4SamplePageRequest pageRequest;
    pageRequest.pageIndex = sampleIndex;
    pageRequest.pageSize = 1;
    pageRequest.cancellation = options.cancellation;

    auto page = index->descriptorPage(pageRequest);
    if (!page.built() || page.descriptors.size() != 1) {
        result.status = page.built() ? AnalysisSessionSampleStatus::InconsistentTables
                                    : sampleStatus(page.status);
        result.errorMessage = page.errorMessage.isEmpty()
                                  ? QStringLiteral("Sample %1 produced no descriptor")
                                        .arg(sampleIndex)
                                  : std::move(page.errorMessage);
        return result;
    }
    const core::SampleDescriptor sample = std::move(page.descriptors.front());

    const auto sampleMapping =
        core::SourceMapping::create(core::LogicalViewId(1), sample.sourceSpans);
    if (!sampleMapping) {
        result.status = AnalysisSessionSampleStatus::InvalidSampleRange;
        result.errorMessage =
            QStringLiteral("Sample %1 does not project onto a usable source range")
                .arg(sampleIndex);
        return result;
    }

    const auto* binding = index->sampleDescription(sample.sampleDescriptionIndex);
    if (binding == nullptr) {
        result.status = AnalysisSessionSampleStatus::InconsistentTables;
        result.errorMessage = QStringLiteral("Sample description %1 is not declared by the track")
                                  .arg(sample.sampleDescriptionIndex);
        return result;
    }
    if (binding->targetFormat.trimmed().isEmpty()) {
        result.status = AnalysisSessionSampleStatus::MissingTargetFormat;
        result.errorMessage =
            QStringLiteral("The sample entry declares no target format, so no rule claims it");
        return result;
    }
    const QString targetFormat = binding->targetFormat;

    const auto lookup =
        catalog.resolveByFormat(targetFormat, rules::languageVersion(), core::version());
    if (!lookup.succeeded()) {
        result.status = sampleStatus(lookup.status);
        result.errorMessage = lookup.errorMessage;
        return result;
    }
    const auto package = lookup.package;
    if (!package || !lookup.entryPoint.has_value()) {
        result.status = AnalysisSessionSampleStatus::InvalidRulePackage;
        result.errorMessage = QStringLiteral("Resolved package or entrypoint is invalid");
        return result;
    }
    const auto entryPoint = *lookup.entryPoint;

    auto compiled = compileSampleEntry(*package, entryPoint);
    if (!compiled.program.has_value()) {
        result.status = compiled.status == AnalysisSessionSampleStatus::Available
                            ? AnalysisSessionSampleStatus::InvalidDefinition
                            : compiled.status;
        result.errorMessage = std::move(compiled.errorMessage);
        return result;
    }
    const rules::DslTypedProgram& program = *compiled.program;

    const auto entryIdentity =
        rules::RuleEntryPointIdentity::create(package->identity(), entryPoint.id);
    if (!entryIdentity) {
        result.status = AnalysisSessionSampleStatus::InvalidRulePackage;
        result.errorMessage = QStringLiteral("Resolved entrypoint identity is invalid");
        return result;
    }
    const QString sessionKey = entryIdentity->toString();

    // Presence of a length prefix decides framing, so no codec name is consulted
    // here (ADR-0105 section 4 and the P5j-3b binding contract).
    const bool lengthPrefixed = binding->prefixLengthBytes.has_value();

    // Opaque samples reference a configuration instead of decoding units, and an
    // absent configuration is reported rather than guessed at.
    std::optional<core::AnalysisNodeId> configurationNode;
    QString configurationSummary;
    if (!lengthPrefixed) {
        if (!binding->configurationNode.has_value()) {
            result.status = AnalysisSessionSampleStatus::DependencyUnavailable;
            result.errorMessage = QStringLiteral(
                "The sample entry binds no configuration for its opaque access units");
            return result;
        }
        configurationNode = binding->configurationNode;

        const auto& containerTree = tree();
        const auto configNode = containerTree.node(*configurationNode);
        if (!configNode || !configNode->location().has_value() ||
            configNode->location()->sourceSpans().empty()) {
            result.status = AnalysisSessionSampleStatus::DependencyUnavailable;
            result.errorMessage =
                QStringLiteral("The bound configuration has no readable source location");
            return result;
        }
        const auto configMapping = core::SourceMapping::create(
            core::LogicalViewId(1), configNode->location()->sourceSpans());
        if (!configMapping) {
            result.status = AnalysisSessionSampleStatus::DependencyUnavailable;
            result.errorMessage =
                QStringLiteral("Failed to map the bound configuration's source span");
            return result;
        }

        const auto configExecution =
            rules::StructuralEntryRunner::execute(*source_, *configMapping, program, options);
        if (!configExecution.succeeded()) {
            result.status = AnalysisSessionSampleStatus::DependencyUnavailable;
            result.errorMessage = configExecution.execution.errorMessage.isEmpty()
                                      ? QStringLiteral("The bound configuration could not be decoded")
                                      : configExecution.execution.errorMessage;
            return result;
        }

        // The summary is resolved through the target-format keyed registry, so
        // the session hands over the format string and never names a provider.
        rules::ConfigurationSummaryRequest summaryRequest;
        summaryRequest.configurationTree = configExecution.tree.get();
        summaryRequest.configurationNode =
            configExecution.execution.structureNode.value_or(configExecution.tree->rootId());
        summaryRequest.targetFormat = targetFormat;
        const auto summary = rules::bundledConfigurationSummaryRegistry().format(summaryRequest);
        if (summary.status == rules::ConfigurationSummaryStatus::DependencyUnavailable) {
            result.status = AnalysisSessionSampleStatus::DependencyUnavailable;
            result.errorMessage = summary.errorMessage.isEmpty()
                                      ? QStringLiteral("The bound configuration is incomplete")
                                      : summary.errorMessage;
            return result;
        }
        // An unclaimed format still yields a navigable envelope: the summary is
        // presentation detail, while the access unit's boundaries are not.
        configurationSummary = summary.formatted() ? summary.summary : QString();
    } else if (program.payloadDispatchHeaderStructureIndex() != program.entry.targetIndex) {
        // A length-prefixed unit is executed as header plus dispatched payload, so
        // an entry without that shape cannot decode this track's units.
        result.status = AnalysisSessionSampleStatus::InvalidDefinition;
        result.errorMessage =
            QStringLiteral("Rule entry for '%1' does not dispatch a unit payload")
                .arg(targetFormat);
        return result;
    }

    // Sessions are cached per entrypoint identity so parameter sets published by
    // one sample stay visible to the next sample of the same track.
    auto sessionIterator = sampleSessions_.find(sessionKey);
    if (sessionIterator == sampleSessions_.end()) {
        auto treeOpt = core::AnalysisTree::create(sessionKey);
        if (!treeOpt) {
            result.status = AnalysisSessionSampleStatus::InvalidDefinition;
            result.errorMessage = QStringLiteral("Failed to create the sample analysis tree");
            return result;
        }
        SubFormatSession created;
        created.tree = std::make_shared<core::AnalysisTree>(std::move(*treeOpt));
        if (lengthPrefixed) {
            created.ruleSession = std::make_unique<rules::RuleExecutionSession>(program, 1);
        }
        sessionIterator = sampleSessions_.emplace(sessionKey, std::move(created)).first;
    }
    auto& sampleSession = sessionIterator->second;

    // Snapshot before executing: a failed sample must leave the sample tree, the
    // navigation stack, and the cached index exactly as they were.
    auto treeSnapshot = sampleSession.tree->snapshot();

    rules::SamplePayloadRunRequest runRequest;
    runRequest.source = source_.get();
    runRequest.sample = &sample;
    runRequest.framing = lengthPrefixed ? rules::SamplePayloadFraming::LengthPrefixed
                                        : rules::SamplePayloadFraming::OpaqueAccessUnit;
    if (lengthPrefixed) {
        runRequest.prefixLengthBytes = *binding->prefixLengthBytes;
        runRequest.executionSession = sampleSession.ruleSession.get();
        runRequest.unitStructureIndex = program.entry.targetIndex;
        runRequest.transformRegistry = &rules::bundledPayloadTransformRegistry();
    }
    runRequest.tree = sampleSession.tree.get();
    runRequest.parentId = sampleSession.tree->rootId();
    runRequest.configurationNode = configurationNode;
    runRequest.configurationSummary = configurationSummary;
    runRequest.sampleNodeName = QStringLiteral("Sample %1").arg(sampleIndex);
    runRequest.options.limits = options.limits;
    runRequest.options.cancellation = options.cancellation;

    auto run = rules::SamplePayloadRunner::run(runRequest);
    if (!run.executed() || !run.sampleNode.has_value()) {
        (void)sampleSession.tree->restore(std::move(treeSnapshot));
        result.status = sampleStatus(run.status);
        result.errorMessage = std::move(run.errorMessage);
        return result;
    }

    std::vector<core::AnalysisNodeId> unitNodeIds;
    unitNodeIds.reserve(run.units.size());
    for (const auto& unit : run.units) {
        if (unit.unitNode.has_value()) {
            unitNodeIds.push_back(*unit.unitNode);
        }
    }

    NavigationFrame frame{
        .parentTargetNodeId = *run.sampleNode,
        .targetFormat = targetFormat,
        .package = package,
        .entryPoint = entryPoint,
        .sourceMapping = *sampleMapping,
        .tree = sampleSession.tree,
        .childRootStructureNodeId = *run.sampleNode,
        .sample = SampleNavigationFrame{.trackId = trackId,
                                        .sampleIndex = sampleIndex,
                                        .sample = sample,
                                        .targetFormat = targetFormat},
    };
    navigationStack_.push_back(std::move(frame));

    result.status = AnalysisSessionSampleStatus::Available;
    result.sampleNodeId = run.sampleNode;
    result.tree = sampleSession.tree;
    result.sample = sample;
    result.unitNodeIds = std::move(unitNodeIds);
    result.errorMessage.clear();
    return result;
}

AnalysisSessionReturnResult AnalysisSession::returnToParent() {
    if (navigationStack_.empty()) {
        return {AnalysisSessionReturnStatus::AtRoot, std::nullopt, &activeTree(), std::nullopt};
    }
    const auto parentTargetNodeId = navigationStack_.back().parentTargetNodeId;
    // Moved out before the pop so a sample frame's descriptor survives the
    // unwind: it is what lets the caller restore the sample row and highlight
    // without re-paging the track.
    auto restoredSample = std::move(navigationStack_.back().sample);
    navigationStack_.pop_back();
    return {AnalysisSessionReturnStatus::Returned, parentTargetNodeId, &activeTree(),
            std::move(restoredSample)};
}

AnalysisSessionNavigationResult AnalysisSession::enterChildFormat(
    core::AnalysisNodeId nodeId,
    const rules::RulePackageCatalog& catalog,
    const rules::StructuralExecutionOptions& options) {
    AnalysisSessionNavigationResult result;

    const auto& currentActiveTree = activeTree();
    const auto targetNode = currentActiveTree.node(nodeId);
    if (!targetNode) {
        result.status = AnalysisSessionNavigationStatus::NodeNotFound;
        result.errorMessage =
            QStringLiteral("Target node was not found in the active analysis tree");
        return result;
    }

    const auto& targetFormat = targetNode->metadata().targetFormat;
    if (!targetFormat.has_value() || targetFormat->trimmed().isEmpty()) {
        result.status = AnalysisSessionNavigationStatus::MissingTargetFormat;
        result.errorMessage = QStringLiteral("Target node does not specify a target format");
        return result;
    }

    const auto location = targetNode->location();
    if (!location || location->sourceSpans().empty()) {
        result.status = AnalysisSessionNavigationStatus::InvalidTargetLocation;
        result.errorMessage = QStringLiteral("Target node has no source location");
        return result;
    }

    if ((location->logicalRange().start().bitOffset() % 8U) != 0 ||
        (location->logicalRange().bitLength() % 8U) != 0) {
        result.status = AnalysisSessionNavigationStatus::InvalidTargetLocation;
        result.errorMessage = QStringLiteral("Target node source location is not byte-aligned");
        return result;
    }

    for (const auto& span : location->sourceSpans()) {
        if (span.start().bitOffsetInByte() != 0 || (span.bitLength() % 8U) != 0) {
            result.status = AnalysisSessionNavigationStatus::InvalidTargetLocation;
            result.errorMessage =
                QStringLiteral("Target node source location spans are not byte-aligned");
            return result;
        }
    }

    if (location->logicalRange().bitLength() == 0) {
        result.status = AnalysisSessionNavigationStatus::InvalidTargetLocation;
        result.errorMessage = QStringLiteral("Target node source location length is zero");
        return result;
    }

    const auto mappingOpt = core::SourceMapping::create(core::LogicalViewId(1), location->sourceSpans());
    if (!mappingOpt) {
        result.status = AnalysisSessionNavigationStatus::InvalidTargetLocation;
        result.errorMessage =
            QStringLiteral("Failed to construct source mapping for target node");
        return result;
    }
    const core::SourceMapping sourceMapping = *mappingOpt;

    // Catalog resolution
    const auto lookup = catalog.resolveByFormat(
        *targetFormat, rules::languageVersion(), core::version());
    if (!lookup.succeeded()) {
        result.status = navigationStatus(lookup.status);
        result.errorMessage = lookup.errorMessage;
        return result;
    }

    const auto package = lookup.package;
    if (!package || !lookup.entryPoint.has_value()) {
        result.status = AnalysisSessionNavigationStatus::InvalidRulePackage;
        result.errorMessage = QStringLiteral("Resolved package or entrypoint is invalid");
        return result;
    }
    const auto entryPoint = *lookup.entryPoint;

    const QByteArray* sourceBytes = package->fileContents(entryPoint.sourcePath);
    if (!sourceBytes) {
        result.status = AnalysisSessionNavigationStatus::InvalidRulePackage;
        result.errorMessage =
            QStringLiteral("Rule file not found in package: %1").arg(entryPoint.sourcePath);
        return result;
    }

    const auto parseResult = rules::DslParser::parse(QString::fromUtf8(*sourceBytes));
    if (!parseResult.succeeded()) {
        result.status = AnalysisSessionNavigationStatus::InvalidDefinition;
        result.errorMessage =
            QStringLiteral("Failed to parse rule file: %1").arg(entryPoint.sourcePath);
        return result;
    }

    const auto compileResult =
        rules::DslCompiler::compileForTarget(parseResult.program, entryPoint.target);
    if (!compileResult.succeeded() || !compileResult.program.has_value()) {
        result.status = AnalysisSessionNavigationStatus::InvalidDefinition;
        result.errorMessage =
            QStringLiteral("Failed to compile rule file for target '%1'").arg(entryPoint.target.value_or(QString()));
        return result;
    }

    const rules::DslTypedProgram& program = *compileResult.program;
    if (program.entry.kind != rules::DslEntryKind::Structure) {
        result.status = AnalysisSessionNavigationStatus::InvalidDefinition;
        result.errorMessage = QStringLiteral("Compiled entry is not a structure entry");
        return result;
    }

    const bool isCompound =
        program.payloadDispatch.has_value() &&
        (program.payloadDispatchHeaderStructureIndex() == program.entry.targetIndex);

    if (isCompound) {
        const auto entryIdentity = rules::RuleEntryPointIdentity::create(
            package->identity(), entryPoint.id);
        if (!entryIdentity) {
            result.status = AnalysisSessionNavigationStatus::InvalidRulePackage;
            result.errorMessage = QStringLiteral("Resolved entrypoint identity is invalid");
            return result;
        }
        const QString sessionKey = entryIdentity->toString();
        auto it = subFormatSessions_.find(sessionKey);
        if (it == subFormatSessions_.end()) {
            auto treeOpt = core::AnalysisTree::create(sessionKey);
            if (!treeOpt) {
                result.status = AnalysisSessionNavigationStatus::InvalidDefinition;
                result.errorMessage = QStringLiteral("Failed to create sub-format analysis tree");
                return result;
            }
            SubFormatSession subSession;
            subSession.tree = std::make_shared<core::AnalysisTree>(std::move(*treeOpt));
            subSession.ruleSession =
                std::make_unique<rules::RuleExecutionSession>(program, 1);
            it = subFormatSessions_.emplace(sessionKey, std::move(subSession)).first;
        }

        auto& subSession = it->second;
        auto treeSnapshot = subSession.tree->snapshot();

        rules::CompoundRuleExecutionRequest request;
        request.source = source_.get();
        request.headerMapping = &sourceMapping;
        request.headerStructureIndex = program.entry.targetIndex;
        request.payloadMapping = &sourceMapping;
        request.payloadLogicalStart = 0;
        request.transformRegistry = &rules::bundledPayloadTransformRegistry();
        request.tree = subSession.tree.get();
        request.parentId = subSession.tree->rootId();
        request.enclosingSourceSpans = location->sourceSpans();
        request.options.limits = options.limits;
        request.options.cancellation = options.cancellation;
        request.requireExactConsumption = true;
        request.autoDispatchPayload = true;

        const auto execRes = subSession.ruleSession->runCompound(request);
        if (!execRes.materialized()) {
            (void)subSession.tree->restore(std::move(treeSnapshot));
            result.status = navigationStatus(execRes.status);
            result.errorMessage = execRes.errorMessage;
            return result;
        }

        const auto childRootId =
            execRes.execution.headerNodeId.value_or(subSession.tree->rootId());
        NavigationFrame frame{
            .parentTargetNodeId = nodeId,
            .targetFormat = *targetFormat,
            .package = package,
            .entryPoint = entryPoint,
            .sourceMapping = sourceMapping,
            .tree = subSession.tree,
            .childRootStructureNodeId = childRootId,
        };

        navigationStack_.push_back(std::move(frame));

        result.status = AnalysisSessionNavigationStatus::Entered;
        result.childRootStructureNodeId = childRootId;
        result.tree = subSession.tree;
        return result;
    }

    // Standard non-compound structure
    const auto execRes =
        rules::StructuralEntryRunner::execute(*source_, sourceMapping, program, options);
    if (!execRes.succeeded()) {
        result.status = navigationStatus(execRes.execution.status);
        result.errorMessage = execRes.execution.errorMessage;
        return result;
    }

    const auto childRootId = execRes.execution.structureNode.value_or(execRes.tree->rootId());
    NavigationFrame frame{
        .parentTargetNodeId = nodeId,
        .targetFormat = *targetFormat,
        .package = package,
        .entryPoint = entryPoint,
        .sourceMapping = sourceMapping,
        .tree = execRes.tree,
        .childRootStructureNodeId = childRootId,
    };

    navigationStack_.push_back(std::move(frame));

    result.status = AnalysisSessionNavigationStatus::Entered;
    result.childRootStructureNodeId = childRootId;
    result.tree = execRes.tree;
    return result;
}

} // namespace streamview::app
