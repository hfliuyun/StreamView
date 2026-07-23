#include <streamview/rules/h264_annex_b_analyzer.h>

#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/rules/dsl_executor.h>

#include <QFile>
#include <QIODevice>

#include <limits>
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

} // namespace

QString h264AnnexBRuleSource(QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    initializeStreamViewOfficialRules();

    QFile file(QStringLiteral(":/streamview/rules/h264_annex_b.svfmt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return {};
    }

    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return {};
    }
    return QString::fromUtf8(contents);
}

std::optional<H264AnnexBAnalyzer>
H264AnnexBAnalyzer::create(const core::RandomAccessSource& source,
                           QString* errorMessage,
                           std::optional<core::CancellationToken> cancellation) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    QString loadError;
    const QString ruleSource = h264AnnexBRuleSource(&loadError);
    if (ruleSource.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = loadError.isEmpty() ? QStringLiteral("Bundled H.264 rule is empty")
                                                : loadError;
        }
        return std::nullopt;
    }

    const DslParseResult parsed = DslParser::parse(ruleSource);
    if (!parsed.succeeded()) {
        if (errorMessage != nullptr) {
            const DslDiagnostic& diagnostic = parsed.diagnostics.front();
            *errorMessage = QStringLiteral("Bundled H.264 rule is invalid at %1:%2: %3")
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
                *errorMessage = QStringLiteral("Bundled H.264 rule failed static compilation");
            } else {
                const DslDiagnostic& diagnostic = compiled.diagnostics.front();
                *errorMessage =
                    QStringLiteral("Bundled H.264 rule failed static compilation at %1:%2: %3")
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
            *errorMessage = QStringLiteral("Bundled H.264 rule has no Annex B entry scan");
        }
        return std::nullopt;
    }
    const DslTypedScan& entryScan =
        compiled.program->scans.at(compiled.program->entry.targetIndex);
    if (entryScan.scanner != DslScannerKind::H264StartCode ||
        entryScan.elementStructIndex >= compiled.program->structs.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Bundled H.264 rule has no Annex B entry scan");
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
                                std::move(*compiled.program),
                                elementStructIndex,
                                std::move(*tree));
    return std::optional<H264AnnexBAnalyzer>(std::move(analyzer));
}

