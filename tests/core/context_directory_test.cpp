#include <streamview/core/context_directory.h>

#include <QTest>

#include <optional>
#include <utility>
#include <vector>

using streamview::core::AnalysisNodeId;
using streamview::core::ContextDefinitionKind;
using streamview::core::ContextDefinitionSpec;
using streamview::core::ContextDirectory;
using streamview::core::ContextKey;
using streamview::core::ContextLookupStatus;
using streamview::core::ContextRegistrationStatus;
using streamview::core::SourceBitAddress;
using streamview::core::SourceSpan;

namespace {

ContextDefinitionSpec definition(ContextKey key,
                                 quint64 sourceBitOffset,
                                 quint64 bitLength,
                                 quint64 nodeId,
                                 std::vector<streamview::core::ContextDefinitionId> dependencies =
                                     {}) {
    const auto span = SourceSpan::create(SourceBitAddress(sourceBitOffset), bitLength);
    Q_ASSERT(span.has_value());
    return ContextDefinitionSpec{key, *span, AnalysisNodeId(nodeId), std::move(dependencies)};
}

} // namespace

class ContextDirectoryTest final : public QObject {
    Q_OBJECT

private slots:
    void selectsTheNearestCompletedDefinitionBeforeTheSourcePosition() {
        ContextDirectory directory;
        const ContextKey key{ContextDefinitionKind::H264SequenceParameterSet, 0, 3};

        const auto first = directory.registerDefinition(definition(key, 16, 8, 11));
        QCOMPARE(first.status, ContextRegistrationStatus::Registered);
        QVERIFY(first.definitionId.has_value());

        QCOMPARE(directory.resolveBefore(key, SourceBitAddress(23)).status,
                 ContextLookupStatus::NotFound);
        const auto atFirstEnd = directory.resolveBefore(key, SourceBitAddress(24));
        QCOMPARE(atFirstEnd.status, ContextLookupStatus::Found);
        QVERIFY(atFirstEnd.definition.has_value());
        QCOMPARE(atFirstEnd.definition->id, *first.definitionId);
        QCOMPARE(atFirstEnd.definition->analysisNodeId, AnalysisNodeId(11));

        const auto second = directory.registerDefinition(definition(key, 64, 16, 12));
        QCOMPARE(second.status, ContextRegistrationStatus::Registered);
        QVERIFY(second.definitionId.has_value());

        const auto insideSecond = directory.resolveBefore(key, SourceBitAddress(79));
        QCOMPARE(insideSecond.status, ContextLookupStatus::Found);
        QVERIFY(insideSecond.definition.has_value());
        QCOMPARE(insideSecond.definition->id, *first.definitionId);

        const auto atSecondEnd = directory.resolveBefore(key, SourceBitAddress(80));
        QCOMPARE(atSecondEnd.status, ContextLookupStatus::Found);
        QVERIFY(atSecondEnd.definition.has_value());
        QCOMPARE(atSecondEnd.definition->id, *second.definitionId);
    }

    void invalidatesAContextWhoseExactDependencyWasRedefined() {
        ContextDirectory directory;
        const ContextKey spsKey{ContextDefinitionKind::H264SequenceParameterSet, 0, 1};
        const ContextKey ppsKey{ContextDefinitionKind::H264PictureParameterSet, 0, 4};

        const auto firstSps = directory.registerDefinition(definition(spsKey, 0, 8, 20));
        QVERIFY(firstSps.succeeded());
        QVERIFY(firstSps.definitionId.has_value());
        const auto firstPps = directory.registerDefinition(
            definition(ppsKey, 16, 8, 21, {*firstSps.definitionId}));
        QCOMPARE(firstPps.status, ContextRegistrationStatus::Registered);
        QVERIFY(firstPps.definitionId.has_value());

        const auto initial = directory.resolveBefore(ppsKey, SourceBitAddress(24));
        QCOMPARE(initial.status, ContextLookupStatus::Found);
        QVERIFY(initial.definition.has_value());
        QCOMPARE(initial.definition->id, *firstPps.definitionId);

        const auto secondSps = directory.registerDefinition(definition(spsKey, 32, 8, 22));
        QVERIFY(secondSps.succeeded());
        QVERIFY(secondSps.definitionId.has_value());
        const auto stalePps = directory.resolveBefore(ppsKey, SourceBitAddress(40));
        QCOMPARE(stalePps.status, ContextLookupStatus::DependencyUnavailable);
        QCOMPARE(stalePps.unavailableDependency, firstSps.definitionId);

        const auto secondPps = directory.registerDefinition(
            definition(ppsKey, 48, 8, 23, {*secondSps.definitionId}));
        QCOMPARE(secondPps.status, ContextRegistrationStatus::Registered);
        QVERIFY(secondPps.definitionId.has_value());
        const auto rebound = directory.resolveBefore(ppsKey, SourceBitAddress(56));
        QCOMPARE(rebound.status, ContextLookupStatus::Found);
        QVERIFY(rebound.definition.has_value());
        QCOMPARE(rebound.definition->id, *secondPps.definitionId);
    }

