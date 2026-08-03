#include <streamview/rules/h264_annex_b_analyzer.h>

#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/version.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/language_version.h>
#include <streamview/rules/rule_package.h>

#include <QFile>
#include <QIODevice>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

static void initializeStreamViewOfficialRules() {
    Q_INIT_RESOURCE(streamview_official_rules);
}

namespace streamview::rules {

namespace {

[[nodiscard]] H264AnnexBAnalysisStatus analysisStatus(StartCodeScanStatus status) noexcept {
    switch (status) {
    case StartCodeScanStatus::InProgress:
        return H264AnnexBAnalysisStatus::InProgress;
    case StartCodeScanStatus::Complete:
        return H264AnnexBAnalysisStatus::Complete;
    case StartCodeScanStatus::Cancelled:
        return H264AnnexBAnalysisStatus::Cancelled;
    case StartCodeScanStatus::SourceError:
        return H264AnnexBAnalysisStatus::SourceError;
    case StartCodeScanStatus::InvalidBatchSize:
        return H264AnnexBAnalysisStatus::InvalidBatchSize;
    }
    return H264AnnexBAnalysisStatus::InvalidRule;
}

[[nodiscard]] std::optional<QByteArray> readBundledPackageFile(const QString& path,
                                                               QString* errorMessage) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to open bundled rule file %1: %2")
                                .arg(path, file.errorString());
        }
        return std::nullopt;
    }
    QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to read bundled rule file %1: %2")
                                .arg(path, file.errorString());
        }
        return std::nullopt;
    }
    return contents;
}

[[nodiscard]] core::DiagnosticCode
diagnosticCode(H264AnnexBAnalysisStatus status) noexcept {
    switch (status) {
    case H264AnnexBAnalysisStatus::Cancelled:
        return core::DiagnosticCode::Cancelled;
    case H264AnnexBAnalysisStatus::SourceError:
        return core::DiagnosticCode::SourceError;
    case H264AnnexBAnalysisStatus::ResourceLimit:
        return core::DiagnosticCode::ResourceLimit;
    case H264AnnexBAnalysisStatus::InProgress:
    case H264AnnexBAnalysisStatus::Complete:
    case H264AnnexBAnalysisStatus::InvalidBatchSize:
    case H264AnnexBAnalysisStatus::InvalidRule:
        return core::DiagnosticCode::InvalidSyntax;
    }
    return core::DiagnosticCode::InvalidSyntax;
}

[[nodiscard]] bool hasExtensionHeader(quint64 nalUnitType) noexcept {
    return nalUnitType == 14U || nalUnitType == 20U || nalUnitType == 21U;
}

[[nodiscard]] QString issueMessage(H264EbspRbspIssueKind kind) {
    switch (kind) {
    case H264EbspRbspIssueKind::Prohibited000000:
        return QStringLiteral("H.264 EBSP contains prohibited byte sequence 00 00 00");
    case H264EbspRbspIssueKind::Prohibited000001:
        return QStringLiteral("H.264 EBSP contains prohibited byte sequence 00 00 01");
    case H264EbspRbspIssueKind::Prohibited000002:
        return QStringLiteral("H.264 EBSP contains prohibited byte sequence 00 00 02");
    case H264EbspRbspIssueKind::Prohibited000003xx:
        return QStringLiteral(
            "H.264 EBSP contains prohibited byte sequence 00 00 03 xx with xx greater than 03");
    case H264EbspRbspIssueKind::FinalZeroByte:
        return QStringLiteral("H.264 EBSP final byte must not be 00");
    }
    return QStringLiteral("H.264 EBSP violates byte-sequence conformance requirements");
}

[[nodiscard]] H264AnnexBAnalysisStatus
analysisStatus(H264EbspRbspMapStatus status) noexcept {
    switch (status) {
    case H264EbspRbspMapStatus::InProgress:
        return H264AnnexBAnalysisStatus::InProgress;
    case H264EbspRbspMapStatus::Complete:
        return H264AnnexBAnalysisStatus::Complete;
    case H264EbspRbspMapStatus::Cancelled:
        return H264AnnexBAnalysisStatus::Cancelled;
    case H264EbspRbspMapStatus::SourceError:
        return H264AnnexBAnalysisStatus::SourceError;
    case H264EbspRbspMapStatus::InvalidBatchSize:
        return H264AnnexBAnalysisStatus::InvalidBatchSize;
    case H264EbspRbspMapStatus::ResourceLimit:
        return H264AnnexBAnalysisStatus::ResourceLimit;
    case H264EbspRbspMapStatus::InvalidInput:
        return H264AnnexBAnalysisStatus::InvalidRule;
    }
    return H264AnnexBAnalysisStatus::InvalidRule;
}

[[nodiscard]] QString mappingFailureMessage(H264EbspRbspMapStatus status,
                                            const QString& mapperMessage) {
    if (!mapperMessage.isEmpty()) {
        return mapperMessage;
    }
    switch (status) {
    case H264EbspRbspMapStatus::Cancelled:
        return QStringLiteral("H.264 EBSP-to-RBSP mapping was cancelled");
    case H264EbspRbspMapStatus::SourceError:
        return QStringLiteral("Unable to read source while mapping H.264 EBSP");
    case H264EbspRbspMapStatus::ResourceLimit:
        return QStringLiteral("H.264 EBSP-to-RBSP mapping exceeded a resource limit");
    case H264EbspRbspMapStatus::InvalidInput:
    case H264EbspRbspMapStatus::InvalidBatchSize:
        return QStringLiteral("H.264 EBSP-to-RBSP mapper rejected analyzer input");
    case H264EbspRbspMapStatus::InProgress:
    case H264EbspRbspMapStatus::Complete:
        break;
    }
    return QStringLiteral("H.264 EBSP-to-RBSP mapping failed");
}

} // namespace

RulePackageLoadResult loadH264AnnexBRulePackage() {
    initializeStreamViewOfficialRules();
    QString errorMessage;
    const QString root = QStringLiteral(":/streamview/rules/org.streamview.h264/");
    auto manifest = readBundledPackageFile(root + QStringLiteral("rule.toml"), &errorMessage);
    auto source = readBundledPackageFile(root + QStringLiteral("src/h264_annex_b.svfmt"),
                                         &errorMessage);
    if (!manifest || !source) {
        return {RulePackageLoadStatus::InvalidTree, std::nullopt, std::move(errorMessage)};
    }
    RulePackageLoadResult loaded = RulePackage::fromFiles({
        {QStringLiteral("rule.toml"), std::move(*manifest)},
        {QStringLiteral("src/h264_annex_b.svfmt"), std::move(*source)},
    });
    if (!loaded.succeeded()) {
        loaded.errorMessage = QStringLiteral("Bundled H.264 package is invalid: %1")
                                  .arg(loaded.errorMessage);
    }
    return loaded;
}

