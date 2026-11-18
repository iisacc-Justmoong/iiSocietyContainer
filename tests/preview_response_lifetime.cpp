#include "CTestSupport.h"
#include "PreviewResponseGuard.h"
#include "SignalRecorder.h"
#include <QCoreApplication>
#include <QTimer>
#include <memory>

struct ResponseState {
    bool completed = false;
    bool destroyed = false;
    bool destroyedEarly = false;
    int cancellations = 0;
};
class FakeResponse final : public QObject {
    Q_OBJECT
public:
    explicit FakeResponse(ResponseState &state, bool finishesOnCancel = true)
        : state_(state), finishesOnCancel_(finishesOnCancel) {}
    ~FakeResponse() override { state_.destroyed = true; state_.destroyedEarly = !state_.completed; }
    void cancel() {
        ++state_.cancellations;
        if (finishesOnCancel_) QTimer::singleShot(0, this, [this] { complete(); });
    }
    void complete() { state_.completed = true; emit finished(); }
signals:
    void finished();
private:
    ResponseState &state_;
    bool finishesOnCancel_;
};
using Recorder = society_ctest::SignalRecorder<decltype(&FakeResponse::finished)>;

static void completedResponseIsSafeToDestroy() {
    ResponseState state;
    auto response = std::make_unique<FakeResponse>(state);
    Recorder finished(response.get(), &FakeResponse::finished);
    response->complete();
    CTEST_CHECK(society_ctest::retirePreviewResponse(response, &finished, 0));
    CTEST_COMPARE(finished.size(), 1);
    CTEST_COMPARE(state.cancellations, 0);
    response.reset();
    CTEST_CHECK(state.destroyed && !state.destroyedEarly);
}
static void cancellationWaitsForCompletion() {
    ResponseState state;
    auto response = std::make_unique<FakeResponse>(state);
    Recorder finished(response.get(), &FakeResponse::finished);
    CTEST_CHECK(society_ctest::retirePreviewResponse(response, &finished, 1000));
    CTEST_COMPARE(state.cancellations, 1);
    CTEST_COMPARE(finished.size(), 1);
    CTEST_CHECK(state.completed && !state.destroyed);
    response.reset();
    CTEST_CHECK(state.destroyed && !state.destroyedEarly);
}
static void missingCompletionNeverDestroysTheBorrowedResponse() {
    ResponseState state;
    auto response = std::make_unique<FakeResponse>(state, false);
    auto *raw = response.get();
    Recorder finished(raw, &FakeResponse::finished);
    CTEST_CHECK(!society_ctest::retirePreviewResponse(response, &finished, 0));
    CTEST_CHECK(!response && !state.destroyed);
    CTEST_COMPARE(state.cancellations, 1);
    state.completed = true; // The fake has no background worker; reclaim its allocation.
    delete raw;
    CTEST_CHECK(state.destroyed && !state.destroyedEarly);
}
static void missingRecorderNeverDestroysTheBorrowedResponse() {
    ResponseState state;
    auto response = std::make_unique<FakeResponse>(state, false);
    auto *raw = response.get();
    CTEST_CHECK(!society_ctest::retirePreviewResponse(response, static_cast<const Recorder *>(nullptr), 0));
    CTEST_CHECK(!response && !state.destroyed);
    state.completed = true;
    delete raw;
    CTEST_CHECK(state.destroyed && !state.destroyedEarly);
}
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    society_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    runner.run("completedResponseIsSafeToDestroy", completedResponseIsSafeToDestroy);
    runner.run("cancellationWaitsForCompletion", cancellationWaitsForCompletion);
    runner.run("missingCompletionNeverDestroysTheBorrowedResponse", missingCompletionNeverDestroysTheBorrowedResponse);
    runner.run("missingRecorderNeverDestroysTheBorrowedResponse", missingRecorderNeverDestroysTheBorrowedResponse);
    return runner.result();
}
#include "preview_response_lifetime.moc"
