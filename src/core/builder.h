#pragma once

/// \file
/// \brief Builds a Tree through handles, in any order, without recursion.

#include <cstdint>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/provider.h"
#include "core/result.h"
#include "core/source.h"
#include "core/tree.h"

namespace nmtreediff {

class DomNode;
class DomProperty;
class TreeBuilder;

/// \brief Identifies an element of the document a provider read, before it
///        was shaped.
///
/// \remarks The same number as the NodeId of that element in the tree
///          `read()` produced, because the DOM is a view over that tree. Kept
///          as its own name so that a builder handle's source and a shaped
///          node's id cannot be confused for one another.
using DomId = std::uint32_t;

/// \brief The id that means "no source element".
inline constexpr DomId kInvalidDom = 0xFFFFFFFFu;

/// \brief What a builder handle stands on.
enum class RefKind : std::uint8_t {
    Node,      ///< A node of the tree being built.
    Property,  ///< A property of a node, or a part of a property.
};

/// \brief How far an identity key reaches.
enum class Identity : std::uint8_t {
    Weak,    ///< A hint the matcher may ignore.
    Strong,  ///< Honoured before any structural heuristic runs.
};

/// \brief Names one node or property inside a TreeBuilder.
///
/// \remarks Small and trivially copyable, so a job queued for later can carry
///          one and rehydrate it through TreeBuilder::at().
struct RefId {
    /// \brief The index that means "no node or property".
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

    /// \brief Index into the builder's node or property arena.
    std::uint32_t index = kNone;
    /// \brief Which arena.
    RefKind kind = RefKind::Node;

    /// \brief Compares two ids for equality.
    friend bool operator==(const RefId&, const RefId&) = default;
};

/// \brief Raised when a builder is asked for something that cannot be built.
///
/// \remarks A child with no kind under a node, a value on a node, an item
///          outside a sequence. These are bugs in a provider rather than
///          facts about a file, so they are exceptions rather than parse
///          errors. The shaping drain catches one at the job boundary and
///          records it as a failure against the job's element.
class BuildError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// \brief A handle on one node or property being built.
///
/// \remarks One type for both, deliberately. A caller holding a handle does
///          not track whether it stands on a node or inside a property,
///          because child() does the right thing wherever it stands: a node
///          under a node, a named part under a record, an unnamed item under a
///          sequence. The property() family always makes a property, wherever
///          it is called.
///
///          Handles stay valid for the life of the builder. Nothing lives in
///          a nested vector until finish() assembles the tree.
class Ref {
public:
    /// \brief Makes a handle on nothing.
    Ref() = default;

    /// \brief Reports whether this handle names anything.
    ///
    /// \returns `true` when the handle came from a builder.
    [[nodiscard]] bool valid() const noexcept { return builder_ != nullptr; }

    /// \brief Returns what the handle stands on.
    ///
    /// \returns Node, or one of the property kinds.
    [[nodiscard]] RefKind kind() const noexcept { return id_.kind; }

    /// \brief Reports whether the handle stands on a node.
    ///
    /// \returns `true` for a node, `false` for a property or a part.
    [[nodiscard]] bool isNode() const noexcept { return id_.kind == RefKind::Node; }

    /// \brief Returns the form of the property this handle stands on.
    ///
    /// \returns The form, or PropertyForm::Scalar for a node.
    [[nodiscard]] PropertyForm form() const;

    /// \brief Returns the id, for carrying across a queued job.
    ///
    /// \returns The id the builder issued, which at() turns back into a handle.
    [[nodiscard]] RefId id() const noexcept { return id_; }

    /// \brief Adds whatever a child is where this handle stands.
    ///
    /// \param name The child's kind under a node, or its name under a
    ///        property. Empty under a sequence for an unnamed item.
    ///
    /// \returns The new child.
    ///
    /// \throws BuildError under a node with an empty name, or under a record
    ///         with one.
    ///
    /// \remarks Under a scalar property, promotes it: to a record when a name
    ///          is given and to a sequence when none is. The scalar's value is
    ///          kept either way.
    Ref child(std::string_view name = {});

