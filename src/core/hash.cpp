#include "core/hash.h"

#include <algorithm>
#include <vector>

namespace nmxd {

namespace {

// FNV-1a, chosen because it is fixed, tiny, and has no platform-dependent
// behaviour. This is not a security hash; it only has to be stable and to
// spread ordinary document fragments.
constexpr std::uint64_t kOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kPrime = 1099511628211ull;

constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t value) noexcept {
    for (int byte = 0; byte < 8; ++byte) {
        h ^= (value >> (byte * 8)) & 0xFFull;
        h *= kPrime;
    }
    return h;
}

}  // namespace

std::uint64_t hashBytes(std::string_view bytes, std::uint64_t seed) noexcept {
    std::uint64_t h = seed == 0 ? kOffsetBasis : seed;
    for (const char c : bytes) {
        h ^= static_cast<std::uint8_t>(c);
        h *= kPrime;
    }
    return h;
}

void computeHashes(Tree& tree, const IFormatProvider& provider, std::stop_token token) {
    if (tree.empty()) {
        return;
    }

    std::vector<std::uint64_t> propertyHashes;
    std::vector<std::uint64_t> childHashes;

    // Children always have a higher index than their parent, so walking
    // backwards visits every child before its parent. No recursion, and no
    // stack depth to worry about on a deep tree.
    for (std::size_t i = tree.size(); i-- > 0;) {
        if ((i & 0xFFFu) == 0 && token.stop_requested()) {
            return;
        }

        Node& node = tree.node(static_cast<NodeId>(i));

        std::uint64_t h = hashBytes(node.kind, kOffsetBasis);

        // Properties as a set: hash each pair, sort the hashes, then fold. A
        // reordered attribute list therefore hashes the same.
        propertyHashes.clear();
        propertyHashes.reserve(node.properties.size());
        for (const auto& property : node.properties) {
            std::uint64_t ph = hashBytes(property.name, kOffsetBasis);
            ph = hashBytes("=", ph);
            ph = hashBytes(property.value, ph);
            propertyHashes.push_back(ph);
        }
        std::sort(propertyHashes.begin(), propertyHashes.end());
        h = mix(h, propertyHashes.size());
        for (const auto ph : propertyHashes) {
            h = mix(h, ph);
        }

        childHashes.clear();
        childHashes.reserve(node.children.size());
        for (const auto child : node.children) {
            childHashes.push_back(tree.node(child).contentHash);
        }
        if (!provider.childrenOrdered(tree, node.id)) {
            std::sort(childHashes.begin(), childHashes.end());
        }
        h = mix(h, childHashes.size());
        for (const auto ch : childHashes) {
            h = mix(h, ch);
        }

        node.contentHash = h;
    }
}

}  // namespace nmxd
