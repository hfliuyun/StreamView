#pragma once

#include <streamview/rules/rule_catalog.h>
#include <streamview/rules/rule_package_store.h>

#include <QDialog>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <set>
#include <vector>

class QLabel;
class QPushButton;
class QTableWidget;
class QDialogButtonBox;

namespace streamview::app {

struct RulePackageViewModel {
    rules::RulePackageIdentity identity;
    QString description;
    QStringList entryPoints;
    bool isBundled = false;

    RulePackageViewModel(rules::RulePackageIdentity id,
                         QString desc,
                         QStringList eps,
                         bool bundled)
        : identity(std::move(id)),
          description(std::move(desc)),
          entryPoints(std::move(eps)),
          isBundled(bundled) {}
};

class RuleManagerDialog : public QDialog {
    Q_OBJECT

public:
    using FileDialogHandler = std::function<QString(QWidget*, const QString&, const QString&)>;
    using MessageDialogHandler = std::function<void(QWidget*, const QString&, const QString&)>;

    explicit RuleManagerDialog(
        QWidget* parent,
        rules::RulePackageCatalog& catalog,
        QString storeRoot,
        std::set<QString> bundledPackageIds = {});

    void setFileDialogHandlerForTesting(FileDialogHandler handler) {
        fileDialogHandler_ = std::move(handler);
    }

    void setMessageDialogHandlerForTesting(MessageDialogHandler handler) {
        messageDialogHandler_ = std::move(handler);
    }

    [[nodiscard]] const std::vector<RulePackageViewModel>& packages() const noexcept {
        return packages_;
    }

    void refreshPackages();

public slots:
    void installPackage();

private:
    rules::RulePackageCatalog& catalog_;
    QString storeRoot_;
    std::set<QString> bundledPackageIds_;

    QLabel* headerLabel_ = nullptr;
    QTableWidget* tableWidget_ = nullptr;
    QPushButton* installButton_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;

    std::vector<RulePackageViewModel> packages_;

    FileDialogHandler fileDialogHandler_;
    MessageDialogHandler messageDialogHandler_;
};

} // namespace streamview::app
