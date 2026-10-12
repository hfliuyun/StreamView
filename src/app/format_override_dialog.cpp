#include "format_override_dialog.h"
#include "localization_manager.h"

#include <streamview/core/source.h>
#include <streamview/core/version.h>
#include <streamview/rules/aac_adts_analyzer.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/language_version.h>
#include <streamview/rules/mp4_isobmff_analyzer.h>
#include <streamview/rules/rule_execution_session.h>

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace streamview::app {

namespace {

class DummySource final : public core::RandomAccessSource {
public:
    [[nodiscard]] QString identity() const override { return QStringLiteral("dummy"); }
    [[nodiscard]] quint64 sizeBytes() const noexcept override { return 0; }
    [[nodiscard]] core::SourceReadResult
    readAt(quint64, std::span<std::byte>) const override {
        return core::SourceReadResult{core::SourceReadStatus::EndOfSource, 0, QString()};
    }
};

bool canConstructRootAnalyzer(const rules::RuleCatalogLookupResult& resolvedRule,
                              rules::DetectedFormat* outFormat) {
    if (!resolvedRule.succeeded() || !resolvedRule.entryPoint) {
        return false;
    }
    if (resolvedRule.entryPoint->depth == QStringLiteral("structural")) {
        return false;
    }

    DummySource dummy;
    QString err;

    if (resolvedRule.package->manifest().packageId == QStringLiteral("org.streamview.aac") ||
        resolvedRule.entryPoint->format == QStringLiteral("audio.aac.adts")) {
        if (auto analyzer = rules::AacAdtsAnalyzer::create(dummy, resolvedRule, &err)) {
            if (outFormat != nullptr) {
                *outFormat = rules::DetectedFormat::AacAdts;
            }
            return true;
        }
    } else if (resolvedRule.package->manifest().packageId == QStringLiteral("org.streamview.mp4") ||
               resolvedRule.entryPoint->format == QStringLiteral("video.mp4") ||
               resolvedRule.entryPoint->format == QStringLiteral("video/mp4")) {
        if (auto analyzer = rules::Mp4IsobmffAnalyzer::create(dummy, resolvedRule, &err)) {
            if (outFormat != nullptr) {
                *outFormat = rules::DetectedFormat::Mp4Isobmff;
            }
            return true;
        }
    } else {
        if (auto analyzer = rules::H264AnnexBAnalyzer::create(dummy, resolvedRule, &err)) {
            if (outFormat != nullptr) {
                *outFormat = rules::DetectedFormat::H264AnnexB;
            }
            return true;
        }
    }
    return false;
}

} // namespace

FormatOverrideDialog::FormatOverrideDialog(
    QWidget* parent,
    const rules::RulePackageCatalog& catalog,
    const rules::FormatSelection& formatSelection,
    const rules::RuleEntryPointIdentity& currentRule)
    : QDialog(parent),
      catalog_(&catalog),
      formatSelection_(formatSelection),
      currentRule_(currentRule) {
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
    buttonBox_->installEventFilter(this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(tr("Override"));
    mainLayout->addWidget(buttonBox_);

    connect(optionsListWidget_, &QListWidget::currentRowChanged,
            this, &FormatOverrideDialog::onItemSelectionChanged);
    connect(optionsListWidget_, &QListWidget::itemDoubleClicked,
            this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(&LocalizationManager::instance(), &LocalizationManager::languageChanged,
            this, &FormatOverrideDialog::retranslateUi);

    populateOptions(catalog, formatSelection, currentRule);
}

void FormatOverrideDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

bool FormatOverrideDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == buttonBox_ && event != nullptr && event->type() == QEvent::LanguageChange) {
        buttonBox_->removeEventFilter(this);
        QCoreApplication::sendEvent(buttonBox_, event);
        buttonBox_->installEventFilter(this);
        retranslateUi();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void FormatOverrideDialog::retranslateUi() {
    setWindowTitle(tr("Override Format"));
    if (headerLabel_ != nullptr) {
        headerLabel_->setText(
            tr("Select the target format to override automatic format detection:"));
    }
    if (ambiguityBannerLabel_ != nullptr && formatSelection_.ambiguous()) {
        ambiguityBannerLabel_->setText(
            tr("Notice: Multiple conflicting formats (container vs elementary stream) were detected. "
               "Choose your intended format to resolve the ambiguity."));
    }
    if (buttonBox_ != nullptr && buttonBox_->button(QDialogButtonBox::Ok) != nullptr) {
        buttonBox_->button(QDialogButtonBox::Ok)->setText(tr("Override"));
    }
    if (catalog_ != nullptr) {
        const auto prevSelected = selectedRuleEntryPoint();
        populateOptions(*catalog_, formatSelection_, currentRule_);
        if (prevSelected.has_value()) {
            for (int i = 0; i < static_cast<int>(options_.size()); ++i) {
                if (options_[static_cast<std::size_t>(i)].entryPoint == *prevSelected) {
                    optionsListWidget_->setCurrentRow(i);
                    break;
                }
            }
        }
    }
}

void FormatOverrideDialog::populateOptions(
    const rules::RulePackageCatalog& catalog,
    const rules::FormatSelection& formatSelection,
    const rules::RuleEntryPointIdentity& currentRule) {
    options_.clear();
    optionsListWidget_->clear();

    struct CandidateItem {
        int rank = 3;
        QString displayName;
        QString description;
        rules::RuleEntryPointIdentity entryPoint;
        rules::DetectedFormat detectedFormat = rules::DetectedFormat::H264AnnexB;
        bool isCurrent = false;
        bool isConflictCandidate = false;
        QString sortKey1;
        QString sortKey2;
        QByteArray sortKey3;
        QString sortKey4;
    };

    std::vector<CandidateItem> candidates;

    // 1. Iterate over catalog packages
    for (const auto& package : catalog.allPackages()) {
        if (!package) {
            continue;
        }
        const auto& manifest = package->manifest();
        const auto pkgIdentity = package->identity();

        for (const auto& ep : manifest.entryPoints) {
            // Depth check: skip structural only
            if (ep.depth == QStringLiteral("structural")) {
                continue;
            }

            auto entryId = rules::RuleEntryPointIdentity::create(pkgIdentity, ep.id);
            if (!entryId.has_value()) {
                continue;
            }

            auto resolvedRule = catalog.resolve(
                entryId->packageIdentity(), entryId->entryPointId(),
                rules::languageVersion(), core::version());
            if (!resolvedRule.succeeded() || !resolvedRule.package || !resolvedRule.entryPoint) {
                continue;
            }

            rules::DetectedFormat detectedFormat = rules::DetectedFormat::H264AnnexB;
            if (!canConstructRootAnalyzer(resolvedRule, &detectedFormat)) {
                continue;
            }

            int rank = 3;
            QString displayName;
            QString description;

            if (manifest.packageId == QStringLiteral("org.streamview.mp4") && ep.id == QStringLiteral("main")) {
                rank = 0;
                displayName = tr("MP4 (ISOBMFF)");
                description = tr("ISO Base Media File Format (ISO/IEC 14496-12) container with box structures.");
            } else if (manifest.packageId == QStringLiteral("org.streamview.h264") && ep.id == QStringLiteral("annex-b")) {
                rank = 1;
                displayName = tr("H.264 (Annex B)");
                description = tr("H.264 / AVC byte stream with Annex B start codes (0x000001).");
            } else if (manifest.packageId == QStringLiteral("org.streamview.aac") && ep.id == QStringLiteral("adts")) {
                rank = 2;
                displayName = tr("AAC (ADTS)");
                description = tr("Advanced Audio Coding (AAC) in Audio Data Transport Stream (ADTS) frames.");
            } else {
                rank = 3;
                displayName = QStringLiteral("%1 v%2 (%3)").arg(manifest.packageId, pkgIdentity.packageVersion(), ep.id);
                description = tr("Package: %1\nVersion: %2\nEntry Point: %3\nFormat: %4\nHash: %5")
                                  .arg(manifest.packageId,
                                       pkgIdentity.packageVersion(),
                                       ep.id,
                                       ep.format,
                                       pkgIdentity.contentHashText());
            }

            const bool isCurrent = (*entryId == currentRule);
            bool isConflictCandidate = false;
            if (formatSelection.ambiguous()) {
                if (detectedFormat == rules::DetectedFormat::Mp4Isobmff ||
                    detectedFormat == rules::DetectedFormat::H264AnnexB) {
                    isConflictCandidate = true;
                }
            }

            candidates.push_back(CandidateItem{
                .rank = rank,
                .displayName = std::move(displayName),
                .description = std::move(description),
                .entryPoint = std::move(*entryId),
                .detectedFormat = detectedFormat,
                .isCurrent = isCurrent,
                .isConflictCandidate = isConflictCandidate,
                .sortKey1 = manifest.packageId,
                .sortKey2 = pkgIdentity.packageVersion(),
                .sortKey3 = pkgIdentity.contentHash(),
                .sortKey4 = ep.id,
            });
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const CandidateItem& a, const CandidateItem& b) {
        if (a.rank != b.rank) {
            return a.rank < b.rank;
        }
        if (a.sortKey1 != b.sortKey1) {
            return a.sortKey1 < b.sortKey1;
        }
        if (a.sortKey2 != b.sortKey2) {
            return a.sortKey2 < b.sortKey2;
        }
        if (a.sortKey3 != b.sortKey3) {
            return a.sortKey3 < b.sortKey3;
        }
        return a.sortKey4 < b.sortKey4;
    });

    int selectedRow = -1;
    int conflictCandidateRow = -1;

    for (auto& cand : candidates) {
        FormatOverrideOption opt{
            .displayName = cand.displayName,
            .description = cand.description,
            .entryPoint = cand.entryPoint,
            .isConflictCandidate = cand.isConflictCandidate,
            .isCurrent = cand.isCurrent,
        };

        QString itemText = opt.displayName;
        if (opt.isCurrent) {
            itemText += tr(" [Current]");
        }
        if (opt.isConflictCandidate) {
            itemText += tr(" [Candidate]");
        }

        auto* item = new QListWidgetItem(itemText, optionsListWidget_);
        if (opt.isConflictCandidate) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }

        const int currentRow = static_cast<int>(options_.size());
        options_.push_back(std::move(opt));

        if (cand.isCurrent) {
            selectedRow = currentRow;
        } else if (cand.isConflictCandidate && conflictCandidateRow < 0) {
            conflictCandidateRow = currentRow;
        }
    }

    if (formatSelection.ambiguous() && conflictCandidateRow >= 0) {
        optionsListWidget_->setCurrentRow(conflictCandidateRow);
    } else if (selectedRow >= 0) {
        optionsListWidget_->setCurrentRow(selectedRow);
    } else if (!options_.empty()) {
        optionsListWidget_->setCurrentRow(0);
    } else {
        descriptionLabel_->clear();
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(false);
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
