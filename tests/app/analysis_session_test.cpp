#include "analysis_session.h"

#include <streamview/core/source.h>
#include <streamview/rules/h264_annex_b_detector.h>

#include <QTest>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <vector>

using streamview::app::AnalysisSession;
using streamview::core::MaterializationState;
using streamview::core::RandomAccessSource;
using streamview::core::SourceReadResult;
using streamview::core::SourceReadStatus;

namespace {

class MemorySource final : public RandomAccessSource {
public:
    explicit MemorySource(std::vector<std::byte> bytes,
                          QString identity = QStringLiteral("memory-source"))
        : bytes_(std::move(bytes)), identity_(std::move(identity)) {}

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return bytes_.size(); }
    [[nodiscard]] QString identity() const override { return identity_; }

    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        if (byteOffset >= bytes_.size()) {
            return {SourceReadStatus::EndOfSource, 0, {}};
        }
        const auto available = bytes_.size() - static_cast<std::size_t>(byteOffset);
        const auto count = std::min(available, destination.size());
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(byteOffset), count,
                    destination.begin());
        const auto status = count == destination.size() ? SourceReadStatus::Complete
                                                        : SourceReadStatus::EndOfSource;
        return {status, count, {}};
    }

private:
    std::vector<std::byte> bytes_;
    QString identity_;
};

class OversizedSource final : public RandomAccessSource {
public:
    explicit OversizedSource(bool* destroyed) : destroyed_(destroyed) {}
    ~OversizedSource() override { *destroyed_ = true; }

    [[nodiscard]] quint64 sizeBytes() const noexcept override {
        return (std::numeric_limits<quint64>::max() / 8U) + 1U;
    }
    [[nodiscard]] QString identity() const override { return QStringLiteral("oversized"); }
    [[nodiscard]] SourceReadResult
    readAt(quint64, std::span<std::byte> destination) const override {
        std::fill(destination.begin(), destination.end(), std::byte{0});
        return {SourceReadStatus::Complete, destination.size(), {}};
    }

private:
    bool* destroyed_ = nullptr;
};

class InitialReadFailureSource final : public RandomAccessSource {
public:
    explicit InitialReadFailureSource(bool* destroyed) : destroyed_(destroyed) {}
    ~InitialReadFailureSource() override { *destroyed_ = true; }

    [[nodiscard]] quint64 sizeBytes() const noexcept override { return 4; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("unreadable"); }
    [[nodiscard]] SourceReadResult
    readAt(quint64, std::span<std::byte>) const override {
        return {SourceReadStatus::Error, 0, QStringLiteral("initial page unavailable")};
    }

private:
    bool* destroyed_ = nullptr;
};

std::vector<std::byte> validAnnexB() {
    return {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x65}};
}

class SingleInitialReadSource final : public RandomAccessSource {
public:
    [[nodiscard]] quint64 sizeBytes() const noexcept override { return 4; }
    [[nodiscard]] QString identity() const override { return QStringLiteral("single-read"); }
    [[nodiscard]] SourceReadResult
    readAt(quint64 byteOffset, std::span<std::byte> destination) const override {
        ++readCount_;
        if (readCount_ != 1U) {
            return {SourceReadStatus::Error, 0, QStringLiteral("unexpected repeated read")};
        }
        const auto fixture = validAnnexB();
        if (byteOffset != 0U || destination.size() != fixture.size()) {
            return {SourceReadStatus::Error, 0, QStringLiteral("unexpected page request")};
        }
        std::copy(fixture.begin(), fixture.end(), destination.begin());
        return {SourceReadStatus::Complete, fixture.size(), {}};
    }

    [[nodiscard]] std::size_t readCount() const noexcept { return readCount_; }

private:
    mutable std::size_t readCount_ = 0;
};

} // namespace

class AnalysisSessionTest final : public QObject {
    Q_OBJECT

private slots:
    void ownsTheSourceAndRunsTheSharedAnalyzer() {
        QString errorMessage;
        auto session = AnalysisSession::create(
            std::make_unique<MemorySource>(validAnnexB(), QStringLiteral("fixture.264")),
            &errorMessage);

        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QCOMPARE(session->identity(), QStringLiteral("fixture.264"));
        QCOMPARE(session->sizeBytes(), quint64{4});
        QVERIFY(!session->finished());
        QVERIFY(session->formatDetection().candidate.has_value());
        QCOMPARE(session->formatDetection().candidate->confidence,
                 streamview::rules::H264AnnexBDetectionConfidence::Probable);

        while (!session->finished()) {
            const auto batch = session->analyzeBatch(1);
            QVERIFY(batch.status != streamview::rules::H264AnnexBAnalysisStatus::InvalidBatchSize);
        }
        QCOMPARE(session->scanCursor(), session->sizeBytes());

        const auto root = session->tree().node(session->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Materialized);
        QVERIFY(session->tree().nodeCount() > std::size_t{1});
    }

    void rejectsAnEmptySourceOwner() {
        QString errorMessage;

        const auto session = AnalysisSession::create(nullptr, &errorMessage);

        QVERIFY(session == nullptr);
        QVERIFY(errorMessage.contains(QStringLiteral("source"), Qt::CaseInsensitive));
    }

    void keepsTheSourceAliveThroughADeferredCoordinateError() {
        bool destroyed = false;
        QString errorMessage;

        {
            auto session = AnalysisSession::create(
                std::make_unique<OversizedSource>(&destroyed), &errorMessage);

            QVERIFY2(session != nullptr, qPrintable(errorMessage));
            const auto batch = session->analyzeBatch();
            QCOMPARE(batch.status, streamview::rules::H264AnnexBAnalysisStatus::SourceError);
            QVERIFY(batch.errorMessage.contains(QStringLiteral("bit coordinate")));
            QVERIFY(!destroyed);
        }
        QVERIFY(destroyed);
    }

    void rejectsASourceWhenItsInitialRawPageCannotBeRead() {
        bool destroyed = false;
        QString errorMessage;

        const auto session = AnalysisSession::create(
            std::make_unique<InitialReadFailureSource>(&destroyed), &errorMessage);

        QVERIFY(session == nullptr);
        QVERIFY(destroyed);
        QCOMPARE(errorMessage, QStringLiteral("initial page unavailable"));
    }

    void reusesThePreparedRawPageForFormatDetection() {
        QString errorMessage;
        auto source = std::make_unique<SingleInitialReadSource>();
        const auto* sourceObserver = source.get();

        const auto session = AnalysisSession::create(std::move(source), &errorMessage);

        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QCOMPARE(sourceObserver->readCount(), std::size_t{1});
        QVERIFY(session->formatDetection().candidate.has_value());
    }

    void keepsUnknownSourceBytesAvailableWithoutACandidate() {
        QString errorMessage;
        auto session = AnalysisSession::create(
            std::make_unique<MemorySource>(
                std::vector<std::byte>{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}}),
            &errorMessage);

        QVERIFY2(session != nullptr, qPrintable(errorMessage));
        QVERIFY(!session->formatDetection().candidate.has_value());
        QCOMPARE(session->initialPage().bytes.size(), std::size_t{3});

        while (!session->finished()) {
            (void)session->analyzeBatch();
        }
        const auto root = session->tree().node(session->tree().rootId());
        QVERIFY(root.has_value());
        QCOMPARE(root->state(), MaterializationState::Invalid);
    }
};

QTEST_GUILESS_MAIN(AnalysisSessionTest)

#include "analysis_session_test.moc"
