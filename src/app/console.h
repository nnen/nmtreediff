#pragma once

/// \file
/// \brief Finding the console or pipe a GUI-subsystem process should write to.

#include <cstdio>

namespace nmtreediff {

/// \brief Where the process's two output streams can go.
struct OutputStreams {
    /// \brief Standard output, or null when nothing is listening.
    std::FILE* out = nullptr;
    /// \brief Standard error, or null when nothing is listening.
    std::FILE* err = nullptr;
    /// \brief Whether the process attached itself to its parent's console.
    ///
    /// \remarks Reported so a caller can tell the three cases apart: streams
    ///          it was handed, a console it borrowed, or nothing at all.
    bool attachedToParent = false;
};

/// \brief Works out where standard output and error should go, and points
///        them there.
///
/// \returns The streams to forward to, null where there is nowhere to write.
///
/// \remarks On Windows the program is a GUI-subsystem binary, so a launch from
///          the desktop or a version control client opens no console window.
///          That costs it standard output, which the headless path and every
///          integration guide depend on, so this puts it back where it can be:
///          a standard handle the caller redirected to a file or a pipe is
///          used as it is; otherwise the parent process's console is attached
///          and the streams reopened on it; and with neither the process has
///          nowhere to write, which is the desktop launch and is right. Call it
///          first, before anything is written.
///
///          A command shell does not wait for a GUI-subsystem process, so a
///          headless run typed at a prompt returns the prompt before the
///          report and interleaves the two. Version control tools wait on the
///          process handle and are unaffected.
///
///          On every other platform the standard streams are what they always
///          were and this changes nothing.
OutputStreams attachToCaller();

}  // namespace nmtreediff
