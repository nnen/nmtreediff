/// \file
/// \brief Implementation of the window, the docking layout and the frame loop.

#include "ui/app_window.h"

#include "ui/screenshot.h"

#include <algorithm>
#include <cstdio>

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

/// \brief Window title of the details panel.
constexpr const char* kDetailsTitle = "Details";

/// \brief Window title of the status panel.
constexpr const char* kStatusTitle = "Status";

/// \brief The first frame counted towards the steady-state frame budget.
///
/// \remarks Frames before this are still settling: the window is being shown and
///          the compositor has not finished with it. That cost is fixed, lands a
///          few frames in, and has nothing to do with what the program is
///          computing, so folding it into the budget would hide every real stall
///          smaller than it.
constexpr unsigned kSettledFrame = 60;

/// \brief Prints a GLFW error to the standard error stream.
///
/// \param code The GLFW error code.
/// \param description GLFW's description, which may be null.
void reportGlfwError(int code, const char* description) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, description ? description : "");
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
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    running_ = true;
    return true;
}

int AppWindow::run() {
    if (!running_) {
        return 2;
    }

    // Queued before the first frame so the read happens on a worker while the
    // window is already up and drawing.
    if (options_.hasInputs()) {
        session_.open(SessionRequest{options_.leftPath, options_.rightPath, options_.leftLabel,
                                     options_.rightLabel, options_.format});
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
        std::printf(
            "startup_ms=%.1f frames=%u steady_worst_ms=%.2f settling_worst_ms=%.2f at_frame=%u "
            "workers=%u\n",
            startupMillis_, framesPresented_, worstSteadyFrameMillis_, worstFrameMillis_,
            worstFrameIndex_, session_.threadCount());
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
        std::fprintf(stderr, "could not write %s\n", options_.screenshotPath.string().c_str());
    }
}

void AppWindow::buildFrame() {
    drawMenuBar();
    layoutDockSpaceOnce();

    const auto snapshot = session_.snapshot();
    static const DiffSnapshot kEmpty;
    const DiffSnapshot& current = snapshot ? *snapshot : kEmpty;

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

    drawTextView(current);
    drawNodeView(current);
    drawDetails(current);
    drawStatusBar(current);

    // Focusing a window requires it to exist, and the panels are only created
    // by the calls above. Doing this after the first frame has built them is
    // what makes --view actually pick the tab that opens.
    if (!initialViewFocused_ && framesPresented_ > 0) {
        ImGui::SetWindowFocus(view_ == InitialView::Node ? kNodeViewTitle : kTextViewTitle);
        initialViewFocused_ = true;
    }
}

void AppWindow::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Reload", "Ctrl+R", false, options_.hasInputs())) {
            session_.open(SessionRequest{options_.leftPath, options_.rightPath, options_.leftLabel,
                                         options_.rightLabel, options_.format});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
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
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
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

    // An outline of the parsed tree, coloured by change status. The node view
    // that draws this as a graph arrives at M4. The right side is shown because
    // that is the version being reviewed.
    if (ImGui::BeginChild("outline")) {
        drawTreeOutline(*snapshot.rightTree, *snapshot.provider, snapshot.rightTree->root(),
                        snapshot.treeDiff.get(), Side::Right);
    }
    ImGui::EndChild();

    ImGui::End();
}

void AppWindow::drawTreeOutline(const Tree& tree, const IFormatProvider& provider, NodeId id,
                                const DiffModel* diff, Side side) {
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
            colour = IM_COL32(96, 200, 140, 255);
            break;
        case NodeStatus::Deleted:
            colour = IM_COL32(226, 110, 105, 255);
            break;
        case NodeStatus::Modified:
            colour = IM_COL32(224, 176, 82, 255);
            break;
        case NodeStatus::Moved:
            colour = IM_COL32(168, 143, 224, 255);
            break;
        case NodeStatus::Unchanged:
            break;
    }

    ImGui::PushID(static_cast<int>(id));
    ImGui::PushStyleColor(ImGuiCol_Text, colour);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (node.isLeaf() && node.properties.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    const bool open = ImGui::TreeNodeEx("node", flags, "%s", style.title.c_str());
    ImGui::PopStyleColor();

    if (!style.subtitle.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", style.subtitle.c_str());
    }

    if (open) {
        // Properties in the provider's order, not document order. Ranking is
        // presentation only; matching still treats them as an unordered set.
        for (const auto index : propertyDisplayOrder(provider, tree, id)) {
            const Property& property = node.properties[index];
            ImGui::TextDisabled("%s", property.name.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(property.value.c_str());
        }
        for (const NodeId child : node.children) {
            drawTreeOutline(tree, provider, child, diff, side);
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

    if (snapshot.stage == Stage::Failed) {
        ImGui::TextColored(ImVec4(0.88f, 0.45f, 0.43f, 1.0f), "%s", snapshot.message.c_str());
    }

    ImGui::End();
}

}  // namespace nmxd