    /// \brief Adds a child made from a source element.
    ///
    /// \param element The element to represent.
    ///
    /// \returns The new child, named after the element, with its span, and
    ///          with the element recorded as its source.
    ///
    /// \remarks The short form for the common case, and what makes the
    ///          element count as represented rather than dropped.
    Ref child(const DomNode& element);

    /// \brief Adds a scalar property.
    ///
    /// \param name The property's name.
    /// \param value The property's value.
    ///
    /// \returns The new property.
    ///
    /// \remarks A property of a node, or a part of a property. Names may
    ///          repeat: calling this twice with one name makes two properties.
    Ref property(std::string_view name, std::string_view value = {});

    /// \brief Adds a copy of a source property, parts and all.
    ///
    /// \param source The property to copy.
    ///
    /// \returns The new property.
    ///
    /// \remarks Name, value, form, span and every part at every depth, copied
    ///          with a worklist rather than recursion.
    Ref property(const DomProperty& source);

    /// \brief Adds a property standing for a source element.
    ///
    /// \param element The element the property represents.
    ///
    /// \returns The new scalar property, named after the element, with its
    ///          span, and with the element recorded as its source. Its value
    ///          is empty and its parts are for the caller to add.
    ///
    /// \remarks What a format that folds an element into the node above
    ///          starts from.
    Ref property(const DomNode& element);

    /// \brief Adds a copy of a property value, parts and all.
    ///
    /// \param source The property to copy.
    ///
    /// \returns The new property.
    ///
    /// \remarks For a provider that assembled a property first and hands it
    ///          over whole. Copied with a worklist rather than recursion.
    Ref property(const Property& source);

    /// \brief Sets a property's form outright.
    ///
    /// \param form The form to give it.
    ///
    /// \returns This handle, for chaining.
    ///
    /// \throws BuildError on a node, or when making a property with parts a
    ///         scalar.
    ///
    /// \remarks child() promotes a scalar as parts arrive; this is for the
    ///          case with no parts to promote through, an empty array being
    ///          the usual one.
    Ref& setForm(PropertyForm form);

    /// \brief Adds a property that will hold named parts.
    ///
    /// \param name The property's name.
    ///
    /// \returns The new record.
    Ref record(std::string_view name);

    /// \brief Adds a property that will hold positional parts.
    ///
    /// \param name The property's name.
    ///
    /// \returns The new sequence.
    Ref sequence(std::string_view name);

    /// \brief Appends an unnamed item to a sequence.
    ///
    /// \param value The item's value.
    ///
    /// \returns The new item.
    ///
    /// \throws BuildError anywhere but on a sequence.
    Ref item(std::string_view value = {});

    /// \brief Returns the enclosing node or property.
    ///
    /// \returns The parent, or an invalid handle for the root.
    [[nodiscard]] Ref parent() const;

    /// \brief Returns the nearest enclosing node.
    ///
    /// \returns The node this handle stands on or inside.
    [[nodiscard]] Ref owner() const;

    /// \brief Sets the kind of a node or the name of a property.
    ///
    /// \param name The kind or name to give it.
    ///
    /// \returns This handle, for chaining.
    Ref& setName(std::string_view name);

    /// \brief Sets a property's value.
    ///
    /// \param value The value, in whatever form the property has.
    ///
    /// \returns This handle, for chaining.
    ///
    /// \throws BuildError on a node.
    Ref& setValue(std::string_view value);

    /// \brief Sets where the node or property sits in the source bytes.
    ///
    /// \param span The extent in the source file.
    ///
    /// \returns This handle, for chaining.
    Ref& setSpan(SourceSpan span);

    /// \brief Records which element of the source document this came from.
    ///
    /// \param element The source element.
    ///
    /// \returns This handle, for chaining.
    ///
    /// \remarks What makes the element count as represented. The overloads
    ///          taking a DOM element call this themselves.
    Ref& setSource(DomId element);

    /// \brief Says whether a node's children have a meaningful order.
    ///
    /// \param ordered `true` when reordering the children is a change.
    ///
    /// \returns This handle, for chaining.
    ///
    /// \throws BuildError on a property.
    Ref& setChildrenOrdered(bool ordered);

    /// \brief Sets what makes a node the same node across versions.
    ///
    /// \param value The key, or empty for none.
    /// \param strength How far it reaches.
    ///
    /// \returns This handle, for chaining.
    ///
    /// \remarks Ignored on a property.
    Ref& setIdentity(std::string_view value, Identity strength = Identity::Weak);

