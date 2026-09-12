#pragma once

/// \file
/// \brief Subtree content hashing, the input to the matcher's second pass.

#include <cstdint>
#include <stop_token>

#include "core/provider.h"
#include "core/tree.h"

namespace nmxd {

/// \brief Fills in Node::contentHash for every node in a tree, bottom up.
///
/// \param tree The tree to hash, modified in place.
/// \param provider The format provider, consulted for child ordering.
/// \param token Checked periodically; the function returns early when a stop is
///        requested, leaving the remaining hashes unset.
///
/// \remarks A node's hash covers its kind, its properties as an unordered set,
///          and its children. Properties are order-independent so that a
///          reordered attribute list is not a change. Children follow the
///          provider: ordered children hash in order, unordered children hash
///          as a set, so reordering the members of a JSON object leaves the
///          hash alone while reordering an array does not.
///
///          Two subtrees that are the same document fragment must hash the same
///          in every run and on every machine, because a change in hashing
///          changes which nodes pair up and therefore what the user is shown.
///          Nothing here consults the address of anything, and the mixing is
///          fixed.
///
///          Requires Tree::finalize() to have run, and walks the arena
///          backwards so that every child is hashed before its parent without
///          recursing.
void computeHashes(Tree& tree, const IFormatProvider& provider, std::stop_token token = {});

/// \brief Hashes one property, parts and all.
///
/// \param property The property to hash.
///
/// \returns A hash covering the name, the value, the form and the whole
///          subtree of parts.
///
/// \remarks A record's parts are folded in sorted order and a sequence's in
///          the order they appear, which is what makes reordering a transform's
///          fields invisible and reordering a list of tags a change. The form
///          is folded in for a record or a sequence, so an empty record, an
///          empty sequence and an empty scalar are three hashes. A scalar
///          hashes exactly as it did before properties had forms.
///
///          This is the one definition of "the same property" in the tool. The
///          change list and the details panel compare through it, so they
///          cannot disagree with matching about what changed.
///
///          Walks the parts with an explicit stack rather than recursing, so a
///          property nested to any depth costs memory rather than the process.
[[nodiscard]] std::uint64_t hashProperty(const Property& property);

/// \brief Hashes a byte range with FNV-1a.
///
/// \param bytes The bytes to hash.
/// \param seed The starting value, or zero to start from the standard offset
///        basis. Pass a previous result to chain several ranges together.
///
/// \returns The hash.
///
/// \remarks Not a security hash. It only has to be stable across runs and
///          machines, and to spread ordinary document fragments.
[[nodiscard]] std::uint64_t hashBytes(std::string_view bytes, std::uint64_t seed = 0) noexcept;

}  // namespace nmxd
