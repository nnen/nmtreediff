/// \file
/// \brief Implementation of the window, the docking layout and the frame loop.

#include "ui/app_window.h"

#include "ui/screenshot.h"
#include "ui/welcome.h"

#include "core/log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

namespace nmxd {

namespace {

/// \brief Window title of the text view panel.
///
/// \remarks ImGui identifies a window by its title, so these double as the keys
///          the docking layout and the saved layout file use.
constexpr const char* kTextViewTitle = "Text view";

/// \brief Window title of the node view panel.
constexpr const char* kNodeViewTitle = "Node view";

/// \brief Window title of the pane shown before a comparison is open.
///
/// \remarks A window of its own rather than the text view wearing a
///          different face, because the tab strip names what a tab holds and a
///          welcome pane labelled "Text view" tells the reader something untrue.
constexpr const char* kWelcomeTitle = "Welcome";

/// \brief Window title of the details panel.
constexpr const char* kDetailsTitle = "Details";

/// \brief Window title of the status panel.
constexpr const char* kStatusTitle = "Status";

/// \brief Colour of an added node, or of a property that is new.
constexpr ImU32 kAddedColour = IM_COL32(96, 200, 140, 255);
/// \brief Colour of a deleted node, or of a property that is gone.
constexpr ImU32 kDeletedColour = IM_COL32(226, 110, 105, 255);
/// \brief Colour of a modified node, or of a property whose value changed.
constexpr ImU32 kModifiedColour = IM_COL32(224, 176, 82, 255);
/// \brief Colour of a moved node.
constexpr ImU32 kMovedColour = IM_COL32(168, 143, 224, 255);

/// \brief What stands between an old value and the new one.
constexpr const char* kValueArrow = "->";

/// \brief Labels a property that has parts, for the header of its subtree.
///
/// \param property The property to label.
/// \param before What the property was on the other side, or null.
///
/// \returns The name, a count in brackets for a sequence or braces for a
///          record, and the value where the property carries one. When the
///          value differs from \p before, both values with the arrow between
///          them, the way a scalar's line reads.
[[nodiscard]] std::string partsLabel(const Property& property, const Property* before) {
    std::string label = property.name;
    label += property.ordered() ? " [" + std::to_string(property.children.size()) + "]" : " {}";
    if (before != nullptr && before->value != property.value) {
        label += " " + before->value + " " + kValueArrow + " " + property.value;
    } else if (!property.value.empty()) {
        label += " " + property.value;
    }
    return label;
}

/// \brief Reports whether a name appears in a list of changed properties.
///
/// \param names The names the diff recorded as differing.
/// \param name The property to look for.
///
/// \returns `true` when the diff says this property differs.
///
/// \remarks A linear scan because the list holds the properties of one node
///          that actually changed, which is a handful. Building a set per node
///          per frame would cost more than the scan saves.
[[nodiscard]] bool namedIn(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

/// \brief The first frame counted towards the steady-state frame budget.
///
/// \remarks Frames before this are still settling: the window is being shown and
///          the compositor has not finished with it. That cost is fixed, lands a
///          few frames in, and has nothing to do with what the program is
///          computing, so folding it into the budget would hide every real stall
///          smaller than it.
constexpr unsigned kSettledFrame = 60;

/// \brief How much larger than its natural size the interface text is drawn.
///
/// \remarks Dear ImGui's built-in font is 13 pixels tall, which is small on
///          the high-resolution displays this tool is read on all day. The node
///          view's layout is measured in the same unit, so the same factor is
///          applied to its metrics and the cards grow with the text they hold.
constexpr float kFontScale = 1.5f;

/// \brief Turns a key's name into the key itself.
///
/// \param name The name a configuration or the command line gave.
///
/// \returns The key, or ImGuiKey_None when the name is `none` or is not one
///          this build knows.
///
/// \remarks A small vocabulary on purpose: the single letters, the function
///          keys, and Escape. Anything else is refused rather than guessed at,
///          because a key that silently does nothing is worse than being told
///          the name was wrong.
[[nodiscard]] ImGuiKey keyFromName(const std::string& name) {
    if (name.empty() || name == "none") {
        return ImGuiKey_None;
    }
    if (name == "escape" || name == "esc") {
        return ImGuiKey_Escape;
    }
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'a'));
    }
    if (name.size() >= 2 && name[0] == 'f') {
        const int number = std::atoi(name.c_str() + 1);
        if (number >= 1 && number <= 12) {
            return static_cast<ImGuiKey>(ImGuiKey_F1 + number - 1);
        }
    }
    return ImGuiKey_None;
}

