#include <streamview/rules/mp4_isobmff_analyzer.h>

#include <streamview/core/bit_reader.h>
#include <streamview/core/coordinates.h>
#include <streamview/core/version.h>
#include <streamview/rules/dsl.h>
#include <streamview/rules/dsl_executor.h>
#include <streamview/rules/dsl_ir.h>
#include <streamview/rules/language_version.h>

#include <QFile>

#include <algorithm>
#include <limits>

static void initializeStreamViewOfficialRulesMp4() {
    Q_INIT_RESOURCE(streamview_official_rules_mp4);
}

namespace streamview::rules {

namespace {

[[nodiscard]] std::optional<QByteArray> readBundledPackageFile(const QString& resourcePath,
                                                               QString* errorMessage) {
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to read bundled MP4 resource %1: %2")
                                .arg(resourcePath, file.errorString());
        }
        return std::nullopt;
    }
    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to read bundled MP4 resource %1: %2")
                                .arg(resourcePath, file.errorString());
        }
        return std::nullopt;
    }
    return contents;
}

[[nodiscard]] core::DiagnosticCode diagnosticCode(Mp4IsobmffAnalysisStatus status) noexcept {
    switch (status) {
    case Mp4IsobmffAnalysisStatus::Cancelled:
        return core::DiagnosticCode::Cancelled;
    case Mp4IsobmffAnalysisStatus::SourceError:
        return core::DiagnosticCode::SourceError;
    case Mp4IsobmffAnalysisStatus::ResourceLimit:
        return core::DiagnosticCode::ResourceLimit;
    case Mp4IsobmffAnalysisStatus::InvalidRule:
    case Mp4IsobmffAnalysisStatus::InvalidBatchSize:
    case Mp4IsobmffAnalysisStatus::InProgress:
    case Mp4IsobmffAnalysisStatus::Complete:
        return core::DiagnosticCode::InvalidSyntax;
    }
    return core::DiagnosticCode::InvalidSyntax;
}

class NestingDepthGuard final {
public:
    explicit NestingDepthGuard(const std::shared_ptr<RunnerExecutionBudget>& budget)
        : budget_(budget) {
        if (budget_) {
            ++budget_->currentNestingDepth;
        }
    }

    NestingDepthGuard(const NestingDepthGuard&) = delete;
    NestingDepthGuard& operator=(const NestingDepthGuard&) = delete;

    ~NestingDepthGuard() {
        if (budget_) {
            --budget_->currentNestingDepth;
        }
    }

private:
    std::shared_ptr<RunnerExecutionBudget> budget_;
};

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

        constexpr quint64 maxByteCoordinate = std::numeric_limits<quint64>::max() / 8U;
        if (byteOffset > maxByteCoordinate ||
            static_cast<quint64>(count) > maxByteCoordinate) {
            return {core::SourceReadStatus::Error, 0,
                    QStringLiteral("Container source view coordinate overflow")};
        }

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

struct BundledMp4Rule final {
    RuleCatalogLookupResult resolved;
    QString errorMessage;
};

} // namespace

RulePackageLoadResult loadMp4IsobmffRulePackage() {
    initializeStreamViewOfficialRulesMp4();
    QString errorMessage;
    const QString root = QStringLiteral(":/streamview/rules/org.streamview.mp4/");
    auto manifest = readBundledPackageFile(root + QStringLiteral("rule.toml"), &errorMessage);
    auto source = readBundledPackageFile(root + QStringLiteral("src/mp4_isobmff.svfmt"),
                                         &errorMessage);
    if (!manifest || !source) {
        return {RulePackageLoadStatus::InvalidTree, std::nullopt, std::move(errorMessage)};
    }
    RulePackageLoadResult loaded = RulePackage::fromFiles({
        {QStringLiteral("rule.toml"), std::move(*manifest)},
        {QStringLiteral("src/mp4_isobmff.svfmt"), std::move(*source)},
    });
    if (!loaded.succeeded()) {
        loaded.errorMessage = QStringLiteral("Bundled MP4 package is invalid: %1")
                                  .arg(loaded.errorMessage);
    }
    return loaded;
}