namespace {

struct BundledRule final {
    RuleCatalogLookupResult resolved;
    QString errorMessage;
};

[[nodiscard]] const BundledRule& bundledH264AnnexBRule() {
    static const BundledRule bundled = [] {
        BundledRule result;
        RulePackageLoadResult loaded = loadH264AnnexBRulePackage();
        if (!loaded.succeeded()) {
            result.errorMessage = std::move(loaded.errorMessage);
            return result;
        }
        const RulePackageIdentity identity = loaded.package->identity();
        RulePackageCatalog catalog;
        const RuleCatalogRegistrationResult registered =
            catalog.registerPackage(std::move(*loaded.package));
        if (!registered.succeeded()) {
            result.errorMessage = registered.errorMessage;
            return result;
        }
        result.resolved = catalog.resolve(identity, u"annex-b", languageVersion(),
                                          core::version());
        if (!result.resolved.succeeded()) {
            result.errorMessage = result.resolved.errorMessage;
        }
        return result;
    }();
    return bundled;
}

} // namespace

QString h264AnnexBRuleSource(QString* errorMessage) {
    const BundledRule& bundled = bundledH264AnnexBRule();
    if (!bundled.resolved.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = bundled.errorMessage;
        }
        return {};
    }
    const QByteArray* contents =
        bundled.resolved.package->fileContents(bundled.resolved.entryPoint->sourcePath);
    if (contents == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Bundled H.264 package has no Annex B source");
        }
        return {};
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return QString::fromUtf8(*contents);
}

std::optional<H264AnnexBAnalyzer>
H264AnnexBAnalyzer::create(const core::RandomAccessSource& source,
                           QString* errorMessage,
                           std::optional<core::CancellationToken> cancellation,
                           H264EbspRbspMapLimits mapperLimits) {
    const BundledRule& bundled = bundledH264AnnexBRule();
    if (!bundled.resolved.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = bundled.errorMessage;
        }
        return std::nullopt;
    }
    return create(source, bundled.resolved, errorMessage, std::move(cancellation),
                  mapperLimits);
}

std::optional<H264AnnexBAnalyzer>
H264AnnexBAnalyzer::create(const core::RandomAccessSource& source,
                           const RuleCatalogLookupResult& resolvedRule,
                           QString* errorMessage,
                           std::optional<core::CancellationToken> cancellation,
                           H264EbspRbspMapLimits mapperLimits) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (mapperLimits.maximumMappingSegments == 0 ||
        mapperLimits.maximumExcludedSpans == 0 || mapperLimits.maximumIssues == 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "H.264 EBSP-to-RBSP cumulative limits must be greater than zero");
        }
        return std::nullopt;
    }

    if (!resolvedRule.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = resolvedRule.errorMessage.isEmpty()
                                ? QStringLiteral("H.264 rule was not resolved exactly")
                                : resolvedRule.errorMessage;
        }
        return std::nullopt;
    }
    const QByteArray* ruleBytes =
        resolvedRule.package->fileContents(resolvedRule.entryPoint->sourcePath);
    if (ruleBytes == nullptr || ruleBytes->isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Resolved H.264 rule source is missing or empty");
        }
        return std::nullopt;
    }
    QString identityError;
    auto ruleIdentity = RuleEntryPointIdentity::create(
        resolvedRule.package->identity(), resolvedRule.entryPoint->id, &identityError);
    if (!ruleIdentity) {
        if (errorMessage != nullptr) {
            *errorMessage = std::move(identityError);
        }
        return std::nullopt;
    }
    const QString ruleSource = QString::fromUtf8(*ruleBytes);

    const DslParseResult parsed = DslParser::parse(ruleSource);
    if (!parsed.succeeded()) {
        if (errorMessage != nullptr) {
            const DslDiagnostic& diagnostic = parsed.diagnostics.front();
            *errorMessage = QStringLiteral("Resolved H.264 rule is invalid at %1:%2: %3")
                                .arg(diagnostic.range.start.line)
                                .arg(diagnostic.range.start.column)
                                .arg(diagnostic.message);
        }
        return std::nullopt;
    }

    DslCompileResult compiled = DslCompiler::compile(parsed.program);
    if (!compiled.succeeded()) {
        if (errorMessage != nullptr) {
            if (compiled.diagnostics.empty()) {
                *errorMessage = QStringLiteral("Resolved H.264 rule failed static compilation");
            } else {
                const DslDiagnostic& diagnostic = compiled.diagnostics.front();
                *errorMessage =
                    QStringLiteral("Resolved H.264 rule failed static compilation at %1:%2: %3")
                        .arg(diagnostic.range.start.line)
                        .arg(diagnostic.range.start.column)
                        .arg(diagnostic.message);
            }
        }
        return std::nullopt;
    }

    if (compiled.program->entry.kind != DslEntryKind::Sequence ||
        compiled.program->entry.targetIndex >= compiled.program->scans.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Resolved H.264 rule has no Annex B entry scan");
        }
        return std::nullopt;
    }
    const DslTypedScan& entryScan =
        compiled.program->scans.at(compiled.program->entry.targetIndex);
    if (entryScan.scanner != DslScannerKind::H264StartCode ||
        entryScan.elementStructIndex >= compiled.program->structs.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Resolved H.264 rule has no Annex B entry scan");
        }
        return std::nullopt;
    }
    const quint32 elementStructIndex = entryScan.elementStructIndex;

    auto tree = core::AnalysisTree::create(
        source.identity().isEmpty() ? QStringLiteral("H.264 Annex B") : source.identity());
    if (!tree) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to create the H.264 analysis tree");
        }
        return std::nullopt;
    }

    H264AnnexBAnalyzer analyzer(source,
                                std::move(cancellation),
                                mapperLimits,
                                std::move(*ruleIdentity),
                                std::move(*compiled.program),
                                elementStructIndex,
                                std::move(*tree));
    return std::optional<H264AnnexBAnalyzer>(std::move(analyzer));
}

