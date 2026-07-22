#pragma once

#include <streamview/core/analysis_model.h>

#include <QAbstractItemModel>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <optional>
#include <vector>

namespace streamview::app {

/// Read-only Qt item model that presents an AnalysisTree in a QTreeView.
///
/// Columns:
///   0 – Name      (node name, e.g. "nal_unit[0]")
///   1 – Value     (decoded value, e.g. "5")
///   2 – Bits      (source bit range, e.g. "[24, 32)")
///   3 – State     (materialization state text)
class AnalysisTreeModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Column : int {
        Name = 0,
        Value = 1,
        Bits = 2,
        State = 3,
        ColumnCount = 4,
    };

    explicit AnalysisTreeModel(QObject* parent = nullptr);

    /// Replace the backing snapshot.  Emits modelReset.
    void resetFromTree(const core::AnalysisTree& tree);

    /// Clear all data.  Emits modelReset.
    void clear();

    // --- QAbstractItemModel overrides ---
    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;

    /// Return the AnalysisNodeId stored at the given model index, if valid.
    [[nodiscard]] std::optional<core::AnalysisNodeId> nodeIdAt(const QModelIndex& index) const;

private:
    /// Flat snapshot of one AnalysisNode, stored by index for O(1) lookup.
    struct FlatNode {
        core::AnalysisNodeId id{0};
        core::AnalysisNodeKind kind = core::AnalysisNodeKind::Root;
        QString name;
        core::MaterializationState state = core::MaterializationState::Materialized;
        QVariant value;
        std::optional<core::FieldLocation> location;
        std::vector<core::ParseDiagnostic> diagnostics;

        int parentFlatIndex = -1; ///< -1 = invisible root
        int rowInParent = 0;      ///< Row within parent's children
        std::vector<int> childFlatIndices; ///< Flat indices of children
    };

    int flatten(const core::AnalysisTree& tree, core::AnalysisNodeId id, int parentFlatIndex,
                int rowInParent);

    [[nodiscard]] int flatIndexAt(const QModelIndex& index) const;
    [[nodiscard]] const FlatNode* flatNodeAt(const QModelIndex& index) const;
    [[nodiscard]] static QString formatBits(const std::optional<core::FieldLocation>& location);
    [[nodiscard]] static QString stateName(core::MaterializationState state);

    std::vector<FlatNode> nodes_;
};

} // namespace streamview::app
