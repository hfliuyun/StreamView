#include <streamview/rules/wav_riff_analyzer.h>

#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/version.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/language_version.h>
#include <streamview/rules/rule_package.h>

#include <QFile>
#include <QIODevice>

#include <limits>
#include <utility>

static void initializeStreamViewOfficialWavRules() {
    Q_INIT_RESOURCE(streamview_official_rules_wav);
}

namespace streamview::rules {

namespace {

[[nodiscard]] core::DiagnosticCode diagnosticCode(WavRiffAnalysisStatus status) noexcept {
    switch (status) {
    case WavRiffAnalysisStatus::Cancelled:
        return core::DiagnosticCode::Cancelled;
    case WavRiffAnalysisStatus::SourceError:
        return core::DiagnosticCode::SourceError;
    case WavRiffAnalysisStatus::ResourceLimit:
        return core::DiagnosticCode::ResourceLimit;
    case WavRiffAnalysisStatus::InProgress:
    case WavRiffAnalysisStatus::Complete:
    case WavRiffAnalysisStatus::InvalidRule:
    case WavRiffAnalysisStatus::InvalidBatchSize:
        return core::DiagnosticCode::InvalidSyntax;
    }
    return core::DiagnosticCode::InvalidSyntax;
}

[[nodiscard]] WavRiffAnalysisStatus analysisStatus(RiffChunkScanStatus scanStatus) noexcept {
    switch (scanStatus) {
    case RiffChunkScanStatus::InProgress:
        return WavRiffAnalysisStatus::InProgress;
    case RiffChunkScanStatus::Complete:
        return WavRiffAnalysisStatus::Complete;
    case RiffChunkScanStatus::Cancelled:
        return WavRiffAnalysisStatus::Cancelled;
    case RiffChunkScanStatus::SourceError:
        return WavRiffAnalysisStatus::SourceError;
    case RiffChunkScanStatus::InvalidBatchSize:
        return WavRiffAnalysisStatus::InvalidBatchSize;
    }
    return WavRiffAnalysisStatus::InvalidRule;
}

[[nodiscard]] QString recordName(const RiffChunkRecord& record, quint64 index) {
    if (record.isFormHeader) {
        return QStringLiteral("riff_form_header");
    }
    return QStringLiteral("chunk[%1]").arg(index);
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

} // namespace

RulePackageLoadResult loadWavRulePackage() {
    initializeStreamViewOfficialWavRules();
    QString errorMessage;
    const QString root = QStringLiteral(":/streamview/rules/org.streamview.wav/");
    auto manifest = readBundledPackageFile(root + QStringLiteral("rule.toml"), &errorMessage);
    auto source = readBundledPackageFile(root + QStringLiteral("src/wav_riff.svfmt"),
                                         &errorMessage);
    if (!manifest || !source) {
        return {RulePackageLoadStatus::InvalidTree, std::nullopt, std::move(errorMessage)};
    }
    RulePackageLoadResult loaded = RulePackage::fromFiles({
        {QStringLiteral("rule.toml"), std::move(*manifest)},
        {QStringLiteral("src/wav_riff.svfmt"), std::move(*source)},
    });
    if (!loaded.succeeded()) {
        loaded.errorMessage = QStringLiteral("Bundled WAV package is invalid: %1")
                                  .arg(loaded.errorMessage);
    }
    return loaded;
}

namespace {

struct BundledWavRule final {
    RuleCatalogLookupResult resolved;
    QString errorMessage;
};

[[nodiscard]] const BundledWavRule& bundledWavRule() {
    static const BundledWavRule bundled = [] {
        BundledWavRule result;
        RulePackageLoadResult loaded = loadWavRulePackage();
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
        result.resolved = catalog.resolve(identity, u"wave", languageVersion(),
                                          core::version());
        if (!result.resolved.succeeded()) {
            result.errorMessage = result.resolved.errorMessage;
        }
        return result;
    }();
    return bundled;
}

} // namespace

std::optional<WavRiffAnalyzer>
WavRiffAnalyzer::create(const core::RandomAccessSource& source,
                       QString* errorMessage,
                       std::optional<core::CancellationToken> cancellation) {
    const BundledWavRule& bundled = bundledWavRule();
    if (!bundled.resolved.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = bundled.errorMessage;
        }
        return std::nullopt;
    }
    return create(source, bundled.resolved, errorMessage, std::move(cancellation));
}

std::optional<WavRiffAnalyzer>
WavRiffAnalyzer::create(const core::RandomAccessSource& source,
                       const RuleCatalogLookupResult& resolvedRule,
                       QString* errorMessage,
                       std::optional<core::CancellationToken> cancellation) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (!resolvedRule.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = resolvedRule.errorMessage.isEmpty()
                                ? QStringLiteral("WAV rule was not resolved exactly")
                                : resolvedRule.errorMessage;
        }
        return std::nullopt;
    }

    const QByteArray* ruleBytes =
        resolvedRule.package->fileContents(resolvedRule.entryPoint->sourcePath);
    if (ruleBytes == nullptr || ruleBytes->isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Resolved WAV rule source is missing or empty");
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
            *errorMessage = QStringLiteral("Resolved WAV rule is invalid at %1:%2: %3")
                                .arg(diagnostic.range.start.line)
                                .arg(diagnostic.range.start.column)
                                .arg(diagnostic.message);
        }
        return std::nullopt;
    }

    DslCompileResult compiled =
        DslCompiler::compileForTarget(parsed.program, resolvedRule.entryPoint->target);
    if (!compiled.succeeded()) {
        if (errorMessage != nullptr) {
            if (compiled.diagnostics.empty()) {
                *errorMessage = QStringLiteral("Resolved WAV rule failed static compilation");
            } else {
                const DslDiagnostic& diagnostic = compiled.diagnostics.front();
                *errorMessage =
                    QStringLiteral("Resolved WAV rule failed static compilation at %1:%2: %3")
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
            *errorMessage = QStringLiteral("Resolved WAV rule has no RIFF record entry scan");
        }
        return std::nullopt;
    }

    const DslTypedScan& entryScan =
        compiled.program->scans.at(compiled.program->entry.targetIndex);
    if (entryScan.scanner != DslScannerKind::RiffChunk ||
        entryScan.elementStructIndex >= compiled.program->structs.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Resolved WAV rule has no RIFF record entry scan");
        }
        return std::nullopt;
    }

    auto tree = core::AnalysisTree::create(QStringLiteral("wav_riff"));
    if (!tree) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to initialize WAV analysis tree");
        }
        return std::nullopt;
    }

    return WavRiffAnalyzer(source, std::move(*ruleIdentity), std::move(*compiled.program),
                           entryScan.elementStructIndex, std::move(*tree),
                           std::move(cancellation));
}