H264AnnexBAnalyzer::H264AnnexBAnalyzer(const core::RandomAccessSource& source,
                                       std::optional<core::CancellationToken> cancellation,
                                       H264EbspRbspMapLimits mapperLimits,
                                       RuleEntryPointIdentity ruleIdentity,
                                       DslTypedProgram program,
                                       quint32 elementStructIndex,
                                       core::AnalysisTree tree)
    : source_(&source), scanner_(source, cancellation), cancellation_(std::move(cancellation)),
      mapperLimits_(mapperLimits), ruleIdentity_(std::move(ruleIdentity)),
      elementStructIndex_(elementStructIndex), tree_(std::move(tree)),
      executionSession_(std::move(program)) {}

bool H264AnnexBAnalyzer::resumeAfterCancellation(
    std::optional<core::CancellationToken> cancellation,
    QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    const auto reject = [errorMessage](const QString& message) {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    };

    if (!terminal_ || terminalStatus_ != H264AnnexBAnalysisStatus::Cancelled) {
        return reject(QStringLiteral("Only a cancelled H.264 analysis can be resumed"));
    }
    if (cancellation && cancellation->isCancellationRequested()) {
        return reject(QStringLiteral("Replacement cancellation token is already requested"));
    }
    const auto root = tree_.node(tree_.rootId());
    if (!root || root->state() != core::MaterializationState::Cancelled) {
        return reject(QStringLiteral("Cancelled H.264 analysis root is unavailable"));
    }
    if (!tree_.resumeCancelled(tree_.rootId())) {
        return reject(QStringLiteral("Unable to resume the H.264 analysis root"));
    }

    cancellation_ = std::move(cancellation);
    scanner_.replaceCancellationToken(cancellation_);
    if (deferredScanStatus_ == StartCodeScanStatus::Cancelled) {
        deferredScanStatus_.reset();
        deferredScanErrorMessage_.clear();
    }
    terminal_ = false;
    terminalStatus_ = H264AnnexBAnalysisStatus::InProgress;
    terminalErrorMessage_.clear();
    return true;
}

std::optional<core::FieldLocation>
H264AnnexBAnalyzer::makeLocation(std::vector<core::SourceSpan> sourceSpans) {
    if (nextViewId_ == 0) {
        return std::nullopt;
    }
    const core::LogicalViewId viewId(nextViewId_);
    nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;

    const auto mapping = core::SourceMapping::create(viewId, std::move(sourceSpans));
    if (!mapping) {
        return std::nullopt;
    }
    const auto range = core::LogicalRange::create(
        core::LogicalBitAddress(viewId, 0), mapping->logicalBitLength());
    return range ? mapping->locate(*range) : std::nullopt;
}

