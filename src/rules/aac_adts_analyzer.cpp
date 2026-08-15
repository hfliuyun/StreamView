#include <streamview/rules/aac_adts_analyzer.h>

#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/dsl_ir.h>

#include <limits>
#include <utility>

namespace streamview::rules {

namespace {

[[nodiscard]] core::DiagnosticCode diagnosticCode(AacAdtsAnalysisStatus status) noexcept {
    switch (status) {
    case AacAdtsAnalysisStatus::Cancelled:
        return core::DiagnosticCode::Cancelled;
    case AacAdtsAnalysisStatus::SourceError:
        return core::DiagnosticCode::SourceError;
    case AacAdtsAnalysisStatus::ResourceLimit:
        return core::DiagnosticCode::ResourceLimit;
    case AacAdtsAnalysisStatus::InProgress:
    case AacAdtsAnalysisStatus::Complete:
    case AacAdtsAnalysisStatus::InvalidRule:
    case AacAdtsAnalysisStatus::InvalidBatchSize:
        return core::DiagnosticCode::InvalidSyntax;
    }
    return core::DiagnosticCode::InvalidSyntax;
}

[[nodiscard]] AacAdtsAnalysisStatus analysisStatus(AacAdtsScanStatus scanStatus) noexcept {
    switch (scanStatus) {
    case AacAdtsScanStatus::InProgress:
        return AacAdtsAnalysisStatus::InProgress;
    case AacAdtsScanStatus::Complete:
        return AacAdtsAnalysisStatus::Complete;
    case AacAdtsScanStatus::Cancelled:
        return AacAdtsAnalysisStatus::Cancelled;
    case AacAdtsScanStatus::SourceError:
        return AacAdtsAnalysisStatus::SourceError;
    case AacAdtsScanStatus::InvalidBatchSize:
        return AacAdtsAnalysisStatus::InvalidBatchSize;
    }
    return AacAdtsAnalysisStatus::InvalidRule;
}

} // namespace

std::optional<AacAdtsAnalyzer>
AacAdtsAnalyzer::create(const core::RandomAccessSource& /*source*/,
                       QString* errorMessage,
                       std::optional<core::CancellationToken> /*cancellation*/) {
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("No AAC ADTS rule package is bundled");
    }
    return std::nullopt;
}

std::optional<AacAdtsAnalyzer>
AacAdtsAnalyzer::create(const core::RandomAccessSource& source,
                       const RuleCatalogLookupResult& resolvedRule,
                       QString* errorMessage,
                       std::optional<core::CancellationToken> cancellation) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (!resolvedRule.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = resolvedRule.errorMessage.isEmpty()
                                ? QStringLiteral("AAC ADTS rule was not resolved exactly")
                                : resolvedRule.errorMessage;
        }
        return std::nullopt;
    }

    const QByteArray* ruleBytes =
        resolvedRule.package->fileContents(resolvedRule.entryPoint->sourcePath);
    if (ruleBytes == nullptr || ruleBytes->isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Resolved AAC ADTS rule source is missing or empty");
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
            *errorMessage = QStringLiteral("Resolved AAC rule is invalid at %1:%2: %3")
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
                *errorMessage = QStringLiteral("Resolved AAC rule failed static compilation");
            } else {
                const DslDiagnostic& diagnostic = compiled.diagnostics.front();
                *errorMessage =
                    QStringLiteral("Resolved AAC rule failed static compilation at %1:%2: %3")
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
            *errorMessage = QStringLiteral("Resolved AAC rule has no ADTS frame entry scan");
        }
        return std::nullopt;
    }

    const DslTypedScan& entryScan =
        compiled.program->scans.at(compiled.program->entry.targetIndex);
    if (entryScan.scanner != DslScannerKind::AacAdtsFrame ||
        entryScan.elementStructIndex >= compiled.program->structs.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Resolved AAC rule has no ADTS frame entry scan");
        }
        return std::nullopt;
    }

    auto tree = core::AnalysisTree::create(QStringLiteral("adts_stream"));
    if (!tree) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to initialize AAC analysis tree");
        }
        return std::nullopt;
    }

    return AacAdtsAnalyzer(source, std::move(*ruleIdentity), std::move(*compiled.program),
                          entryScan.elementStructIndex, std::move(*tree),
                          std::move(cancellation));
}