    void supportsEveryContextKindAndSeparatesScopes() {
        ContextDirectory directory;
        const std::vector<ContextKey> keys{
            {ContextDefinitionKind::H264SequenceParameterSet, 7, 1},
            {ContextDefinitionKind::H264PictureParameterSet, 7, 1},
            {ContextDefinitionKind::AacAudioSpecificConfig, 7, 0},
            {ContextDefinitionKind::IsoBmffSampleDescription, 7, 1},
            {ContextDefinitionKind::IsoBmffSampleDescription, 8, 1},
        };

        for (std::size_t index = 0; index < keys.size(); ++index) {
            const auto registered = directory.registerDefinition(
                definition(keys.at(index), 0, 8, static_cast<quint64>(index + 30)));
            QVERIFY(registered.succeeded());
        }

        QCOMPARE(directory.definitionCount(), keys.size());
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const auto resolved = directory.resolveBefore(keys.at(index), SourceBitAddress(8));
            QCOMPARE(resolved.status, ContextLookupStatus::Found);
            QVERIFY(resolved.definition.has_value());
            QCOMPARE(resolved.definition->analysisNodeId.value(),
                     static_cast<quint64>(index + 30));
        }
    }

    void acceptsOutOfOrderDiscoveryAndKeepsStableSnapshots() {
        ContextDirectory directory;
        const ContextKey key{ContextDefinitionKind::AacAudioSpecificConfig, 9, 0};

        const auto later = directory.registerDefinition(definition(key, 80, 8, 40));
        QVERIFY(later.succeeded());
        QVERIFY(later.definitionId.has_value());
        const auto laterSnapshot = directory.definition(*later.definitionId);
        QVERIFY(laterSnapshot.has_value());

        const auto earlier = directory.registerDefinition(definition(key, 16, 8, 41));
        const auto middle = directory.registerDefinition(definition(key, 48, 8, 42));
        QVERIFY(earlier.succeeded());
        QVERIFY(middle.succeeded());
        QVERIFY(earlier.definitionId.has_value());
        QVERIFY(middle.definitionId.has_value());

        const auto atEarlier = directory.resolveBefore(key, SourceBitAddress(24));
        const auto atMiddle = directory.resolveBefore(key, SourceBitAddress(56));
        const auto atLater = directory.resolveBefore(key, SourceBitAddress(88));
        QCOMPARE(atEarlier.status, ContextLookupStatus::Found);
        QCOMPARE(atMiddle.status, ContextLookupStatus::Found);
        QCOMPARE(atLater.status, ContextLookupStatus::Found);
        QVERIFY(atEarlier.definition.has_value());
        QVERIFY(atMiddle.definition.has_value());
        QVERIFY(atLater.definition.has_value());
        QCOMPARE(atEarlier.definition->id, *earlier.definitionId);
        QCOMPARE(atMiddle.definition->id, *middle.definitionId);
        QCOMPARE(atLater.definition->id, *later.definitionId);
        QCOMPARE(laterSnapshot->id, *later.definitionId);
        QCOMPARE(laterSnapshot->analysisNodeId, AnalysisNodeId(40));
        QCOMPARE(laterSnapshot->sourceSpan.start().absoluteBitOffset(), quint64(80));
    }

    void rejectsInvalidOrAmbiguousDefinitionsWithoutMutation() {
        ContextDirectory directory;
        const ContextKey key{ContextDefinitionKind::H264SequenceParameterSet, 0, 2};
        const auto original = directory.registerDefinition(definition(key, 16, 16, 50));
        QVERIFY(original.succeeded());
        QVERIFY(original.definitionId.has_value());

        auto invalidKind = definition(key, 48, 8, 51);
        invalidKind.key.kind = static_cast<ContextDefinitionKind>(255);
        auto result = directory.registerDefinition(std::move(invalidKind));
        QCOMPARE(result.status, ContextRegistrationStatus::InvalidDefinition);
        QVERIFY(!result.errorMessage.isEmpty());

        const auto emptySpan = SourceSpan::create(SourceBitAddress(48), 0);
        QVERIFY(emptySpan.has_value());
        result = directory.registerDefinition(ContextDefinitionSpec{
            key, *emptySpan, AnalysisNodeId(52), {}});
        QCOMPARE(result.status, ContextRegistrationStatus::InvalidDefinition);

        result = directory.registerDefinition(definition(key, 48, 8, 0));
        QCOMPARE(result.status, ContextRegistrationStatus::InvalidDefinition);

        result = directory.registerDefinition(definition(key, 24, 16, 53));
        QCOMPARE(result.status, ContextRegistrationStatus::DuplicateDefinition);

        result = directory.registerDefinition(
            definition(key, 48, 8, 54, {*original.definitionId}));
        QCOMPARE(result.status, ContextRegistrationStatus::InvalidDefinition);

        const ContextKey ppsKey{ContextDefinitionKind::H264PictureParameterSet, 0, 2};
        result = directory.registerDefinition(
            definition(ppsKey,
                       48,
                       8,
                       55,
                       {*original.definitionId, *original.definitionId}));
        QCOMPARE(result.status, ContextRegistrationStatus::InvalidDefinition);
        QCOMPARE(directory.definitionCount(), std::size_t(1));
        const auto unchanged = directory.resolveBefore(key, SourceBitAddress(64));
        QCOMPARE(unchanged.status, ContextLookupStatus::Found);
        QVERIFY(unchanged.definition.has_value());
        QCOMPARE(unchanged.definition->id, *original.definitionId);
    }

    void rejectsDependenciesThatWereNotCurrentAtDefinitionStart() {
        ContextDirectory directory;
        const ContextKey spsKey{ContextDefinitionKind::H264SequenceParameterSet, 2, 1};
        const ContextKey ppsKey{ContextDefinitionKind::H264PictureParameterSet, 2, 1};
        const auto firstSps = directory.registerDefinition(definition(spsKey, 0, 8, 60));
        const auto secondSps = directory.registerDefinition(definition(spsKey, 16, 8, 61));
        QVERIFY(firstSps.succeeded());
        QVERIFY(secondSps.succeeded());
        QVERIFY(firstSps.definitionId.has_value());
        QVERIFY(secondSps.definitionId.has_value());

        const auto stale = directory.registerDefinition(
            definition(ppsKey, 32, 8, 62, {*firstSps.definitionId}));
        QCOMPARE(stale.status, ContextRegistrationStatus::DependencyUnavailable);
        QVERIFY(!stale.errorMessage.isEmpty());

        const auto futureSps = directory.registerDefinition(definition(spsKey, 64, 8, 63));
        QVERIFY(futureSps.succeeded());
        QVERIFY(futureSps.definitionId.has_value());
        const auto future = directory.registerDefinition(
            definition(ppsKey, 48, 8, 64, {*futureSps.definitionId}));
        QCOMPARE(future.status, ContextRegistrationStatus::DependencyUnavailable);
        QCOMPARE(directory.definitionCount(), std::size_t(3));
    }

    void reportsCrossGenerationDependencyCyclesAsUnavailable() {
        ContextDirectory directory;
        const ContextKey ascKey{ContextDefinitionKind::AacAudioSpecificConfig, 4, 0};
        const ContextKey descriptionKey{
            ContextDefinitionKind::IsoBmffSampleDescription, 4, 1};

        const auto firstAsc = directory.registerDefinition(definition(ascKey, 0, 8, 70));
        QVERIFY(firstAsc.succeeded());
        QVERIFY(firstAsc.definitionId.has_value());
        const auto description = directory.registerDefinition(
            definition(descriptionKey, 16, 8, 71, {*firstAsc.definitionId}));
        QVERIFY(description.succeeded());
        QVERIFY(description.definitionId.has_value());
        const auto secondAsc = directory.registerDefinition(
            definition(ascKey, 32, 8, 72, {*description.definitionId}));
        QVERIFY(secondAsc.succeeded());
        QVERIFY(secondAsc.definitionId.has_value());

        const auto resolved = directory.resolveBefore(ascKey, SourceBitAddress(40));
        QCOMPARE(resolved.status, ContextLookupStatus::DependencyUnavailable);
        QCOMPARE(resolved.unavailableDependency, secondAsc.definitionId);
    }

    void allowsAdjacentDefinitionsForOneKey() {
        ContextDirectory directory;
        const ContextKey key{ContextDefinitionKind::IsoBmffSampleDescription, 12, 1};
        const auto first = directory.registerDefinition(definition(key, 0, 8, 80));
        const auto adjacent = directory.registerDefinition(definition(key, 8, 8, 81));
        QVERIFY(first.succeeded());
        QVERIFY(adjacent.succeeded());
        QVERIFY(first.definitionId.has_value());
        QVERIFY(adjacent.definitionId.has_value());

        const auto atBoundary = directory.resolveBefore(key, SourceBitAddress(8));
        QCOMPARE(atBoundary.status, ContextLookupStatus::Found);
        QVERIFY(atBoundary.definition.has_value());
        QCOMPARE(atBoundary.definition->id, *first.definitionId);

        const auto afterAdjacent = directory.resolveBefore(key, SourceBitAddress(16));
        QCOMPARE(afterAdjacent.status, ContextLookupStatus::Found);
        QVERIFY(afterAdjacent.definition.has_value());
        QCOMPARE(afterAdjacent.definition->id, *adjacent.definitionId);
    }

    void boundsDependencyResolutionDepth() {
        ContextDirectory directory;
        std::optional<streamview::core::ContextDefinitionId> previous;
        ContextKey finalKey;

        for (std::size_t depth = 0; depth <= ContextDirectory::maximumDependencyDepth(); ++depth) {
            const ContextKey key{ContextDefinitionKind::AacAudioSpecificConfig,
                                 static_cast<quint64>(depth + 1),
                                 0};
            std::vector<streamview::core::ContextDefinitionId> dependencies;
            if (previous) {
                dependencies.push_back(*previous);
            }
            const auto registered = directory.registerDefinition(
                definition(key,
                           static_cast<quint64>(depth * 16),
                           8,
                           static_cast<quint64>(depth + 100),
                           std::move(dependencies)));
            QVERIFY(registered.succeeded());
            QVERIFY(registered.definitionId.has_value());
            previous = registered.definitionId;
            finalKey = key;
        }

        const auto resolved = directory.resolveBefore(
            finalKey,
            SourceBitAddress(
                static_cast<quint64>(ContextDirectory::maximumDependencyDepth() * 16 + 8)));
        QCOMPARE(resolved.status, ContextLookupStatus::DependencyUnavailable);
        QVERIFY(resolved.unavailableDependency.has_value());
    }

    void resolvesAmbientContextReturnsNotFoundOnEmptyDirectory() {
        ContextDirectory directory;
        const auto resolved = directory.resolveLatestBefore(
            ContextDefinitionKind::H264SequenceParameterSet, 0, SourceBitAddress(100));
        QCOMPARE(resolved.status, ContextLookupStatus::NotFound);
        QVERIFY(!resolved.definition.has_value());
        QVERIFY(!resolved.unavailableDependency.has_value());
    }

    void resolvesAmbientContextSelectingLatestAcrossMultipleKeys() {
        ContextDirectory directory;
        const ContextKey sps0{ContextDefinitionKind::H264SequenceParameterSet, 0, 0};
        const ContextKey sps1{ContextDefinitionKind::H264SequenceParameterSet, 0, 1};
        const ContextKey sps2{ContextDefinitionKind::H264SequenceParameterSet, 0, 2};

        const auto def0 = directory.registerDefinition(definition(sps0, 0, 8, 50));
        const auto def1 = directory.registerDefinition(definition(sps1, 16, 8, 51));
        const auto def2 = directory.registerDefinition(definition(sps2, 32, 8, 52));
        QVERIFY(def0.succeeded() && def1.succeeded() && def2.succeeded());

        // Before any definition finishes
        QCOMPARE(directory.resolveLatestBefore(ContextDefinitionKind::H264SequenceParameterSet, 0, SourceBitAddress(7)).status,
                 ContextLookupStatus::NotFound);

        // At pos 8..15, only SPS 0 is complete
        const auto at8 = directory.resolveLatestBefore(ContextDefinitionKind::H264SequenceParameterSet, 0, SourceBitAddress(8));
        QCOMPARE(at8.status, ContextLookupStatus::Found);
        QVERIFY(at8.definition.has_value());
        QCOMPARE(at8.definition->id, *def0.definitionId);
        QCOMPARE(at8.definition->analysisNodeId, AnalysisNodeId(50));

        // At pos 24..31, SPS 1 is the latest complete definition
        const auto at24 = directory.resolveLatestBefore(ContextDefinitionKind::H264SequenceParameterSet, 0, SourceBitAddress(24));
        QCOMPARE(at24.status, ContextLookupStatus::Found);
        QVERIFY(at24.definition.has_value());
        QCOMPARE(at24.definition->id, *def1.definitionId);
        QCOMPARE(at24.definition->analysisNodeId, AnalysisNodeId(51));

        // At pos 40, SPS 2 is the latest complete definition
        const auto at40 = directory.resolveLatestBefore(ContextDefinitionKind::H264SequenceParameterSet, 0, SourceBitAddress(40));
        QCOMPARE(at40.status, ContextLookupStatus::Found);
        QVERIFY(at40.definition.has_value());
        QCOMPARE(at40.definition->id, *def2.definitionId);
        QCOMPARE(at40.definition->analysisNodeId, AnalysisNodeId(52));
    }

    void resolvesAmbientContextSelectingLatestGenerationAfterSameKeyRedefinition() {
        ContextDirectory directory;
        const ContextKey sps0{ContextDefinitionKind::H264SequenceParameterSet, 0, 0};
        const ContextKey sps1{ContextDefinitionKind::H264SequenceParameterSet, 0, 1};

        const auto def0Gen1 = directory.registerDefinition(definition(sps0, 0, 8, 60));
        const auto def1Gen1 = directory.registerDefinition(definition(sps1, 16, 8, 61));
        const auto def0Gen2 = directory.registerDefinition(definition(sps0, 32, 8, 62));
        QVERIFY(def0Gen1.succeeded() && def1Gen1.succeeded() && def0Gen2.succeeded());

        // At pos 24, SPS 1 gen 1 (at [16, 24)) is latest
        const auto at24 = directory.resolveLatestBefore(ContextDefinitionKind::H264SequenceParameterSet, 0, SourceBitAddress(24));
        QCOMPARE(at24.status, ContextLookupStatus::Found);
        QVERIFY(at24.definition.has_value());
        QCOMPARE(at24.definition->id, *def1Gen1.definitionId);
        QCOMPARE(at24.definition->analysisNodeId, AnalysisNodeId(61));

        // At pos 40, SPS 0 gen 2 (at [32, 40)) is latest
        const auto at40 = directory.resolveLatestBefore(ContextDefinitionKind::H264SequenceParameterSet, 0, SourceBitAddress(40));
        QCOMPARE(at40.status, ContextLookupStatus::Found);
        QVERIFY(at40.definition.has_value());
        QCOMPARE(at40.definition->id, *def0Gen2.definitionId);
        QCOMPARE(at40.definition->analysisNodeId, AnalysisNodeId(62));
    }

    void reportsDependencyUnavailableWhenAmbientTargetDependencyFails() {
        ContextDirectory directory;
        const ContextKey spsKey{ContextDefinitionKind::H264SequenceParameterSet, 0, 0};
        const ContextKey ppsKey{ContextDefinitionKind::H264PictureParameterSet, 0, 0};

        const auto firstSps = directory.registerDefinition(definition(spsKey, 0, 8, 70));
        QVERIFY(firstSps.succeeded());
        const auto firstPps = directory.registerDefinition(
            definition(ppsKey, 16, 8, 71, {*firstSps.definitionId}));
        QVERIFY(firstPps.succeeded());

        // Redefine SPS 0 at [32, 40)
        const auto secondSps = directory.registerDefinition(definition(spsKey, 32, 8, 72));
        QVERIFY(secondSps.succeeded());

        // At pos 40, resolve latest PPS. PPS 0 is selected, but its dependency (SPS 0 gen 1) was invalidated
        const auto ambientPps = directory.resolveLatestBefore(
            ContextDefinitionKind::H264PictureParameterSet, 0, SourceBitAddress(40));
        QCOMPARE(ambientPps.status, ContextLookupStatus::DependencyUnavailable);
        QCOMPARE(ambientPps.unavailableDependency, firstSps.definitionId);
    }

    void separatesAmbientResolutionsAcrossDistinctKindsAndScopes() {
        ContextDirectory directory;
        const ContextKey spsScope0{ContextDefinitionKind::H264SequenceParameterSet, 0, 0};
        const ContextKey spsScope1{ContextDefinitionKind::H264SequenceParameterSet, 1, 0};
        const ContextKey ppsScope0{ContextDefinitionKind::H264PictureParameterSet, 0, 0};

        const auto defSps0 = directory.registerDefinition(definition(spsScope0, 0, 8, 80));
        const auto defSps1 = directory.registerDefinition(definition(spsScope1, 16, 8, 81));
        const auto defPps0 = directory.registerDefinition(definition(ppsScope0, 32, 8, 82));
        QVERIFY(defSps0.succeeded() && defSps1.succeeded() && defPps0.succeeded());

        // Query (SPS, scope 0) at pos 40 -> finds defSps0 at [0, 8), does not crosstalk with scope 1 or PPS
        const auto resolvedSpsScope0 = directory.resolveLatestBefore(
            ContextDefinitionKind::H264SequenceParameterSet, 0, SourceBitAddress(40));
        QCOMPARE(resolvedSpsScope0.status, ContextLookupStatus::Found);
        QVERIFY(resolvedSpsScope0.definition.has_value());
        QCOMPARE(resolvedSpsScope0.definition->id, *defSps0.definitionId);
        QCOMPARE(resolvedSpsScope0.definition->analysisNodeId, AnalysisNodeId(80));

        // Query (SPS, scope 1) at pos 40 -> finds defSps1 at [16, 24)
        const auto resolvedSpsScope1 = directory.resolveLatestBefore(
            ContextDefinitionKind::H264SequenceParameterSet, 1, SourceBitAddress(40));
        QCOMPARE(resolvedSpsScope1.status, ContextLookupStatus::Found);
        QVERIFY(resolvedSpsScope1.definition.has_value());
        QCOMPARE(resolvedSpsScope1.definition->id, *defSps1.definitionId);
        QCOMPARE(resolvedSpsScope1.definition->analysisNodeId, AnalysisNodeId(81));

        // Query (PPS, scope 0) at pos 40 -> finds defPps0 at [32, 40)
        const auto resolvedPpsScope0 = directory.resolveLatestBefore(
            ContextDefinitionKind::H264PictureParameterSet, 0, SourceBitAddress(40));
        QCOMPARE(resolvedPpsScope0.status, ContextLookupStatus::Found);
        QVERIFY(resolvedPpsScope0.definition.has_value());
        QCOMPARE(resolvedPpsScope0.definition->id, *defPps0.definitionId);
        QCOMPARE(resolvedPpsScope0.definition->analysisNodeId, AnalysisNodeId(82));

        // Query (AAC, scope 0) at pos 40 -> NotFound
        const auto resolvedAac = directory.resolveLatestBefore(
            ContextDefinitionKind::AacAudioSpecificConfig, 0, SourceBitAddress(40));
        QCOMPARE(resolvedAac.status, ContextLookupStatus::NotFound);
    }
};

QTEST_GUILESS_MAIN(ContextDirectoryTest)

#include "context_directory_test.moc"