H264AnnexBAnalyzer::H264AnnexBAnalyzer(const core::RandomAccessSource& source,
                                       std::optional<core::CancellationToken> cancellation,
                                       DslTypedProgram program,
                                       quint32 elementStructIndex,
                                       core::AnalysisTree tree)
    : source_(&source), scanner_(source, cancellation), cancellation_(std::move(cancellation)),
      program_(std::move(program)), elementStructIndex_(elementStructIndex),
      tree_(std::move(tree)) {}

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
    const auto nalLocation = makeLocation(std::move(nalSpans));
    const auto startCodeLocation = makeLocation({*record.startCode});
    if (!nalLocation || !startCodeLocation) {
        *errorMessage = QStringLiteral("Unable to map Annex B record to source coordinates");
        return false;
    }

    core::AnalysisNodeSpec nalSpec;
    nalSpec.kind = core::AnalysisNodeKind::Region;
    nalSpec.name = QStringLiteral("nal_unit[%1]").arg(nextNalUnitIndex_);
    nalSpec.state = core::MaterializationState::Indexing;
    nalSpec.location = *nalLocation;
    const auto nalNode = tree_.appendChild(tree_.rootId(), std::move(nalSpec));
    if (!nalNode) {
        *errorMessage = QStringLiteral("Unable to append NAL unit to analysis tree");
        return false;
    }
    batch.nalUnitNodes.push_back(*nalNode);
    ++nextNalUnitIndex_;

    core::AnalysisNodeSpec startCodeSpec;
    startCodeSpec.kind = core::AnalysisNodeKind::Region;
    startCodeSpec.name = QStringLiteral("start_code");
    startCodeSpec.state = core::MaterializationState::Materialized;
    startCodeSpec.location = *startCodeLocation;
    if (!tree_.appendChild(*nalNode, std::move(startCodeSpec))) {
        *errorMessage = QStringLiteral("Unable to append start code to analysis tree");
        return false;
    }

    const core::SourceBitAddress headerStart = record.nalUnit
                                                   ? record.nalUnit->start()
                                                   : record.startCode->endExclusive();
    const quint64 headerBitLength = record.nalUnit ? 8 : 0;
    const auto headerSpan = core::SourceSpan::create(headerStart, headerBitLength);
    if (!headerSpan) {
        *errorMessage = QStringLiteral("NAL header exceeds source coordinate limits");
        return false;
    }
    if (nextViewId_ == 0) {
        *errorMessage = QStringLiteral("Logical view identifier limit reached");
        return false;
    }
    const core::LogicalViewId headerViewId(nextViewId_);
    nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;
    std::vector<core::SourceSpan> headerSpans;
    if (headerSpan->bitLength() != 0) {
        headerSpans.push_back(*headerSpan);
    }
    const auto mapping = core::SourceMapping::create(headerViewId, std::move(headerSpans));
    if (!mapping) {
        *errorMessage = QStringLiteral("Unable to create direct NAL header mapping");
        return false;
    }

    core::BitReader reader(*source_, *headerSpan);
    DslExecutionOptions executionOptions;
    if (allowExecutionCancellation) {
        executionOptions.cancellation = cancellation_;
    }
    const DslExecutionResult execution = DslExecutor::decodeStruct(program_,
                                                                    elementStructIndex_,
                                                                    reader,
                                                                    *mapping,
                                                                    0,
                                                                    tree_,
                                                                    *nalNode,
                                                                    executionOptions);
    if (!execution.materialized()) {
        if (!execution.structureNode) {
            *errorMessage = execution.errorMessage.isEmpty()
                              ? QStringLiteral("Unable to decode NAL unit header")
                              : execution.errorMessage;
            if (execution.status == DslExecutionStatus::Cancelled) {
                *failureStatus = H264AnnexBAnalysisStatus::Cancelled;
            } else if (execution.status == DslExecutionStatus::ResourceLimit) {
                *failureStatus = H264AnnexBAnalysisStatus::ResourceLimit;
            } else if (execution.status == DslExecutionStatus::SourceError) {
                *failureStatus = H264AnnexBAnalysisStatus::SourceError;
            }
            return false;
        }
        if (execution.status == DslExecutionStatus::InvalidDefinition) {
            *errorMessage = execution.errorMessage.isEmpty()
                              ? QStringLiteral("Unable to decode NAL unit header")
                              : execution.errorMessage;
            return false;
        }

        const auto structure = tree_.node(*execution.structureNode);
        if (!structure || structure->diagnostics().empty()) {
            *errorMessage = execution.errorMessage.isEmpty()
                              ? QStringLiteral("NAL unit header failed without a diagnostic")
                              : execution.errorMessage;
            return false;
        }
        core::ParseDiagnostic diagnostic = structure->diagnostics().front();
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
        const auto nalState = execution.status == DslExecutionStatus::Cancelled
                                  ? core::MaterializationState::Cancelled
                                  : core::MaterializationState::Invalid;
        if (!tree_.markPartial(*nalNode, nalState, std::move(diagnostic))) {
            *errorMessage = QStringLiteral("Unable to mark NAL unit as a partial result");
            return false;
        }
        *errorMessage = execution.errorMessage.isEmpty()
                          ? QStringLiteral("NAL unit header contains invalid or truncated syntax")
                          : execution.errorMessage;
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
    if (!tree_.transition(*nalNode, core::MaterializationState::Materialized)) {
        *errorMessage = QStringLiteral("Unable to materialize NAL unit node");
        return false;
    }
    return true;
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
    std::size_t maximumRecords, quint64 maximumInspectedPositions) {
    H264AnnexBAnalysisBatch result;
    if (terminal_) {
        result.status = terminalStatus_;
        result.errorMessage = terminalErrorMessage_;
        return result;
    }

    const bool cancellationRequestedBeforeScan =
        cancellation_ && cancellation_->isCancellationRequested();
    const StartCodeScanBatch scanBatch =
        scanner_.scanBatch(maximumRecords, maximumInspectedPositions);
    result.status = analysisStatus(scanBatch.status);
    result.errorMessage = scanBatch.errorMessage;
    const bool cancellationRequestedAfterScan =
        cancellation_ && cancellation_->isCancellationRequested();
    const bool allowExecutionCancellation =
        cancellation_.has_value() && !cancellationRequestedBeforeScan &&
        !cancellationRequestedAfterScan;
    for (const H264StartCodeRecord& record : scanBatch.records) {
        QString publishError;
        H264AnnexBAnalysisStatus failureStatus = H264AnnexBAnalysisStatus::InvalidRule;
        if (!publishRecord(record,
                           result,
                           allowExecutionCancellation,
                           &failureStatus,
                           &publishError)) {
            result.status = failureStatus;
            result.errorMessage = publishError;
            terminal_ = true;
            terminalStatus_ = result.status;
            terminalErrorMessage_ = result.errorMessage;
            const auto rootState = result.status == H264AnnexBAnalysisStatus::Cancelled
                                       ? core::MaterializationState::Cancelled
                                       : core::MaterializationState::Invalid;
            markRootPartial(diagnosticCode(result.status),
                            rootState,
                            result.errorMessage);
            return result;
        }
    }

    switch (scanBatch.status) {
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
        markRootPartial(core::DiagnosticCode::Cancelled,
                        core::MaterializationState::Cancelled,
                        QStringLiteral("H.264 Annex B scan was cancelled"));
        terminal_ = true;
        break;
    case StartCodeScanStatus::SourceError:
        markRootPartial(core::DiagnosticCode::SourceError,
                        core::MaterializationState::Invalid,
                        result.errorMessage);
        terminal_ = true;
        break;
    case StartCodeScanStatus::InProgress:
    case StartCodeScanStatus::InvalidBatchSize:
        break;
    }

    if (terminal_) {
        terminalStatus_ = result.status;
        terminalErrorMessage_ = result.errorMessage;
    }
    return result;
}

} // namespace streamview::rules