    /// \brief Sets a node card's title and subtitle.
    ///
    /// \param title The first line of the card.
    /// \param subtitle The second line, or empty for one line.
    ///
    /// \returns This handle, for chaining.
    ///
    /// \remarks Ignored on a property. An empty title leaves the kind in
    ///          place.
    Ref& setTitle(std::string_view title, std::string_view subtitle = {});

    /// \brief Sets a node's own colour as 0xRRGGBB.
    ///
    /// \param rgb The colour, red in the high byte.
    ///
    /// \returns This handle, for chaining.
    ///
    /// \remarks Ignored on a property. Zero leaves the colour derived from
    ///          the kind.
    Ref& setAccent(std::uint32_t rgb);

    /// \brief Says whether the node view may draw the node stacked on its
    ///        only child, as one block with no edge between them.
    ///
    /// \param stacked `true` for a node that decorates the one node under it.
    ///
    /// \returns This handle, for chaining.
    ///
    /// \remarks Ignored on a property. Presentation only; the view honours
    ///          it only while the node has exactly one child in the drawn
    ///          union, so a decorator whose child was replaced draws with
    ///          both children under it.
    Ref& setStacked(bool stacked = true);

private:
    friend class TreeBuilder;

    Ref(TreeBuilder* builder, RefId id) : builder_(builder), id_(id) {}

    TreeBuilder* builder_ = nullptr;
    RefId id_;
};

/// \brief Builds a Tree from handles, in whatever order the calls come.
///
/// \remarks Nodes and properties live in two flat arenas with parent links and
///          per-parent child lists while the tree is being built. Sibling order
///          is the order of child() and property() calls on one parent handle
///          and nothing else, so two jobs building under different parents
///          cannot affect each other and a breadth-first walk, a depth-first
///          walk and a hand-written recursion all build the same tree.
///
///          finish() renumbers the nodes into document order in one stack-based
///          pass and folds the property arena into the tree by walking it
///          backwards, so the tree's arena invariants hold and nothing
///          recursed on the way. A provider never learns those invariants
///          exist.
class TreeBuilder {
public:
    /// \brief Prepares an empty builder.
    ///
    /// \param formatName The provider name the finished tree records.
    /// \param token Checked by finish(), which gives up when a stop is
    ///        requested.
    explicit TreeBuilder(std::string formatName, std::stop_token token = {});

    /// \brief Creates the root node.
    ///
    /// \param kind What kind of node the root is.
    /// \param span Its extent in the source bytes.
    ///
    /// \returns The root.
    ///
    /// \throws BuildError when a root already exists.
    Ref root(std::string_view kind, SourceSpan span = {});

    /// \brief Creates the root node from a source element.
    ///
    /// \param element The element the root represents, usually the
    ///        document's own.
    ///
    /// \returns The root, named after the element, with its span, and with
    ///          the element recorded as its source.
    Ref root(const DomNode& element);

    /// \brief Rehydrates a handle from its id.
    ///
    /// \param id An id a handle gave out earlier.
    ///
    /// \returns The handle, or an invalid one for an id this builder never
    ///          issued.
    [[nodiscard]] Ref at(RefId id);

    /// \brief Returns the root handle.
    ///
    /// \returns The root, or an invalid handle before root() is called.
    [[nodiscard]] Ref rootRef();

    /// \brief Returns how many nodes have been created.
    ///
    /// \returns The node count so far.
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }

    /// \brief Returns the provider name the finished tree will record.
    ///
    /// \returns The name, as given to the constructor.
    [[nodiscard]] const std::string& formatName() const noexcept { return formatName_; }

    /// \brief Returns how many properties and parts have been created.
    ///
    /// \returns The count so far, parts at every depth included.
    [[nodiscard]] std::size_t propertyCount() const noexcept { return properties_.size(); }

    /// \brief Reports whether the stop token has been signalled.
    ///
    /// \returns `true` once a stop has been requested.
    [[nodiscard]] bool cancelled() const noexcept { return token_.stop_requested(); }

