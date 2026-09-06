#include "format_override_dialog.h"

#include <streamview/core/version.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/language_version.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/rule_execution_session.h>

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace streamview::app {

FormatOverrideDialog::FormatOverrideDialog(
    QWidget* parent,
    const rules::RulePackageCatalog& catalog,
    const rules::FormatSelection& formatSelection,
    const rules::RuleEntryPointIdentity& currentRule)
    : QDialog(parent) {
    setWindowTitle(tr("Override Format"));
    setObjectName(QStringLiteral("formatOverrideDialog"));
    resize(520, 400);

    auto* mainLayout = new QVBoxLayout(this);

    headerLabel_ = new QLabel(
        tr("Select the target format to override automatic format detection:"), this);
    headerLabel_->setObjectName(QStringLiteral("headerLabel"));
    mainLayout->addWidget(headerLabel_);

    ambiguityBannerLabel_ = new QLabel(this);
    ambiguityBannerLabel_->setObjectName(QStringLiteral("dialogAmbiguityBanner"));
    ambiguityBannerLabel_->setWordWrap(true);
    ambiguityBannerLabel_->setStyleSheet(
        QStringLiteral("background-color: #fff3cd; color: #856404; border: 1px solid #ffeeba; "
                       "padding: 8px; border-radius: 4px; font-size: 11px;"));
    if (formatSelection.ambiguous()) {
        ambiguityBannerLabel_->setText(
            tr("Notice: Multiple conflicting formats (container vs elementary stream) were detected. "
               "Choose your intended format to resolve the ambiguity."));
        ambiguityBannerLabel_->show();
    } else {
        ambiguityBannerLabel_->hide();
    }
    mainLayout->addWidget(ambiguityBannerLabel_);

    optionsListWidget_ = new QListWidget(this);
    optionsListWidget_->setObjectName(QStringLiteral("optionsListWidget"));
    mainLayout->addWidget(optionsListWidget_);

    descriptionLabel_ = new QLabel(this);
    descriptionLabel_->setObjectName(QStringLiteral("descriptionLabel"));
    descriptionLabel_->setWordWrap(true);
    descriptionLabel_->setStyleSheet(
        QStringLiteral("color: #666666; font-size: 11px; padding: 4px;"));
    mainLayout->addWidget(descriptionLabel_);

    buttonBox_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->setObjectName(QStringLiteral("buttonBox"));
    buttonBox_->button(QDialogButtonBox::Ok)->setText(tr("Override"));
    mainLayout->addWidget(buttonBox_);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(optionsListWidget_, &QListWidget::itemSelectionChanged,
            this, &FormatOverrideDialog::onItemSelectionChanged);
    connect(optionsListWidget_, &QListWidget::itemDoubleClicked,
            this, &QDialog::accept);

    populateOptions(catalog, formatSelection, currentRule);
}

void FormatOverrideDialog::populateOptions(
    const rules::RulePackageCatalog& catalog,
    const rules::FormatSelection& formatSelection,
    const rules::RuleEntryPointIdentity& currentRule) {
    options_.clear();
    optionsListWidget_->clear();

    struct BuiltinSpec {
        QString displayName;
        QString description;
        rules::RulePackageIdentity identity;
        QString entryPoint;
        rules::DetectedFormat format;
    };

    std::vector<BuiltinSpec> specs;

    if (auto mp4 = rules::loadMp4IsobmffRulePackage(); mp4.succeeded() && mp4.package.has_value()) {
        specs.push_back({
            QStringLiteral("MP4 (ISOBMFF)"),
            QStringLiteral("ISO Base Media File Format (ISO/IEC 14496-12) container with box structures."),
            mp4.package->identity(),
            QStringLiteral("main"),
            rules::DetectedFormat::Mp4Isobmff
        });
    }

    if (auto h264 = rules::loadH264AnnexBRulePackage(); h264.succeeded() && h264.package.has_value()) {
        specs.push_back({
            QStringLiteral("H.264 (Annex B)"),
            QStringLiteral("H.264 / AVC byte stream with Annex B start codes (0x000001)."),
            h264.package->identity(),
            QStringLiteral("annex-b"),
            rules::DetectedFormat::H264AnnexB
        });
    }

    if (auto aac = rules::loadAacAdtsRulePackage(); aac.succeeded() && aac.package.has_value()) {
        specs.push_back({
            QStringLiteral("AAC (ADTS)"),
            QStringLiteral("Advanced Audio Coding (AAC) in Audio Data Transport Stream (ADTS) frames."),
            aac.package->identity(),
            QStringLiteral("adts"),
            rules::DetectedFormat::AacAdts
        });
    }

    int selectedRow = -1;
    int conflictCandidateRow = -1;

    for (const auto& spec : specs) {
        auto resolved = catalog.resolve(spec.identity, spec.entryPoint,
                                        rules::languageVersion(), core::version());
        if (!resolved.succeeded()) {
            continue;
        }
        auto entryId = rules::RuleEntryPointIdentity::create(spec.identity, spec.entryPoint);
        if (!entryId.has_value()) {
            continue;
        }

        const bool isCurrent = (*entryId == currentRule);
        bool isConflictCandidate = false;
        if (formatSelection.ambiguous()) {
            if (spec.format == rules::DetectedFormat::Mp4Isobmff ||
                spec.format == rules::DetectedFormat::H264AnnexB) {
                isConflictCandidate = true;
            }
        }

        FormatOverrideOption opt{
            .displayName = spec.displayName,
            .description = spec.description,
            .entryPoint = *entryId,
            .isConflictCandidate = isConflictCandidate,
            .isCurrent = isCurrent,
        };

        QString itemText = opt.displayName;
        if (isCurrent) {
            itemText += tr(" [Current]");
        }
        if (isConflictCandidate) {
            itemText += tr(" [Candidate]");
        }

        auto* item = new QListWidgetItem(itemText, optionsListWidget_);
        if (isConflictCandidate) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }

        const int currentRow = static_cast<int>(options_.size());
        options_.push_back(std::move(opt));

        if (isCurrent) {
            selectedRow = currentRow;
        } else if (isConflictCandidate && conflictCandidateRow < 0) {
            conflictCandidateRow = currentRow;
        }
    }

    if (formatSelection.ambiguous() && conflictCandidateRow >= 0) {
        optionsListWidget_->setCurrentRow(conflictCandidateRow);
    } else if (selectedRow >= 0) {
        optionsListWidget_->setCurrentRow(selectedRow);
    } else if (!options_.empty()) {
        optionsListWidget_->setCurrentRow(0);
    }
}

void FormatOverrideDialog::onItemSelectionChanged() {
    const int row = optionsListWidget_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= options_.size()) {
        descriptionLabel_->clear();
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }
    descriptionLabel_->setText(options_[static_cast<std::size_t>(row)].description);
    buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(true);
}

std::optional<rules::RuleEntryPointIdentity> FormatOverrideDialog::selectedRuleEntryPoint() const {
    const int row = optionsListWidget_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= options_.size()) {
        return std::nullopt;
    }
    return options_[static_cast<std::size_t>(row)].entryPoint;
}

} // namespace streamview::app