namespace {

[[nodiscard]] const BundledMp4Rule& bundledMp4IsobmffRule() {
    static const BundledMp4Rule bundled = [] {
        BundledMp4Rule result;
        RulePackageLoadResult loaded = loadMp4IsobmffRulePackage();
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
        result.resolved = catalog.resolve(identity, u"main", languageVersion(),
                                          core::version());
        if (!result.resolved.succeeded()) {
            result.errorMessage = result.resolved.errorMessage;
        }
        return result;
    }();
    return bundled;
}

} // namespace

std::optional<Mp4IsobmffAnalyzer>
Mp4IsobmffAnalyzer::create(const core::RandomAccessSource& source,
                           QString* errorMessage,
                           std::optional<core::CancellationToken> cancellation) {
    const auto& bundled = bundledMp4IsobmffRule();
    if (!bundled.resolved.succeeded()) {
        if (errorMessage != nullptr) {
            *errorMessage = bundled.errorMessage.isEmpty()
                                ? QStringLiteral("No installed package matches format: video/mp4")
                                : bundled.errorMessage;
        }
        return std::nullopt;
    }
    return create(source, bundled.resolved, errorMessage, std::move(cancellation));
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

    if (budget_) {
        --budget_->remainingNodes;
    }

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
        if (errorMessage) {
            *errorMessage = execResult.errorMessage.isEmpty()
                                ? QStringLiteral("Top-level MP4 rule execution returned an invalid definition")
                                : execResult.errorMessage;
        }
        return false;
    }

    // Syntax and dependency failures are content-level results. The VM has
    // already marked the structure node partial, so preserve it and continue
    // with later boxes without attempting container re-entry on an invalid
    // or waiting structure.
    if (execResult.status == DslExecutionStatus::Unsupported ||
        execResult.status == DslExecutionStatus::InvalidSyntax ||
        execResult.status == DslExecutionStatus::TruncatedSource ||
        execResult.status == DslExecutionStatus::DependencyUnavailable) {
        return true;
    }

    // Drill containers inside this box node
    if (!recursivelyDrillContainer(*boxNode, failureStatus, errorMessage)) {
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

            NestingDepthGuard nestingGuard(budget_);

            (void)tree_->transition(childId, core::MaterializationState::Indexing);

            const auto& containerLoc = childOpt->location();
            if (!containerLoc.has_value()) {
                if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::InvalidRule;
                if (errorMessage) *errorMessage = QStringLiteral("Container node has no source location");
                return false;
            }

            const quint64 containerBitLength = containerLoc->logicalRange().bitLength();
            if ((containerBitLength % 8U) != 0) {
                if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::InvalidRule;
                if (errorMessage) *errorMessage = QStringLiteral("Container source location is not byte-aligned");
                return false;
            }
            const quint64 containerByteLength = containerBitLength / 8U;
            if (containerByteLength >= 8) {
                const auto containerMapping = core::SourceMapping::create(
                    containerLoc->logicalRange().start().viewId(), containerLoc->sourceSpans());
                if (!containerMapping.has_value()) {
                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
                    if (errorMessage) *errorMessage = QStringLiteral("Failed to create container source mapping");
                    return false;
                }
                BoundedSourceView containerSource(*source_, *containerMapping, containerByteLength);
                Mp4BoxScanner containerScanner(containerSource, cancellation_);

                while (!containerScanner.finished()) {
                            auto scanBatchRes = containerScanner.scanBatch(256, 256U * 1024U);
                            if (scanBatchRes.status == Mp4BoxScanStatus::Cancelled) {
                                if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::Cancelled;
                                return false;
                            }
                            if (scanBatchRes.status == Mp4BoxScanStatus::SourceError) {
                                if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
                                return false;
                            }

                            for (const auto& childRecord : scanBatchRes.records) {
                                if (!childRecord.boxSpan.has_value()) continue;

                                if (budget_ && (budget_->remainingNodes == 0 || budget_->remainingInstructions == 0)) {
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
                                    return false;
                                }

                                // Map child record span back to base source coordinates
                                auto childLogicalRange = core::LogicalRange::create(
                                    core::LogicalBitAddress(containerMapping->viewId(), childRecord.boxSpan->start().absoluteBitOffset()),
                                    childRecord.boxSpan->bitLength());
                                if (!childLogicalRange) {
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
                                    if (errorMessage) *errorMessage = QStringLiteral("Failed to create child box logical range");
                                    return false;
                                }

                                auto childSpansRes = containerMapping->locate(*childLogicalRange);
                                if (!childSpansRes.has_value() || childSpansRes->sourceSpans().empty()) {
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
                                    if (errorMessage) *errorMessage = QStringLiteral("Failed to map child box source spans");
                                    return false;
                                }

                                auto childLoc = makeLocation(childSpansRes->sourceSpans());
                                if (!childLoc.has_value()) {
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
                                    if (errorMessage) *errorMessage = QStringLiteral("Failed to create child box location");
                                    return false;
                                }

                                if (nextViewId_ == 0) {
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
                                    if (errorMessage) *errorMessage = QStringLiteral("Logical view identifier limit reached");
                                    return false;
                                }
                                const core::LogicalViewId childViewId(nextViewId_);
                                nextViewId_ = nextViewId_ == std::numeric_limits<quint64>::max() ? 0 : nextViewId_ + 1;

                                const auto childMapping = core::SourceMapping::create(childViewId, childSpansRes->sourceSpans());
                                if (!childMapping) {
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
                                    if (errorMessage) *errorMessage = QStringLiteral("Failed to create child box source mapping");
                                    return false;
                                }

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
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::Cancelled;
                                    return false;
                                }
                                if (childExec.status == DslExecutionStatus::ResourceLimit) {
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::ResourceLimit;
                                    return false;
                                }
                                if (childExec.status == DslExecutionStatus::SourceError) {
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::SourceError;
                                    return false;
                                }
                                if (childExec.status == DslExecutionStatus::InvalidDefinition) {
                                    if (failureStatus) *failureStatus = Mp4IsobmffAnalysisStatus::InvalidRule;
                                    if (errorMessage) {
                                        *errorMessage = childExec.errorMessage.isEmpty()
                                                            ? QStringLiteral("Nested MP4 rule execution returned an invalid definition")
                                                            : childExec.errorMessage;
                                    }
                                    return false;
                                }

                                if (childExec.status == DslExecutionStatus::Unsupported ||
                                    childExec.status == DslExecutionStatus::InvalidSyntax ||
                                    childExec.status == DslExecutionStatus::TruncatedSource ||
                                    childExec.status == DslExecutionStatus::DependencyUnavailable) {
                                    continue;
                                }

                                // Drill nested children if this child is also a container
                                if (childExec.structureNode.has_value() &&
                                    !recursivelyDrillContainer(*childExec.structureNode, failureStatus, errorMessage)) {
                                    return false;
                                }
                            }
                }
            }

            (void)tree_->transition(childId, core::MaterializationState::Materialized);

        } else {
            // Not a direct container lazy node, but check its children
            if (!recursivelyDrillContainer(childId, failureStatus, errorMessage)) {
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
        batch.status = terminalStatus_;
        batch.errorMessage = terminalErrorMessage_;
        return batch;
    }

    const auto terminalizeFailure = [this, &batch](Mp4IsobmffAnalysisStatus status,
                                                   QString message) {
        batch.status = status;
        batch.errorMessage = std::move(message);
        terminal_ = true;
        terminalStatus_ = batch.status;
        terminalErrorMessage_ = batch.errorMessage;
        const auto rootState = status == Mp4IsobmffAnalysisStatus::Cancelled
                                   ? core::MaterializationState::Cancelled
                                   : core::MaterializationState::Invalid;
        markRootPartial(diagnosticCode(status), rootState, batch.errorMessage);
    };

    if (!deferredScanStatus_) {
        const auto scanBatchRes = scanner_.scanBatch(maximumRecords, maximumInspectedPositions);
        if (scanBatchRes.status == Mp4BoxScanStatus::InvalidBatchSize) {
            batch.status = Mp4IsobmffAnalysisStatus::InvalidBatchSize;
            batch.errorMessage = scanBatchRes.errorMessage;
            return batch;
        }
        for (const auto& record : scanBatchRes.records) {
            queuedRecords_.push_back({record});
        }
        deferredScanStatus_ = scanBatchRes.status;
        deferredScanErrorMessage_ = scanBatchRes.errorMessage;
    }

    while (!queuedRecords_.empty()) {
        auto treeSnapshot = tree_->snapshot(tree_->rootId());
        const quint64 nextBoxIndexBefore = nextBoxIndex_;
        const quint64 nextViewIdBefore = nextViewId_;
        const std::size_t publishedCount = batch.boxNodes.size();
        Mp4IsobmffAnalysisStatus failureStatus = Mp4IsobmffAnalysisStatus::InvalidRule;
        QString errorMessage;
        if (publishRecord(queuedRecords_.front().record,
                          batch,
                          &failureStatus,
                          &errorMessage)) {
            queuedRecords_.pop_front();
            continue;
        }
        if (!tree_->restore(std::move(treeSnapshot))) {
            terminalizeFailure(Mp4IsobmffAnalysisStatus::InvalidRule,
                                QStringLiteral("Unable to restore failed MP4 record transaction"));
            return batch;
        }
        nextBoxIndex_ = nextBoxIndexBefore;
        nextViewId_ = nextViewIdBefore;
        batch.boxNodes.resize(publishedCount);
        terminalizeFailure(failureStatus, std::move(errorMessage));
        return batch;
    }

    if (!deferredScanStatus_) {
        terminalizeFailure(Mp4IsobmffAnalysisStatus::InvalidRule,
                           QStringLiteral("MP4 analyzer lost its deferred scanner status"));
        return batch;
    }

    const Mp4BoxScanStatus scanStatus = *deferredScanStatus_;
    batch.status = scanStatus == Mp4BoxScanStatus::Complete
                       ? Mp4IsobmffAnalysisStatus::Complete
                       : scanStatus == Mp4BoxScanStatus::Cancelled
                           ? Mp4IsobmffAnalysisStatus::Cancelled
                           : scanStatus == Mp4BoxScanStatus::SourceError
                               ? Mp4IsobmffAnalysisStatus::SourceError
                               : Mp4IsobmffAnalysisStatus::InProgress;
    batch.errorMessage = deferredScanErrorMessage_;
    switch (scanStatus) {
    case Mp4BoxScanStatus::Complete:
        if (!tree_->transition(tree_->rootId(), core::MaterializationState::Materialized)) {
            terminalizeFailure(Mp4IsobmffAnalysisStatus::InvalidRule,
                               QStringLiteral("Unable to materialize MP4 analysis root"));
            return batch;
        }
        terminal_ = true;
        terminalStatus_ = batch.status;
        terminalErrorMessage_.clear();
        deferredScanStatus_.reset();
        break;
    case Mp4BoxScanStatus::Cancelled:
        terminalizeFailure(Mp4IsobmffAnalysisStatus::Cancelled,
                            batch.errorMessage.isEmpty()
                                ? QStringLiteral("MP4 box scan was cancelled")
                                : batch.errorMessage);
        break;
    case Mp4BoxScanStatus::SourceError:
        terminalizeFailure(Mp4IsobmffAnalysisStatus::SourceError, batch.errorMessage);
        break;
    case Mp4BoxScanStatus::InProgress:
        deferredScanStatus_.reset();
        deferredScanErrorMessage_.clear();
        batch.status = Mp4IsobmffAnalysisStatus::InProgress;
        batch.errorMessage.clear();
        break;
    case Mp4BoxScanStatus::InvalidBatchSize:
        terminalizeFailure(Mp4IsobmffAnalysisStatus::InvalidRule,
                           QStringLiteral("MP4 scanner rejected a validated analysis batch"));
        break;
    }

    return batch;
}

bool Mp4IsobmffAnalyzer::resumeAfterCancellation(
    std::optional<core::CancellationToken> cancellation,
    QString* errorMessage) {
    if (!terminal_ || terminalStatus_ != Mp4IsobmffAnalysisStatus::Cancelled) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("MP4 analyzer is not waiting to resume cancellation");
        }
        return false;
    }
    cancellation_ = std::move(cancellation);
    if (budget_) {
        budget_->cancellation = cancellation_;
    }
    if (tree_) {
        (void)tree_->resumeCancelled(tree_->rootId());
    }
    terminal_ = false;
    terminalStatus_ = Mp4IsobmffAnalysisStatus::InProgress;
    terminalErrorMessage_.clear();
    if (deferredScanStatus_ == Mp4BoxScanStatus::Cancelled) {
        deferredScanStatus_ = Mp4BoxScanStatus::InProgress;
        deferredScanErrorMessage_.clear();
    }
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
    auto& state = windowDecoderStates_[windowNodeId.value()];
    if (!state) {
        state = WindowDecoder::createState();
    }
    return WindowDecoder(program_,
                         *source_,
                         *mapping,
                         tree_,
                         windowNodeId,
                         budget_,
                         state,
                         cancellation_);
}

} // namespace streamview::rules