bool H264AnnexBAnalyzer::publishRecord(const H264StartCodeRecord& record,
                                       H264AnnexBAnalysisBatch& batch,
                                       bool allowExecutionCancellation,
                                       H264AnnexBAnalysisStatus* failureStatus,
                                       QString* errorMessage) {
    *failureStatus = H264AnnexBAnalysisStatus::InvalidRule;
    if (!record.startCode) {
        *errorMessage =
            QStringLiteral("Start-code scanner returned a record without a prefix span");
        return false;
    }

    std::vector<core::SourceSpan> nalSpans{*record.startCode};
    if (record.nalUnit) {
        nalSpans.push_back(*record.nalUnit);
    }
    if (record.trailingZero8Bits) {
        nalSpans.push_back(*record.trailingZero8Bits);
    }
    const auto nalLocation = makeLocation(std::move(nalSpans));
    const auto startCodeLocation = makeLocation({*record.startCode});
    if (!nalLocation || !startCodeLocation) {
        *errorMessage = QStringLiteral("Unable to map Annex B record to source coordinates");
        return false;
    }

    const quint64 nalUnitIndex = nextNalUnitIndex_;
    core::AnalysisNodeSpec nalSpec;
    nalSpec.kind = core::AnalysisNodeKind::Region;
    nalSpec.name = QStringLiteral("nal_unit[%1]").arg(nalUnitIndex);
    nalSpec.state = core::MaterializationState::Indexing;
    nalSpec.location = *nalLocation;
    const auto nalNode = tree_.appendChild(tree_.rootId(), std::move(nalSpec));
    if (!nalNode) {
        *errorMessage = QStringLiteral("Unable to append NAL unit to analysis tree");
        return false;
    }
    ++nextNalUnitIndex_;
    const auto failPublishedNal = [this,
                                   &batch,
                                   &nalLocation,
                                   nalNode,
                                   nalUnitIndex,
                                   failureStatus,
                                   errorMessage](QString message) {
        *errorMessage = std::move(message);
        const auto node = tree_.node(*nalNode);
        if (node && node->state() == core::MaterializationState::Indexing) {
            core::ParseDiagnostic diagnostic;
            diagnostic.code = diagnosticCode(*failureStatus);
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = *errorMessage;
            diagnostic.fieldPath = QStringLiteral("nal_unit[%1]").arg(nalUnitIndex);
            diagnostic.location = *nalLocation;
            (void)tree_.markPartial(*nalNode,
                                    *failureStatus == H264AnnexBAnalysisStatus::Cancelled
                                        ? core::MaterializationState::Cancelled
                                        : core::MaterializationState::Invalid,
                                    std::move(diagnostic));
        }
        batch.nalUnitNodes.push_back(*nalNode);
        return false;
    };

    core::AnalysisNodeSpec startCodeSpec;
    startCodeSpec.kind = core::AnalysisNodeKind::Region;
    startCodeSpec.name = QStringLiteral("start_code");
    startCodeSpec.state = core::MaterializationState::Materialized;
    startCodeSpec.location = *startCodeLocation;
    if (!tree_.appendChild(*nalNode, std::move(startCodeSpec))) {
        return failPublishedNal(QStringLiteral("Unable to append start code to analysis tree"));
    }

    const core::SourceBitAddress headerStart = record.nalUnit
                                                   ? record.nalUnit->start()
                                                   : record.startCode->endExclusive();
    const quint64 headerBitLength = record.nalUnit ? 8 : 0;
    const auto headerSpan = core::SourceSpan::create(headerStart, headerBitLength);
    if (!headerSpan) {
        return failPublishedNal(QStringLiteral("NAL header exceeds source coordinate limits"));
    }
    if (nextViewId_ == 0) {
        return failPublishedNal(QStringLiteral("Logical view identifier limit reached"));
    }
    const core::LogicalViewId headerViewId(nextViewId_);
    nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;
    std::vector<core::SourceSpan> headerSpans;
    if (headerSpan->bitLength() != 0) {
        headerSpans.push_back(*headerSpan);
    }
    const auto mapping = core::SourceMapping::create(headerViewId, std::move(headerSpans));
    if (!mapping) {
        return failPublishedNal(QStringLiteral("Unable to create direct NAL header mapping"));
    }

    core::BitReader reader(*source_, *mapping);
    DslExecutionOptions executionOptions;
    if (allowExecutionCancellation) {
        executionOptions.cancellation = cancellation_;
    }
    const DslExecutionResult execution = DslExecutor::decodeStruct(executionSession_.program(),
                                                                    elementStructIndex_,
                                                                    reader,
                                                                    *mapping,
                                                                    0,
                                                                    tree_,
                                                                    *nalNode,
                                                                    executionOptions);
    if (!execution.materialized()) {
        core::ParseDiagnostic diagnostic;
        if (execution.structureNode) {
            const auto structure = tree_.node(*execution.structureNode);
            if (structure && !structure->diagnostics().empty()) {
                diagnostic = structure->diagnostics().front();
            }
        }
        if (diagnostic.message.isEmpty()) {
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = execution.errorMessage.isEmpty()
                                     ? QStringLiteral("Unable to decode NAL unit header")
                                     : execution.errorMessage;
            diagnostic.fieldPath = QStringLiteral("nal_unit[%1].NalUnitHeader")
                                       .arg(nalUnitIndex);
            switch (execution.status) {
            case DslExecutionStatus::TruncatedSource:
                diagnostic.code = core::DiagnosticCode::TruncatedSource;
                break;
            case DslExecutionStatus::SourceError:
                diagnostic.code = core::DiagnosticCode::SourceError;
                break;
            case DslExecutionStatus::Cancelled:
                diagnostic.code = core::DiagnosticCode::Cancelled;
                break;
            case DslExecutionStatus::ResourceLimit:
                diagnostic.code = core::DiagnosticCode::ResourceLimit;
                break;
            case DslExecutionStatus::DependencyUnavailable:
                diagnostic.code = core::DiagnosticCode::DependencyUnavailable;
                break;
            case DslExecutionStatus::InvalidSyntax:
            case DslExecutionStatus::InvalidDefinition:
            case DslExecutionStatus::Materialized:
                diagnostic.code = core::DiagnosticCode::InvalidSyntax;
                break;
            }
        }
        if (!diagnostic.location || diagnostic.location->sourceSpans().empty()) {
            if (headerSpan->bitLength() != 0) {
                const auto headerRange = core::LogicalRange::create(
                    core::LogicalBitAddress(headerViewId, 0), headerSpan->bitLength());
                diagnostic.location = headerRange ? mapping->locate(*headerRange) : std::nullopt;
            }
            if (!diagnostic.location || diagnostic.location->sourceSpans().empty()) {
                diagnostic.location = *nalLocation;
            }
        }
        if (!appendTrailingZeroRegion(record, *nalNode, errorMessage)) {
            return failPublishedNal(*errorMessage);
        }
        const auto nalState = execution.status == DslExecutionStatus::Cancelled
                                  ? core::MaterializationState::Cancelled
                                  : core::MaterializationState::Invalid;
        if (!tree_.markPartial(*nalNode, nalState, std::move(diagnostic))) {
            return failPublishedNal(
                QStringLiteral("Unable to mark NAL unit as a partial result"));
        }
        *errorMessage = execution.errorMessage.isEmpty()
                          ? QStringLiteral("NAL unit header contains invalid or truncated syntax")
                          : execution.errorMessage;
        batch.nalUnitNodes.push_back(*nalNode);
        if (execution.status == DslExecutionStatus::SourceError) {
            *failureStatus = H264AnnexBAnalysisStatus::SourceError;
            return false;
        }
        if (execution.status == DslExecutionStatus::Cancelled) {
            *failureStatus = H264AnnexBAnalysisStatus::Cancelled;
            return false;
        }
        if (execution.status == DslExecutionStatus::ResourceLimit ||
            execution.status == DslExecutionStatus::InvalidDefinition) {
            *failureStatus = execution.status == DslExecutionStatus::ResourceLimit
                                 ? H264AnnexBAnalysisStatus::ResourceLimit
                                 : H264AnnexBAnalysisStatus::InvalidRule;
            return false;
        }
        return true;
    }

    if (!execution.structureNode) {
        return failPublishedNal(
            QStringLiteral("Materialized NAL header has no structure node"));
    }
    const auto headerNode = tree_.node(*execution.structureNode);
    if (!headerNode) {
        return failPublishedNal(QStringLiteral("Materialized NAL header is unavailable"));
    }
    std::optional<core::AnalysisNode> nalUnitTypeNode;
    for (const core::AnalysisNodeId childId : headerNode->children()) {
        const auto child = tree_.node(childId);
        if (!child || child->name() != QStringLiteral("nal_unit_type")) {
            continue;
        }
        if (nalUnitTypeNode) {
            return failPublishedNal(
                QStringLiteral("Materialized NAL header has duplicate nal_unit_type fields"));
        }
        nalUnitTypeNode = child;
    }
    if (!nalUnitTypeNode) {
        return failPublishedNal(
            QStringLiteral("Materialized NAL header has no nal_unit_type field"));
    }
    bool typeConversionSucceeded = false;
    const quint64 nalUnitType = nalUnitTypeNode->value().toULongLong(&typeConversionSucceeded);
    if (!typeConversionSucceeded) {
        return failPublishedNal(
            QStringLiteral("Materialized NAL header has an invalid nal_unit_type field"));
    }

    const quint64 nalBitLength = record.nalUnit ? record.nalUnit->bitLength() : 0;
    std::optional<DslTypedPayloadCase> payloadCase = payloadCaseFor(nalUnitType);
    const bool mapPayload =
        !hasExtensionHeader(nalUnitType) && (nalBitLength > 8U || payloadCase.has_value());
    if (mapPayload) {
        const quint64 nalStart = record.nalUnit->start().absoluteBitOffset();
        if (nalStart > std::numeric_limits<quint64>::max() - 8U) {
            return failPublishedNal(
                QStringLiteral("H.264 EBSP payload start exceeds coordinate limits"));
        }
        const core::SourceBitAddress payloadStart(nalStart + 8U);
        const auto payloadSpan = core::SourceSpan::create(payloadStart, nalBitLength - 8U);
        if (!payloadSpan || nextViewId_ == 0) {
            return failPublishedNal(
                QStringLiteral("Unable to represent H.264 EBSP payload coordinates"));
        }
        const core::LogicalViewId rbspViewId(nextViewId_);
        nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;
        pendingNalUnit_.emplace(PendingNalUnit{
            record,
            *nalNode,
            nalUnitIndex,
            H264EbspRbspMapper(
                *source_, rbspViewId, *payloadSpan, mapperLimits_, cancellation_),
            std::move(payloadCase),
            allowExecutionCancellation,
        });
        return true;
    }

    if (!appendTrailingZeroRegion(record, *nalNode, errorMessage)) {
        return failPublishedNal(*errorMessage);
    }
    if (!tree_.transition(*nalNode, core::MaterializationState::Materialized)) {
        return failPublishedNal(QStringLiteral("Unable to materialize NAL unit node"));
    }
    batch.nalUnitNodes.push_back(*nalNode);
    return true;
}

