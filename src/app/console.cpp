/// \file
/// \brief Implementation of the console attachment.

#include "app/console.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <iostream>
#endif

namespace nmtreediff {

#ifdef _WIN32

namespace {

/// \brief Reports whether a standard handle points at something.
///
/// \param which STD_OUTPUT_HANDLE or STD_ERROR_HANDLE.
///
/// \returns `true` when the caller handed the process a file, a pipe or a
///          console for that stream.
///
/// \remarks A GUI-subsystem process launched without redirection gets no
///          standard handles at all, which is what tells this apart from a
///          launch whose output someone is collecting.
[[nodiscard]] bool handleIsLive(DWORD which) {
    const HANDLE handle = GetStdHandle(which);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    return GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

/// \brief Reopens a C stream on the console the process just attached to.
///
/// \param stream stdout or stderr.
///
/// \returns The stream when it reopened, or null.
[[nodiscard]] std::FILE* reopenOnConsole(std::FILE* stream) {
    std::FILE* reopened = nullptr;
    if (freopen_s(&reopened, "CONOUT$", "w", stream) != 0) {
        return nullptr;
    }
    // Line buffered would be the natural choice, but the console is what a
    // person reads and a flush per line is cheap; the sink flushes anyway.
    setvbuf(stream, nullptr, _IONBF, 0);
    return stream;
}

}  // namespace

OutputStreams attachToCaller() {
    OutputStreams streams;
    const bool outLive = handleIsLive(STD_OUTPUT_HANDLE);
    const bool errLive = handleIsLive(STD_ERROR_HANDLE);

    // Streams the caller redirected are used as they are: a build job
    // collecting the report into a file must get the file.
    if (outLive && errLive) {
        streams.out = stdout;
        streams.err = stderr;
        return streams;
    }

    // Otherwise borrow the console the process was started from, when there
    // was one. A desktop launch has none, and then there is nowhere to write.
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        streams.out = outLive ? stdout : nullptr;
        streams.err = errLive ? stderr : nullptr;
        return streams;
    }
    streams.attachedToParent = true;
    streams.out = outLive ? stdout : reopenOnConsole(stdout);
    streams.err = errLive ? stderr : reopenOnConsole(stderr);

    // The C++ streams sit on the C ones and pick the change up, but anything
    // they buffered before the reopen would land nowhere; nothing has been
    // written yet by contract, so this only resets their state.
    std::cout.clear();
    std::cerr.clear();
    return streams;
}

#else

OutputStreams attachToCaller() {
    OutputStreams streams;
    streams.out = stdout;
    streams.err = stderr;
    return streams;
}

#endif

}  // namespace nmtreediff
