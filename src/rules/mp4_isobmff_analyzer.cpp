#include <streamview/rules/mp4_isobmff_analyzer.h>

#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/dsl_ir.h>

#include <algorithm>
#include <limits>

namespace streamview::rules {

namespace {

class BoundedSourceView final : public core::RandomAccessSource {
public:
    BoundedSourceView(const core::RandomAccessSource& baseSource,
                      core::SourceMapping mapping,
                      quint64 sizeBytes)
        : baseSource_(&baseSource), mapping_(std::move(mapping)), sizeBytes_(sizeBytes) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return sizeBytes_; }
    [[nodiscard]] QString identity() const override { return baseSource_->identity(); }

    [[nodiscard]] core::SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (destination.empty()) {
            return {core::SourceReadStatus::Complete, 0, {}};
        }
        if (byteOffset >= sizeBytes_) {
            return {core::SourceReadStatus::EndOfSource, 0, {}};
        }
        const quint64 available = sizeBytes_ - byteOffset;
        const std::size_t count = static_cast<std::size_t>(std::min(static_cast<quint64>(destination.size()), available));

        auto range = core::LogicalRange::create(
            core::LogicalBitAddress(mapping_.viewId(), byteOffset * 8U), count * 8U);
        if (!range) {
            return {core::SourceReadStatus::Error, 0, QStringLiteral("Invalid range in container source view")};
        }
        auto locateRes = mapping_.locate(*range);
        if (!locateRes.has_value() || locateRes->sourceSpans().empty()) {
            return {core::SourceReadStatus::Error, 0, QStringLiteral("Failed to locate spans in container")};
        }

        std::size_t bytesFilled = 0;
        for (const auto& span : locateRes->sourceSpans()) {
            const quint64 spanStartByte = span.start().byteOffset();
            const std::size_t spanLength = static_cast<std::size_t>(span.bitLength() / 8U);
            const std::size_t toRead = std::min(spanLength, count - bytesFilled);
            auto readRes = baseSource_->readAt(spanStartByte, destination.subspan(bytesFilled, toRead));
            if (!readRes.complete()) {
                return readRes;
            }
            bytesFilled += readRes.bytesRead;
            if (bytesFilled >= count) break;
        }
        return {bytesFilled == destination.size() ? core::SourceReadStatus::Complete : core::SourceReadStatus::EndOfSource,
                bytesFilled, {}};
    }

private:
    const core::RandomAccessSource* baseSource_;
    core::SourceMapping mapping_;
    quint64 sizeBytes_;
};

} // namespace

RulePackageLoadResult loadMp4IsobmffRulePackage() {
    RulePackageLoadResult result;
    result.status = RulePackageLoadStatus::InvalidTree;
    result.errorMessage = QStringLiteral("No bundled MP4 rule package is installed");
    return result;
}

std::optional<Mp4IsobmffAnalyzer>
Mp4IsobmffAnalyzer::create(const core::RandomAccessSource& /*source*/,
                           QString* errorMessage,
                           std::optional<core::CancellationToken> /*cancellation*/) {
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("No installed package matches format: video/mp4");
    }
    return std::nullopt;
}

