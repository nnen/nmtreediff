#pragma once

/// \file
/// \brief Asking the operating system for a file without stalling the frame.

#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace nmxd {

/// \brief Which side of the comparison a chosen file belongs to.
enum class PickerTarget {
    Left,   ///< The older side.
    Right,  ///< The newer side.
};

/// \brief The outcome of one trip through the native dialog.
enum class PickerOutcome {
    None,       ///< Nothing has been asked for.
    Pending,    ///< The dialog is open and the answer has not arrived.
    Chosen,     ///< A file was chosen and is waiting to be collected.
    Cancelled,  ///< The dialog was dismissed without a choice.
    Failed,     ///< The dialog could not be opened.
};

/// \brief Opens the operating system's file dialog off the frame loop.
///
/// \remarks A native dialog blocks the thread that opens it for as long as the
///          user is looking at it, which can be minutes. Constraint C does not
///          make an exception for waiting on a person, so the dialog runs on a
///          thread of its own and the frame loop asks for the answer once a
///          frame. The window keeps drawing behind it, which also means a
///          comparison already on screen stays readable while a new file is
///          being chosen.
///
///          One dialog at a time. A second request while one is open is
///          ignored rather than queued, because two native dialogs fighting
///          over the same parent window is a worse answer than nothing.
class FilePicker {
public:
    /// \brief Creates a picker with no dialog open.
    FilePicker() = default;

    /// \brief Waits for any dialog still open.
    ~FilePicker();

    FilePicker(const FilePicker&) = delete;
    FilePicker& operator=(const FilePicker&) = delete;

    /// \brief Opens the dialog, unless one is already open.
    ///
    /// \param target Which side the chosen file is for.
    /// \param startIn A directory to open in, or empty for the system default.
    ///
    /// \remarks Returns at once. The answer arrives through poll().
    void open(PickerTarget target, const std::filesystem::path& startIn = {});

    /// \brief Reports whether a dialog is open right now.
    ///
    /// \returns `true` while the user is still looking at a dialog.
    [[nodiscard]] bool busy() const;

    /// \brief Collects the answer, if one has arrived.
    ///
    /// \returns What happened. PickerOutcome::Chosen is returned exactly once
    ///          per choice, after which the picker is idle again.
    ///
    /// \remarks Called once a frame. Anything but Chosen leaves nothing to
    ///          collect, so the caller can ignore the rest.
    [[nodiscard]] PickerOutcome poll();

    /// \brief Returns the file chosen by the last successful poll().
    ///
    /// \returns The path, or an empty path when nothing was chosen.
    [[nodiscard]] const std::filesystem::path& chosen() const { return chosen_; }

    /// \brief Returns which side the last successful poll() was for.
    ///
    /// \returns The side named when the dialog was opened.
    [[nodiscard]] PickerTarget target() const { return target_; }

    /// \brief Returns why the dialog failed, if it did.
    ///
    /// \returns A message from the platform, or an empty string.
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    std::shared_ptr<struct PendingPick> state_;
    std::thread worker_;
    std::filesystem::path chosen_;
    std::string error_;
    PickerTarget target_ = PickerTarget::Left;
};

}  // namespace nmxd