bool H264AnnexBAnalyzer::appendTrailingZeroRegion(const H264StartCodeRecord& record,
                                                  core::AnalysisNodeId nalNode,
                                                  QString* errorMessage) {
    if (!record.trailingZero8Bits) {
        if (record.trailingZero8BitsLength != 0) {
            *errorMessage =
                QStringLiteral("Annex B record has a trailing-zero length without a source span");
            return false;
        }
        return true;
    }
    if (record.trailingZero8BitsLength == 0) {
        *errorMessage =
            QStringLiteral("Annex B record has a trailing-zero source span without a length");
        return false;
    }

    const auto location = makeLocation({*record.trailingZero8Bits});
    if (!location) {
        *errorMessage = QStringLiteral("Unable to map Annex B trailing-zero framing");
        return false;
    }

    core::AnalysisNodeSpec spec;
    spec.kind = core::AnalysisNodeKind::Region;
    spec.name = QStringLiteral("trailing_zero_8bits");
    spec.state = core::MaterializationState::Materialized;
    spec.location = *location;
    spec.metadata.typeName = QStringLiteral("trailing_zero_8bits");
    spec.metadata.description = QStringLiteral("Annex B zero-byte framing after the NAL unit.");
    spec.metadata.specification =
        core::AnalysisSpecification{QStringLiteral("ITU-T H.264"), QStringLiteral("B.1.1")};
    if (!tree_.appendChild(nalNode, std::move(spec))) {
        *errorMessage = QStringLiteral("Unable to append Annex B trailing-zero framing");
        return false;
    }
    return true;
}

std::optional<DslTypedPayloadCase> H264AnnexBAnalyzer::payloadCaseFor(
    quint64 nalUnitType) const {
    const DslTypedProgram& program = executionSession_.program();
    if (!program.payloadDispatch) {
        return std::nullopt;
    }
    const DslTypedPayloadDispatch& dispatch = *program.payloadDispatch;
    if (dispatch.scanIndex >= program.scans.size() ||
        program.scans.at(dispatch.scanIndex).elementStructIndex != elementStructIndex_) {
        return std::nullopt;
    }
    const DslTypedPayloadCase* payloadCase = dispatch.find(nalUnitType);
    return payloadCase == nullptr ? std::nullopt : std::optional(*payloadCase);
}

bool H264AnnexBAnalyzer::decodePayloadStructure(PendingNalUnit& pending,
                                                core::AnalysisNodeId rbspNode,
                                                const QString& rbspPath,
                                                bool* payloadDecoded,
                                                H264AnnexBAnalysisStatus* failureStatus,
                                                QString* errorMessage) {
    *payloadDecoded = false;
    *failureStatus = H264AnnexBAnalysisStatus::InvalidRule;
    const core::SourceMapping& mapping = pending.mapper.mapping();
    const quint64 payloadBits = mapping.logicalBitLength();

    const auto reportPayload = [this, rbspNode, &rbspPath](core::DiagnosticCode code,
                                                           core::MaterializationState state,
                                                           QString message) {
        core::ParseDiagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.severity = core::DiagnosticSeverity::Error;
        diagnostic.message = std::move(message);
        diagnostic.fieldPath = rbspPath;
        const auto node = tree_.node(rbspNode);
        if (node) {
            diagnostic.location = node->location();
        }
        return tree_.markPartial(rbspNode, state, std::move(diagnostic));
    };

    if (!pending.payloadCase->structureIndex) {
        if (payloadBits == 0) {
            *payloadDecoded = true;
            return true;
        }
        if (!reportPayload(core::DiagnosticCode::InvalidSyntax,
                           core::MaterializationState::Invalid,
                           QStringLiteral("This NAL unit type requires an empty RBSP"))) {
            *errorMessage = QStringLiteral("Unable to reject a non-empty empty-RBSP payload");
            return false;
        }
        return true;
    }

    const quint32 structureIndex = *pending.payloadCase->structureIndex;
    if (structureIndex >= executionSession_.program().structs.size()) {
        *errorMessage = QStringLiteral("H.264 payload dispatch names an unknown structure");
        return false;
    }

    DslExecutionOptions executionOptions;
    if (pending.allowExecutionCancellation) {
        executionOptions.cancellation = cancellation_;
    }
    RuleExecutionRequest request;
    request.source = source_;
    request.structureIndex = structureIndex;
    request.mapping = &mapping;
    request.tree = &tree_;
    request.parentId = rbspNode;
    request.enclosingSourceSpan = *pending.record.nalUnit;
    request.options = executionOptions;
    const RuleExecutionResult execution = executionSession_.run(request);

    if (execution.materialized()) {
        *payloadDecoded = true;
        return true;
    }

    core::DiagnosticCode code = core::DiagnosticCode::InvalidSyntax;
    core::MaterializationState state = core::MaterializationState::Invalid;
    QString message;
    bool terminal = false;
    message = execution.errorMessage.isEmpty()
                  ? QStringLiteral("Unable to decode the H.264 RBSP payload")
                  : execution.errorMessage;
    switch (execution.status) {
        case RuleExecutionStatus::TruncatedSource:
            code = core::DiagnosticCode::TruncatedSource;
            break;
        case RuleExecutionStatus::DependencyUnavailable:
            code = core::DiagnosticCode::DependencyUnavailable;
            break;
        case RuleExecutionStatus::SourceError:
            code = core::DiagnosticCode::SourceError;
            *failureStatus = H264AnnexBAnalysisStatus::SourceError;
            terminal = true;
            break;
        case RuleExecutionStatus::Cancelled:
            code = core::DiagnosticCode::Cancelled;
            state = core::MaterializationState::Cancelled;
            *failureStatus = H264AnnexBAnalysisStatus::Cancelled;
            terminal = true;
            break;
        case RuleExecutionStatus::ResourceLimit:
            code = core::DiagnosticCode::ResourceLimit;
            *failureStatus = H264AnnexBAnalysisStatus::ResourceLimit;
            terminal = true;
            break;
        case RuleExecutionStatus::InvalidDefinition:
            *failureStatus = H264AnnexBAnalysisStatus::InvalidRule;
            terminal = true;
            break;
        case RuleExecutionStatus::InvalidSyntax:
        case RuleExecutionStatus::Materialized:
            break;
    }
    if (!reportPayload(code, state, message)) {
        *errorMessage = QStringLiteral("Unable to mark the H.264 RBSP payload as a partial result");
        return false;
    }
    if (terminal) {
        *errorMessage = std::move(message);
        return false;
    }
    return true;
}

