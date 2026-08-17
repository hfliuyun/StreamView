#pragma once

#include <streamview/core/coordinates.h>

#include <QString>
#include <QVariant>
#include <QtGlobal>

#include <compare>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace streamview::core {

class AnalysisNodeId final {
public:
    constexpr explicit AnalysisNodeId(quint64 value = 0) noexcept : value_(value) {}

    [[nodiscard]] constexpr quint64 value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const AnalysisNodeId&, const AnalysisNodeId&) = default;

private:
    quint64 value_ = 0;
};

enum class AnalysisNodeKind : quint8 {
    Root,
    Structure,
    SyntaxField,
    ComputedField,
    CompressedPayload,
    Region,
};

enum class MaterializationState : quint8 {
    Lazy,
    Indexing,
    WaitingDependency,
    Cancelled,
    Unsupported,
    Invalid,
    Materialized,
};

enum class DiagnosticSeverity : quint8 {
    Info,
    Warning,
    Error,
};

enum class DiagnosticCode : quint8 {
    TruncatedSource,
    InvalidSyntax,
    UnsupportedSyntax,
    Cancelled,
    SourceError,
    ResourceLimit,
    DependencyUnavailable,
};

struct ParseDiagnostic final {
    DiagnosticCode code = DiagnosticCode::InvalidSyntax;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    QString message;
    QString fieldPath;
    std::optional<FieldLocation> location;
};

struct AnalysisSpecification final {
    QString standard;
    QString clause;

    bool operator==(const AnalysisSpecification&) const = default;
};

struct AnalysisNodeWindowMetadata final {
    quint32 entryStructIndex = 0;
    quint32 entryCountFieldIndex = 0;
    quint64 entrySizeBits = 0;
    quint64 entryCount = 0;

    bool operator==(const AnalysisNodeWindowMetadata&) const = default;
};

struct AnalysisNodeMetadata final {
    QString typeName;
    QString description;
    std::optional<AnalysisSpecification> specification;
    std::optional<quint32> containerChildStructIndex;
    std::optional<QString> targetFormat;
    std::optional<AnalysisNodeWindowMetadata> window;

    bool operator==(const AnalysisNodeMetadata&) const = default;
};

struct AnalysisNodeSpec final {
    AnalysisNodeKind kind = AnalysisNodeKind::Structure;
    QString name;
    MaterializationState state = MaterializationState::Materialized;
    QVariant value;
    std::optional<FieldLocation> location;
    AnalysisNodeMetadata metadata;
};

class AnalysisNode final {
public:
    AnalysisNode(const AnalysisNode&) = default;
    AnalysisNode(AnalysisNode&&) noexcept = default;
    AnalysisNode& operator=(const AnalysisNode&) = default;
    AnalysisNode& operator=(AnalysisNode&&) noexcept = default;

    [[nodiscard]] AnalysisNodeId id() const noexcept { return id_; }
    [[nodiscard]] std::optional<AnalysisNodeId> parentId() const noexcept { return parentId_; }
    [[nodiscard]] AnalysisNodeKind kind() const noexcept { return kind_; }
    [[nodiscard]] const QString& name() const noexcept { return name_; }
    [[nodiscard]] MaterializationState state() const noexcept { return state_; }
    [[nodiscard]] const QVariant& value() const noexcept { return value_; }
    [[nodiscard]] const std::optional<FieldLocation>& location() const noexcept {
        return location_;
    }
    [[nodiscard]] const AnalysisNodeMetadata& metadata() const noexcept { return metadata_; }
    [[nodiscard]] const std::vector<AnalysisNodeId>& children() const noexcept {
        return children_;
    }
    [[nodiscard]] const std::vector<ParseDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    friend class AnalysisTree;

    AnalysisNode(AnalysisNodeId id,
                 std::optional<AnalysisNodeId> parentId,
                 AnalysisNodeSpec spec)
        : id_(id), parentId_(parentId), kind_(spec.kind), name_(std::move(spec.name)),
          state_(spec.state), value_(std::move(spec.value)),
          location_(std::move(spec.location)), metadata_(std::move(spec.metadata)) {}

