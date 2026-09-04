#pragma once

// Subtree content hashing, the input to the matcher's second pass.
//
// Two subtrees that are the same document fragment must hash the same in every
// run and on every machine, because a change in hashing changes which nodes
// pair up and therefore what the user is shown. Nothing here consults the
// address of anything, and the mixing is fixed.

#include <cstdint>
#include <stop_token>

#include "core/provider.h"
#include "core/tree.h"

namespace nmxd {

// Fills in contentHash for every node, bottom up.
//
// A node's hash covers its kind, its properties as an unordered set, and its
// children. Properties are order-independent so that a reordered attribute
// list is not a change. Children follow the provider: ordered children hash in
// order, unordered children hash as a set, so that reordering the members of a
// JSON object leaves the hash alone while reordering an array does not.
void computeHashes(Tree& tree, const IFormatProvider& provider, std::stop_token token = {});

// Exposed for tests and for the matcher's own keying.
[[nodiscard]] std::uint64_t hashBytes(std::string_view bytes, std::uint64_t seed = 0) noexcept;

}  // namespace nmxd