WavRiffAnalyzer::WavRiffAnalyzer(const core::RandomAccessSource& source,
                                 RuleEntryPointIdentity ruleIdentity,
                                 DslTypedProgram program,
                                 quint32 recordStructIndex,
                                 core::AnalysisTree tree,
                                 std::optional<core::CancellationToken> cancellation)
    : source_(&source),
      ruleIdentity_(std::move(ruleIdentity)),
      executionSession_(std::move(program)),
      recordStructIndex_(recordStructIndex),
      scanner_(source, cancellation),
      tree_(std::move(tree)),
      cancellation_(std::move(cancellation)),
      nextRecordIndex_(0),
      nextViewId_(1),
      terminal_(false),
      terminalStatus_(WavRiffAnalysisStatus::InProgress) {}

std::optional<core::FieldLocation>
WavRiffAnalyzer::makeLocation(std::vector<core::SourceSpan> sourceSpans) {
    if (nextViewId_ == 0) {
        return std::nullopt;
    }
    const core::LogicalViewId viewId(nextViewId_);
    nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;

    const auto mapping = core::SourceMapping::create(viewId, sourceSpans);
    if (!mapping) {
        return std::nullopt;
    }
    const auto range = core::LogicalRange::create(
        core::LogicalBitAddress(viewId, 0), mapping->logicalBitLength());
    return range ? mapping->locate(*range) : std::nullopt;
}