/// \brief Builds the node view's layout sizes for the interface's text size.
///
/// \param scale How much larger than its natural size the text is drawn.
///
/// \returns The default sizes, every one of them scaled.
///
/// \remarks Layout units are character cells, so every size scales together
///          and the elision width stays the same number of characters.
LayoutMetrics scaledMetrics(float scale) {
    LayoutMetrics metrics;
    metrics.characterWidth *= scale;
    metrics.lineHeight *= scale;
    metrics.padding *= scale;
    metrics.minimumWidth *= scale;
    metrics.maximumWidth *= scale;
    metrics.siblingGap *= scale;
    metrics.levelGap *= scale;
    return metrics;
}

/// \brief Builds the request that opens the files named on the command line.
///
/// \param options The parsed command line.
/// \param direction The reader's standing choice of graph direction.
///
/// \returns The request, laid out at the size the interface draws text in.
SessionRequest makeRequest(const Options& options, GraphDirection direction) {
    SessionRequest request{options.leftPath, options.rightPath, options.leftLabel,
                           options.rightLabel, options.format};
    request.layoutMetrics = scaledMetrics(kFontScale);
    request.graphDirection = direction;
    return request;
}

/// \brief Prints a GLFW error to the standard error stream.
///
/// \param code The GLFW error code.
/// \param description GLFW's description, which may be null.
void reportGlfwError(int code, const char* description) {
    logErr("nmxmldiff: glfw error " + std::to_string(code) + ": " +
           (description != nullptr ? description : ""));
}

}  // namespace

AppWindow::AppWindow(const Options& options, std::chrono::steady_clock::time_point processStart)
    : options_(options), processStart_(processStart), view_(options.view) {}

