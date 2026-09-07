#pragma once

/// \file
/// \brief The one interface a format has to implement.

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

/// \brief The version of the format provider interface this build publishes.
///
/// \remarks This is the studio-facing surface, in both languages: this
///          interface and the scripted form built on it. A provider written
///          against version 2 keeps working in every later version 2 build,
///          because changes to the interface are additive: a new method
///          arrives with a default implementation, and nothing already
///          declared changes shape or meaning. The number goes up only when
///          that promise is broken, which is a decision rather than an
///          accident. Version 2 is where the scripted form became the enter
///          and exit callbacks of IShaper; the C++ interface did not change.
///
///          Documented in docs/PROVIDERS.md, which carries the same number.
inline constexpr int kProviderInterfaceVersion = 2;

/// \brief An opaque colour, with eight bits per channel.
struct Color {
    std::uint8_t r = 0;    ///< Red channel.
    std::uint8_t g = 0;    ///< Green channel.
    std::uint8_t b = 0;    ///< Blue channel.
    std::uint8_t a = 255;  ///< Alpha channel, opaque by default.

    /// \brief Compares two colours for equality.
    friend bool operator==(const Color&, const Color&) = default;
};

/// \brief How a node is presented in the node view.
///
/// \remarks Diff status is tinted on top of this, so a provider's palette never
///          hides whether a node changed.
struct NodeStyle {
    /// \brief The main line on the node card.
    std::string title;
    /// \brief An optional second line, usually a distinguishing property.
    std::string subtitle;
    /// \brief The provider's own colour, before diff status is applied.
    Color accent;
    /// \brief An optional glyph key.
    std::string icon;
};

/// \brief Which way a node graph runs.
///
/// \remarks Top down suits a wide, shallow tree. Left to right suits a deep
///          one, which is the shape a behaviour tree usually has. The choice is
///          the reader's, so a provider that has no opinion says so rather than
///          overruling it.
enum class GraphDirection {
    Inherit,      ///< No opinion; use whatever the reader chose.
    TopDown,      ///< Children below their parent.
    LeftToRight,  ///< Children to the right of their parent.
};

/// \brief How the matcher decides two nodes are the same node.
struct IdentityKey {
    /// \brief Whether this key may be matched across arbitrary distance.
    ///
    /// \remarks A strong key is honoured before any structural heuristic runs,
    ///          which is what makes a behavior-tree node with a stable
    ///          identifier follow its move. A weak key is only a hint and the
    ///          matcher may ignore it.
    bool strong = false;

    /// \brief The key itself. An empty value never anchors anything.
    std::string value;
};

/// \brief Why a document could not be parsed.
enum class ParseError {
    NotWellFormed,        ///< The document does not parse.
    UnsupportedEncoding,  ///< The encoding is one this provider cannot read.
    Empty,                ///< The document holds no content.
    Cancelled,            ///< Parsing stopped because the token was signalled.
};

/// \brief Converts a parse error into a phrase suitable for a message.
///
/// \param error The error to describe.
///
/// \returns A short sentence fragment, never null.
[[nodiscard]] const char* describe(ParseError error) noexcept;

/// \brief Why a tree could not be written back out.
enum class SerializeError {
    NotSupported,  ///< This provider cannot serialise yet.
};

/// \brief Turns a source file into a tree, and tells the views how to present
///        it.
///
/// \remarks Deliberately free of templates and of any GUI type, so that the Lua
///          bridge planned for later is a plain subclass rather than a
///          redesign. Nothing above this interface knows that XML has
///          attributes or that JSON has arrays.
///
///          Implementations must be stateless and safe to call from several
///          threads at once, because parsing runs on a worker pool.
class IFormatProvider {
public:
    /// \brief Destroys the provider.
    virtual ~IFormatProvider() = default;

    /// \brief Returns the stable identifier for this format.
    ///
    /// \returns The name the `--format` option accepts.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// \brief Returns the name shown to a person.
    ///
    /// \returns A human-readable format name.
    [[nodiscard]] virtual std::string_view displayName() const = 0;

    /// \brief Returns the file extensions this format claims by default.
    ///
    /// \returns Extensions in lower case, each including its leading dot.
    ///
    /// \remarks Declared rather than buried in scoring logic, so the registry
    ///          can list what handles what and a user can see why a file
    ///          resolved the way it did. An explicit `--format` always wins over
    ///          an extension.
    [[nodiscard]] virtual std::span<const std::string_view> defaultExtensions() const = 0;

    /// \brief Scores how well this provider recognises a file.
    ///
    /// \param source The file being resolved.
    ///
    /// \returns A score, where zero means the provider does not recognise the
    ///          file at all and higher is a better match.
    ///
    /// \remarks Handles whatever the extensions do not settle, usually by a
    ///          cheap look at the head of the file. The registry picks the
    ///          highest scorer.
    [[nodiscard]] virtual int score(const SourceFile& source) const = 0;

    /// \brief Reports whether this provider claims a file by extension alone.
    ///
    /// \param source The file being resolved.
    ///
    /// \returns `true` when the file's extension appears in
    ///          defaultExtensions().
    [[nodiscard]] bool claimsExtension(const SourceFile& source) const;

