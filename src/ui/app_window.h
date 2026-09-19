#pragma once

/// \file
/// \brief The window and the frame loop.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/cli.h"
#include "core/log.h"
#include "core/provider.h"
#include "core/session.h"
#include "ui/file_picker.h"
#include "ui/node_view.h"
#include "ui/selection.h"
#include "ui/text_view.h"
#include "ui/welcome.h"

struct GLFWwindow;

namespace nmtreediff {

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
    /// \brief Draws one node and everything below it.
    ///
    /// \param tree The tree being shown, which is the newer side.
    /// \param otherTree The other side, consulted for what a value was
    ///        before, or null when there is none.
    /// \param provider The format provider, for titles and property order.
    /// \param root The node to draw, with everything under it.
    /// \param diff What changed, or null before the diff is ready.
    /// \param side Which side \p tree is.
    void drawTreeOutline(const Tree& tree, const Tree* otherTree, const IFormatProvider& provider,
                         NodeId root, const DiffModel* diff, Side side, bool withChildren = true);

    /// \brief Draws one node of the outline and opens it if the reader has it
    ///        open.
    ///
    /// \param tree The tree the node belongs to.
    /// \param otherTree The other side, or null when there is none.
    /// \param provider The format provider, for titles and property order.
    /// \param id The node to draw.
    /// \param diff What changed, or null before the diff is ready.
    /// \param side Which side \p tree is.
    /// \param withChildren Whether the node's children will be drawn under it.
    /// \param pushed Set to whether the node pushed a tree level that the
    ///        caller has to pop once the children are drawn.
    ///
    /// \returns `true` when the node is open, in which case its id is still
    ///          pushed and the caller owns the pop; `false` when it is closed
    ///          or does not exist, with nothing left to undo.
    ///
    /// \remarks Split from drawTreeOutline() so that the walk over the
    ///          document can keep its own stack rather than the call stack.
    bool drawOutlineNode(const Tree& tree, const Tree* otherTree, const IFormatProvider& provider,
                         NodeId id, const DiffModel* diff, Side side, bool withChildren,
                         bool& pushed);

    /// \brief Draws the outline rooted wherever the reader asked for.
    ///
    /// \param snapshot The comparison being shown.
    void drawOutline(const DiffSnapshot& snapshot);

    /// \brief Draws one node's properties, marking what changed.
    ///
    /// \param tree The tree being shown.
    /// \param otherTree The other side, or null.
    /// \param provider The format provider, for property order.
    /// \param id The node whose properties to draw.
    /// \param diff What changed, or null.
    /// \param side Which side \p tree is.
    void drawProperties(const Tree& tree, const Tree* otherTree, const IFormatProvider& provider,
                        NodeId id, const DiffModel* diff, Side side);

    /// \brief Draws one property row.
    ///
    /// \param property The property to draw.
    /// \param changed Whether the diff says this property differs.
    /// \param before The same property on the other side, or null when it
    ///        is new.
    void drawProperty(const Property& property, bool changed, const Property* before);

    /// \brief Draws a property that has parts, as a small tree.
    ///
    /// \param property The property to draw.
    /// \param changed Whether the diff says this property differs.
    /// \param before The same property on the other side, or null.
    void drawPropertyParts(const Property& property, bool changed, const Property* before);

    /// \brief Draws a property the other side had and this one does not.
    ///
    /// \param property The property as it was before it was removed.
    void drawRemovedProperty(const Property& property);
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

    /// \brief Whether the details outline shows the whole document.
    ///
    /// \remarks Off by default, which shows the selected node alone. A large
    ///          document makes an outline long enough that finding the node you
    ///          just clicked is work, and the panel exists to answer a question
    ///          about that one node. With nothing selected there is no node to
    ///          narrow to, so the whole tree is shown whatever this says.
    bool detailsWholeTree_ = false;

    /// \brief Draws the Output pane, when it is shown.
    ///
    /// \remarks What the program wrote to standard output and standard error,
    ///          which for a window launched from the desktop exist nowhere
    ///          else. Error lines are coloured; a Clear button empties the log
    ///          and a Copy button puts the whole of it on the clipboard.
    void drawOutputPane();

    /// \brief Picks up what the log gained since last frame.
    ///
    /// \remarks Appends the new lines to the pane's own copy, so drawing never
    ///          copies the whole buffer, and opens the pane on the first error
    ///          line it has not shown yet. The pane is off in a fresh layout so
    ///          a reader going through a changelist is not shown a log, and
    ///          this is what makes it appear when there is something to read.
    void pollLog();

    /// \brief Whether the Output pane is shown.
    bool showOutput_ = false;
    /// \brief How many more frames the pane asks for focus.
    ///
    /// \remarks More than one, because a window docks the frame after it
    ///          first appears and a focus given before that selects no tab.
    int focusOutputFrames_ = 0;
    /// \brief Whether the pane keeps scrolling to the newest line.
    bool outputFollows_ = true;
    /// \brief The pane's copy of the log, oldest first.
    std::vector<LogLine> outputLines_;
    /// \brief The sequence of the last line copied into outputLines_.
    std::uint64_t outputSequence_ = 0;
    /// \brief The sequence of the last error line the pane opened for.
    std::uint64_t outputOpenedFor_ = 0;

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

    /// \brief Takes the settings a configuration script may set, with the
    ///        command line's overrides on top.
    ///
    /// \remarks Called once at startup and again on every Reload, so a changed
    ///          script's graph direction and exit key take effect without a
    ///          restart. A setting the configuration leaves alone is left alone
    ///          here too, so a direction chosen from the menu survives a Reload
    ///          unless a script now says otherwise.
    void applyConfiguredSettings();

    /// \brief Reads every configuration file again, rebuilds the providers,
    ///        and compares the two files afresh.
    ///
    /// \remarks What Ctrl+R and File, Reload do. The configuration is read by
    ///          the same function startup used, over the same files in the same
    ///          order. If it fails, the problems go to the log and the previous
    ///          providers stay, which is the startup rule that a bad
    ///          configuration is not partly applied; the files are still read
    ///          again, because that part cannot be wrong.
    void reload();

    /// \brief The key that closes the window, or ImGuiKey_None.
    ///
    /// \remarks Stored rather than read from the options each frame, because
    ///          it is settled from the configuration and the command line at
    ///          startup and again only on Reload.
    int exitKey_ = 0;

    /// \brief The reader's standing choice of graph direction.
    ///
    /// \remarks Only a default. A format that names a direction of its own
    ///          overrides it, so the menu shows which way the reader asked for
    ///          rather than which way the graph on screen actually runs.
    GraphDirection graphDirection_ = GraphDirection::TopDown;
};

}  // namespace nmtreediff
