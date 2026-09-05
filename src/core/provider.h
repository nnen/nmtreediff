#pragma once

// The one interface a format has to implement.
//
// It is deliberately free of templates and of any GUI type, so that the Lua
// bridge planned for later is a plain subclass rather than a redesign. Nothing
// above this interface knows that XML has attributes or that JSON has arrays.

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "core/source.h"
#include "core/tree.h"

namespace nmxd {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    friend bool operator==(const Color&, const Color&) = default;
};

// What the node view puts on a card. Diff status is applied on top of this, so
// a provider palette never hides whether a node changed.
struct NodeStyle {
    std::string title;
    std::string subtitle;
    Color accent;
    std::string icon;
};

// How the matcher decides two nodes are the same node.
//
// A strong key is matched across arbitrary distance, before any structural
// heuristic runs: that is what makes a behavior-tree node with a stable
// identifier follow its move. A weak key is only a hint.
struct IdentityKey {
    bool strong = false;
    std::string value;
};

enum class ParseError {
    NotWellFormed,
    UnsupportedEncoding,
    Empty,
    Cancelled,
};

[[nodiscard]] const char* describe(ParseError error) noexcept;

enum class SerializeError {
    NotSupported,
};

class IFormatProvider {
public:
    virtual ~IFormatProvider() = default;

    // Stable identifier, the one the --format option takes.
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual std::string_view displayName() const = 0;

    // The file extensions this format claims by default, lower case and
    // including the leading dot. Declared rather than buried in scoring logic,
    // so the registry can list what handles what and a user can see why a file
    // resolved the way it did.
    [[nodiscard]] virtual std::span<const std::string_view> defaultExtensions() const = 0;

    // Ranked sniffing for everything the extensions do not settle: a cheap look
    // at the head of the file. Zero means the provider does not recognise it at
    // all. The registry picks the highest scorer, and an explicit --format
    // always beats both.
    [[nodiscard]] virtual int score(const SourceFile& source) const = 0;

    // True when this provider claims the file by extension alone.
    [[nodiscard]] bool claimsExtension(const SourceFile& source) const;

    // Called on a worker; must not touch shared mutable state. Long loops are
    // expected to check the token.
    [[nodiscard]] virtual Result<Tree, ParseError> parse(const SourceFile& source,
                                                         std::stop_token token) const = 0;

    [[nodiscard]] virtual IdentityKey identity(const Tree& tree, NodeId id) const = 0;

    [[nodiscard]] virtual NodeStyle style(const Tree& tree, NodeId id) const = 0;

    // Display order for a node's properties: lower ranks sort first, and equal
    // ranks keep document order. Ordering is presentation only. Matching
    // compares properties as an unordered set, so reordering an attribute list
    // never registers as a change.
    [[nodiscard]] virtual int propertyRank(const Tree& tree, NodeId id,
                                           std::string_view propertyName) const {
        (void)tree;
        (void)id;
        (void)propertyName;
        return 0;
    }

    // Ordered children mean sibling position is meaningful, so a reordering is
    // a move. Unordered means position carries nothing and reordering is not a
    // change at all. XML elements are ordered; JSON object members are not.
    [[nodiscard]] virtual bool childrenOrdered(const Tree& tree, NodeId id) const {
        (void)tree;
        (void)id;
        return true;
    }

    // Reserved for the merge milestone. Declared now so that providers are
    // written with round-tripping in mind rather than discovering later that
    // they threw away what a merge needs.
    [[nodiscard]] virtual Result<std::string, SerializeError> serialize(const Tree& tree) const {
        (void)tree;
        return fail(SerializeError::NotSupported);
    }
};

// Helper for the common case: a fixed leading order, everything else trailing
// in document order. Names not in the list rank after every name that is.
[[nodiscard]] int rankFromList(std::span<const std::string_view> order, std::string_view name);

// Returns the node's properties as indices, sorted for display by the
// provider's ranking. Stable, so equal ranks keep document order.
[[nodiscard]] std::vector<std::uint32_t> propertyDisplayOrder(const IFormatProvider& provider,
                                                              const Tree& tree, NodeId id);

}  // namespace nmxd