std::optional<Mp4IsobmffAnalyzer>
Mp4IsobmffAnalyzer::create(const core::RandomAccessSource& source,
                           const RuleCatalogLookupResult& resolvedRule,
                           QString* errorMessage,
                           std::optional<core::CancellationToken> cancellation) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (!resolvedRule.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = resolvedRule.errorMessage.isEmpty()
                                ? QStringLiteral("Resolved rule package is invalid")
                                : resolvedRule.errorMessage;
        }
        return std::nullopt;
    }

    const QByteArray* ruleBytes =
        resolvedRule.package->fileContents(resolvedRule.entryPoint->sourcePath);
    if (ruleBytes == nullptr || ruleBytes->isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Resolved MP4 rule source is missing or empty");
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
            *errorMessage = QStringLiteral("Resolved MP4 rule is invalid at %1:%2: %3")
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
                *errorMessage = QStringLiteral("Resolved MP4 rule failed static compilation");
            } else {
                const DslDiagnostic& diagnostic = compiled.diagnostics.front();
                *errorMessage =
                    QStringLiteral("Resolved MP4 rule failed static compilation at %1:%2: %3")
                        .arg(diagnostic.range.start.line)
                        .arg(diagnostic.range.start.column)
                        .arg(diagnostic.message);
            }
        }
        return std::nullopt;
    }

    quint32 headerStructIndex = 0;
    if (compiled.program->entry.kind == DslEntryKind::Sequence &&
        compiled.program->entry.targetIndex < compiled.program->scans.size()) {
        headerStructIndex = compiled.program->scans[compiled.program->entry.targetIndex].elementStructIndex;
    } else {
        bool found = false;
        for (quint32 i = 0; i < compiled.program->structs.size(); ++i) {
            if (compiled.program->structs[i].name == resolvedRule.entryPoint->id) {
                headerStructIndex = i;
                found = true;
                break;
            }
        }
        if (!found && !compiled.program->structs.empty()) {
            headerStructIndex = 0;
        }
    }

    auto treeOpt = core::AnalysisTree::create(QStringLiteral("mp4_isobmff"));
    if (!treeOpt.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Failed to create analysis tree");
        }
        return std::nullopt;
    }
    auto tree = std::make_shared<core::AnalysisTree>(std::move(*treeOpt));
    auto budget = std::make_shared<RunnerExecutionBudget>();
    budget->cancellation = cancellation;

    return Mp4IsobmffAnalyzer(source, std::move(*ruleIdentity), std::move(*compiled.program), headerStructIndex,
                             std::move(tree), std::move(budget), std::move(cancellation));
}

Mp4IsobmffAnalyzer::Mp4IsobmffAnalyzer(
    const core::RandomAccessSource& source,
    RuleEntryPointIdentity ruleIdentity,
    DslTypedProgram program,
    quint32 headerStructIndex,
    std::shared_ptr<core::AnalysisTree> tree,
    std::shared_ptr<RunnerExecutionBudget> budget,
    std::optional<core::CancellationToken> cancellation)
    : source_(&source)
    , ruleIdentity_(std::move(ruleIdentity))
    , program_(std::move(program))
    , headerStructIndex_(headerStructIndex)
    , scanner_(source, cancellation)
    , tree_(std::move(tree))
    , budget_(std::move(budget))
    , cancellation_(std::move(cancellation)) {}

