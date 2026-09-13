/// \file
/// \brief Implementation of the off-thread native file dialog.

#include "ui/file_picker.h"

#include <atomic>
#include <iterator>
#include <string>
#include <utility>

#include <nfd.h>

namespace nmxd {

/// \brief What the dialog thread writes and the frame loop reads.
///
/// \remarks Lives here rather than in the header because nothing outside this
///          file has any use for it. Held behind a shared pointer so the thread
///          never touches a picker that has already been destroyed.
struct PendingPick {
    /// \brief Set last, once every other field is written.
    std::atomic<bool> finished{false};
    /// \brief Whether the dialog was dismissed without a choice.
    bool cancelled = false;
    /// \brief The chosen file, or empty.
    std::filesystem::path path;
    /// \brief Why the dialog failed, or empty.
    std::string error;
};

namespace {

/// \brief Turns the dialog's UTF-8 answer into a path.
///
/// \param utf8 A NUL-terminated UTF-8 string from the dialog.
///
/// \returns The same path, decoded.
///
/// \remarks The UTF-8 entry points are used rather than the native ones so that
///          one code path serves every platform. Going through `char8_t` is
///          what makes the conversion mean UTF-8 on Windows too, where building
///          a path from a plain `char` string would read it in the active code
///          page and mangle any name outside it.
[[nodiscard]] std::filesystem::path pathFromUtf8(const char* utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8)));
}

/// \brief Turns a path into the UTF-8 the dialog expects.
///
/// \param path The directory to open in.
///
/// \returns The same path as UTF-8 bytes.
[[nodiscard]] std::string utf8FromPath(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.c_str()), encoded.size());
}

/// \brief Runs one dialog and records what came back.
///
/// \param state Where to write the answer.
/// \param filters The file types to offer, owned here for the dialog's life.
/// \param startIn A directory to open in as UTF-8, or empty for the default.
///
/// \remarks Runs on its own thread. The library is started and stopped around
///          each dialog rather than once for the process, so the whole of its
///          lifetime stays on the thread that uses it, which is what the
///          platform back ends expect. The filters come from the registry, so
///          a format a script defined is offered by its own name; the library
///          adds an entry admitting every file after them.
void runDialog(std::shared_ptr<PendingPick> state, std::vector<FileFilter> filters,
               std::string startIn) {
    if (NFD_Init() != NFD_OKAY) {
        state->error = "the file dialog could not start";
        state->finished = true;
        return;
    }

    // The library takes pointers into strings that must outlive the call; the
    // vector this thread owns is where they point.
    std::vector<nfdu8filteritem_t> items;
    items.reserve(filters.size());
    for (const FileFilter& filter : filters) {
        items.push_back(nfdu8filteritem_t{filter.name.c_str(), filter.extensions.c_str()});
    }

    nfdu8char_t* chosen = nullptr;
    const nfdresult_t result =
        NFD_OpenDialogU8(&chosen, items.empty() ? nullptr : items.data(),
                         static_cast<nfdfiltersize_t>(items.size()),
                         startIn.empty() ? nullptr : startIn.c_str());

    if (result == NFD_OKAY && chosen != nullptr) {
        state->path = pathFromUtf8(chosen);
        NFD_FreePathU8(chosen);
    } else if (result == NFD_CANCEL) {
        state->cancelled = true;
    } else {
        const char* message = NFD_GetError();
        state->error = message != nullptr ? message : "the file dialog failed";
    }

    NFD_Quit();
    state->finished = true;
}

}  // namespace

FilePicker::~FilePicker() {
    // The dialog owns a window of its own, so the process must not exit from
    // under it. Waiting is the only correct answer, because there is no way to
    // dismiss a native dialog from outside it.
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool FilePicker::busy() const { return state_ != nullptr && !state_->finished; }

void FilePicker::open(PickerTarget target, std::vector<FileFilter> filters,
                      const std::filesystem::path& startIn) {
    if (busy()) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    target_ = target;
    error_.clear();
    chosen_.clear();
    state_ = std::make_shared<PendingPick>();
    worker_ = std::thread(runDialog, state_, std::move(filters), utf8FromPath(startIn));
}

PickerOutcome FilePicker::poll() {
    if (state_ == nullptr) {
        return PickerOutcome::None;
    }
    if (!state_->finished) {
        return PickerOutcome::Pending;
    }

    // Everything else was written before the flag was set, so it can be read
    // now. Joining here also keeps the next open() cheap.
    if (worker_.joinable()) {
        worker_.join();
    }

    const std::shared_ptr<PendingPick> finished = std::move(state_);
    state_.reset();

    if (!finished->error.empty()) {
        error_ = finished->error;
        return PickerOutcome::Failed;
    }
    if (finished->cancelled || finished->path.empty()) {
        return PickerOutcome::Cancelled;
    }

    chosen_ = finished->path;
    return PickerOutcome::Chosen;
}

}  // namespace nmxd