    /// \brief Returns the source elements that no handle was made from.
    ///
    /// \param sourceCount How many elements the source document had.
    ///
    /// \returns Every DomId below \p sourceCount that no handle recorded, in
    ///          ascending order.
    [[nodiscard]] std::vector<DomId> unrepresented(std::size_t sourceCount) const;

    /// \brief Reports whether a source element has a handle made from it.
    ///
    /// \param element The element to ask about.
    ///
    /// \returns `true` when some handle recorded it as its source, or
    ///          represent() was called for it.
    [[nodiscard]] bool represents(DomId element) const noexcept;

    /// \brief Marks a source element as represented.
    ///
    /// \param element The element to mark.
    ///
    /// \remarks Ref::setSource() does this for the element a handle came
    ///          from. A provider that folds a whole subtree into one property
    ///          marks the rest of that subtree here, since the property has
    ///          one source and the subtree has many.
    void represent(DomId element);

    /// \brief Records that a shaping job failed.
    ///
    /// \param owner The handle the job was building under, or none.
    /// \param span The element the job was working on, or empty.
    /// \param message What went wrong, in one line.
    /// \param detail The whole of what went wrong, when there was more than
    ///        the line; empty otherwise.
    ///
    /// \remarks Kept here rather than on the context, because the owner is a
    ///          builder handle and only finish() knows which node it became.
    void recordFailure(RefId owner, SourceSpan span, std::string message,
                       std::string detail = {});

    /// \brief Returns how many failures have been recorded.
    ///
    /// \returns The count of recordFailure() calls so far.
    [[nodiscard]] std::size_t failureCount() const noexcept { return failures_.size(); }

    /// \brief Records the source bytes no handle accounted for.
    ///
    /// \param spans Disjoint spans in ascending order, handed to the tree.
    void setUnrepresented(std::vector<SourceSpan> spans) { unrepresented_ = std::move(spans); }

    /// \brief Assembles the tree.
    ///
    /// \returns The tree, finalised and hashed, or ParseError::Empty when no
    ///          root was made, or ParseError::Cancelled when the token was
    ///          signalled.
    ///
    /// \remarks Consumes the builder's arenas. Every handle is invalid after
    ///          this call.
    [[nodiscard]] Result<Tree, ParseError> finish();

private:
    friend class Ref;

    /// \brief One node in the arena.
    struct BuiltNode {
        std::string kind;
        SourceSpan span;
        std::uint32_t parent = RefId::kNone;
        std::vector<std::uint32_t> children;    ///< Node indices, in call order.
        std::vector<std::uint32_t> properties;  ///< Property indices, in call order.
        NodeAnnotation annotation;
        bool annotated = false;
        bool childrenOrdered = true;
        DomId source = kInvalidDom;
    };

    /// \brief One property or part in the arena.
    struct BuiltProperty {
        std::string name;
        std::string value;
        PropertyForm form = PropertyForm::Scalar;
        SourceSpan span;
        RefId parent;                            ///< A node or a property.
        std::vector<std::uint32_t> parts;        ///< Property indices, in call order.
        DomId source = kInvalidDom;
    };

    [[nodiscard]] BuiltNode& node(RefId id);
    [[nodiscard]] BuiltProperty& property(RefId id);
    [[nodiscard]] bool holds(RefId id) const noexcept;

    /// \brief Appends a node under a parent node.
    RefId addNode(RefId parent, std::string_view kind);

    /// \brief Appends a property under a node or a property.
    RefId addProperty(RefId parent, std::string_view name, std::string_view value,
                      PropertyForm form);

    /// \brief Renumbers the nodes into document order and adds them to a tree.
    ///
    /// \returns For each arena index, the NodeId it became.
    std::vector<NodeId> placeNodes(Tree& tree) const;

    /// \brief Assembles every property from the arena and attaches it.
    void placeProperties(Tree& tree, const std::vector<NodeId>& placed);

    /// \brief A failure whose owner is still a builder handle.
    struct PendingFailure {
        RefId owner;
        SourceSpan span;
        std::string message;
        std::string detail;
    };

    std::string formatName_;
    std::stop_token token_;
    std::vector<BuiltNode> nodes_;
    std::vector<BuiltProperty> properties_;
    std::vector<bool> represented_;
    std::vector<PendingFailure> failures_;
    std::vector<SourceSpan> unrepresented_;
};

}  // namespace nmtreediff