std::optional<core::FieldLocation>
Mp4IsobmffAnalyzer::makeLocation(std::vector<core::SourceSpan> sourceSpans) {
    if (nextViewId_ == 0 || sourceSpans.empty()) {
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

void Mp4IsobmffAnalyzer::markRootPartial(core::DiagnosticCode code,
                                         core::MaterializationState state,
                                         const QString& message) {
    if (!tree_) return;
    core::ParseDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.severity = core::DiagnosticSeverity::Error;
    diagnostic.message = message;
    diagnostic.fieldPath = QStringLiteral("boxes");
    (void)tree_->markPartial(tree_->rootId(), state, std::move(diagnostic));
}

bool Mp4IsobmffAnalyzer::publishRecord(const Mp4BoxRecord& record,
                                      Mp4IsobmffAnalysisBatch& batch,
                                      bool /*allowExecutionCancellation*/,
                                      Mp4IsobmffAnalysisStatus* failureStatus,
                                      QString* errorMessage) {
    if (!record.boxSpan.has_value()) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::InvalidRule;
        if (errorMessage) *errorMessage = QStringLiteral("Scanner returned box record without span");
        return false;
    }

    if (cancellation_ && cancellation_->isCancellationRequested()) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::Cancelled;
        return false;
    }
    if (budget_ && budget_->cancellation && budget_->cancellation->isCancellationRequested()) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::Cancelled;
        return false;
    }
    if (budget_ && (budget_->remainingNodes == 0 || budget_->remainingInstructions == 0)) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
        return false;
    }

    const auto boxLocation = makeLocation({*record.boxSpan});
    if (!boxLocation.has_value()) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
        if (errorMessage) *errorMessage = QStringLiteral("Failed to map box location");
        return false;
    }

    const quint64 boxIndex = nextBoxIndex_;
    core::AnalysisNodeSpec boxSpec;
    boxSpec.kind = core::AnalysisNodeKind::Region;
    boxSpec.name = QStringLiteral("box[%1]").arg(boxIndex);
    boxSpec.state = core::MaterializationState::Indexing;
    boxSpec.location = *boxLocation;

    const auto boxNode = tree_->appendChild(tree_->rootId(), std::move(boxSpec));
    if (!boxNode.has_value()) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
        if (errorMessage) *errorMessage = QStringLiteral("Unable to append box node to analysis tree");
        return false;
    }
    ++nextBoxIndex_;

    if (nextViewId_ == 0) {
        if (errorMessage) *errorMessage = QStringLiteral("Logical view identifier limit reached");
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
        return false;
    }
    const core::LogicalViewId boxViewId(nextViewId_);
    nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;

    const auto mapping = core::SourceMapping::create(boxViewId, {*record.boxSpan});
    if (!mapping) {
        if (errorMessage) *errorMessage = QStringLiteral("Unable to create box source mapping");
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
        return false;
    }

    core::BitReader reader(*source_, *mapping);

    DslExecutionOptions options;
    if (budget_) {
        options.limits.maximumMaterializedNodes = budget_->remainingNodes;
        options.limits.maximumInstructions = budget_->remainingInstructions;
        if (budget_->cancellation) {
            options.cancellation = budget_->cancellation;
        }
    }
    if (cancellation_) {
        options.cancellation = cancellation_;
    }

    auto execResult = DslExecutor::decodeStruct(
        program_, headerStructIndex_, reader, *mapping, 0, *tree_, *boxNode, options);

    if (budget_) {
        budget_->remainingNodes = (execResult.nodesCreated >= budget_->remainingNodes)
                                      ? 0
                                      : (budget_->remainingNodes - execResult.nodesCreated);
        budget_->remainingInstructions =
            (execResult.instructionsExecuted >= budget_->remainingInstructions)
                ? 0
                : (budget_->remainingInstructions - execResult.instructionsExecuted);
    }

    if (record.truncated) {
        core::ParseDiagnostic diag;
        diag.code = core::DiagnosticCode::TruncatedSource;
        diag.severity = core::DiagnosticSeverity::Warning;
        diag.message = QStringLiteral("Truncated box header or payload at end of stream");
        diag.fieldPath = QStringLiteral("box[%1]").arg(boxIndex);
        diag.location = *boxLocation;
        (void)tree_->addDiagnostic(*boxNode, std::move(diag));
    }

    batch.boxNodes.push_back(*boxNode);

    if (execResult.status == DslExecutionStatus::Cancelled) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::Cancelled;
        return false;
    }
    if (execResult.status == DslExecutionStatus::ResourceLimit) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
        return false;
    }
    if (execResult.status == DslExecutionStatus::SourceError) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
        return false;
    }
    if (execResult.status == DslExecutionStatus::InvalidDefinition) {
        if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::InvalidRule;
        return false;
    }

    // Drill containers inside this box node
    if (!recursivelyDrillContainer(*boxNode, 0, *boxLocation, failureStatus, errorMessage)) {
        return false;
    }

    auto currBoxOpt = tree_->node(*boxNode);
    if (currBoxOpt && currBoxOpt->state() == core::MaterializationState::Indexing) {
        (void)tree_->transition(*boxNode, core::MaterializationState::Materialized);
    }

    return true;
}

