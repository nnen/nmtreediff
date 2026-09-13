#pragma once

/// \file
/// \brief The one place every line the program writes goes through.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nmxd {

/// \brief Which of the process's two output streams a line belongs to.
enum class LogStream : std::uint8_t {
    Out,  ///< Standard output: reports, listings, what the program produced.
    Err,  ///< Standard error: problems, warnings, what went wrong.
};

/// \brief One line the program wrote.
struct LogLine {
    /// \brief Where the line was addressed.
    LogStream stream = LogStream::Out;
    /// \brief The text, without its trailing newline. May span several lines.
    std::string text;
    /// \brief A counter that only increases, so a reader can ask for what is
    ///        new since it last looked.
    std::uint64_t sequence = 0;
};

/// \brief Collects what the program writes and forwards it where it can go.
///
/// \remarks A window launched from the desktop has no console, so standard
///          output and standard error of that process go nowhere; the Output
///          pane is then not a mirror of them but the only place they exist.
///          Every write therefore lands here first. The sink keeps a bounded
///          buffer the pane draws from and forwards each line to the real
///          stream when the process has one, which is how a headless run and
///          the pane see the same text.
///
///          Safe to write from any thread: shaping jobs run on workers and a
///          script may fail on any of them.
class Log {
public:
    /// \brief How many lines the buffer keeps before the oldest are dropped.
    ///
    /// \remarks Enough to hold a long session of failures and prints; small
    ///          enough that a script printing in a loop cannot grow the
    ///          process without bound.
    static constexpr std::size_t kCapacity = 4000;

    /// \brief Returns the process-wide sink.
    ///
    /// \returns The one instance, made on first use.
    static Log& instance();

    /// \brief Records a line and forwards it.
    ///
    /// \param stream Which stream the line is addressed to.
    /// \param text The text. A trailing newline is dropped; one is written
    ///        after the text when forwarding.
    void write(LogStream stream, std::string_view text);

    /// \brief Says where forwarded lines may go.
    ///
    /// \param out The C stream for LogStream::Out, or null for none.
    /// \param err The C stream for LogStream::Err, or null for none.
    ///
    /// \remarks Set once at startup, after the process has worked out whether
    ///          it has a console or a pipe to write to. Until then both are the
    ///          standard streams, which is right for a console build and for
    ///          the tests.
    void forwardTo(std::FILE* out, std::FILE* err);

    /// \brief Returns the lines written after a point.
    ///
    /// \param afterSequence Return lines with a greater sequence than this;
    ///        zero returns everything the buffer holds.
    ///
    /// \returns The lines, oldest first.
    [[nodiscard]] std::vector<LogLine> linesAfter(std::uint64_t afterSequence) const;

    /// \brief Returns the sequence of the last line written.
    ///
    /// \returns The sequence, or zero when nothing has been written.
    [[nodiscard]] std::uint64_t lastSequence() const;

    /// \brief Returns the sequence of the last error line written.
    ///
    /// \returns The sequence, or zero when no error has been written.
    ///
    /// \remarks What the Output pane watches to open itself on the first
    ///          problem without reading every line each frame.
    [[nodiscard]] std::uint64_t lastErrorSequence() const;

    /// \brief Returns how many lines the buffer holds.
    ///
    /// \returns The count, at most kCapacity.
    [[nodiscard]] std::size_t size() const;

    /// \brief Drops every buffered line.
    ///
    /// \remarks Sequences keep counting, so a reader holding an old sequence
    ///          is not handed the same lines twice.
    void clear();

private:
    Log() = default;

    mutable std::mutex mutex_;
    std::deque<LogLine> lines_;
    std::uint64_t nextSequence_ = 1;
    std::uint64_t lastError_ = 0;
    std::FILE* out_ = stdout;
    std::FILE* err_ = stderr;
};

/// \brief Writes a line to standard output through the sink.
///
/// \param text The line, with or without its newline.
void logOut(std::string_view text);

/// \brief Writes a line to standard error through the sink.
///
/// \param text The line, with or without its newline.
void logErr(std::string_view text);

}  // namespace nmxd
