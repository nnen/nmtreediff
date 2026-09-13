#pragma once

/// \file
/// \brief The file types an open dialog offers, derived from the registry.

#include <string>
#include <vector>

namespace nmxd {

class ProviderRegistry;

/// \brief One entry in a file dialog's type list.
struct FileFilter {
    /// \brief What the entry is called in the dialog.
    std::string name;
    /// \brief The extensions it admits, lower-cased, without their dots,
    ///        comma-separated, in the form the dialog library takes.
    std::string extensions;
};

/// \brief Builds the type list for the open dialog from what the registry
///        knows.
///
/// \param registry The formats this run resolves with, configuration applied.
///
/// \returns The entries in the order the dialog should offer them: first one
///          that admits every known extension, then one per provider that
///          claims any, named after the provider.
///
/// \remarks Built from the registry rather than written down, so a format a
///          script defined is offered by the name the script gave it, and an
///          extension a configuration pointed at a format is admitted under
///          that format's entry. The widest entry comes first because it is
///          the one that is right most often. A provider claiming no extension
///          gets no entry, since there is nothing to filter by.
[[nodiscard]] std::vector<FileFilter> fileFiltersFor(const ProviderRegistry& registry);

}  // namespace nmxd