    AnalysisNodeId id_;
    std::optional<AnalysisNodeId> parentId_;
    AnalysisNodeKind kind_ = AnalysisNodeKind::Structure;
    QString name_;
    MaterializationState state_ = MaterializationState::Materialized;
    QVariant value_;
    std::optional<FieldLocation> location_;
    AnalysisNodeMetadata metadata_;
    std::vector<AnalysisNodeId> children_;
    std::vector<ParseDiagnostic> diagnostics_;
};

class AnalysisTreeSnapshot final {
public:
    AnalysisTreeSnapshot(const AnalysisTreeSnapshot&) = delete;
    AnalysisTreeSnapshot(AnalysisTreeSnapshot&&) noexcept = default;
    AnalysisTreeSnapshot& operator=(const AnalysisTreeSnapshot&) = delete;
    AnalysisTreeSnapshot& operator=(AnalysisTreeSnapshot&&) noexcept = default;

private:
    friend class AnalysisTree;

    AnalysisTreeSnapshot(quint64 treeIdentity,
                         std::size_t nodeCount,
                         std::vector<AnalysisNode> preservedNodes)
        : treeIdentity_(treeIdentity)
        , nodeCount_(nodeCount)
        , preservedNodes_(std::move(preservedNodes)) {}

    quint64 treeIdentity_ = 0;
    std::size_t nodeCount_ = 0;
    std::vector<AnalysisNode> preservedNodes_;
};

class AnalysisTree final {
public:
    [[nodiscard]] static std::optional<AnalysisTree> create(const QString& rootName);

    AnalysisTree(const AnalysisTree& other);
    AnalysisTree(AnalysisTree&& other) noexcept;
    AnalysisTree& operator=(const AnalysisTree& other);
    AnalysisTree& operator=(AnalysisTree&& other) noexcept;

    [[nodiscard]] AnalysisNodeId rootId() const noexcept { return AnalysisNodeId(1); }
    [[nodiscard]] quint64 instanceIdentity() const noexcept { return instanceIdentity_; }
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::optional<AnalysisNode> node(AnalysisNodeId id) const;
    /// Prefer depth, then smaller total source coverage, then the lower stable node ID.
    [[nodiscard]] std::optional<AnalysisNodeId>
    mostSpecificMaterializedNodeAt(SourceBitAddress sourceBit) const noexcept;

    [[nodiscard]] std::optional<AnalysisNodeId>
    appendChild(AnalysisNodeId parentId, AnalysisNodeSpec spec);

    [[nodiscard]] bool transition(AnalysisNodeId id, MaterializationState nextState) noexcept;
    [[nodiscard]] bool addDiagnostic(AnalysisNodeId id, ParseDiagnostic diagnostic);
    [[nodiscard]] bool markPartial(AnalysisNodeId id,
                                   MaterializationState terminalState,
                                   ParseDiagnostic diagnostic);
    [[nodiscard]] bool resumeCancelled(AnalysisNodeId id) noexcept;

    [[nodiscard]] AnalysisTreeSnapshot snapshot(
        std::optional<AnalysisNodeId> preservedNode = std::nullopt) const;
    [[nodiscard]] bool restore(AnalysisTreeSnapshot snapshot) noexcept;

    [[nodiscard]] bool hasPartialResults() const noexcept;
    [[nodiscard]] bool isFullyMaterialized() const noexcept;

private:
    explicit AnalysisTree(AnalysisNode root) : instanceIdentity_(nextInstanceIdentity()) {
        nodes_.push_back(std::move(root));
    }
    [[nodiscard]] static quint64 nextInstanceIdentity() noexcept;

    [[nodiscard]] AnalysisNode* nodeForMutation(AnalysisNodeId id) noexcept;
    [[nodiscard]] const AnalysisNode* nodeForRead(AnalysisNodeId id) const noexcept;
    [[nodiscard]] static bool canTransition(MaterializationState from,
                                             MaterializationState to) noexcept;
    [[nodiscard]] static bool isValidSpec(const AnalysisNodeSpec& spec) noexcept;

    quint64 instanceIdentity_ = 0;
    std::vector<AnalysisNode> nodes_;
};

} // namespace streamview::core