bool Mp4IsobmffAnalyzer::recursivelyDrillContainer(
    core::AnalysisNodeId nodeId,
    quint32 /*unused*/,
    const core::FieldLocation& parentLocation,
    Mp4IsobmffAnalysisStatus* failureStatus,
    QString* errorMessage) {
    auto nodeOpt = tree_->node(nodeId);
    if (!nodeOpt.has_value()) {
        return true;
    }

    const auto childNodeIds = nodeOpt->children();
    for (const auto childId : childNodeIds) {
        auto childOpt = tree_->node(childId);
        if (!childOpt.has_value()) continue;

        const auto& meta = childOpt->metadata();
        if (meta.containerChildStructIndex.has_value()) {
            const quint32 childStructIndex = *meta.containerChildStructIndex;
            if (childStructIndex >= program_.structs.size()) {
                if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::InvalidRule;
                if (errorMessage) *errorMessage = QStringLiteral("Container child struct index is out of range");
                return false;
            }

            if (budget_ && budget_->currentNestingDepth >= 256) {
                if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
                if (errorMessage) *errorMessage = QStringLiteral("Maximum container nesting depth 256 exceeded");
                return false;
            }

            if (budget_) {
                budget_->currentNestingDepth++;
            }

            (void)tree_->transition(childId, core::MaterializationState::Indexing);

            const auto& containerLoc = childOpt->location();
            if (containerLoc.has_value()) {
                const quint64 containerByteLength = containerLoc->logicalRange().bitLength() / 8U;
                if (containerByteLength >= 8) {
                    const auto containerMapping = core::SourceMapping::create(
                        containerLoc->logicalRange().start().viewId(), containerLoc->sourceSpans());
                    if (containerMapping.has_value()) {
                        BoundedSourceView containerSource(*source_, *containerMapping, containerByteLength);
                        Mp4BoxScanner containerScanner(containerSource, cancellation_);

                        while (!containerScanner.finished()) {
                            auto scanBatchRes = containerScanner.scanBatch(256, 256U * 1024U);
                            if (scanBatchRes.status == Mp4BoxScanStatus::Cancelled) {
                                if (budget_) budget_->currentNestingDepth--;
                                if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::Cancelled;
                                return false;
                            }
                            if (scanBatchRes.status == Mp4BoxScanStatus::SourceError) {
                                if (budget_) budget_->currentNestingDepth--;
                                if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
                                return false;
                            }

                            for (const auto& childRecord : scanBatchRes.records) {
                                if (!childRecord.boxSpan.has_value()) continue;

                                if (budget_ && (budget_->remainingNodes == 0 || budget_->remainingInstructions == 0)) {
                                    if (budget_) budget_->currentNestingDepth--;
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
                                    return false;
                                }

                                // Map child record span back to base source coordinates
                                auto childLogicalRange = core::LogicalRange::create(
                                    core::LogicalBitAddress(containerMapping->viewId(), childRecord.boxSpan->start().absoluteBitOffset()),
                                    childRecord.boxSpan->bitLength());
                                if (!childLogicalRange) continue;

                                auto childSpansRes = containerMapping->locate(*childLogicalRange);
                                if (!childSpansRes.has_value() || childSpansRes->sourceSpans().empty()) continue;

                                auto childLoc = makeLocation(childSpansRes->sourceSpans());
                                if (!childLoc.has_value()) continue;

                                if (nextViewId_ == 0) continue;
                                const core::LogicalViewId childViewId(nextViewId_);
                                nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;

                                const auto childMapping = core::SourceMapping::create(childViewId, childSpansRes->sourceSpans());
                                if (!childMapping) continue;

                                core::BitReader childReader(*source_, *childMapping);

                                DslExecutionOptions childOptions;
                                if (budget_) {
                                    childOptions.limits.maximumMaterializedNodes = budget_->remainingNodes;
                                    childOptions.limits.maximumInstructions = budget_->remainingInstructions;
                                    if (budget_->cancellation) {
                                        childOptions.cancellation = budget_->cancellation;
                                    }
                                }
                                if (cancellation_) {
                                    childOptions.cancellation = cancellation_;
                                }

                                auto childExec = DslExecutor::decodeStruct(
                                    program_, childStructIndex, childReader, *childMapping,
                                    0, *tree_, childId, childOptions);

                                if (budget_) {
                                    budget_->remainingNodes =
                                        (childExec.nodesCreated >= budget_->remainingNodes)
                                            ? 0
                                            : (budget_->remainingNodes - childExec.nodesCreated);
                                    budget_->remainingInstructions =
                                        (childExec.instructionsExecuted >= budget_->remainingInstructions)
                                            ? 0
                                            : (budget_->remainingInstructions - childExec.instructionsExecuted);
                                }

                                if (childRecord.truncated && childExec.structureNode.has_value()) {
                                    core::ParseDiagnostic diag;
                                    diag.code = core::DiagnosticCode::TruncatedSource;
                                    diag.severity = core::DiagnosticSeverity::Warning;
                                    diag.message = QStringLiteral("Truncated box header or payload in container");
                                    diag.fieldPath = QStringLiteral("child_box");
                                    diag.location = *childLoc;
                                    (void)tree_->addDiagnostic(*childExec.structureNode, std::move(diag));
                                }

                                if (childExec.status == DslExecutionStatus::Cancelled) {
                                    if (budget_) budget_->currentNestingDepth--;
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::Cancelled;
                                    return false;
                                }
                                if (childExec.status == DslExecutionStatus::ResourceLimit) {
                                    if (budget_) budget_->currentNestingDepth--;
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
                                    return false;
                                }
                                if (childExec.status == DslExecutionStatus::SourceError) {
                                    if (budget_) budget_->currentNestingDepth--;
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
                                    return false;
                                }

                                // Drill nested children if this child is also a container
                                if (childExec.structureNode.has_value() &&
                                    !recursivelyDrillContainer(*childExec.structureNode, 0, *childLoc, failureStatus, errorMessage)) {
                                    if (budget_) budget_->currentNestingDepth--;
                                    return false;
                                }
                            }
                        }
                    }
                }
            }

            (void)tree_->transition(childId, core::MaterializationState::Materialized);

            if (budget_) {
                budget_->currentNestingDepth--;
            }
        } else {
            // Not a direct container lazy node, but check its children
            if (!recursivelyDrillContainer(childId, 0, parentLocation, failureStatus, errorMessage)) {
                return false;
            }
        }
    }

    return true;
}

