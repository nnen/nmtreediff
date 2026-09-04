#include "core/session.h"

#include "core/textdiff.h"

#include <chrono>
#include <utility>

namespace nmxd {

const char* describe(Stage stage) noexcept {
    switch (stage) {
        case Stage::Idle:
            return "idle";
        case Stage::Loading:
            return "loading";
        case Stage::SourcesReady:
            return "sources loaded";
        case Stage::TextReady:
            return "text diff ready";
        case Stage::TreeReady:
            return "tree diff ready";
        case Stage::Failed:
            return "failed";
    }
    return "unknown";
}

Session::Session(unsigned threadCount) : jobs_(threadCount) {
    box_.publish(std::make_shared<const DiffSnapshot>());
}

void Session::open(SessionRequest request) {
    jobs_.cancelAll();

    DiffSnapshot loading;
    loading.stage = Stage::Loading;
    box_.publish(std::make_shared<const DiffSnapshot>(std::move(loading)));

    const Generation generation = jobs_.submit(
        [this, request = std::move(request)](std::stop_token token) mutable {
            // The generation the job was queued in is the one the token belongs
            // to, so checking the token is enough to know it is still wanted.
            runOpen(request, std::move(token), 0);
        });
    (void)generation;
}

void Session::cancel() {
    jobs_.cancelAll();
}

void Session::publish(DiffSnapshot snapshot, Generation) {
    box_.publish(std::make_shared<const DiffSnapshot>(std::move(snapshot)));
}

void Session::runOpen(const SessionRequest& request, std::stop_token token, Generation generation) {
    const auto started = std::chrono::steady_clock::now();

    const auto elapsedMillis = [started] {
        const auto delta = std::chrono::steady_clock::now() - started;
        return std::chrono::duration<double, std::milli>(delta).count();
    };

    DiffSnapshot result;

    auto left = SourceFile::load(request.leftPath, request.leftLabel);
    if (token.stop_requested()) {
        return;
    }
    if (!left) {
        result.stage = Stage::Failed;
        result.message = request.leftPath.string() + ": " + describe(left.error());
        result.elapsedMillis = elapsedMillis();
        publish(std::move(result), generation);
        return;
    }

    auto right = SourceFile::load(request.rightPath, request.rightLabel);
    if (token.stop_requested()) {
        return;
    }
    if (!right) {
        result.stage = Stage::Failed;
        result.message = request.rightPath.string() + ": " + describe(right.error());
        result.elapsedMillis = elapsedMillis();
        publish(std::move(result), generation);
        return;
    }

    result.stage = Stage::SourcesReady;
    result.left = std::make_shared<const SourceFile>(std::move(left).value());
    result.right = std::make_shared<const SourceFile>(std::move(right).value());
    result.elapsedMillis = elapsedMillis();

    // Published before the diff runs. On a large pair the raw text is on
    // screen while the alignment is still going, rather than the window
    // sitting empty until everything is finished.
    publish(result, generation);

    auto text = std::make_shared<TextDiff>(diffText(*result.left, *result.right, token));
    if (token.stop_requested() || text->cancelled) {
        return;
    }

    result.stage = Stage::TextReady;
    result.text = std::move(text);
    result.elapsedMillis = elapsedMillis();
    publish(std::move(result), generation);
}

}  // namespace nmxd
