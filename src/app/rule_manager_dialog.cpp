#include "rule_manager_dialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace streamview::app {

RuleManagerDialog::RuleManagerDialog(
    QWidget* parent,
    rules::RulePackageCatalog& catalog,
    QString storeRoot,
    std::set<QString> bundledPackageIds)
    : QDialog(parent),
      catalog_(catalog),
      storeRoot_(std::move(storeRoot)),
      bundledPackageIds_(std::move(bundledPackageIds)) {
    if (!storeRoot_.isEmpty()) {
        QDir().mkpath(storeRoot_);
        const QString canonical = QFileInfo(storeRoot_).canonicalFilePath();
        if (!canonical.isEmpty()) {
            storeRoot_ = canonical;
        }
    }
    setWindowTitle(tr("Rule Package Manager"));
    setObjectName(QStringLiteral("ruleManagerDialog"));
    resize(760, 420);

    auto* mainLayout = new QVBoxLayout(this);

    headerLabel_ = new QLabel(
        tr("Manage format rule packages. Bundled packages are read-only official assets. "
           "External packages (.svrule) can be installed into the local user repository."),
        this);
    headerLabel_->setObjectName(QStringLiteral("headerLabel"));
    headerLabel_->setWordWrap(true);
    headerLabel_->setStyleSheet(QStringLiteral("color: #555555; font-size: 11px; margin-bottom: 4px;"));
    mainLayout->addWidget(headerLabel_);

    tableWidget_ = new QTableWidget(this);
    tableWidget_->setObjectName(QStringLiteral("rulePackageTable"));
    tableWidget_->setColumnCount(6);
    tableWidget_->setHorizontalHeaderLabels({
        tr("ID"),
        tr("Version"),
        tr("Origin"),
        tr("Content Hash"),
        tr("Entry Points"),
        tr("Description")
    });
    tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableWidget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableWidget_->horizontalHeader()->setStretchLastSection(true);
    tableWidget_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    tableWidget_->verticalHeader()->setVisible(false);
    mainLayout->addWidget(tableWidget_);

    auto* bottomLayout = new QHBoxLayout();
    installButton_ = new QPushButton(tr("Install Package (.svrule)..."), this);
    installButton_->setObjectName(QStringLiteral("installPackageButton"));
    bottomLayout->addWidget(installButton_);

    bottomLayout->addStretch();

    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttonBox_->setObjectName(QStringLiteral("buttonBox"));
    bottomLayout->addWidget(buttonBox_);

    mainLayout->addLayout(bottomLayout);

    connect(installButton_, &QPushButton::clicked, this, &RuleManagerDialog::installPackage);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::accept);

    refreshPackages();
}

void RuleManagerDialog::refreshPackages() {
    packages_.clear();
    tableWidget_->setRowCount(0);

    const auto catalogPackages = catalog_.allPackages();
    packages_.reserve(catalogPackages.size());

    for (const auto& pkg : catalogPackages) {
        if (!pkg) {
            continue;
        }
        QStringList entryPointIds;
        QStringList formats;
        for (const auto& ep : pkg->manifest().entryPoints) {
            entryPointIds.append(ep.id);
            if (!ep.format.isEmpty() && !formats.contains(ep.format)) {
                formats.append(ep.format);
            }
        }
        const QString description = formats.join(QStringLiteral(", "));
        const bool isBundled =
            (bundledPackageIds_.find(pkg->identity().packageId()) != bundledPackageIds_.end());

        RulePackageViewModel vm(pkg->identity(), description, entryPointIds, isBundled);

        const int row = tableWidget_->rowCount();
        tableWidget_->insertRow(row);

        // ID
        auto* idItem = new QTableWidgetItem(vm.identity.packageId());
        tableWidget_->setItem(row, 0, idItem);

        // Version
        auto* verItem = new QTableWidgetItem(vm.identity.packageVersion());
        tableWidget_->setItem(row, 1, verItem);

        // Origin
        auto* originItem = new QTableWidgetItem(vm.isBundled ? tr("[Bundled]") : tr("[Installed]"));
        if (vm.isBundled) {
            originItem->setForeground(QColor(0x00, 0x66, 0xcc));
        } else {
            originItem->setForeground(QColor(0x28, 0xa7, 0x45));
        }
        tableWidget_->setItem(row, 2, originItem);

        // Content Hash (abbreviated, full in tooltip)
        const QString fullHex = QString::fromLatin1(vm.identity.contentHash().toHex());
        const QString shortHex = fullHex.left(12) + QStringLiteral("...");
        auto* hashItem = new QTableWidgetItem(shortHex);
        hashItem->setToolTip(fullHex);
        tableWidget_->setItem(row, 3, hashItem);

        // Entry Points
        auto* epItem = new QTableWidgetItem(vm.entryPoints.join(QStringLiteral(", ")));
        tableWidget_->setItem(row, 4, epItem);

        // Description
        auto* descItem = new QTableWidgetItem(vm.description);
        tableWidget_->setItem(row, 5, descItem);

        packages_.push_back(std::move(vm));
    }
}