AacAdtsAnalyzer::AacAdtsAnalyzer(const core::RandomAccessSource& source,
                                 RuleEntryPointIdentity ruleIdentity,
                                 DslTypedProgram program,
                                 quint32 headerStructIndex,
                                 core::AnalysisTree tree,
                                 std::optional<core::CancellationToken> cancellation)
    : source_(&source),
      ruleIdentity_(std::move(ruleIdentity)),
      executionSession_(std::move(program)),
      headerStructIndex_(headerStructIndex),
      scanner_(source, cancellation),
      tree_(std::move(tree)),
      cancellation_(std::move(cancellation)),
      nextFrameIndex_(0),
      nextViewId_(1),
      terminal_(false),
      terminalStatus_(AacAdtsAnalysisStatus::InProgress) {}

std::optional<core::FieldLocation>
AacAdtsAnalyzer::makeLocation(std::vector<core::SourceSpan> sourceSpans) {
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

void AacAdtsAnalyzer::markRootPartial(core::DiagnosticCode code,
                                      core::MaterializationState state,
                                      const QString& message) {
    core::ParseDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.severity = core::DiagnosticSeverity::Error;
    diagnostic.message = message;
    diagnostic.fieldPath = QStringLiteral("frames");
    (void)tree_.markPartial(tree_.rootId(), state, std::move(diagnostic));
}

bool AacAdtsAnalyzer::publishRecord(const AacAdtsRecord& record,
                                    AacAdtsAnalysisBatch& batch,
                                    bool allowExecutionCancellation,
                                    AacAdtsAnalysisStatus* failureStatus,
                                    QString* errorMessage) {
    *failureStatus = AacAdtsAnalysisStatus::InvalidRule;
    if (!record.frameSpan) {
        *errorMessage = QStringLiteral("ADTS scanner returned a record without a frame span");
        return false;
    }

    const auto frameLocation = makeLocation({*record.frameSpan});
    if (!frameLocation) {
        *errorMessage = QStringLiteral("Unable to map ADTS frame to source coordinates");
        return false;
    }

    const quint64 frameIndex = nextFrameIndex_;
    core::AnalysisNodeSpec frameSpec;
    frameSpec.kind = core::AnalysisNodeKind::Region;
    frameSpec.name = QStringLiteral("adts_frame[%1]").arg(frameIndex);
    frameSpec.state = core::MaterializationState::Indexing;
    frameSpec.location = *frameLocation;

    const auto frameNode = tree_.appendChild(tree_.rootId(), std::move(frameSpec));
    if (!frameNode) {
        *errorMessage = QStringLiteral("Unable to append ADTS frame to analysis tree");
        return false;
    }
    ++nextFrameIndex_;

    const auto failPublishedFrame = [this,
                                     &batch,
                                     &frameLocation,
                                     frameNode,
                                     frameIndex,
                                     failureStatus,
                                     errorMessage](QString message) {
        *errorMessage = std::move(message);
        const auto node = tree_.node(*frameNode);
        if (node && node->state() == core::MaterializationState::Indexing) {
            core::ParseDiagnostic diagnostic;
            diagnostic.code = diagnosticCode(*failureStatus);
            diagnostic.severity = core::DiagnosticSeverity::Error;
            diagnostic.message = *errorMessage;
            diagnostic.fieldPath = QStringLiteral("adts_frame[%1]").arg(frameIndex);
            diagnostic.location = *frameLocation;
            (void)tree_.markPartial(*frameNode,
                                    *failureStatus == AacAdtsAnalysisStatus::Cancelled
                                        ? core::MaterializationState::Cancelled
                                        : core::MaterializationState::Invalid,
                                    std::move(diagnostic));
        }
        batch.frameNodes.push_back(*frameNode);
        return false;
    };

    if (record.headerSpan && record.headerSpan->bitLength() > 0) {
        if (nextViewId_ == 0) {
            return failPublishedFrame(QStringLiteral("Logical view identifier limit reached"));
        }
        const core::LogicalViewId headerViewId(nextViewId_);
        nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;

        const auto mapping = core::SourceMapping::create(headerViewId, {*record.headerSpan});
        if (!mapping) {
            return failPublishedFrame(QStringLiteral("Unable to create ADTS header mapping"));
        }

        core::BitReader reader(*source_, *mapping);
        DslExecutionOptions executionOptions;
        if (allowExecutionCancellation) {
            executionOptions.cancellation = cancellation_;
        }

        const DslExecutionResult execution = DslExecutor::decodeStruct(
            executionSession_.program(),
            headerStructIndex_,
            reader,
            *mapping,
            0,
            tree_,
            *frameNode,
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
                                         ? QStringLiteral("Unable to decode ADTS frame header")
                                         : execution.errorMessage;
                diagnostic.fieldPath = QStringLiteral("adts_frame[%1]").arg(frameIndex);
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
            if (execution.status == DslExecutionStatus::Cancelled) {
                *failureStatus = AacAdtsAnalysisStatus::Cancelled;
            } else if (execution.status == DslExecutionStatus::ResourceLimit) {
                *failureStatus = AacAdtsAnalysisStatus::ResourceLimit;
            } else if (execution.status == DslExecutionStatus::SourceError) {
                *failureStatus = AacAdtsAnalysisStatus::SourceError;
            } else {
                *failureStatus = AacAdtsAnalysisStatus::InvalidRule;
            }
            return failPublishedFrame(diagnostic.message);
        }
    }

    if (!tree_.transition(*frameNode, core::MaterializationState::Materialized)) {
        *errorMessage = QStringLiteral("Unable to materialize decoded ADTS frame node");
        return failPublishedFrame(*errorMessage);
    }

    batch.frameNodes.push_back(*frameNode);
    return true;
}

AacAdtsAnalysisBatch AacAdtsAnalyzer::analyzeBatch(
    std::size_t maximumRecords,
    quint64 maximumInspectedPositions) {
    AacAdtsAnalysisBatch result;
    if (terminal_) {
        result.status = terminalStatus_;
        result.errorMessage = terminalErrorMessage_;
        return result;
    }
    if (maximumRecords == 0 || maximumInspectedPositions == 0) {
        result.status = AacAdtsAnalysisStatus::InvalidBatchSize;
        result.errorMessage = QStringLiteral(
            "Maximum records and inspected positions must be greater than zero");
        return result;
    }

    const auto terminalizeFailure = [this, &result](AacAdtsAnalysisStatus status,
                                                    QString message) {
        result.status = status;
        result.errorMessage = std::move(message);
        terminal_ = true;
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
        const auto rootState = result.status == AacAdtsAnalysisStatus::Cancelled
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
        AacAdtsAnalysisStatus failureStatus = AacAdtsAnalysisStatus::InvalidRule;
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
    case AacAdtsScanStatus::Complete:
        if (const auto root = tree_.node(tree_.rootId()); root && root->children().empty()) {
            markRootPartial(core::DiagnosticCode::InvalidSyntax,
                            core::MaterializationState::Invalid,
                            QStringLiteral("No AAC ADTS frame was found"));
        } else if (!tree_.transition(tree_.rootId(), core::MaterializationState::Materialized)) {
            result.status = AacAdtsAnalysisStatus::InvalidRule;
            result.errorMessage = QStringLiteral("Unable to materialize AAC analysis root");
            markRootPartial(core::DiagnosticCode::InvalidSyntax,
                            core::MaterializationState::Invalid,
                            result.errorMessage);
        }
        terminal_ = true;
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
        break;
    case AacAdtsScanStatus::Cancelled:
        if (result.errorMessage.isEmpty()) {
            result.errorMessage = QStringLiteral("AAC ADTS scan was cancelled");
        }
        markRootPartial(core::DiagnosticCode::Cancelled,
                        core::MaterializationState::Cancelled,
                        result.errorMessage);
        terminal_ = true;
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
        break;
    case AacAdtsScanStatus::SourceError:
        markRootPartial(core::DiagnosticCode::SourceError,
                        core::MaterializationState::Invalid,
                        result.errorMessage);
        terminal_ = true;
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
        break;
    case AacAdtsScanStatus::InProgress:
        result.status = AacAdtsAnalysisStatus::InProgress;
        result.errorMessage.clear();
        break;
    case AacAdtsScanStatus::InvalidBatchSize:
        result.status = AacAdtsAnalysisStatus::InvalidRule;
        result.errorMessage = QStringLiteral("AAC scanner rejected a validated analysis batch");
        markRootPartial(core::DiagnosticCode::InvalidSyntax,
                        core::MaterializationState::Invalid,
                        result.errorMessage);
        terminal_ = true;
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
        break;
    }

    return result;
}

bool AacAdtsAnalyzer::resumeAfterCancellation(
    std::optional<core::CancellationToken> cancellation,
    QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (!terminal_ || terminalStatus_ != AacAdtsAnalysisStatus::Cancelled) {
        return true;
    }
    if (!tree_.resumeCancelled(tree_.rootId())) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to resume cancelled AAC analysis root");
        }
        return false;
    }
    cancellation_ = std::move(cancellation);
    scanner_.replaceCancellationToken(cancellation_);
    terminal_ = false;
    terminalStatus_ = AacAdtsAnalysisStatus::InProgress;
    terminalErrorMessage_.clear();
    return true;
}

} // namespace streamview::rules