AppWindow::~AppWindow() {
    if (window_ != nullptr) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

bool AppWindow::open() {
    glfwSetErrorCallback(reportGlfwError);
    if (glfwInit() == GLFW_FALSE) {
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    const std::string title =
        options_.hasInputs()
            ? "NM Tree Diff  -  " + options_.leftLabel + "  vs  " + options_.rightLabel
            : std::string("NM Tree Diff");
    window_ = glfwCreateWindow(1440, 900, title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().FontScaleMain = kFontScale;
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    running_ = true;
    return true;
}

int AppWindow::run() {
    if (!running_) {
        return 2;
    }

    // The command line has already checked that every name in here is one the
    // registry knows, so nothing can go wrong at this point.
    (void)session_.configureProviders(options_.providerConfig);

    // Settled once. A configuration script asks, and the command line overrides
    // what it asked for, which is the same order everything else resolves in.
    if (options_.providerConfig.graphDirection != GraphDirection::Inherit) {
        graphDirection_ = options_.providerConfig.graphDirection;
    }
    const std::string& exitKey =
        options_.exitKey.empty() ? options_.providerConfig.exitKey : options_.exitKey;
    exitKey_ = exitKey.empty() ? ImGuiKey_Escape : keyFromName(exitKey);

    // Queued before the first frame so the read happens on a worker while the
    // window is already up and drawing.
    if (options_.hasInputs()) {
        session_.open(makeRequest(options_, graphDirection_));
    }

    // Vertical sync pins the frame rate to the display, which would make a
    // fixed-frame timing run measure the monitor rather than the program.
    if (options_.maxFrames > 0) {
        glfwSwapInterval(0);
    }

    while (!glfwWindowShouldClose(window_) && !requestClose_) {
        if (options_.maxFrames > 0 && framesPresented_ >= options_.maxFrames) {
            break;
        }
        glfwPollEvents();

        const auto frameStart = std::chrono::steady_clock::now();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        buildFrame();

        ImGui::Render();

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Read back before the swap, while the frame just drawn is still
        // the one attached to the context.
        if (!options_.screenshotPath.empty() && options_.maxFrames > 0 &&
            framesPresented_ + 1 >= options_.maxFrames) {
            captureFrame(width, height);
        }

        glfwSwapBuffers(window_);

        const auto frameEnd = std::chrono::steady_clock::now();
        const double frameMillis =
            std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();

        if (framesPresented_ == 0) {
            startupMillis_ =
                std::chrono::duration<double, std::milli>(frameEnd - processStart_).count();
        } else {
            if (frameMillis > worstFrameMillis_) {
                worstFrameMillis_ = frameMillis;
                worstFrameIndex_ = framesPresented_;
            }
            // Reported separately from the settling frames. Showing a window
            // costs a compositor round trip that lands a few frames in and has
            // nothing to do with what the program is computing; folding it into
            // the frame budget would hide every stall smaller than it.
            if (framesPresented_ >= kSettledFrame) {
                worstSteadyFrameMillis_ = std::max(worstSteadyFrameMillis_, frameMillis);
            }
        }
        ++framesPresented_;
    }

    session_.cancel();

    if (options_.maxFrames > 0) {
        char timings[256];
        std::snprintf(timings, sizeof(timings),
                      "startup_ms=%.1f frames=%u steady_worst_ms=%.2f settling_worst_ms=%.2f "
                      "at_frame=%u workers=%u",
                      startupMillis_, framesPresented_, worstSteadyFrameMillis_,
                      worstFrameMillis_, worstFrameIndex_, session_.threadCount());
        logOut(timings);
    }
    return 0;
}

void AppWindow::captureFrame(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    if (writeBitmap(options_.screenshotPath, width, height, pixels)) {
        std::printf("screenshot=%s\n", options_.screenshotPath.string().c_str());
    } else {
        logErr("nmxmldiff: could not write " + options_.screenshotPath.string());
    }
}

void AppWindow::buildFrame() {
    drawMenuBar();
    layoutDockSpaceOnce();

    const auto snapshot = session_.snapshot();
    static const DiffSnapshot kEmpty;
    const DiffSnapshot& current = snapshot ? *snapshot : kEmpty;

    // Escape closes the window, which is what a version control diff tool is
    // expected to do: a review is a run of files opened one after another, and
    // dismissing each with one key is what makes going through a large
    // changelist bearable. Confirming would ask the same question dozens of
    // times in a row. M8 makes the key configurable, for anyone whose habits
    // this fights with.
    //
    // A menu or a context menu takes it first, because closing what is open is
    // what Escape means everywhere else, and quitting instead would punish
    // someone for opening a menu by mistake.
    const bool popupOpen =
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (exitKey_ != ImGuiKey_None && !popupOpen &&
        ImGui::IsKeyPressed(static_cast<ImGuiKey>(exitKey_), false)) {
        requestClose_ = true;
    }

    // Change navigation is bound globally rather than to a focused widget, so
    // it works wherever the caret happens to be.
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) {
        const bool backwards = ImGui::GetIO().KeyShift;
        if (view_ == InitialView::Node) {
            // Node changes and line changes are different lists, so navigation
            // follows whichever view the reader is actually looking at.
            if (backwards) {
                nodeView_.goToPreviousChange(current, selection_);
            } else {
                nodeView_.goToNextChange(current, selection_);
            }
        } else if (backwards) {
            textView_.goToPreviousChange(current);
        } else {
            textView_.goToNextChange(current);
        }
    }

    collectPickedFile();

    if (options_.hasInputs()) {
        drawTextView(current);
        drawNodeView(current);
        drawDetails(current);
    } else {
        drawWelcomePane();
    }
    drawStatusBar(current);

    // Focusing a window requires it to exist, and the panels are only created
    // by the calls above. Doing this after the first frame has built them is
    // what makes --view actually pick the tab that opens.
    if (!initialViewFocused_ && framesPresented_ > 0 && options_.hasInputs()) {
        ImGui::SetWindowFocus(view_ == InitialView::Node ? kNodeViewTitle : kTextViewTitle);
        initialViewFocused_ = true;
    }
}

void AppWindow::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open left...", nullptr, false, !picker_.busy())) {
            askForFile(PickerTarget::Left);
        }
        if (ImGui::MenuItem("Open right...", nullptr, false, !picker_.busy())) {
            askForFile(PickerTarget::Right);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reload", "Ctrl+R", false, options_.hasInputs())) {
            session_.open(makeRequest(options_, graphDirection_));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Esc")) {
            requestClose_ = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Text", "Ctrl+1", view_ == InitialView::Text)) {
            view_ = InitialView::Text;
            ImGui::SetWindowFocus(kTextViewTitle);
        }
        if (ImGui::MenuItem("Node", "Ctrl+2", view_ == InitialView::Node)) {
            view_ = InitialView::Node;
            ImGui::SetWindowFocus(kNodeViewTitle);
        }

        ImGui::Separator();
        drawDirectionMenu();

        // What the format did not carry into the tree, and where a script
        // failed, are marked in the text view. Two toggles rather than one,
        // because the two are different news and a reader may want either.
        ImGui::Separator();
        ImGui::MenuItem("Mark dropped content", nullptr, &textView_.showDropped());
        ImGui::MenuItem("Mark failed jobs", nullptr, &textView_.showFailed());
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void AppWindow::askForFile(PickerTarget target) {
    // Opening where the other side already sits saves the reader navigating to
    // the same directory twice, which is where both versions usually live.
    const std::filesystem::path& other =
        target == PickerTarget::Left ? options_.rightPath : options_.leftPath;
    pickerError_.clear();
    picker_.open(target, other.empty() ? std::filesystem::path{} : other.parent_path());
}

void AppWindow::collectPickedFile() {
    switch (picker_.poll()) {
        case PickerOutcome::Chosen:
            break;
        case PickerOutcome::Failed:
            pickerError_ = picker_.error();
            return;
        case PickerOutcome::None:
        case PickerOutcome::Pending:
        case PickerOutcome::Cancelled:
            return;
    }

    // A label stands in for the path, so it has to follow the path that changed
    // rather than keeping whatever the command line said about the old one.
    const std::filesystem::path chosen = picker_.chosen();
    if (picker_.target() == PickerTarget::Left) {
        options_.leftPath = chosen;
        options_.leftLabel = chosen.string();
    } else {
        options_.rightPath = chosen;
        options_.rightLabel = chosen.string();
    }

    // Two files is a comparison. One is still an invitation.
    if (options_.hasInputs()) {
        session_.open(makeRequest(options_, graphDirection_));
    }
}

void AppWindow::drawWelcomePane() {
    if (!ImGui::Begin(kWelcomeTitle)) {
        ImGui::End();
        return;
    }

    WelcomeState state;
    state.leftPath = options_.leftPath;
    state.rightPath = options_.rightPath;
    state.picking = picker_.busy();
    state.error = pickerError_;

    switch (drawWelcome(state)) {
        case WelcomeChoice::Left:
            askForFile(PickerTarget::Left);
            break;
        case WelcomeChoice::Right:
            askForFile(PickerTarget::Right);
            break;
        case WelcomeChoice::None:
            break;
    }

    ImGui::End();
}

void AppWindow::drawDirectionMenu() {
    if (!ImGui::BeginMenu("Graph direction")) {
        return;
    }

    // Positions live in the layout, so a change is a new layout job rather than
    // a different way of drawing the one already published.
    const bool topDown = graphDirection_ == GraphDirection::TopDown;
    if (ImGui::MenuItem("Top down", nullptr, topDown) && !topDown) {
        setGraphDirection(GraphDirection::TopDown);
    }
    if (ImGui::MenuItem("Left to right", nullptr, !topDown) && topDown) {
        setGraphDirection(GraphDirection::LeftToRight);
    }

    ImGui::TextDisabled("A format may choose for itself.");
    ImGui::EndMenu();
}

void AppWindow::setGraphDirection(GraphDirection direction) {
    graphDirection_ = direction;
    if (options_.hasInputs()) {
        session_.open(makeRequest(options_, graphDirection_));
    }
}

void AppWindow::layoutDockSpaceOnce() {
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport();

    if (dockLayoutDone_) {
        return;
    }
    dockLayoutDone_ = true;

    // Respect a layout the user has already arranged and saved.
    if (ImGui::DockBuilderGetNode(dockspace) != nullptr &&
        ImGui::DockBuilderGetNode(dockspace)->IsSplitNode()) {
        return;
    }

    ImGuiID main = dockspace;
    ImGui::DockBuilderRemoveNode(main);
    ImGui::DockBuilderAddNode(main, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(main, ImGui::GetMainViewport()->WorkSize);

    const ImGuiID bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.16f, nullptr, &main);
    const ImGuiID right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.26f, nullptr, &main);

    ImGui::DockBuilderDockWindow(kWelcomeTitle, main);
    ImGui::DockBuilderDockWindow(kTextViewTitle, main);
    ImGui::DockBuilderDockWindow(kNodeViewTitle, main);
    ImGui::DockBuilderDockWindow(kDetailsTitle, right);
    ImGui::DockBuilderDockWindow(kStatusTitle, bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void AppWindow::drawTextView(const DiffSnapshot& snapshot) {
    if (!ImGui::Begin(kTextViewTitle)) {
        ImGui::End();
        return;
    }
    textView_.draw(snapshot, selection_);
    ImGui::End();
}

void AppWindow::drawNodeView(const DiffSnapshot& snapshot) {
    if (!ImGui::Begin(kNodeViewTitle)) {
        ImGui::End();
        return;
    }

    nodeView_.draw(snapshot, selection_);
    ImGui::End();

    // The view asks rather than acts, because changing direction rebuilds the
    // layout and the session that owns it lives here.
    if (const auto asked = nodeView_.takeDirectionRequest()) {
        setGraphDirection(*asked);
    }
}

void AppWindow::drawDetails(const DiffSnapshot& snapshot) {
    if (!ImGui::Begin(kDetailsTitle)) {
        ImGui::End();
        return;
    }

    if (!snapshot.hasSources()) {
        ImGui::TextDisabled("Nothing loaded.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("sides", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Left");
        ImGui::TableSetupColumn("Right");
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Bytes");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%zu", snapshot.left->size());
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%zu", snapshot.right->size());

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Lines");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%zu", snapshot.left->lineCount());
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%zu", snapshot.right->lineCount());

        if (snapshot.leftTree && snapshot.rightTree) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Nodes");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%zu", snapshot.leftTree->size());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%zu", snapshot.rightTree->size());
        }

        ImGui::EndTable();
    }

    if (snapshot.leftTree == nullptr || snapshot.provider == nullptr) {
        ImGui::Spacing();
        ImGui::TextDisabled(snapshot.stage == Stage::Failed ? "Not parsed." : "Parsing...");
        ImGui::End();
        return;
    }

    ImGui::Spacing();
    ImGui::Text("Format: %.*s", static_cast<int>(snapshot.provider->displayName().size()),
                snapshot.provider->displayName().data());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Whole tree", &detailsWholeTree_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off: only the selected node. On: the whole document.");
    }
    ImGui::Spacing();

    if (ImGui::BeginChild("outline")) {
        drawOutline(snapshot);
    }
    ImGui::EndChild();

    ImGui::End();
}

