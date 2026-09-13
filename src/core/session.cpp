/// \file
/// \brief Implementation of the staged comparison pipeline.

#include "core/session.h"

#include "core/layout_tree.h"
#include "core/provider.h"
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
        case Stage::TreesParsed:
            return "trees parsed";
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

    // The registry is captured now rather than read on the worker, so that a
    // Reload swapping it while this comparison runs changes nothing this
    // comparison sees.
    const Generation generation = jobs_.submit(
        [this, request = std::move(request), registry = registry_](std::stop_token token) mutable {
            // The generation the job was queued in is the one the token belongs
            // to, so checking the token is enough to know it is still wanted.
            runOpen(request, std::move(registry), std::move(token), 0);
        });
    (void)generation;
}

void Session::cancel() {
    jobs_.cancelAll();
}

std::vector<std::string> Session::configureProviders(const ProviderConfig& config) {
    auto fresh = std::make_shared<ProviderRegistry>(makeDefaultRegistry());

    // Scripted formats are registered before the mappings are applied, so an
    // extension may be pointed at a format the same file defined.
    std::vector<std::string> unknown = addScriptedProviders(*fresh, config);
    const std::vector<std::string> rest = fresh->apply(config);
    unknown.insert(unknown.end(), rest.begin(), rest.end());

    // The old registry is not destroyed here: a running comparison and every
    // published snapshot hold their own reference to it.
    registry_ = std::move(fresh);
    return unknown;
}

void Session::publish(DiffSnapshot snapshot, Generation) {
    box_.publish(std::make_shared<const DiffSnapshot>(std::move(snapshot)));
}

void Session::runOpen(const SessionRequest& request,
                      std::shared_ptr<const ProviderRegistry> registry, std::stop_token token,
                      Generation generation) {
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
    publish(result, generation);

    // Parsing is the last stage M2 provides. Matching, and the node view that
    // reads it, arrive at M3 and M4.
    bool unknownFormat = false;
    const IFormatProvider* provider =
        registry->resolve(*result.left, request.format, &unknownFormat);
    if (unknownFormat || provider == nullptr) {
        result.stage = Stage::Failed;
        result.message = "unknown format '" + request.format + "'; known formats are: ";
        bool first = true;
        for (const auto known : registry->names()) {
            if (!first) {
                result.message += ", ";
            }
            first = false;
            result.message.append(known);
        }
        publish(std::move(result), generation);
        return;
    }

    auto leftTree = provider->parse(*result.left, token);
    if (token.stop_requested()) {
        return;
    }
    if (!leftTree) {
        result.stage = Stage::Failed;
        result.message = result.left->label() + ": " + describe(leftTree.error());
        result.elapsedMillis = elapsedMillis();
        publish(std::move(result), generation);
        return;
    }

    auto rightTree = provider->parse(*result.right, token);
    if (token.stop_requested()) {
        return;
    }
    if (!rightTree) {
        result.stage = Stage::Failed;
        result.message = result.right->label() + ": " + describe(rightTree.error());
        result.elapsedMillis = elapsedMillis();
        publish(std::move(result), generation);
        return;
    }

    result.stage = Stage::TreesParsed;
    result.provider = provider;
    result.registry = registry;
    result.leftTree = std::make_shared<const Tree>(std::move(leftTree).value());
    result.rightTree = std::make_shared<const Tree>(std::move(rightTree).value());
    result.elapsedMillis = elapsedMillis();
    publish(result, generation);

    auto treeDiff = std::make_shared<DiffModel>(
        diffTrees(*result.leftTree, *result.rightTree, *provider, token));
    if (token.stop_requested() || treeDiff->cancelled) {
        return;
    }

    auto layout = std::make_shared<TreeLayout>(
        buildLayout(*result.leftTree, *result.rightTree, *treeDiff, *provider, token,
                    request.layoutMetrics,
                    resolveDirection(*provider, request.graphDirection)));
    if (token.stop_requested() || layout->cancelled) {
        return;
    }

    result.stage = Stage::TreeReady;
    result.treeDiff = std::move(treeDiff);
    result.layout = std::move(layout);
    result.elapsedMillis = elapsedMillis();
    publish(std::move(result), generation);
}

}  // namespace nmxd
