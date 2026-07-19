#include <streamview/core/cancellation.h>

#include <QTest>

#include <atomic>
#include <thread>

using streamview::core::CancellationSource;

class CancellationTest final : public QObject {
    Q_OBJECT

private slots:
    void tokensObserveAnIdempotentRequest() {
        CancellationSource source;
        const auto firstToken = source.token();
        const auto secondToken = source.token();
        QVERIFY(firstToken.canBeCancelled());
        QVERIFY(!firstToken.isCancellationRequested());
        QVERIFY(!secondToken.isCancellationRequested());

        QVERIFY(source.requestCancellation());
        QVERIFY(!source.requestCancellation());
        QVERIFY(source.isCancellationRequested());
        QVERIFY(firstToken.isCancellationRequested());
        QVERIFY(secondToken.isCancellationRequested());
    }

    void requestIsObservedAcrossThreads() {
        CancellationSource source;
        const auto token = source.token();
        std::atomic_bool observed{false};

        std::thread worker([token, &observed]() {
            while (!token.isCancellationRequested()) {
                std::this_thread::yield();
            }
            observed.store(true, std::memory_order_release);
        });

        const bool requested = source.requestCancellation();
        worker.join();
        QVERIFY(requested);
        QVERIFY(observed.load(std::memory_order_acquire));
    }

    void tokenKeepsRequestedStateAlive() {
        const auto token = []() {
            CancellationSource source;
            (void)source.requestCancellation();
            return source.token();
        }();

        QVERIFY(token.canBeCancelled());
        QVERIFY(token.isCancellationRequested());
    }
};

QTEST_GUILESS_MAIN(CancellationTest)

#include "cancellation_test.moc"
