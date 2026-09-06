#pragma once

/// \file
/// \brief The window and the frame loop.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/cli.h"
#include "core/provider.h"
#include "core/session.h"
#include "ui/file_picker.h"
#include "ui/node_view.h"
#include "ui/selection.h"
#include "ui/text_view.h"
#include "ui/welcome.h"

struct GLFWwindow;

namespace nmxd {

/// \brief Owns the window, the docking layout, and the frame loop.
///
/// \remarks The only place that knows about GLFW or ImGui. It never does work
///          itself: it reads the current snapshot and asks the Session for more,
///          which is what keeps the frame loop from blocking.
class AppWindow {
public:
    /// \brief Prepares the window without creating it.
    ///
    /// \param options The run's options.
    /// \param processStart When the process started, captured in main before
    ///        anything else happens so the startup budget measures what the user
    ///        actually waits for.
    AppWindow(const Options& options, std::chrono::steady_clock::time_point processStart);

    /// \brief Destroys the window and shuts down the graphics stack.
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    /// \brief Creates the window and initialises the graphics stack.
    ///
    /// \returns `true` on success, `false` when no window could be created.
    [[nodiscard]] bool open();

    /// \brief Runs until the window is closed.
    ///
    /// \returns The process exit code.
    ///
    /// \remarks Queues the file read before the first frame, so the window is up
    ///          and drawing while the read happens on a worker. When
    ///          Options::maxFrames is set, stops after that many frames and
    ///          prints the timings instead.
    int run();

private:
    void buildFrame();
    void drawMenuBar();
    void drawTextView(const DiffSnapshot& snapshot);
    void drawNodeView(const DiffSnapshot& snapshot);
    void drawDetails(const DiffSnapshot& snapshot);
    void drawStatusBar(const DiffSnapshot& snapshot);
    void drawTreeOutline(const Tree& tree, const IFormatProvider& provider, NodeId id,
                         const DiffModel* diff, Side side);
    void layoutDockSpaceOnce();

    /// \brief Draws the pane shown until both files are chosen.
    void drawWelcomePane();

    /// \brief Collects an answer from the file dialog, if one arrived.
    ///
    /// \remarks Called once a frame. Choosing the second of the two files is
    ///          what starts a comparison, so this is where a session begins when
    ///          the tool was launched with no arguments.
    void collectPickedFile();

    /// \brief Opens the dialog for one side.
    ///
    /// \param target Which side to choose a file for.
    void askForFile(PickerTarget target);

    /// \brief Reads the framebuffer back and saves it.
    ///
    /// \param width Framebuffer width in pixels.
    /// \param height Framebuffer height in pixels.
    void captureFrame(int width, int height);

    Options options_;
    Session session_;
    FilePicker picker_;

    /// \brief Why the last file dialog failed, or empty.
    std::string pickerError_;
    TextView textView_;
    NodeView nodeView_;

    /// \brief What both views agree is selected.
    Selection selection_;

    GLFWwindow* window_ = nullptr;
    bool running_ = false;
    bool dockLayoutDone_ = false;
    bool requestClose_ = false;
    bool initialViewFocused_ = false;

    std::chrono::steady_clock::time_point processStart_;
    double startupMillis_ = 0.0;  // process start to first frame presented
    double worstFrameMillis_ = 0.0;
    unsigned worstFrameIndex_ = 0;
    double worstSteadyFrameMillis_ = 0.0;
    unsigned framesPresented_ = 0;

    InitialView view_ = InitialView::Text;

    /// \brief Draws the graph direction submenu.
    void drawDirectionMenu();

    /// \brief Changes the standing direction and lays the graph out again.
    ///
    /// \param direction The direction to adopt.
    void setGraphDirection(GraphDirection direction);

    /// \brief The reader's standing choice of graph direction.
    ///
    /// \remarks Only a default. A format that names a direction of its own
    ///          overrides it, so the menu shows which way the reader asked for
    ///          rather than which way the graph on screen actually runs.
    GraphDirection graphDirection_ = GraphDirection::TopDown;
};

}  // namespace nmxd
