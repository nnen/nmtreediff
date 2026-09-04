#pragma once

// Which provider handles a given file.
//
// Resolution order is: the name the user asked for, then the highest sniffing
// score, then the fallback. Explicit always wins, because when someone passes
// --format they are usually correcting a wrong guess.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/provider.h"

namespace nmxd {

class ProviderRegistry {
public:
    void add(std::unique_ptr<IFormatProvider> provider);

    [[nodiscard]] const IFormatProvider* byName(std::string_view name) const;

    // Nullptr only when the registry is empty. An unknown explicit name is a
    // mistake worth reporting rather than silently sniffing instead, so the
    // caller is told through `unknownName`.
    [[nodiscard]] const IFormatProvider* resolve(const SourceFile& source,
                                                 std::string_view explicitName = {},
                                                 bool* unknownName = nullptr) const;

    [[nodiscard]] std::vector<std::string_view> names() const;
    [[nodiscard]] std::size_t size() const noexcept { return providers_.size(); }
    [[nodiscard]] bool empty() const noexcept { return providers_.empty(); }

    // The provider used when nothing scores. Defaults to the first added.
    void setFallback(std::string_view name);

private:
    std::vector<std::unique_ptr<IFormatProvider>> providers_;
    std::string fallback_;
};

// Builds a registry holding the built-in formats.
//
// Registration is explicit rather than a static initialiser per format: in a
// static library a translation unit nothing references is dropped by the
// linker, taking its self-registration with it. One list that has to be edited
// is better than a format that silently disappears from a release build.
[[nodiscard]] ProviderRegistry makeDefaultRegistry();

}  // namespace nmxd
