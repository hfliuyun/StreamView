#pragma once

#include <streamview/rules/format_selection.h>
#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_package.h>

#include <QDialog>
#include <optional>
#include <vector>

class QLabel;
class QListWidget;
class QDialogButtonBox;

namespace streamview::app {

struct FormatOverrideOption {
    QString displayName;
    QString description;
    rules::RuleEntryPointIdentity entryPoint;
    bool isConflictCandidate = false;
    bool isCurrent = false;
};

class FormatOverrideDialog : public QDialog {
    Q_OBJECT

public:
    explicit FormatOverrideDialog(
        QWidget* parent,
        const rules::RulePackageCatalog& catalog,
        const rules::FormatSelection& formatSelection,
        const rules::RuleEntryPointIdentity& currentRule);

    [[nodiscard]] std::optional<rules::RuleEntryPointIdentity> selectedRuleEntryPoint() const;

private slots:
    void onItemSelectionChanged();

private:
    void populateOptions(const rules::RulePackageCatalog& catalog,
                         const rules::FormatSelection& formatSelection,
                         const rules::RuleEntryPointIdentity& currentRule);

    QLabel* headerLabel_ = nullptr;
    QLabel* ambiguityBannerLabel_ = nullptr;
    QListWidget* optionsListWidget_ = nullptr;
    QLabel* descriptionLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;

    std::vector<FormatOverrideOption> options_;
};

} // namespace streamview::app