bool H264AnnexBAnalyzer::finishPendingNalUnit(const H264EbspRbspMapBatch& mapBatch,
                                              H264AnnexBAnalysisBatch& batch,
                                              H264AnnexBAnalysisStatus* failureStatus,
                                              QString* errorMessage) {
    *failureStatus = H264AnnexBAnalysisStatus::InvalidRule;
    if (!pendingNalUnit_ || mapBatch.status == H264EbspRbspMapStatus::InProgress) {
        *errorMessage = QStringLiteral("H.264 analyzer has no terminal pending mapping");
        return false;
    }

    PendingNalUnit& pending = *pendingNalUnit_;
    const auto failPendingNal = [this,
                                 &batch,
                                 &pending,
                                 failureStatus,
                                 errorMessage](QString message) {
        *failureStatus = H264AnnexBAnalysisStatus::InvalidRule;
        *errorMessage = std::move(message);
        const core::AnalysisNodeId nodeId = pending.node;
        const auto node = tree_.node(nodeId);
        if (node && node->state() == core::MaterializationState::Indexing) {
            core::ParseDiagnostic diagnostic;
            diagnostic.code = core::DiagnosticCode::InvalidSyntax;
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = *errorMessage;
            diagnostic.fieldPath = QStringLiteral("nal_unit[%1]").arg(pending.index);
            diagnostic.location = node->location();
            (void)tree_.markPartial(nodeId,
                                    core::MaterializationState::Invalid,
                                    std::move(diagnostic));
        }
        batch.nalUnitNodes.push_back(nodeId);
        pendingNalUnit_.reset();
        return false;
    };
    const core::SourceMapping& mapping = pending.mapper.mapping();
    std::optional<core::FieldLocation> rbspLocation;
    if (mapping.logicalBitLength() != 0) {
        const auto range = core::LogicalRange::create(
            core::LogicalBitAddress(mapping.viewId(), 0), mapping.logicalBitLength());
        rbspLocation = range ? mapping.locate(*range) : std::nullopt;
        if (!rbspLocation) {
            return failPendingNal(
                QStringLiteral("Unable to locate the mapped H.264 RBSP payload"));
        }
    }

    const bool mappingComplete = mapBatch.status == H264EbspRbspMapStatus::Complete;
    H264AnnexBAnalysisStatus mappedFailureStatus = analysisStatus(mapBatch.status);
    if (mappedFailureStatus == H264AnnexBAnalysisStatus::InvalidBatchSize) {
        mappedFailureStatus = H264AnnexBAnalysisStatus::InvalidRule;
    }
    const core::MaterializationState rbspState =
        mappingComplete
            ? (pending.payloadCase ? core::MaterializationState::Indexing
                                   : core::MaterializationState::Materialized)
            : (mappedFailureStatus == H264AnnexBAnalysisStatus::Cancelled
                   ? core::MaterializationState::Cancelled
                   : core::MaterializationState::Invalid);

    core::AnalysisNodeSpec rbspSpec;
    rbspSpec.kind = core::AnalysisNodeKind::Region;
    rbspSpec.name = QStringLiteral("rbsp_payload");
    rbspSpec.state = rbspState;
    rbspSpec.location = rbspLocation;
    rbspSpec.metadata.typeName = QStringLiteral("h264_rbsp");
    rbspSpec.metadata.description =
        QStringLiteral("Raw byte sequence payload mapped without emulation-prevention bytes.");
    rbspSpec.metadata.specification = core::AnalysisSpecification{
        QStringLiteral("ITU-T H.264"), QStringLiteral("7.3.1, 7.4.1")};
    const auto rbspNode = tree_.appendChild(pending.node, std::move(rbspSpec));
    if (!rbspNode) {
        return failPendingNal(
            QStringLiteral("Unable to append the mapped H.264 RBSP payload"));
    }

    const QString rbspPath = QStringLiteral("nal_unit[%1].rbsp_payload").arg(pending.index);
    std::optional<core::ParseDiagnostic> mappingFailureDiagnostic;
    if (!mappingComplete) {
        core::ParseDiagnostic diagnostic;
        diagnostic.code = diagnosticCode(mappedFailureStatus);
        diagnostic.severity = core::DiagnosticSeverity::Error;
        diagnostic.message = mappingFailureMessage(mapBatch.status, mapBatch.errorMessage);
        diagnostic.fieldPath = rbspPath;
        diagnostic.location = rbspLocation;
        mappingFailureDiagnostic = diagnostic;
        if (!tree_.addDiagnostic(*rbspNode, diagnostic)) {
            return failPendingNal(
                QStringLiteral("Unable to attach the H.264 RBSP mapping diagnostic"));
        }
    }

    std::size_t excludedIndex = 0;
    for (const H264EbspRbspExcludedSpan& excluded : pending.mapper.excludedSpans()) {
        const auto location = makeLocation({excluded.sourceSpan});
        if (!location) {
            return failPendingNal(
                QStringLiteral("Unable to locate an emulation-prevention byte"));
        }
        core::AnalysisNodeSpec excludedSpec;
        excludedSpec.kind = core::AnalysisNodeKind::Region;
        excludedSpec.name =
            QStringLiteral("emulation_prevention_three_byte[%1]").arg(excludedIndex);
        excludedSpec.state = core::MaterializationState::Materialized;
        excludedSpec.location = *location;
        excludedSpec.metadata.typeName = QStringLiteral("emulation_prevention_three_byte");
        excludedSpec.metadata.description =
            QStringLiteral("Byte excluded while deriving the H.264 RBSP logical view.");
        excludedSpec.metadata.specification = core::AnalysisSpecification{
            QStringLiteral("ITU-T H.264"), QStringLiteral("7.3.1")};
        if (!tree_.appendChild(pending.node, std::move(excludedSpec))) {
            return failPendingNal(
                QStringLiteral("Unable to append an emulation-prevention byte"));
        }
        ++excludedIndex;
    }

    if (!appendTrailingZeroRegion(pending.record, pending.node, errorMessage)) {
        return failPendingNal(*errorMessage);
    }

    if (mappingComplete) {
        for (const H264EbspRbspIssue& issue : pending.mapper.issues()) {
            const auto location = makeLocation({issue.sourceSpan});
            if (!location) {
                return failPendingNal(
                    QStringLiteral("Unable to locate an H.264 EBSP conformance issue"));
            }
            core::ParseDiagnostic diagnostic;
            diagnostic.code = core::DiagnosticCode::InvalidSyntax;
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = issueMessage(issue.kind);
            diagnostic.fieldPath = rbspPath;
            diagnostic.location = *location;
            if (!tree_.addDiagnostic(pending.node, std::move(diagnostic))) {
                return failPendingNal(
                    QStringLiteral("Unable to attach an H.264 EBSP conformance issue"));
            }
        }

        bool payloadDecoded = true;
        if (pending.payloadCase) {
            H264AnnexBAnalysisStatus payloadFailureStatus =
                H264AnnexBAnalysisStatus::InvalidRule;
            QString payloadError;
            const bool payloadContinues = decodePayloadStructure(pending,
                                                                 *rbspNode,
                                                                 rbspPath,
                                                                 &payloadDecoded,
                                                                 &payloadFailureStatus,
                                                                 &payloadError);
            if (payloadDecoded && !tree_.transition(*rbspNode,
                                                    core::MaterializationState::Materialized)) {
                return failPendingNal(
                    QStringLiteral("Unable to materialize the decoded H.264 RBSP payload"));
            }
            if (!payloadContinues) {
                *failureStatus = payloadFailureStatus;
                *errorMessage = payloadError;
                const core::MaterializationState nalState =
                    payloadFailureStatus == H264AnnexBAnalysisStatus::Cancelled
                        ? core::MaterializationState::Cancelled
                        : core::MaterializationState::Invalid;
                core::ParseDiagnostic diagnostic;
                diagnostic.code = diagnosticCode(payloadFailureStatus);
                diagnostic.severity = core::DiagnosticSeverity::Error;
                diagnostic.message = payloadError;
                diagnostic.fieldPath = rbspPath;
                diagnostic.location = rbspLocation;
                if (!tree_.markPartial(pending.node, nalState, std::move(diagnostic))) {
                    return failPendingNal(
                        QStringLiteral("Unable to mark the decoded H.264 NAL unit as partial"));
                }
                batch.nalUnitNodes.push_back(pending.node);
                pendingNalUnit_.reset();
                return false;
            }
        }

        const core::MaterializationState nalState =
            pending.mapper.issues().empty() && payloadDecoded
                ? core::MaterializationState::Materialized
                : core::MaterializationState::Invalid;
        if (!tree_.transition(pending.node, nalState)) {
            return failPendingNal(
                QStringLiteral("Unable to finish the mapped H.264 NAL unit"));
        }
        batch.nalUnitNodes.push_back(pending.node);
        pendingNalUnit_.reset();
        return true;
    }

    *failureStatus = mappedFailureStatus;
    *errorMessage = mappingFailureDiagnostic->message;
    const core::MaterializationState nalState =
        mappedFailureStatus == H264AnnexBAnalysisStatus::Cancelled
            ? core::MaterializationState::Cancelled
            : core::MaterializationState::Invalid;
    if (!tree_.markPartial(pending.node, nalState, *mappingFailureDiagnostic)) {
        return failPendingNal(
            QStringLiteral("Unable to mark the mapped H.264 NAL unit as partial"));
    }
    batch.nalUnitNodes.push_back(pending.node);
    pendingNalUnit_.reset();
    return false;
}

