/// \file
/// \brief Implementation of subtree content hashing.

#include "core/hash.h"

#include <algorithm>
#include <vector>

namespace nmtreediff {

namespace {

/// \brief The FNV-1a offset basis.
///
/// \remarks FNV-1a is chosen because it is fixed, tiny, and has no
///          platform-dependent behaviour. This is not a security hash; it only
///          has to be stable and to spread ordinary document fragments.
constexpr std::uint64_t kOffsetBasis = 1469598103934665603ull;

/// \brief The FNV-1a multiplier.
constexpr std::uint64_t kPrime = 1099511628211ull;

/// \brief Folds a 64-bit value into a running hash, one byte at a time.
///
/// \param h The running hash.
/// \param value The value to fold in.
///
/// \returns The updated hash.
constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t value) noexcept {
    for (int byte = 0; byte < 8; ++byte) {
        h ^= (value >> (byte * 8)) & 0xFFull;
        h *= kPrime;
    }
    return h;
}

/// \brief Hashes what one property says about itself, parts aside.
///
/// \param property The property to hash.
///
/// \returns A hash of the name, the value and, for a record or a sequence,
///          the form.
///
/// \remarks The form is folded only when it is not scalar, so a scalar hashes
///          as it always did and nothing in the golden corpus moves except
///          where a format actually starts nesting.
[[nodiscard]] std::uint64_t hashPropertyHead(const Property& property) {
    std::uint64_t h = hashBytes(property.name, kOffsetBasis);
    h = hashBytes("=", h);
    h = hashBytes(property.value, h);
    if (property.form != PropertyForm::Scalar) {
        h = mix(h, static_cast<std::uint64_t>(property.form));
    }
    return h;
}

/// \brief Folds a property's part hashes into its own.
///
/// \param property The property whose parts were hashed.
/// \param head The hash of the property itself.
/// \param parts One hash per part, in part order. Sorted in place for a
///        record.
///
/// \returns The finished hash.
[[nodiscard]] std::uint64_t foldParts(const Property& property, std::uint64_t head,
                                      std::vector<std::uint64_t>& parts) {
    if (!property.ordered()) {
        std::sort(parts.begin(), parts.end());
    }
    std::uint64_t h = mix(head, parts.size());
    for (const std::uint64_t part : parts) {
        h = mix(h, part);
    }
    return h;
}

/// \brief One property on the way to being hashed.
struct PropertyFrame {
    /// \brief The property.
    const Property* property = nullptr;
    /// \brief How many of its parts have been pushed so far.
    std::size_t next = 0;
    /// \brief The hashes of the parts finished so far.
    std::vector<std::uint64_t> parts;
};

}  // namespace

std::uint64_t hashBytes(std::string_view bytes, std::uint64_t seed) noexcept {
    std::uint64_t h = seed == 0 ? kOffsetBasis : seed;
    for (const char c : bytes) {
        h ^= static_cast<std::uint8_t>(c);
        h *= kPrime;
    }
    return h;
}

std::uint64_t hashProperty(const Property& property) {
    if (!property.hasParts()) {
        return hashPropertyHead(property);
    }

    // Post-order over the parts with a stack of frames, so a property nested
    // to any depth never costs a call frame per level. A frame is finished
    // once every part has been hashed, and its hash goes to the frame below.
    std::vector<PropertyFrame> stack;
    stack.push_back(PropertyFrame{&property});
    while (true) {
        PropertyFrame& frame = stack.back();
        const Property& current = *frame.property;
        if (frame.next < current.children.size()) {
            const Property& part = current.children[frame.next++];
            stack.push_back(PropertyFrame{&part});
            continue;
        }

        std::uint64_t h = hashPropertyHead(current);
        if (current.hasParts()) {
            h = foldParts(current, h, frame.parts);
        }
        stack.pop_back();
        if (stack.empty()) {
            return h;
        }
        stack.back().parts.push_back(h);
    }
}

void computeHashes(Tree& tree, std::stop_token token) {
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
            propertyHashes.push_back(hashProperty(property));
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
        if (!node.childrenOrdered) {
            std::sort(childHashes.begin(), childHashes.end());
        }
        h = mix(h, childHashes.size());
        for (const auto ch : childHashes) {
            h = mix(h, ch);
        }

        node.contentHash = h;
    }
}

}  // namespace nmtreediff
