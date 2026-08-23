#include <streamview/core/analysis_model.h>
#include <streamview/core/coordinates.h>
#include <streamview/rules/configuration_summary.h>

#include <QObject>
#include <QTest>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace streamview::rules {

namespace {

using namespace streamview::core;

/// A one-span location, so mock syntax fields carry the coordinates a real
/// decoded field always has.
[[nodiscard]] std::optional<FieldLocation> makeLocation(quint64 startBit, quint64 bitLength) {
    const auto span = SourceSpan::create(SourceBitAddress(startBit), bitLength);
    if (!span) {
        return std::nullopt;
    }
    const auto range =
        LogicalRange::create(LogicalBitAddress(LogicalViewId(1), startBit), bitLength);
    if (!range) {
        return std::nullopt;
    }
    return FieldLocation::create(*range, {*span});
}

/// Build a two-level tree whose single structure child carries `fieldName`.
///
/// The capability must not know any real configuration layout, so these tests
/// deliberately use invented names: what the registry owns is resolution and
/// failure reporting, not field meaning.
struct MockConfigurationTree final {
    std::optional<AnalysisTree> tree;
    AnalysisNodeId structureNode;
};

[[nodiscard]] MockConfigurationTree makeConfigurationTree(const QString& structureName,
                                                          const QString& fieldName,
                                                          quint64 fieldValue) {
    MockConfigurationTree built;
    built.tree = AnalysisTree::create(QStringLiteral("Root"));
    if (!built.tree) {
        return built;
    }
    // Children may only be appended to a parent that is still Indexing, so the
    // structure is created in that state and left there: these trees are read
    // by providers, never transitioned like a real execution's output.
    const auto structureId =
        built.tree->appendChild(built.tree->rootId(),
                                AnalysisNodeSpec{.kind = AnalysisNodeKind::Structure,
                                                 .name = structureName,
                                                 .state = MaterializationState::Indexing});
    if (!structureId) {
        built.tree.reset();
        return built;
    }
    built.structureNode = *structureId;
    auto location = makeLocation(0, 8);
    if (!location) {
        built.tree.reset();
        return built;
    }
    const auto fieldId = built.tree->appendChild(
        *structureId,
        AnalysisNodeSpec{.kind = AnalysisNodeKind::SyntaxField,
                         .name = fieldName,
                         .value = QVariant::fromValue(fieldValue),
                         .location = std::move(location)});
    if (!fieldId) {
        built.tree.reset();
    }
    return built;
}

/// A provider that names one invented field, so registry behaviour is testable
/// without depending on any codec's real summary format.
class MockConfigurationSummaryProvider final : public ConfigurationSummaryProvider {
public:
    [[nodiscard]] QString targetFormat() const override {
        return QStringLiteral("mock.config");
    }

    [[nodiscard]] ConfigurationSummaryResult format(
        const ConfigurationSummaryRequest& request) const override {
        ConfigurationSummaryResult result;
        const auto node = request.configurationTree->node(request.configurationNode);
        if (!node) {
            result.status = ConfigurationSummaryStatus::InvalidRequest;
            return result;
        }
        for (const auto childId : node->children()) {
            const auto child = request.configurationTree->node(childId);
            if (child && child->name() == QStringLiteral("mock_field")) {
                result.status = ConfigurationSummaryStatus::Formatted;
                result.summary = QStringLiteral("mock %1").arg(child->value().toULongLong());
                return result;
            }
        }
        result.status = ConfigurationSummaryStatus::DependencyUnavailable;
        result.errorMessage = QStringLiteral("mock_field absent");
        return result;
    }
};

} // namespace

class ConfigurationSummaryTest final : public QObject {
    Q_OBJECT

private slots:
    void registersProviderByTargetFormat() {
        ConfigurationSummaryRegistry registry;
        auto provider = std::make_shared<MockConfigurationSummaryProvider>();
        QVERIFY(registry.registerProvider(provider));
        QVERIFY(registry.findProvider(QStringLiteral("mock.config")) != nullptr);
    }

    void rejectsDuplicateTargetFormat() {
        ConfigurationSummaryRegistry registry;
        QVERIFY(registry.registerProvider(std::make_shared<MockConfigurationSummaryProvider>()));
        QVERIFY(!registry.registerProvider(std::make_shared<MockConfigurationSummaryProvider>()));
    }

    void rejectsNullProvider() {
        ConfigurationSummaryRegistry registry;
        QVERIFY(!registry.registerProvider(nullptr));
    }

    void unregistersRegisteredProviderAndReportsUnknownOne() {
        ConfigurationSummaryRegistry registry;
        QVERIFY(registry.registerProvider(std::make_shared<MockConfigurationSummaryProvider>()));
        QVERIFY(registry.unregisterProvider(QStringLiteral("mock.config")));
        QVERIFY(registry.findProvider(QStringLiteral("mock.config")) == nullptr);
        // Unlike PayloadTransformRegistry there is no unremovable built-in: a
        // summary has no meaningful identity fallback, so every provider is
        // removable and removing an absent one simply reports false.
        QVERIFY(!registry.unregisterProvider(QStringLiteral("mock.config")));
    }