    /// \brief Parses a source file into a tree.
    ///
    /// \param source The file to parse.
    /// \param token Checked periodically; parsing gives up when a stop is
    ///        requested.
    ///
    /// \returns The parsed tree, finalised and hashed, or a ParseError.
    ///
    /// \remarks Called on a worker thread and must not touch shared mutable
    ///          state. This is where a provider collapses format detail: a
    ///          behavior-tree format builds a tree holding only its node
    ///          elements and folds the rest into properties, and nothing above
    ///          learns that this happened.
    [[nodiscard]] virtual Result<Tree, ParseError> parse(const SourceFile& source,
                                                         std::stop_token token) const = 0;

    /// \brief Returns the identity key for one node.
    ///
    /// \param tree The tree the node belongs to.
    /// \param id The node to key.
    ///
    /// \returns The key, which may be strong or only a hint.
    [[nodiscard]] virtual IdentityKey identity(const Tree& tree, NodeId id) const = 0;

    /// \brief Returns how one node should be presented.
    ///
    /// \param tree The tree the node belongs to.
    /// \param id The node to style.
    ///
    /// \returns The title, subtitle, colour and icon for the node.
    ///
    /// \remarks Must be deterministic: the two sides of a diff style their
    ///          nodes independently and have to agree.
    [[nodiscard]] virtual NodeStyle style(const Tree& tree, NodeId id) const = 0;

    /// \brief Returns the display rank of one property.
    ///
    /// \param tree The tree the node belongs to.
    /// \param id The node the property belongs to.
    /// \param propertyName The property to rank.
    ///
    /// \returns A rank, where lower sorts first. Equal ranks keep document
    ///          order.
    ///
    /// \remarks Ordering is presentation only. Matching compares properties as
    ///          an unordered set, so reordering an attribute list never
    ///          registers as a change. The default ranks everything equally.
    [[nodiscard]] virtual int propertyRank(const Tree& tree, NodeId id,
                                           std::string_view propertyName) const {
        (void)tree;
        (void)id;
        (void)propertyName;
        return 0;
    }

    /// \brief Reports whether a node's children have a meaningful order.
    ///
    /// \param tree The tree the node belongs to.
    /// \param id The parent node.
    ///
    /// \returns `true` when sibling position carries meaning.
    ///
    /// \remarks Ordered children mean a reordering is a move. Unordered means
    ///          position carries nothing and a reordering is not a change at
    ///          all. XML elements are ordered; JSON object members are not. The
    ///          default is ordered.
    [[nodiscard]] virtual bool childrenOrdered(const Tree& tree, NodeId id) const {
        (void)tree;
        (void)id;
        return true;
    }

    /// \brief Returns the direction this format's graph reads best in.
    ///
    /// \returns A direction, or GraphDirection::Inherit to accept the
    ///          reader's choice.
    ///
    /// \remarks Returning Inherit is not the same as returning TopDown. A
    ///          provider with no opinion must not overrule a reader who has one,
    ///          so the default answers Inherit and only a provider that really
    ///          knows its shape names a direction.
    [[nodiscard]] virtual GraphDirection graphDirection() const { return GraphDirection::Inherit; }

    /// \brief Writes a tree back out in this format.
    ///
    /// \param tree The tree to serialise.
    ///
    /// \returns The serialised document, or SerializeError::NotSupported.
    ///
    /// \remarks Reserved for the merge milestone and unimplemented today.
    ///          Declared now so that providers are written with round-tripping
    ///          in mind rather than discovering later that they threw away what
    ///          a merge needs.
    [[nodiscard]] virtual Result<std::string, SerializeError> serialize(const Tree& tree) const {
        (void)tree;
        return fail(SerializeError::NotSupported);
    }
};

/// \brief Settles which direction a graph is drawn in.
///
/// \param provider The provider whose opinion to ask.
/// \param fallback What to use when the provider has none.
///
/// \returns The provider's direction, or \p fallback when it returns
///          GraphDirection::Inherit.
///
/// \remarks Resolved once, where both answers are known, so that nothing
///          further down has to remember which of the two wins.
[[nodiscard]] GraphDirection resolveDirection(const IFormatProvider& provider,
                                              GraphDirection fallback);

/// \brief Ranks a name against a fixed leading order.
///
/// \param order The names that sort first, in the order they should appear.
/// \param name The name to rank.
///
/// \returns The position of \p name in \p order, or one past the end when it
///          does not appear.
///
/// \remarks Covers the common case for propertyRank(): a fixed leading order
///          with everything else trailing in document order.
[[nodiscard]] int rankFromList(std::span<const std::string_view> order, std::string_view name);

/// \brief Sorts a node's properties for display.
///
/// \param provider The provider whose ranking to apply.
/// \param tree The tree the node belongs to.
/// \param id The node whose properties to order.
///
/// \returns Indices into the node's property list, in display order. The sort
///          is stable, so equal ranks keep document order.
[[nodiscard]] std::vector<std::uint32_t> propertyDisplayOrder(const IFormatProvider& provider,
                                                              const Tree& tree, NodeId id);

}  // namespace nmxd