void AppWindow::drawOutline(const DiffSnapshot& snapshot) {
    const Tree& right = *snapshot.rightTree;

    // Nothing selected means there is no node to narrow to, so the whole
    // document is the only useful answer whatever the toggle says.
    if (detailsWholeTree_ || !selection_.active()) {
        drawTreeOutline(right, snapshot.leftTree.get(), *snapshot.provider, right.root(),
                        snapshot.treeDiff.get(), Side::Right);
        return;
    }

    // A deleted node only exists on the left, and the node view shows it, so
    // the panel has to be able to show it too. Which tree to read comes from
    // the selection rather than from an assumption that it is always the newer
    // one.
    const bool fromLeft = selection_.side == Side::Left;
    const Tree* tree = fromLeft ? snapshot.leftTree.get() : &right;
    const Tree* other = fromLeft ? &right : snapshot.leftTree.get();
    if (tree == nullptr || selection_.node >= tree->size()) {
        ImGui::TextDisabled("The selection is not in this comparison.");
        return;
    }

    drawTreeOutline(*tree, other, *snapshot.provider, selection_.node, snapshot.treeDiff.get(),
                    selection_.side, false);
}

void AppWindow::drawProperties(const Tree& tree, const Tree* otherTree,
                               const IFormatProvider& provider, NodeId id, const DiffModel* diff,
                               Side side) {
    const Node& node = tree.node(id);

    // The outline shows one side, so the other side's node has to be found to
    // say what a property changed from. Without it the panel can say that
    // something changed but not what it was, which is the question a reviewer
    // is actually asking.
    const Change* change = diff != nullptr ? diff->changeFor(side, id) : nullptr;
    const Node* before = nullptr;
    if (change != nullptr && otherTree != nullptr) {
        const NodeId counterpart = side == Side::Right ? change->left : change->right;
        if (counterpart != kInvalidNode && counterpart < otherTree->size()) {
            before = &otherTree->node(counterpart);
        }
    }

    // Properties in the provider's order, not document order. Ranking is
    // presentation only; matching still treats them as an unordered set.
    for (const auto index : propertyDisplayOrder(provider, tree, id)) {
        const Property& property = node.properties[index];
        const bool changed = change != nullptr && namedIn(change->changedProperties, property.name);
        drawProperty(property, changed,
                     before != nullptr ? before->findProperty(property.name) : nullptr);
    }

    // A property the other side had and this one does not would otherwise
    // vanish, leaving the one thing a reviewer cannot see as the one that was
    // taken away. It is listed after the rest, in the colour of a deletion.
    if (before != nullptr) {
        for (const Property& gone : before->properties) {
            if (node.findProperty(gone.name) == nullptr) {
                drawRemovedProperty(gone);
            }
        }
    }
}

