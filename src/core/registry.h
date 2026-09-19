#pragma once

/// \file
/// \brief Deciding which format provider handles a given file.

#include <memory>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/provider.h"

namespace nmtreediff {

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

    /// \brief Registers a provider ahead of everything already registered.
    ///
    /// \param provider The provider to add.
    ///
    /// \remarks Resolution breaks a tie in favour of whichever provider was
    ///          registered first, so this is how a format wins one. A scripted
    ///          format built on generic XML claims the same score for an
    ///          extension it named, and claiming it is the deliberate act: the
    ///          person who wrote the script meant their format to read those
    ///          files, not the one underneath it.
    void addFirst(std::unique_ptr<IFormatProvider> provider);

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

    /// \brief Points an extension at a provider.
    ///
    /// \param extension The extension, including its leading dot.
    /// \param providerName The provider that should handle it.
    ///
    /// \returns `true` when the provider is registered, `false` when the
    ///          mapping was ignored because nothing goes by that name.
    ///
    /// \remarks An override beats sniffing but loses to an explicit `--format`,
    ///          which puts the three ways of choosing a provider in the order of
    ///          how deliberate they are: this one is a studio's standing
    ///          decision, sniffing is a guess, and the command line is a person
    ///          correcting one of them right now.
    bool mapExtension(std::string_view extension, std::string_view providerName);

    /// \brief Applies a configuration file's contents to this registry.
    ///
    /// \param config The configuration to apply.
    ///
    /// \returns The provider names the configuration mentioned that this
    ///          registry does not know, in the order they appeared.
    ///
    /// \remarks An unrecognised name is returned rather than ignored, because a
    ///          typo in a studio-wide configuration would otherwise send every
    ///          artist's diff quietly through the wrong provider.
    [[nodiscard]] std::vector<std::string> apply(const ProviderConfig& config);

    /// \brief Returns the provider an extension has been pointed at.
    ///
    /// \param extension The extension, including its leading dot.
    ///
    /// \returns The provider, or `nullptr` when the extension has no override.
    [[nodiscard]] const IFormatProvider* overrideFor(std::string_view extension) const;

    /// \brief Returns every extension a configuration pointed at a provider.
    ///
    /// \returns Extension to provider name, each with its leading dot, in the
    ///          order they were applied; a later entry for the same extension
    ///          is the one overrideFor() honours.
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& overrides() const noexcept {
        return overrides_;
    }

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
    std::vector<std::pair<std::string, std::string>> overrides_;
    std::string fallback_;
};

/// \brief Builds a registry holding the built-in formats.
///
/// \returns A registry holding the generic XML provider, the generic JSON
///          provider and the sample behavior-tree provider, with generic XML as
///          the fallback.
///
/// \remarks Registration is an explicit list rather than a static initialiser
///          per format. In a static library the linker drops a translation unit
///          nothing references, taking its self-registration with it, and a
///          format that silently vanishes from a release build is far worse than
///          one list that has to be edited.
[[nodiscard]] ProviderRegistry makeDefaultRegistry();

}  // namespace nmtreediff
