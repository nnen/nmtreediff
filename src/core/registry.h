#pragma once

/// \file
/// \brief Deciding which format provider handles a given file.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/provider.h"

namespace nmxd {

/// \brief The set of formats the tool knows, and how a file resolves to one.
///
/// \remarks Resolution order is the name the user asked for, then the highest
///          sniffing score, then the fallback. Explicit always wins, because
///          when someone passes `--format` they are usually correcting a wrong
///          guess.
class ProviderRegistry {
public:
    /// \brief Adds a provider to the registry.
    ///
    /// \param provider The provider to add. A null pointer is ignored.
    ///
    /// \remarks The first provider added becomes the fallback unless
    ///          setFallback() says otherwise.
    void add(std::unique_ptr<IFormatProvider> provider);

    /// \brief Finds a provider by its stable name.
    ///
    /// \param name The name to look for.
    ///
    /// \returns The provider, or `nullptr` when no provider uses that name. The
    ///          pointer stays valid for the lifetime of the registry.
    [[nodiscard]] const IFormatProvider* byName(std::string_view name) const;

    /// \brief Chooses the provider for a file.
    ///
    /// \param source The file to resolve.
    /// \param explicitName A format name the user asked for, or empty to sniff.
    /// \param unknownName Optional. Set to `true` when \p explicitName names no
    ///        known provider, and `false` otherwise.
    ///
    /// \returns The chosen provider, or `nullptr` when the registry is empty or
    ///          when \p explicitName is not recognised.
    ///
    /// \remarks An unknown explicit name is reported through \p unknownName
    ///          rather than silently sniffing instead, because it is usually a
    ///          typo in a diff-tool configuration that would otherwise go
    ///          unnoticed for a long time.
    [[nodiscard]] const IFormatProvider* resolve(const SourceFile& source,
                                                 std::string_view explicitName = {},
                                                 bool* unknownName = nullptr) const;

    /// \brief Lists the names of every registered provider.
    ///
    /// \returns The names, in registration order.
    [[nodiscard]] std::vector<std::string_view> names() const;

    /// \brief Returns how many providers are registered.
    ///
    /// \returns The provider count.
    [[nodiscard]] std::size_t size() const noexcept { return providers_.size(); }

    /// \brief Reports whether any provider is registered.
    ///
    /// \returns `true` when the registry holds nothing.
    [[nodiscard]] bool empty() const noexcept { return providers_.empty(); }

    /// \brief Chooses the provider used when nothing scores.
    ///
    /// \param name The fallback provider's name.
    void setFallback(std::string_view name);

private:
    std::vector<std::unique_ptr<IFormatProvider>> providers_;
    std::string fallback_;
};

/// \brief Builds a registry holding the built-in formats.
///
/// \returns A registry with the generic XML provider registered and set as the
///          fallback.
///
/// \remarks Registration is an explicit list rather than a static initialiser
///          per format. In a static library the linker drops a translation unit
///          nothing references, taking its self-registration with it, and a
///          format that silently vanishes from a release build is far worse than
///          one list that has to be edited.
[[nodiscard]] ProviderRegistry makeDefaultRegistry();

}  // namespace nmxd