void AppWindow::drawProperty(const Property& property, bool changed, const Property* before) {
    // A property with parts is a small tree of its own, so it is drawn as one.
    // The parts of a record keep their names; the parts of a sequence are
    // numbered, because a position in a list is not a name.
    if (property.hasParts()) {
        drawPropertyParts(property, changed, before);
        return;
    }

    ImGui::TextDisabled("%s", property.name.c_str());
    ImGui::SameLine();

    if (!changed) {
        ImGui::TextUnformatted(property.value.c_str());
        return;
    }

    // A value with a previous version reads as a change. One without is new, so
    // it takes the colour of an addition rather than of a modification.
    ImGui::PushStyleColor(ImGuiCol_Text, before != nullptr ? kModifiedColour : kAddedColour);
    if (before != nullptr) {
        ImGui::Text("%s %s %s", before->value.c_str(), kValueArrow, property.value.c_str());
    } else {
        ImGui::TextUnformatted(property.value.c_str());
    }
    ImGui::PopStyleColor();
}

void AppWindow::drawPropertyParts(const Property& property, bool changed,
                                  const Property* before) {
    ImGui::PushID(property.name.c_str());
    if (changed) {
        ImGui::PushStyleColor(ImGuiCol_Text, kModifiedColour);
    }

    // The header carries the value where the property has one, and what it
    // was before when that is what changed, so a record that is both a value
    // and a set of parts reads the way a scalar does.
    const std::string label = partsLabel(property, changed ? before : nullptr);
    // A property that changed opens itself, so the change is visible rather than
    // something the reader has to go looking for.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (changed) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    const bool open = ImGui::TreeNodeEx("parts", flags, "%s", label.c_str());
    if (changed) {
        ImGui::PopStyleColor();
    }

    if (open) {
        for (std::size_t i = 0; i < property.children.size(); ++i) {
            const Property& part = property.children[i];

            // What the same part was before, matched the way its parent
            // compares: a record by name, a sequence by position.
            const Property* was = nullptr;
            if (before != nullptr) {
                was = property.ordered() ? (i < before->children.size() ? &before->children[i] : nullptr)
                                         : before->findPart(part.name);
            }

            Property shown = part;
            if (shown.name.empty()) {
                shown.name = std::to_string(i);
            }
            const bool partChanged =
                before != nullptr && (was == nullptr || propertiesDiffer(part, *was));
            drawProperty(shown, partChanged, was);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AppWindow::drawRemovedProperty(const Property& property) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDeletedColour);
    ImGui::Text("%s", property.name.c_str());
    ImGui::SameLine();
    ImGui::Text("%s %s", property.value.c_str(), kValueArrow);
    ImGui::PopStyleColor();
}

void AppWindow::drawTreeOutline(const Tree& tree, const Tree* otherTree,
                                const IFormatProvider& provider, NodeId id, const DiffModel* diff,
                                Side side, bool withChildren) {
    if (id == kInvalidNode || id >= tree.size()) {
        return;
    }
    const Node& node = tree.node(id);
    const NodeStyle style = provider.style(tree, id);
    const NodeStatus status = diff != nullptr ? diff->statusOf(side, id) : NodeStatus::Unchanged;

    // Diff status is tinted over the provider's colour, so status survives any
    // palette a format chooses for itself.
    ImU32 colour = IM_COL32(style.accent.r, style.accent.g, style.accent.b, 255);
    switch (status) {
        case NodeStatus::Added:
            colour = kAddedColour;
            break;
        case NodeStatus::Deleted:
            colour = kDeletedColour;
            break;
        case NodeStatus::Modified:
            colour = kModifiedColour;
            break;
        case NodeStatus::Moved:
            colour = kMovedColour;
            break;
        case NodeStatus::Unchanged:
            break;
    }

    ImGui::PushID(static_cast<int>(id));
    ImGui::PushStyleColor(ImGuiCol_Text, colour);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if ((node.isLeaf() || !withChildren) && node.properties.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    const bool open = ImGui::TreeNodeEx("node", flags, "%s", style.title.c_str());
    ImGui::PopStyleColor();

    if (!style.subtitle.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", style.subtitle.c_str());
    }

    if (open) {
        drawProperties(tree, otherTree, provider, id, diff, side);
        if (withChildren) {
            for (const NodeId child : node.children) {
                drawTreeOutline(tree, otherTree, provider, child, diff, side);
            }
        } else if (!node.children.empty()) {
            // Say what is being left out, so a narrowed outline never reads as
            // a node that simply has nothing under it.
            ImGui::TextDisabled("%zu below, hidden", node.children.size());
        }
        if ((flags & ImGuiTreeNodeFlags_NoTreePushOnOpen) == 0) {
            ImGui::TreePop();
        }
    }
    ImGui::PopID();
}

void AppWindow::drawStatusBar(const DiffSnapshot& snapshot) {
    if (!ImGui::Begin(kStatusTitle)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Stage: %s", describe(snapshot.stage));
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Text("Workers: %u", session_.threadCount());
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Text("Startup: %.1f ms", startupMillis_);
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Text("Worst frame: %.2f ms", worstFrameMillis_);

    if (snapshot.stage != Stage::Idle && snapshot.elapsedMillis > 0.0) {
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Text("Last job: %.1f ms", snapshot.elapsedMillis);
    }

    if (session_.busy()) {
        ImGui::SameLine(0.0f, 24.0f);
        if (ImGui::SmallButton("Cancel")) {
            session_.cancel();
        }
    }

    if (snapshot.text != nullptr) {
        const TextDiff& text = *snapshot.text;
        ImGui::Separator();

        if (text.identical()) {
            ImGui::TextUnformatted("Files are identical.");
        } else {
            ImGui::TextColored(ImVec4(0.27f, 0.75f, 0.49f, 1.0f), "+%u", text.addedRows);
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::TextColored(ImVec4(0.89f, 0.43f, 0.41f, 1.0f), "-%u", text.deletedRows);
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::TextColored(ImVec4(0.88f, 0.69f, 0.32f, 1.0f), "~%u", text.modifiedRows);
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::Text("in %zu change%s", text.changeBlocks.size(),
                        text.changeBlocks.size() == 1 ? "" : "s");

            ImGui::SameLine(0.0f, 20.0f);
            if (ImGui::SmallButton("Previous")) {
                textView_.goToPreviousChange(snapshot);
            }
            ImGui::SameLine(0.0f, 6.0f);
            if (ImGui::SmallButton("Next")) {
                textView_.goToNextChange(snapshot);
            }
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextDisabled("(F8)");
        }

        // A trimmed result is stated outright. Under-reporting differences
        // without saying so is the one failure a diff tool does not survive.
        if (text.quality != TextDiffQuality::Full) {
            ImGui::TextColored(ImVec4(0.88f, 0.69f, 0.32f, 1.0f), "Reduced: %s",
                               describe(text.quality));
        }
    }

    if (snapshot.treeDiff != nullptr) {
        const DiffModel& tree = *snapshot.treeDiff;
        ImGui::TextUnformatted("Nodes:");
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextColored(ImVec4(0.27f, 0.75f, 0.49f, 1.0f), "+%u", tree.added);
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextColored(ImVec4(0.89f, 0.43f, 0.41f, 1.0f), "-%u", tree.deleted);
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextColored(ImVec4(0.88f, 0.69f, 0.32f, 1.0f), "~%u", tree.modified);
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextColored(ImVec4(0.66f, 0.56f, 0.88f, 1.0f), ">%u", tree.moved);
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("%u unchanged", tree.unchanged);

        if (tree.quality != MatchQuality::Full) {
            ImGui::TextColored(ImVec4(0.88f, 0.69f, 0.32f, 1.0f), "Reduced: %s",
                               describe(tree.quality));
        }
    }

    // What the format left out and where a script failed, stated per side,
    // and only when there is something to state. A tree that says nothing
    // was dropped or failed leaves this line out rather than saying zero.
    if (snapshot.leftTree && snapshot.rightTree) {
        const std::size_t droppedLeft = snapshot.leftTree->unrepresented().size();
        const std::size_t droppedRight = snapshot.rightTree->unrepresented().size();
        const std::size_t failedLeft = snapshot.leftTree->failures().size();
        const std::size_t failedRight = snapshot.rightTree->failures().size();
        if (droppedLeft + droppedRight > 0) {
            ImGui::TextColored(ImVec4(0.62f, 0.66f, 0.76f, 1.0f),
                               "Dropped by the format: %zu stretch%s left, %zu right", droppedLeft,
                               droppedLeft == 1 ? "" : "es", droppedRight);
        }
        if (failedLeft + failedRight > 0) {
            ImGui::TextColored(ImVec4(0.78f, 0.42f, 0.84f, 1.0f),
                               "Failed shaping jobs: %zu left, %zu right", failedLeft, failedRight);
        }
    }

    if (snapshot.stage == Stage::Failed) {
        ImGui::TextColored(ImVec4(0.88f, 0.45f, 0.43f, 1.0f), "%s", snapshot.message.c_str());
    }

    ImGui::End();
}

}  // namespace nmxd