Mp4IsobmffAnalysisBatch Mp4IsobmffAnalyzer::analyzeBatch(
    std::size_t maximumRecords,
    quint64 maximumInspectedPositions) {
    Mp4IsobmffAnalysisBatch batch;

    if (maximumRecords == 0 || maximumInspectedPositions == 0) {
        batch.status = Mp4IsobmffAnalysisStatus::InvalidBatchSize;
        batch.errorMessage = QStringLiteral("Batch parameters must be greater than zero");
        return batch;
    }

    if (terminal_) {
        batch.status = Mp4IsobmffAnalysisStatus::Complete;
        return batch;
    }

    auto scanBatchRes = scanner_.scanBatch(maximumRecords, maximumInspectedPositions);

    if (scanBatchRes.status == Mp4BoxScanStatus::Cancelled) {
        batch.status = Mp4IsobmffAnalysisStatus::Cancelled;
        batch.errorMessage = scanBatchRes.errorMessage;
        markRootPartial(core::DiagnosticCode::Cancelled, core::MaterializationState::Cancelled, scanBatchRes.errorMessage);
        return batch;
    }
    if (scanBatchRes.status == Mp4BoxScanStatus::SourceError) {
        batch.status = Mp4IsobmffAnalysisStatus::SourceError;
        batch.errorMessage = scanBatchRes.errorMessage;
        markRootPartial(core::DiagnosticCode::SourceError, core::MaterializationState::Invalid, scanBatchRes.errorMessage);
        return batch;
    }
    if (scanBatchRes.status == Mp4BoxScanStatus::InvalidBatchSize) {
        batch.status = Mp4IsobmffAnalysisStatus::InvalidBatchSize;
        batch.errorMessage = scanBatchRes.errorMessage;
        return batch;
    }

    for (const auto& record : scanBatchRes.records) {
        Mp4IsobmffAnalysisStatus failStatus = Mp4IsobmffAnalysisStatus::InProgress;
        QString errMsg;
        if (!publishRecord(record, batch, true, &failStatus, &errMsg)) {
            batch.status = failStatus;
            batch.errorMessage = errMsg;
            return batch;
        }
    }

    if (scanner_.finished()) {
        terminal_ = true;
        batch.status = Mp4IsobmffAnalysisStatus::Complete;
    } else {
        batch.status = Mp4IsobmffAnalysisStatus::InProgress;
    }

    return batch;
}

bool Mp4IsobmffAnalyzer::resumeAfterCancellation(
    std::optional<core::CancellationToken> cancellation,
    QString* /*errorMessage*/) {
    cancellation_ = std::move(cancellation);
    if (budget_) {
        budget_->cancellation = cancellation_;
    }
    if (tree_) {
        (void)tree_->resumeCancelled(tree_->rootId());
    }
    terminal_ = false;
    scanner_.replaceCancellationToken(cancellation_);
    return true;
}

std::optional<WindowDecoder> Mp4IsobmffAnalyzer::windowDecoder(core::AnalysisNodeId windowNodeId) const {
    if (!tree_) return std::nullopt;
    auto nodeOpt = tree_->node(windowNodeId);
    if (!nodeOpt.has_value() || !nodeOpt->metadata().window.has_value()) {
        return std::nullopt;
    }
    const auto& locOpt = nodeOpt->location();
    if (!locOpt.has_value()) {
        return std::nullopt;
    }
    const auto mapping = core::SourceMapping::create(locOpt->logicalRange().start().viewId(), locOpt->sourceSpans());
    if (!mapping.has_value()) {
        return std::nullopt;
    }
    return WindowDecoder(program_, *source_, *mapping, tree_, windowNodeId, budget_, cancellation_);
}

} // namespace streamview::rules