void WavRiffAnalyzer::markRootPartial(core::DiagnosticCode code,
                                      core::MaterializationState state,
                                      const QString& message) {
    core::ParseDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.severity = core::DiagnosticSeverity::Error;
    diagnostic.message = message;
    diagnostic.fieldPath = QStringLiteral("records");
    (void)tree_.markPartial(tree_.rootId(), state, std::move(diagnostic));
}

bool WavRiffAnalyzer::publishRecord(const RiffChunkRecord& record,
                                    WavRiffAnalysisBatch& batch,
                                    bool allowExecutionCancellation,
                                    WavRiffAnalysisStatus* failureStatus,
                                    QString* errorMessage) {
    *failureStatus = WavRiffAnalysisStatus::InvalidRule;
    if (!record.chunkSpan) {
        *errorMessage = QStringLiteral("RIFF scanner returned a record without a chunk span");
        return false;
    }

    const auto recordLocation = makeLocation({*record.chunkSpan});
    if (!recordLocation) {
        *errorMessage = QStringLiteral("Unable to map RIFF record to source coordinates");
        return false;
    }

    const quint64 recordIndex = nextRecordIndex_;
    const QString name = recordName(record, recordIndex);
    core::AnalysisNodeSpec recordSpec;
    recordSpec.kind = core::AnalysisNodeKind::Region;
    recordSpec.name = name;
    recordSpec.state = core::MaterializationState::Indexing;
    recordSpec.location = *recordLocation;

    const auto recordNode = tree_.appendChild(tree_.rootId(), std::move(recordSpec));
    if (!recordNode) {
        *errorMessage = QStringLiteral("Unable to append RIFF record to analysis tree");
        return false;
    }
    ++nextRecordIndex_;

    const core::LogicalViewId recordViewId(nextViewId_);
    nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;

    const auto mapping = core::SourceMapping::create(recordViewId, {*record.chunkSpan});
    if (!mapping) {
        *errorMessage = QStringLiteral("Unable to create RIFF record mapping");
        *failureStatus = WavRiffAnalysisStatus::ResourceLimit;
        core::ParseDiagnostic diagnostic;
        diagnostic.code = core::DiagnosticCode::ResourceLimit;
        diagnostic.severity = core::DiagnosticSeverity::Error;
        diagnostic.message = *errorMessage;
        diagnostic.fieldPath = name;
        diagnostic.location = *recordLocation;
        (void)tree_.markPartial(*recordNode, core::MaterializationState::Invalid, std::move(diagnostic));
        batch.recordNodes.push_back(*recordNode);
        return false;
    }

    core::BitReader reader(*source_, *mapping);
    DslExecutionOptions executionOptions;
    if (allowExecutionCancellation) {
        executionOptions.cancellation = cancellation_;
    }

    const DslExecutionResult execution = DslExecutor::decodeStruct(
        executionSession_.program(),
        recordStructIndex_,
        reader,
        *mapping,
        0,
        tree_,
        *recordNode,
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
                                     ? QStringLiteral("RIFF record contains invalid syntax")
                                     : execution.errorMessage;
            diagnostic.fieldPath = name;
            switch (execution.status) {
            case DslExecutionStatus::TruncatedSource:
                diagnostic.code = core::DiagnosticCode::TruncatedSource;
                diagnostic.message = QStringLiteral("RIFF record is truncated");
                break;
            case DslExecutionStatus::SourceError:
                diagnostic.code = core::DiagnosticCode::SourceError;
                break;
            case DslExecutionStatus::Cancelled:
                diagnostic.code = core::DiagnosticCode::Cancelled;
                diagnostic.message = QStringLiteral("Analysis was cancelled");
                break;
            case DslExecutionStatus::ResourceLimit:
                diagnostic.code = core::DiagnosticCode::ResourceLimit;
                break;
            case DslExecutionStatus::DependencyUnavailable:
                diagnostic.code = core::DiagnosticCode::DependencyUnavailable;
                break;
            case DslExecutionStatus::Unsupported:
                diagnostic.code = core::DiagnosticCode::UnsupportedSyntax;
                break;
            case DslExecutionStatus::InvalidSyntax:
            case DslExecutionStatus::InvalidDefinition:
            case DslExecutionStatus::Materialized:
                diagnostic.code = core::DiagnosticCode::InvalidSyntax;
                break;
            }
        }
        if (!diagnostic.location || diagnostic.location->sourceSpans().empty()) {
            diagnostic.location = *recordLocation;
        }
        if (diagnostic.fieldPath.isEmpty()) {
            diagnostic.fieldPath = name;
        }

        const auto terminalState = execution.status == DslExecutionStatus::Cancelled
                                       ? core::MaterializationState::Cancelled
                                   : execution.status == DslExecutionStatus::Unsupported
                                       ? core::MaterializationState::Unsupported
                                       : core::MaterializationState::Invalid;
        (void)tree_.markPartial(*recordNode, terminalState, std::move(diagnostic));
        batch.recordNodes.push_back(*recordNode);

        if (execution.status == DslExecutionStatus::SourceError) {
            *failureStatus = WavRiffAnalysisStatus::SourceError;
            *errorMessage = execution.errorMessage;
            return false;
        }
        if (execution.status == DslExecutionStatus::Cancelled) {
            *failureStatus = WavRiffAnalysisStatus::Cancelled;
            *errorMessage = QStringLiteral("Analysis was cancelled");
            return false;
        }
        if (execution.status == DslExecutionStatus::ResourceLimit) {
            *failureStatus = WavRiffAnalysisStatus::ResourceLimit;
            *errorMessage = execution.errorMessage;
            return false;
        }
        if (execution.status == DslExecutionStatus::InvalidDefinition) {
            *failureStatus = WavRiffAnalysisStatus::InvalidRule;
            *errorMessage = execution.errorMessage;
            return false;
        }

        // Content failures (InvalidSyntax, TruncatedSource, Unsupported,
        // DependencyUnavailable) isolate the current record and continue
        // analyzing subsequent records.
        return true;
    }

    if (!tree_.transition(*recordNode, core::MaterializationState::Materialized)) {
        *errorMessage = QStringLiteral("Unable to materialize decoded RIFF record node");
        *failureStatus = WavRiffAnalysisStatus::InvalidRule;
        core::ParseDiagnostic diagnostic;
        diagnostic.code = core::DiagnosticCode::InvalidSyntax;
        diagnostic.severity = core::DiagnosticSeverity::Error;
        diagnostic.message = *errorMessage;
        diagnostic.fieldPath = name;
        diagnostic.location = *recordLocation;
        (void)tree_.markPartial(*recordNode, core::MaterializationState::Invalid, std::move(diagnostic));
        batch.recordNodes.push_back(*recordNode);
        return false;
    }

    batch.recordNodes.push_back(*recordNode);
    return true;
}

