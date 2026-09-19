/// \file
/// \brief Implementation of the dialog type list.

#include "core/file_filters.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "core/provider.h"
#include "core/registry.h"

namespace nmtreediff {

namespace {

/// \brief What the entry admitting every known extension is called.
constexpr const char* kEveryFormatName = "Tree data";

/// \brief Puts an extension into the form the dialog takes.
///
/// \param extension An extension with or without its leading dot.
///
/// \returns The same extension lower-cased and without the dot.
[[nodiscard]] std::string dialogForm(std::string_view extension) {
    if (!extension.empty() && extension.front() == '.') {
        extension.remove_prefix(1);
    }
    std::string out(extension);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// \brief Adds an extension to a list once.
///
/// \param list The extensions gathered so far, in the dialog's form.
/// \param extension The extension to add, in any form.
void addOnce(std::vector<std::string>& list, std::string_view extension) {
    std::string formed = dialogForm(extension);
    if (formed.empty() || std::find(list.begin(), list.end(), formed) != list.end()) {
        return;
    }
    list.push_back(std::move(formed));
}

/// \brief Joins extensions with commas.
///
/// \param list The extensions, in the dialog's form.
///
/// \returns The comma-separated spec the dialog library takes.
[[nodiscard]] std::string joined(const std::vector<std::string>& list) {
    std::string out;
    for (const std::string& extension : list) {
        if (!out.empty()) {
            out += ',';
        }
        out += extension;
    }
    return out;
}

/// \brief Gathers the extensions one provider admits.
///
/// \param registry The registry, for the configured overrides.
/// \param provider The provider.
///
/// \returns Its default extensions, then every configured extension pointed
///          at it, each once.
[[nodiscard]] std::vector<std::string> extensionsOf(const ProviderRegistry& registry,
                                                    const IFormatProvider& provider) {
    std::vector<std::string> list;
    for (const std::string_view extension : provider.defaultExtensions()) {
        addOnce(list, extension);
    }
    for (const auto& [extension, providerName] : registry.overrides()) {
        // The last mapping of an extension is the one that holds, so an
        // extension is credited to the provider overrideFor() would name.
        if (providerName == provider.name() && registry.overrideFor(extension) == &provider) {
            addOnce(list, extension);
        }
    }
    return list;
}

}  // namespace

std::vector<FileFilter> fileFiltersFor(const ProviderRegistry& registry) {
    std::vector<FileFilter> filters;
    std::vector<std::string> every;

    // One entry per provider that claims anything, in registry order, which
    // puts a format a script defined ahead of the built-in it sits on.
    for (const std::string_view name : registry.names()) {
        const IFormatProvider* provider = registry.byName(name);
        if (provider == nullptr) {
            continue;
        }
        const std::vector<std::string> extensions = extensionsOf(registry, *provider);
        if (extensions.empty()) {
            continue;
        }
        for (const std::string& extension : extensions) {
            addOnce(every, extension);
        }
        filters.push_back(FileFilter{std::string(provider->displayName()), joined(extensions)});
    }

    // The widest entry first, since it is the one that is right most often.
    if (!every.empty()) {
        filters.insert(filters.begin(), FileFilter{kEveryFormatName, joined(every)});
    }
    return filters;
}

}  // namespace nmtreediff
