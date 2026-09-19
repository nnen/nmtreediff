/// \file
/// \brief Implementation of the log sink.

#include "core/log.h"

#include <utility>

namespace nmtreediff {

Log& Log::instance() {
    static Log log;
    return log;
}

void Log::write(LogStream stream, std::string_view text) {
    // One trailing newline is the line's own; anything more is content.
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
        if (!text.empty() && text.back() == '\r') {
            text.remove_suffix(1);
        }
    }

    std::lock_guard lock(mutex_);

    // Buffer the line, dropping the oldest once the buffer is full.
    LogLine line{stream, std::string(text), nextSequence_++};
    if (stream == LogStream::Err) {
        lastError_ = line.sequence;
    }
    lines_.push_back(std::move(line));
    while (lines_.size() > kCapacity) {
        lines_.pop_front();
    }

    // Forward under the lock, so two threads' lines do not interleave in the
    // middle of one another on the console.
    std::FILE* target = stream == LogStream::Out ? out_ : err_;
    if (target != nullptr) {
        std::fwrite(text.data(), 1, text.size(), target);
        std::fputc('\n', target);
        std::fflush(target);
    }
}

void Log::forwardTo(std::FILE* out, std::FILE* err) {
    std::lock_guard lock(mutex_);
    out_ = out;
    err_ = err;
}

std::vector<LogLine> Log::linesAfter(std::uint64_t afterSequence) const {
    std::lock_guard lock(mutex_);
    std::vector<LogLine> result;
    for (const LogLine& line : lines_) {
        if (line.sequence > afterSequence) {
            result.push_back(line);
        }
    }
    return result;
}

std::uint64_t Log::lastSequence() const {
    std::lock_guard lock(mutex_);
    return nextSequence_ - 1;
}

std::uint64_t Log::lastErrorSequence() const {
    std::lock_guard lock(mutex_);
    return lastError_;
}

std::size_t Log::size() const {
    std::lock_guard lock(mutex_);
    return lines_.size();
}

void Log::clear() {
    std::lock_guard lock(mutex_);
    lines_.clear();
}

void logOut(std::string_view text) { Log::instance().write(LogStream::Out, text); }

void logErr(std::string_view text) { Log::instance().write(LogStream::Err, text); }

}  // namespace nmtreediff
