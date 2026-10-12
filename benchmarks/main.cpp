#include "analysis_tree_model.h"
#include "main_window.h"

#include <streamview/core/source_pager.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressBar>
#include <QStatusBar>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QTreeView>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {

using streamview::app::AnalysisTreeModel;
using streamview::app::MainWindow;
using streamview::core::AnalysisNodeId;
using streamview::core::AnalysisNodeSpec;
using streamview::core::AnalysisTree;

void require(bool condition, const QString& message) {
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

double milliseconds(const QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
}

quint64 peakRssBytes() {
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS counters{};
    require(GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                                 static_cast<DWORD>(sizeof(counters))) != 0,
            QStringLiteral("Cannot read process memory counters"));
    return static_cast<quint64>(counters.PeakWorkingSetSize);
#else
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0,
            QStringLiteral("Cannot read process resource usage"));
#if defined(Q_OS_MACOS)
    return static_cast<quint64>(usage.ru_maxrss);
#else
    return static_cast<quint64>(usage.ru_maxrss) * 1024ULL;
#endif
#endif
}

void await(const std::function<bool()>& ready) {
    QElapsedTimer timeout;
    timeout.start();
    while (!ready()) {
        require(timeout.elapsed() < 30'000, QStringLiteral("Analysis timed out"));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

QTreeView* treeView(MainWindow& window) {
    auto* view = window.findChild<QTreeView*>(QStringLiteral("analysisTreeView"));
    require(view != nullptr, QStringLiteral("Tree view is missing"));
    return view;
}

void openAndPaint(MainWindow& window, const QString& path) {
    QString error;
    window.show();
    require(window.openMediaSource(path, &error), error);
    auto* view = treeView(window);
    await([view] { return view->model()->rowCount() > 0; });
    QCoreApplication::processEvents();
    const auto image = window.grab();
    require(!image.isNull(), QStringLiteral("Window painting returned an empty image"));
}

QJsonObject pageReads(const QString& path) {
    QString error;
    auto source = streamview::core::FileSource::open(path, &error);
    require(source != nullptr, error);
    const streamview::core::SourcePager pager(*source);
    require(pager.pageCount() > 1, QStringLiteral("Source must span multiple pages"));
    std::vector<double> durations;
    durations.reserve(1'000);
    quint64 state = 0x123456789abcdef0ULL;
    quint64 bytesRead = 0;
    for (int query = 0; query < 1'000; ++query) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const quint64 pageIndex = state % (pager.pageCount() - 1);
        QElapsedTimer timer;
        timer.start();
        const auto page = pager.loadPage(pageIndex);
        durations.push_back(milliseconds(timer));
        require(page.succeeded() && page.byteOffset ==
                    pageIndex * streamview::core::SourcePager::pageSizeBytes() &&
                    page.bytes.size() == streamview::core::SourcePager::pageSizeBytes(),
                QStringLiteral("Page read failed or returned the wrong range"));
        bytesRead += static_cast<quint64>(page.bytes.size());
    }
    QJsonArray samples;
    for (const double duration : durations) {
        samples.append(duration);
    }
    std::sort(durations.begin(), durations.end());
    const double p95 = durations.at(949);
    require(p95 < 100.0, QStringLiteral("Page read p95 exceeded 100 ms"));
    return {{QStringLiteral("query_count"), 1'000},
            {QStringLiteral("bytes_read"), static_cast<qint64>(bytesRead)},
            {QStringLiteral("page_p95_ms"), p95},
            {QStringLiteral("page_max_ms"), durations.back()},
            {QStringLiteral("page_samples_ms"), samples}};
}

QJsonObject saveTraversal() {
    MainWindow window;
    auto tree = AnalysisTree::create(QStringLiteral("benchmark"));
    require(tree.has_value(), QStringLiteral("Cannot create benchmark tree"));
    constexpr int groups = 128;
    constexpr int depth = 16;
    constexpr int leaves = 64;
    std::vector<AnalysisNodeId> branches;
    for (int group = 0; group < groups; ++group) {
        AnalysisNodeId parent = tree->rootId();
        for (int level = 0; level < depth; ++level) {
            AnalysisNodeSpec spec;
            spec.name = QStringLiteral("branch_%1_%2").arg(group).arg(level);
            spec.state = streamview::core::MaterializationState::Indexing;
            const auto node = tree->appendChild(parent, std::move(spec));
            require(node.has_value(), QStringLiteral("Cannot append benchmark branch"));
            parent = node.value();
            branches.push_back(parent);
        }
        for (int leaf = 0; leaf < leaves; ++leaf) {
            AnalysisNodeSpec spec;
            spec.name = QStringLiteral("leaf_%1").arg(leaf);
            require(tree->appendChild(parent, std::move(spec)).has_value(),
                    QStringLiteral("Cannot append benchmark leaf"));
        }
    }
    branches.push_back(tree->rootId());
    for (const auto id : branches) {
        require(tree->transition(id, streamview::core::MaterializationState::Materialized),
                QStringLiteral("Cannot finish benchmark tree"));
    }
    auto* view = treeView(window);
    auto* model = qobject_cast<AnalysisTreeModel*>(view->model());
    require(model != nullptr, QStringLiteral("Unexpected tree model"));
    model->resetFromTree(tree.value());
    view->expandToDepth(depth - 1);
    QJsonArray durations;
    double maximum = 0;
    qsizetype expanded = 0;
    for (int sample = 0; sample < 10; ++sample) {
        QElapsedTimer timer;
        timer.start();
        const auto saved = window.currentUserState();
        const double elapsed = milliseconds(timer);
        durations.append(elapsed);
        maximum = std::max(maximum, elapsed);
        expanded = saved.expandedPaths.size();
        require(expanded >= groups, QStringLiteral("Expanded paths were not serialized"));
    }
    require(maximum <= 50.0, QStringLiteral("Save traversal exceeded 50 ms"));
    return {{QStringLiteral("node_count"), 1 + groups * (depth + leaves)},
            {QStringLiteral("tree_depth"), depth + 1},
            {QStringLiteral("expanded_count"), expanded},
            {QStringLiteral("save_max_ms"), maximum},
            {QStringLiteral("save_samples_ms"), durations}};
}

QJsonObject restoration(const QString& path) {
    QTemporaryDir directory;
    require(directory.isValid(), QStringLiteral("Cannot create session directory"));
    const QString sessionPath = directory.filePath(QStringLiteral("benchmark.svsession"));
    QString expectedPath;
    int expectedRows = 0;
    {
        MainWindow original;
        openAndPaint(original, path);
        auto* view = treeView(original);
        const int firstRows = view->model()->rowCount();
        await([&original] {
            return original.statusBar()->currentMessage().contains(
                QStringLiteral("Analysis complete"));
        });
        expectedRows = view->model()->rowCount();
        require(expectedRows > firstRows,
                QStringLiteral("Restoration fixture did not require multiple batches"));
        const auto last = view->model()->index(expectedRows - 1, 0);
        require(last.isValid(), QStringLiteral("Late selection is missing"));
        view->setExpanded(last, true);
        view->setCurrentIndex(last);
        const auto selectedPath = original.currentUserState().view.selectedAnalysisPath;
        require(selectedPath.has_value() && !selectedPath->isEmpty(),
                QStringLiteral("Selection path is empty"));
        expectedPath = selectedPath.value();
        original.setSaveFileDialogHandlerForTesting(
            [&sessionPath](QWidget*, const QString&, const QString&) { return sessionPath; });
        require(original.saveSession(), QStringLiteral("Cannot save benchmark session"));
    }
    MainWindow restored;
    restored.show();
    QElapsedTimer elapsed;
    QElapsedTimer heartbeat;
    elapsed.start();
    heartbeat.start();
    double maximumGap = 0;
    int ticks = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        maximumGap = std::max(maximumGap, milliseconds(heartbeat));
        heartbeat.restart();
        ++ticks;
    });
    timer.start(1);
    QString error;
    require(restored.openSessionFile(sessionPath, &error), error);
    auto* view = treeView(restored);
    await([&] {
        return !restored.pendingSelectedAnalysisPathForTesting().has_value() &&
               restored.pendingExpandedPathsForTesting().isEmpty() &&
               view->model()->rowCount() == expectedRows;
    });
    maximumGap = std::max(maximumGap, milliseconds(heartbeat));
    require(restored.currentUserState().view.selectedAnalysisPath == expectedPath &&
                view->isExpanded(view->currentIndex()),
            QStringLiteral("Restoration selected or expanded the wrong node"));
    return {{QStringLiteral("restore_ms"), milliseconds(elapsed)},
            {QStringLiteral("heartbeat_max_gap_ms"), maximumGap},
            {QStringLiteral("heartbeat_ticks"), ticks},
            {QStringLiteral("restored_rows"), expectedRows}};
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        QElapsedTimer startup;
        startup.start();
        QApplication application(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("StreamViewBenchmark"));
        QCoreApplication::setOrganizationName(QStringLiteral("StreamViewBenchmark"));
        const auto arguments = application.arguments();
        require(arguments.size() >= 2, QStringLiteral("A benchmark mode is required"));
        const QString mode = arguments.at(1);
        QJsonObject result{{QStringLiteral("mode"), mode},
                           {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
                           {QStringLiteral("platform"), QSysInfo::prettyProductName()},
                           {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
                           {QStringLiteral("build_type"), QStringLiteral(STREAMVIEW_BENCHMARK_BUILD_TYPE)}};
        if (mode == QStringLiteral("save")) {
            const auto measured = saveTraversal();
            for (auto it = measured.begin(); it != measured.end(); ++it) {
                result.insert(it.key(), it.value());
            }
        } else {
            require(arguments.size() == 3, QStringLiteral("A source path is required"));
            const QString path = arguments.at(2);
            if (mode == QStringLiteral("restore")) {
                const auto measured = restoration(path);
                for (auto it = measured.begin(); it != measured.end(); ++it) {
                    result.insert(it.key(), it.value());
                }
            } else {
                require(mode == QStringLiteral("initial") || mode == QStringLiteral("sparse"),
                        QStringLiteral("Unknown benchmark mode"));
                MainWindow window;
                openAndPaint(window, path);
                const double initialMs = milliseconds(startup);
                result.insert(QStringLiteral("initial_ms"), initialMs);
                if (mode == QStringLiteral("initial")) {
                    require(initialMs <= 2'000.0, QStringLiteral("Initial view exceeded 2 s"));
                } else {
                    const quint64 openingRss = peakRssBytes();
                    result.insert(QStringLiteral("opening_peak_rss_bytes"),
                                  static_cast<qint64>(openingRss));
                    require(openingRss <= 512ULL * 1024ULL * 1024ULL,
                            QStringLiteral("Sparse source exceeded 512 MiB RSS"));
                    const auto measured = pageReads(path);
                    for (auto it = measured.begin(); it != measured.end(); ++it) {
                        result.insert(it.key(), it.value());
                    }
                }
            }
        }
        result.insert(QStringLiteral("peak_rss_bytes"), static_cast<qint64>(peakRssBytes()));
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
}
