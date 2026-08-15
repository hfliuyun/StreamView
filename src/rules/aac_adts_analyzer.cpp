#include <streamview/rules/aac_adts_analyzer.h>

#include <streamview/core/coordinates.h>

#include <QCryptographicHash>

#include <limits>

namespace streamview::rules {

namespace {

[[nodiscard]] RuleEntryPointIdentity defaultAacAdtsIdentity() {
    const QByteArray hash = QCryptographicHash::hash(
        QByteArrayLiteral("org.streamview.aac.default"), QCryptographicHash::Sha256);
    auto pkgId = RulePackageIdentity::create(
        QStringLiteral("org.streamview.aac"),
        QStringLiteral("0.1.0"),
        hash);
    auto identity = RuleEntryPointIdentity::create(
        *pkgId, QStringLiteral("adts-stream"));
    return *identity;
}

[[nodiscard]] std::optional<core::FieldLocation>
makeLocation(core::SourceSpan span, quint64& nextViewId) {
    if (nextViewId == 0) {
        return std::nullopt;
    }
    const core::LogicalViewId viewId(nextViewId);
    nextViewId = nextViewId == std::numeric_limits<quint64>::max() ? 0 : nextViewId + 1;
    const auto mapping = core::SourceMapping::create(viewId, {span});
    if (!mapping) {
        return std::nullopt;
    }
    const auto range = core::LogicalRange::create(
        core::LogicalBitAddress(viewId, 0), mapping->logicalBitLength());
    return range ? mapping->locate(*range) : std::nullopt;
}

} // namespace

QString aacAdtsRuleSource(QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return QStringLiteral(
        "struct AdtsFrameHeader { bits<12> syncword @equals(4095); }\n"
        "@index(progressive) sequence<AdtsFrameHeader> frames = scan(adts_frame);\n"
        "entry frames;\n");
}

RulePackageLoadResult loadAacAdtsRulePackage() {
    RulePackageLoadResult result;
    result.status = RulePackageLoadStatus::InvalidManifest;
    result.errorMessage = QStringLiteral("Official AAC rule package will be bundled in Phase 4 Task T16");
    return result;
}

std::optional<AacAdtsAnalyzer>
AacAdtsAnalyzer::create(const core::RandomAccessSource& source,
                       QString* errorMessage,
                       std::optional<core::CancellationToken> cancellation) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return AacAdtsAnalyzer(source, defaultAacAdtsIdentity(), std::move(cancellation));
}

std::optional<AacAdtsAnalyzer>
AacAdtsAnalyzer::create(const core::RandomAccessSource& source,
                       const RuleCatalogLookupResult& resolvedRule,
                       QString* errorMessage,
                       std::optional<core::CancellationToken> cancellation) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    RuleEntryPointIdentity identity = defaultAacAdtsIdentity();
    if (resolvedRule.succeeded()) {
        auto ruleId = RuleEntryPointIdentity::create(
            resolvedRule.package->identity(), resolvedRule.entryPoint->id, errorMessage);
        if (!ruleId) {
            return std::nullopt;
        }
        identity = std::move(*ruleId);
    }
    return AacAdtsAnalyzer(source, std::move(identity), std::move(cancellation));
}

AacAdtsAnalyzer::AacAdtsAnalyzer(const core::RandomAccessSource& source,
                                 RuleEntryPointIdentity ruleIdentity,
                                 std::optional<core::CancellationToken> cancellation)
    : scanner_(source, std::move(cancellation)),
      tree_(*core::AnalysisTree::create(QStringLiteral("adts_stream"))),
      ruleIdentity_(std::move(ruleIdentity)),
      nextViewId_(1),
      terminal_(false) {}

AacAdtsAnalysisBatch AacAdtsAnalyzer::analyzeBatch(
    std::size_t maximumRecords,
    quint64 maximumInspectedPositions) {
    AacAdtsAnalysisBatch result;
    if (maximumRecords == 0 || maximumInspectedPositions == 0) {
        result.status = AacAdtsAnalysisStatus::InvalidBatchSize;
        result.errorMessage =
            QStringLiteral("Maximum records and inspected positions must be greater than zero");
        return result;
    }

    if (terminal_) {
        result.status = AacAdtsAnalysisStatus::Complete;
        return result;
    }

    const auto scanBatch = scanner_.scanBatch(maximumRecords, maximumInspectedPositions);
    switch (scanBatch.status) {
    case AacAdtsScanStatus::InProgress:
        result.status = AacAdtsAnalysisStatus::InProgress;
        break;
    case AacAdtsScanStatus::Complete:
        result.status = AacAdtsAnalysisStatus::Complete;
        terminal_ = true;
        break;
    case AacAdtsScanStatus::Cancelled:
        result.status = AacAdtsAnalysisStatus::Cancelled;
        return result;
    case AacAdtsScanStatus::SourceError:
        result.status = AacAdtsAnalysisStatus::SourceError;
        result.errorMessage = scanBatch.errorMessage;
        terminal_ = true;
        return result;
    case AacAdtsScanStatus::InvalidBatchSize:
        result.status = AacAdtsAnalysisStatus::InvalidBatchSize;
        result.errorMessage = scanBatch.errorMessage;
        return result;
    }

    for (const auto& record : scanBatch.records) {
        if (!record.frameSpan) {
            continue;
        }
        core::AnalysisNodeSpec frameSpec;
        frameSpec.kind = core::AnalysisNodeKind::Structure;
        frameSpec.name = QStringLiteral("adts_frame");
        frameSpec.state = core::MaterializationState::Indexing;
        frameSpec.location = makeLocation(*record.frameSpan, nextViewId_);

        const auto frameId = tree_.appendChild(tree_.rootId(), std::move(frameSpec));
        if (!frameId.has_value()) {
            continue;
        }
        result.frameNodes.push_back(*frameId);

        if (record.headerSpan) {
            core::AnalysisNodeSpec headerSpec;
            headerSpec.kind = core::AnalysisNodeKind::Structure;
            headerSpec.name = QStringLiteral("header");
            headerSpec.state = core::MaterializationState::Materialized;
            headerSpec.location = makeLocation(*record.headerSpan, nextViewId_);
            (void)tree_.appendChild(*frameId, std::move(headerSpec));
        }

        if (record.payloadSpan && record.payloadLength > 0) {
            core::AnalysisNodeSpec payloadSpec;
            payloadSpec.kind = core::AnalysisNodeKind::Structure;
            payloadSpec.name = QStringLiteral("raw_data_block");
            payloadSpec.state = core::MaterializationState::Materialized;
            payloadSpec.location = makeLocation(*record.payloadSpan, nextViewId_);
            (void)tree_.appendChild(*frameId, std::move(payloadSpec));
        }

        (void)tree_.transition(*frameId, core::MaterializationState::Materialized);
    }

    if (terminal_) {
        (void)tree_.transition(tree_.rootId(), core::MaterializationState::Materialized);
    }

    return result;
}

bool AacAdtsAnalyzer::resumeAfterCancellation(
    std::optional<core::CancellationToken> cancellation,
    QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    scanner_.replaceCancellationToken(std::move(cancellation));
    return true;
}

} // namespace streamview::rules
