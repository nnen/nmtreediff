#pragma once

// The window and the frame loop. This is the only place that knows about GLFW
// or ImGui, and it never does work itself: it reads the current snapshot and
// asks the Session for more.

#include <chrono>
#include <memory>
#include <string>

#include "app/cli.h"
#include "core/session.h"

struct GLFWwindow;

namespace nmxd {

class AppWindow {
public:
    // processStart is captured in main before anything else happens, so the
    // startup budget measures what the user actually waits for.
    AppWindow(const Options& options, std::chrono::steady_clock::time_point processStart);
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    // Returns false when the window could not be created.
    [[nodiscard]] bool open();

    // Runs until the window is closed. Returns the process exit code.
    int run();

private:
    void buildFrame();
    void drawMenuBar();
    void drawTextView(const DiffSnapshot& snapshot);
    void drawNodeView(const DiffSnapshot& snapshot);
    void drawDetails(const DiffSnapshot& snapshot);
    void drawStatusBar(const DiffSnapshot& snapshot);
    void layoutDockSpaceOnce();

    Options options_;
    Session session_;

    GLFWwindow* window_ = nullptr;
    bool running_ = false;
    bool dockLayoutDone_ = false;
    bool requestClose_ = false;

    std::chrono::steady_clock::time_point processStart_;
    double startupMillis_ = 0.0;   // process start to first frame presented
    double worstFrameMillis_ = 0.0;
    unsigned framesPresented_ = 0;

    InitialView view_ = InitialView::Text;
};

}  // namespace nmxd