WavRiffAnalysisBatch WavRiffAnalyzer::analyzeBatch(
    std::size_t maximumRecords,
    quint64 maximumInspectedPositions) {
    WavRiffAnalysisBatch result;
    if (terminal_) {
        result.status = terminalStatus_;
        result.errorMessage = terminalErrorMessage_;
        return result;
    }
    if (maximumRecords == 0 || maximumInspectedPositions == 0) {
        result.status = WavRiffAnalysisStatus::InvalidBatchSize;
        result.errorMessage = QStringLiteral(
            "Maximum records and inspected positions must be greater than zero");
        return result;
    }

    const auto terminalizeFailure = [this, &result](WavRiffAnalysisStatus status,
                                                    QString message) {
        result.status = status;
        result.errorMessage = std::move(message);
        terminal_ = true;
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
        const auto rootState = result.status == WavRiffAnalysisStatus::Cancelled
                                   ? core::MaterializationState::Cancelled
                                   : core::MaterializationState::Invalid;
        markRootPartial(diagnosticCode(result.status), rootState, result.errorMessage);
    };

    const bool cancellationRequestedBeforeScan =
        cancellation_ && cancellation_->isCancellationRequested();
    const auto scanBatch = scanner_.scanBatch(maximumRecords, maximumInspectedPositions);
    const bool cancellationRequestedAfterScan =
        cancellation_ && cancellation_->isCancellationRequested();
    const bool allowExecutionCancellation =
        cancellation_.has_value() && !cancellationRequestedBeforeScan &&
        !cancellationRequestedAfterScan;

    for (const auto& record : scanBatch.records) {
        WavRiffAnalysisStatus failureStatus = WavRiffAnalysisStatus::InvalidRule;
        QString failureMessage;
        if (!publishRecord(record, result, allowExecutionCancellation, &failureStatus,
                           &failureMessage)) {
            terminalizeFailure(failureStatus, std::move(failureMessage));
            return result;
        }
    }

    result.status = analysisStatus(scanBatch.status);
    result.errorMessage = scanBatch.errorMessage;

    switch (scanBatch.status) {
    case RiffChunkScanStatus::Complete:
        if (const auto root = tree_.node(tree_.rootId()); root && root->children().empty()) {
            markRootPartial(core::DiagnosticCode::InvalidSyntax,
                            core::MaterializationState::Invalid,
                            QStringLiteral("No RIFF/WAVE record was found"));
        } else if (!tree_.transition(tree_.rootId(), core::MaterializationState::Materialized)) {
            result.status = WavRiffAnalysisStatus::InvalidRule;
            result.errorMessage = QStringLiteral("Unable to materialize WAV analysis root");
            markRootPartial(core::DiagnosticCode::InvalidSyntax,
                            core::MaterializationState::Invalid,
                            result.errorMessage);
        }
        terminal_ = true;
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
        break;
    case RiffChunkScanStatus::InProgress:
        result.status = WavRiffAnalysisStatus::InProgress;
        result.errorMessage.clear();
        break;
    case RiffChunkScanStatus::Cancelled:
        terminalizeFailure(WavRiffAnalysisStatus::Cancelled,
                           QStringLiteral("RIFF scan was cancelled"));
        break;
    case RiffChunkScanStatus::SourceError:
        terminalizeFailure(WavRiffAnalysisStatus::SourceError,
                           result.errorMessage.isEmpty()
                               ? QStringLiteral("RIFF scan failed due to a source error")
                               : result.errorMessage);
        break;
    case RiffChunkScanStatus::InvalidBatchSize:
        result.status = WavRiffAnalysisStatus::InvalidBatchSize;
        break;
    }
    return result;
}

bool WavRiffAnalyzer::resumeAfterCancellation(
    std::optional<core::CancellationToken> cancellation,
    QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (!terminal_ || terminalStatus_ != WavRiffAnalysisStatus::Cancelled) {
        return true;
    }
    if (!tree_.resumeCancelled(tree_.rootId())) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to resume cancelled WAV analysis root");
        }
        return false;
    }
    cancellation_ = std::move(cancellation);
    scanner_.replaceCancellationToken(cancellation_);
    terminal_ = false;
    terminalStatus_ = WavRiffAnalysisStatus::InProgress;
    terminalErrorMessage_.clear();
    return true;
}

} // namespace streamview::rules