    void resetDropsEveryProvider() {
        ConfigurationSummaryRegistry registry;
        QVERIFY(registry.registerProvider(std::make_shared<MockConfigurationSummaryProvider>()));
        registry.reset();
        QVERIFY(registry.findProvider(QStringLiteral("mock.config")) == nullptr);
    }

    void formatsThroughResolvedProvider() {
        auto built = makeConfigurationTree(QStringLiteral("MockConfig"),
                                           QStringLiteral("mock_field"),
                                           7);
        QVERIFY(built.tree.has_value());
        ConfigurationSummaryRegistry registry;
        QVERIFY(registry.registerProvider(std::make_shared<MockConfigurationSummaryProvider>()));

        const auto result = registry.format({.configurationTree = &*built.tree,
                                             .configurationNode = built.structureNode,
                                             .targetFormat = QStringLiteral("mock.config")});
        QCOMPARE(result.status, ConfigurationSummaryStatus::Formatted);
        QVERIFY(result.formatted());
        QCOMPARE(result.summary, QStringLiteral("mock 7"));
    }

    void reportsUnsupportedFormatWhenNoProviderClaimsIt() {
        auto built = makeConfigurationTree(QStringLiteral("MockConfig"),
                                           QStringLiteral("mock_field"),
                                           7);
        QVERIFY(built.tree.has_value());
        ConfigurationSummaryRegistry registry;

        const auto result = registry.format({.configurationTree = &*built.tree,
                                             .configurationNode = built.structureNode,
                                             .targetFormat = QStringLiteral("mock.config")});
        // Resolution failure must never fall back to an invented summary; ADR-0105
        // section 5 requires reporting instead of guessing.
        QCOMPARE(result.status, ConfigurationSummaryStatus::UnsupportedFormat);
        QVERIFY(!result.formatted());
        QVERIFY(result.summary.isEmpty());
    }

    void reportsInvalidRequestWithoutTree() {
        ConfigurationSummaryRegistry registry;
        QVERIFY(registry.registerProvider(std::make_shared<MockConfigurationSummaryProvider>()));
        const auto result = registry.format(
            {.configurationTree = nullptr, .targetFormat = QStringLiteral("mock.config")});
        QCOMPARE(result.status, ConfigurationSummaryStatus::InvalidRequest);
    }

    void reportsInvalidRequestForNodeOutsideTree() {
        auto built = makeConfigurationTree(QStringLiteral("MockConfig"),
                                           QStringLiteral("mock_field"),
                                           7);
        QVERIFY(built.tree.has_value());
        ConfigurationSummaryRegistry registry;
        QVERIFY(registry.registerProvider(std::make_shared<MockConfigurationSummaryProvider>()));

        const auto result = registry.format({.configurationTree = &*built.tree,
                                             .configurationNode = AnalysisNodeId(9999),
                                             .targetFormat = QStringLiteral("mock.config")});
        QCOMPARE(result.status, ConfigurationSummaryStatus::InvalidRequest);
    }

    void reportsInvalidRequestForEmptyTargetFormat() {
        auto built = makeConfigurationTree(QStringLiteral("MockConfig"),
                                           QStringLiteral("mock_field"),
                                           7);
        QVERIFY(built.tree.has_value());
        ConfigurationSummaryRegistry registry;
        const auto result = registry.format({.configurationTree = &*built.tree,
                                             .configurationNode = built.structureNode,
                                             .targetFormat = QStringLiteral("   ")});
        QCOMPARE(result.status, ConfigurationSummaryStatus::InvalidRequest);
    }

    void forwardsProviderDependencyUnavailable() {
        auto built = makeConfigurationTree(QStringLiteral("MockConfig"),
                                           QStringLiteral("other_field"),
                                           7);
        QVERIFY(built.tree.has_value());
        ConfigurationSummaryRegistry registry;
        QVERIFY(registry.registerProvider(std::make_shared<MockConfigurationSummaryProvider>()));

        const auto result = registry.format({.configurationTree = &*built.tree,
                                             .configurationNode = built.structureNode,
                                             .targetFormat = QStringLiteral("mock.config")});
        QCOMPARE(result.status, ConfigurationSummaryStatus::DependencyUnavailable);
        QVERIFY(!result.formatted());
    }

    void resolvesTargetFormatIgnoringSurroundingWhitespace() {
        auto built = makeConfigurationTree(QStringLiteral("MockConfig"),
                                           QStringLiteral("mock_field"),
                                           7);
        QVERIFY(built.tree.has_value());
        ConfigurationSummaryRegistry registry;
        QVERIFY(registry.registerProvider(std::make_shared<MockConfigurationSummaryProvider>()));

        const auto result = registry.format({.configurationTree = &*built.tree,
                                             .configurationNode = built.structureNode,
                                             .targetFormat = QStringLiteral("  mock.config  ")});
        QCOMPARE(result.status, ConfigurationSummaryStatus::Formatted);
    }

    void sharedInstanceIsEmptyUntilProvidersRegister() {
        // The process-wide instance carries no built-in provider, so an
        // unconfigured lookup reports absence rather than a default summary.
        QVERIFY(ConfigurationSummaryRegistry::instance().findProvider(
                    QStringLiteral("mock.config"))
                == nullptr);
    }
};

} // namespace streamview::rules

QTEST_MAIN(streamview::rules::ConfigurationSummaryTest)
#include "configuration_summary_test.moc"