void RuleManagerDialog::installPackage() {
    QString archivePath;
    if (fileDialogHandler_) {
        archivePath = fileDialogHandler_(
            this,
            tr("Install Rule Package"),
            tr("StreamView Rule Package (*.svrule);;All Files (*)"));
    } else {
        archivePath = QFileDialog::getOpenFileName(
            this,
            tr("Install Rule Package"),
            QString(),
            tr("StreamView Rule Package (*.svrule);;All Files (*)"));
    }

    if (archivePath.isEmpty()) {
        return;
    }

    const auto imported = rules::RulePackageStore::importArchive(archivePath);
    if (!imported.succeeded() || !imported.package.has_value()) {
        const QString detail = imported.errorMessage.isEmpty()
                                   ? tr("Unknown archive corruption or format error")
                                   : imported.errorMessage;
        if (messageDialogHandler_) {
            messageDialogHandler_(
                this,
                tr("Invalid Rule Package"),
                tr("Failed to read package archive %1:\n%2").arg(archivePath, detail));
        } else {
            QMessageBox::warning(
                this,
                tr("Invalid Rule Package"),
                tr("Failed to read package archive %1:\n%2").arg(archivePath, detail));
        }
        return;
    }

    // Protection for bundled packages
    if (bundledPackageIds_.find(imported.package->identity().packageId()) != bundledPackageIds_.end()) {
        const QString msg = tr("Cannot overwrite or replace bundled official rule package: %1")
                                .arg(imported.package->identity().packageId());
        if (messageDialogHandler_) {
            messageDialogHandler_(this, tr("Package Installation Rejected"), msg);
        } else {
            QMessageBox::warning(this, tr("Package Installation Rejected"), msg);
        }
        return;
    }

    // If storeRoot is specified, install content-addressed files
    if (!storeRoot_.isEmpty()) {
        const auto installResult = rules::RulePackageStore::install(*imported.package, storeRoot_);
        if (!installResult.succeeded()) {
            if (messageDialogHandler_) {
                messageDialogHandler_(
                    this,
                    tr("Installation Failed"),
                    tr("Failed to store package content:\n%1").arg(installResult.errorMessage));
            } else {
                QMessageBox::warning(
                    this,
                    tr("Installation Failed"),
                    tr("Failed to store package content:\n%1").arg(installResult.errorMessage));
            }
            return;
        }
    }

    // Register into catalog
    auto pkgCopy = *imported.package;
    const auto regResult = catalog_.registerPackage(std::move(pkgCopy));
    if (!regResult.succeeded()) {
        if (messageDialogHandler_) {
            messageDialogHandler_(
                this,
                tr("Registration Conflict"),
                tr("Failed to register package:\n%1").arg(regResult.errorMessage));
        } else {
            QMessageBox::warning(
                this,
                tr("Registration Conflict"),
                tr("Failed to register package:\n%1").arg(regResult.errorMessage));
        }
        return;
    }

    refreshPackages();

    if (messageDialogHandler_) {
        messageDialogHandler_(
            this,
            tr("Package Installed"),
            tr("Successfully installed rule package %1 (v%2).")
                .arg(imported.package->identity().packageId(),
                     imported.package->identity().packageVersion()));
    } else {
        QMessageBox::information(
            this,
            tr("Package Installed"),
            tr("Successfully installed rule package %1 (v%2).")
                .arg(imported.package->identity().packageId(),
                     imported.package->identity().packageVersion()));
    }
}

} // namespace streamview::app
