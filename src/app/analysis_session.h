#pragma once

#include <streamview/core/analysis_model.h>
#include <streamview/core/source.h>
#include <streamview/core/source_pager.h>
#include <streamview/rules/h264_annex_b_analyzer.h>
#include <streamview/rules/h264_annex_b_detector.h>

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <memory>

namespace streamview::app {

class AnalysisSession final {
public:
    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    openFile(const QString& path, QString* errorMessage = nullptr);

    [[nodiscard]] static std::unique_ptr<AnalysisSession>
    create(std::unique_ptr<core::RandomAccessSource> source,
           QString* errorMessage = nullptr);

    AnalysisSession(const AnalysisSession&) = delete;
    AnalysisSession& operator=(const AnalysisSession&) = delete;
    AnalysisSession(AnalysisSession&&) = delete;
    AnalysisSession& operator=(AnalysisSession&&) = delete;
    ~AnalysisSession() = default;

    [[nodiscard]] const core::RandomAccessSource& source() const noexcept { return *source_; }
    [[nodiscard]] QString identity() const { return source_->identity(); }
    [[nodiscard]] quint64 sizeBytes() const noexcept { return source_->sizeBytes(); }
    [[nodiscard]] const core::SourcePage& initialPage() const noexcept { return initialPage_; }
    [[nodiscard]] const rules::H264AnnexBDetectionResult& formatDetection() const noexcept {
        return formatDetection_;
    }

    [[nodiscard]] rules::H264AnnexBAnalysisBatch analyzeBatch(
        std::size_t maximumRecords = 256,
        quint64 maximumInspectedPositions = rules::H264StartCodeScanner::defaultWorkBudget());
    [[nodiscard]] const core::AnalysisTree& tree() const noexcept { return analyzer_.tree(); }
    [[nodiscard]] bool finished() const noexcept { return analyzer_.finished(); }
    [[nodiscard]] quint64 scanCursor() const noexcept { return analyzer_.scanCursor(); }

private:
    AnalysisSession(std::unique_ptr<core::RandomAccessSource> source,
                    core::SourcePage initialPage,
                    rules::H264AnnexBDetectionResult formatDetection,
                    rules::H264AnnexBAnalyzer analyzer);

    std::unique_ptr<core::RandomAccessSource> source_;
    core::SourcePage initialPage_;
    rules::H264AnnexBDetectionResult formatDetection_;
    rules::H264AnnexBAnalyzer analyzer_;
};

} // namespace streamview::app
