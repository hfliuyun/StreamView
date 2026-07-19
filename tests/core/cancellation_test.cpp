#include <streamview/core/cancellation.h>

#include <QTest>

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
};

QTEST_GUILESS_MAIN(CancellationTest)

#include "cancellation_test.moc"