void H264AnnexBAnalyzer::markRootPartial(core::DiagnosticCode code,
                                         core::MaterializationState state,
                                         const QString& message) {
    core::ParseDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.severity = core::DiagnosticSeverity::Error;
    diagnostic.message = message;
    diagnostic.fieldPath = QStringLiteral("nal_units");
    (void)tree_.markPartial(tree_.rootId(), state, std::move(diagnostic));
}

H264AnnexBAnalysisBatch H264AnnexBAnalyzer::analyzeBatch(
    std::size_t maximumRecords,
    quint64 maximumInspectedPositions,
    quint64 maximumMappedBytes) {
    H264AnnexBAnalysisBatch result;
    if (terminal_) {
        result.status = terminalStatus_;
        result.errorMessage = terminalErrorMessage_;
        return result;
    }
    if (maximumRecords == 0 || maximumInspectedPositions == 0 || maximumMappedBytes == 0) {
        result.status = H264AnnexBAnalysisStatus::InvalidBatchSize;
        result.errorMessage = QStringLiteral(
            "Maximum analysis records, scan positions, and mapped bytes must be greater than zero");
        return result;
    }

    const auto terminalizeFailure = [this, &result](H264AnnexBAnalysisStatus status,
                                                    QString message) {
        result.status = status;
        result.errorMessage = std::move(message);
        terminal_ = true;
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
        const auto rootState = result.status == H264AnnexBAnalysisStatus::Cancelled
                                   ? core::MaterializationState::Cancelled
                                   : core::MaterializationState::Invalid;
        markRootPartial(diagnosticCode(result.status), rootState, result.errorMessage);
    };

    if (!deferredScanStatus_) {
        const bool cancellationRequestedBeforeScan =
            cancellation_ && cancellation_->isCancellationRequested();
        StartCodeScanBatch scanBatch =
            scanner_.scanBatch(maximumRecords, maximumInspectedPositions);
        const bool cancellationRequestedAfterScan =
            cancellation_ && cancellation_->isCancellationRequested();
        const bool allowExecutionCancellation =
            cancellation_.has_value() && !cancellationRequestedBeforeScan &&
            !cancellationRequestedAfterScan;
        const quint64 recordCount = static_cast<quint64>(scanBatch.records.size());
        if (nextStableRecordIndex_ > std::numeric_limits<quint64>::max() - recordCount) {
            terminalizeFailure(
                H264AnnexBAnalysisStatus::ResourceLimit,
                QStringLiteral("H.264 progressive-index record count exceeds 64-bit limits"));
            return result;
        }
        for (const H264StartCodeRecord& record : scanBatch.records) {
            if (record.trailingZero8BitsOffset >
                std::numeric_limits<quint64>::max() - record.trailingZero8BitsLength) {
                terminalizeFailure(
                    H264AnnexBAnalysisStatus::InvalidRule,
                    QStringLiteral("H.264 scanner returned an overflowing stable record"));
                return result;
            }
            stableIndexedThroughByteOffset_ =
                std::max(stableIndexedThroughByteOffset_,
                         record.trailingZero8BitsOffset + record.trailingZero8BitsLength);
        }
        if (scanBatch.status == StartCodeScanStatus::Complete) {
            stableIndexedThroughByteOffset_ = source_->sizeBytes();
        }
        if (!scanBatch.records.empty() || scanBatch.status == StartCodeScanStatus::Complete) {
            H264ProgressiveIndexUpdate update;
            update.firstRecordIndex = nextStableRecordIndex_;
            update.indexedThroughByteOffset = stableIndexedThroughByteOffset_;
            update.endOfSource = scanBatch.status == StartCodeScanStatus::Complete;
            update.records = scanBatch.records;
            result.progressiveIndexUpdate = std::move(update);
        }
        nextStableRecordIndex_ += recordCount;
        for (const H264StartCodeRecord& record : scanBatch.records) {
            queuedRecords_.push_back({record, allowExecutionCancellation});
        }
        deferredScanStatus_ = scanBatch.status;
        deferredScanErrorMessage_ = scanBatch.errorMessage;
    }

    quint64 remainingMappedBytes = maximumMappedBytes;
    while (pendingNalUnit_ || !queuedRecords_.empty()) {
        if (pendingNalUnit_) {
            if (remainingMappedBytes == 0) {
                result.status = H264AnnexBAnalysisStatus::InProgress;
                return result;
            }
            const quint64 cursorBefore = pendingNalUnit_->mapper.sourceCursor();
            const H264EbspRbspMapBatch mapBatch =
                pendingNalUnit_->mapper.mapBatch(remainingMappedBytes);
            const quint64 cursorAfter = pendingNalUnit_->mapper.sourceCursor();
            if (cursorAfter < cursorBefore || cursorAfter - cursorBefore > remainingMappedBytes) {
                terminalizeFailure(
                    H264AnnexBAnalysisStatus::InvalidRule,
                    QStringLiteral("H.264 EBSP mapper exceeded the analyzer work budget"));
                return result;
            }
            remainingMappedBytes -= cursorAfter - cursorBefore;
            if (mapBatch.status == H264EbspRbspMapStatus::InProgress) {
                result.status = H264AnnexBAnalysisStatus::InProgress;
                return result;
            }

            QString publishError;
            H264AnnexBAnalysisStatus failureStatus = H264AnnexBAnalysisStatus::InvalidRule;
            if (!finishPendingNalUnit(mapBatch, result, &failureStatus, &publishError)) {
                terminalizeFailure(failureStatus, publishError);
                return result;
            }
            continue;
        }

        if (remainingMappedBytes == 0) {
            result.status = H264AnnexBAnalysisStatus::InProgress;
            return result;
        }
        QueuedRecord queued = std::move(queuedRecords_.front());
        queuedRecords_.pop_front();
        QString publishError;
        H264AnnexBAnalysisStatus failureStatus = H264AnnexBAnalysisStatus::InvalidRule;
        if (!publishRecord(queued.record,
                           result,
                           queued.allowExecutionCancellation,
                           &failureStatus,
                           &publishError)) {
            terminalizeFailure(failureStatus, publishError);
            return result;
        }
    }

    if (!deferredScanStatus_) {
        terminalizeFailure(H264AnnexBAnalysisStatus::InvalidRule,
                           QStringLiteral("H.264 analyzer lost its deferred scanner status"));
        return result;
    }

    const StartCodeScanStatus scanStatus = *deferredScanStatus_;
    result.status = analysisStatus(scanStatus);
    result.errorMessage = deferredScanErrorMessage_;
    switch (scanStatus) {
    case StartCodeScanStatus::Complete:
        if (const auto root = tree_.node(tree_.rootId()); root && root->children().empty()) {
            markRootPartial(core::DiagnosticCode::InvalidSyntax,
                            core::MaterializationState::Invalid,
                            QStringLiteral("No H.264 Annex B start code was found"));
        } else if (!tree_.transition(tree_.rootId(), core::MaterializationState::Materialized)) {
            result.status = H264AnnexBAnalysisStatus::InvalidRule;
            result.errorMessage = QStringLiteral("Unable to materialize H.264 analysis root");
            markRootPartial(core::DiagnosticCode::InvalidSyntax,
                            core::MaterializationState::Invalid,
                            result.errorMessage);
        }
        terminal_ = true;
        break;
    case StartCodeScanStatus::Cancelled:
        if (result.errorMessage.isEmpty()) {
            result.errorMessage = QStringLiteral("H.264 Annex B scan was cancelled");
        }
        markRootPartial(core::DiagnosticCode::Cancelled,
                        core::MaterializationState::Cancelled,
                        result.errorMessage);
        terminal_ = true;
        break;
    case StartCodeScanStatus::SourceError:
        markRootPartial(core::DiagnosticCode::SourceError,
                        core::MaterializationState::Invalid,
                        result.errorMessage);
        terminal_ = true;
        break;
    case StartCodeScanStatus::InProgress:
        deferredScanStatus_.reset();
        deferredScanErrorMessage_.clear();
        result.status = H264AnnexBAnalysisStatus::InProgress;
        result.errorMessage.clear();
        break;
    case StartCodeScanStatus::InvalidBatchSize:
        result.status = H264AnnexBAnalysisStatus::InvalidRule;
        result.errorMessage = QStringLiteral("H.264 scanner rejected a validated analysis batch");
        markRootPartial(core::DiagnosticCode::InvalidSyntax,
                        core::MaterializationState::Invalid,
                        result.errorMessage);
        terminal_ = true;
        break;
    }

    if (terminal_) {
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
    }
    return result;
}

} // namespace streamview::rules
