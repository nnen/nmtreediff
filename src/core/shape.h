#pragma once

/// \file
/// \brief The shaping layer: a parser reports elements as it meets them, a
///        shaper decides what each one means, and a builder records the answer.

#include <any>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/source.h"
#include "core/tree.h"

namespace nmxd {

class Builder;
class EnterControl;
class ShapeSession;

/// \brief A node a shaper has produced but the arena has not received yet.
///
/// \remarks Nodes are staged rather than added to the Tree straight away,
///          because a shaper decides what an element is when the element
///          ends, and by then its children have already been decided. The
///          arena wants a parent before its children, so the staged tree is
///          written out in one pass once the document is finished.
struct PendingNode {
    /// \brief What kind of node this is.
    std::string kind;
    /// \brief The node's properties, in the order they were added.
    std::vector<Property> properties;
    /// \brief The node's children, in the order they were adopted.
    std::vector<PendingNode> children;
    /// \brief The node's extent in the source bytes.
    SourceSpan span;
    /// \brief What the shaper said about the node, if anything.
    NodeAnnotation annotation;
    /// \brief Whether \ref annotation carries anything worth keeping.
    bool annotated = false;

    /// \brief Makes an empty node.
    PendingNode() = default;
    /// \brief Copies a node and everything under it.
    PendingNode(const PendingNode&) = default;
    /// \brief Takes over a node and everything under it.
    PendingNode(PendingNode&&) noexcept = default;
    /// \brief Copies a node and everything under it.
    ///
    /// \returns This node.
    PendingNode& operator=(const PendingNode&) = default;
    /// \brief Takes over a node and everything under it.
    ///
    /// \returns This node.
    PendingNode& operator=(PendingNode&&) noexcept = default;

    /// \brief Destroys the node and everything under it.
    ///
    /// \remarks Written out because the implicit destructor recurses once
    ///          per level, and a staged tree is as deep as the document. The
    ///          children are taken out and destroyed from an explicit stack
    ///          instead, so a deep document cannot overflow anything here.
    ~PendingNode();
};

/// \brief One thing a shaper produced for an element: a node or a property.
///
/// \remarks An element's children are handed to the shaper as a list of
///          these, already decided, so a parent can adopt them, forward them
///          or reshape them without knowing how they were made.
struct Item {
    /// \brief The node or the property.
    std::variant<PendingNode, Property> value;

    /// \brief Reports whether this item is a node.
    ///
    /// \returns `true` for a node, `false` for a property.
    [[nodiscard]] bool isNode() const noexcept { return value.index() == 0; }

    /// \brief Accesses the node this item holds.
    ///
    /// \returns The node. Undefined when isNode() is `false`.
    [[nodiscard]] PendingNode& node() { return std::get<0>(value); }

    /// \copydoc node()
    [[nodiscard]] const PendingNode& node() const { return std::get<0>(value); }

    /// \brief Accesses the property this item holds.
    ///
    /// \returns The property. Undefined when isNode() is `true`.
    [[nodiscard]] Property& property() { return std::get<1>(value); }

    /// \copydoc property()
    [[nodiscard]] const Property& property() const { return std::get<1>(value); }
};

/// \brief The items produced for one element's children, in document order.
using Items = std::vector<Item>;

/// \brief Turns a staged node into a property with parts.
///
/// \param node The node, consumed.
///
/// \returns A property named after the node's kind, whose parts are the
///          node's properties and, turned the same way, its children.
///
/// \remarks What happens when a node is adopted as a part: a record, because
///          the parts are named. Recursive over the node's children.
[[nodiscard]] Property propertyFromNode(PendingNode&& node);

/// \brief Folds a property with exactly one plain part down to a value.
///
/// \param property The property to fold.
///
/// \remarks An element with one attribute and nothing else reads as a value
///          rather than as a record with one field, which keeps the common
///          case a single line. A property with no parts, or with a part that
///          has parts of its own, is left alone.
void collapseSinglePart(Property& property);

/// \brief Names an attribute not to copy.
using AttributeNames = std::initializer_list<std::string_view>;

/// \brief How an element and everything inside it is handled.
enum class ShapeMode {
    Shaped,   ///< The shaper is asked about it.
    Default,  ///< The default treatment applies and the shaper is not asked.
    Opaque,   ///< It becomes one property holding its raw text; nothing inside is visited.
};

/// \brief One element of the source document, as the shaper sees it.
///
/// \remarks The same object is handed to enter() and to exit(). At enter only
///          the name, the attributes, where the element starts and its
///          ancestors are known. At exit the text, the full span and the items
///          made from its children are there too. Ancestors are always
///          reachable, because they were entered before this element was.
///
///          A reference to an element is valid only while a callback about it
///          or about one of its descendants is running. Nothing may be kept
///          past that.
class Element {
public:
    /// \brief Returns the element's name.
    ///
    /// \returns The tag name for XML, or whatever the source calls it.
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    /// \brief Returns the element's attributes in document order.
    ///
    /// \returns One property per attribute, each with its own span.
    [[nodiscard]] const std::vector<Property>& attributes() const noexcept { return attributes_; }

    /// \brief Finds an attribute by name.
    ///
    /// \param name The attribute name.
    ///
    /// \returns The attribute, or `nullptr` when there is none of that name.
    [[nodiscard]] const Property* attribute(std::string_view name) const noexcept;

    /// \brief Returns an attribute's value.
    ///
    /// \param name The attribute name.
    ///
    /// \returns The value, or an empty view when the attribute is absent.
    [[nodiscard]] std::string_view attributeValue(std::string_view name) const noexcept;

    /// \brief Returns the element's text content.
    ///
    /// \returns The text, or an empty view for an element that has none or
    ///          that also holds elements. Empty until the element is closed.
    [[nodiscard]] std::string_view text() const noexcept { return text_; }

    /// \brief Returns where the text content sits in the source bytes.
    ///
    /// \returns The span, empty when there is no text.
    [[nodiscard]] SourceSpan textSpan() const noexcept { return textSpan_; }

    /// \brief Returns the element's extent in the source bytes.
    ///
    /// \returns At enter, the start tag. At exit, the whole element.
    [[nodiscard]] SourceSpan span() const noexcept { return span_; }

    /// \brief Returns the element's depth.
    ///
    /// \returns Zero for the document element.
    [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }

    /// \brief Returns the element's position among its parent's elements.
    ///
    /// \returns Zero for the first element inside a parent.
    [[nodiscard]] std::uint32_t index() const noexcept { return index_; }

    /// \brief Returns how many elements this one holds directly.
    ///
    /// \returns The count, complete only once the element is closed.
    [[nodiscard]] std::uint32_t childCount() const noexcept { return childCount_; }

    /// \brief Returns where the last element inside this one ended.
    ///
    /// \returns The offset just past it, or zero when there was none.
    [[nodiscard]] std::uint32_t lastChildEnd() const noexcept { return lastChildEnd_; }

    /// \brief Reports whether the element has been closed.
    ///
    /// \returns `true` inside exit(), `false` inside enter().
    [[nodiscard]] bool closed() const noexcept { return closed_; }

    /// \brief Returns the element this one sits inside.
    ///
    /// \returns The parent, or `nullptr` for the document element.
    [[nodiscard]] Element* parent() noexcept { return parent_; }

    /// \copydoc parent()
    [[nodiscard]] const Element* parent() const noexcept { return parent_; }

    /// \brief Finds the nearest ancestor with a given name.
    ///
    /// \param name The name to look for.
    ///
    /// \returns The ancestor, or `nullptr` when no ancestor has that name.
    [[nodiscard]] Element* ancestor(std::string_view name) noexcept;

    /// \copydoc ancestor(std::string_view)
    [[nodiscard]] const Element* ancestor(std::string_view name) const noexcept;

    /// \brief Returns the items made from the elements inside this one.
    ///
    /// \returns The items, in document order. Empty until the element is
    ///          closed, and empty again once they have been adopted or
    ///          forwarded.
    [[nodiscard]] Items& items() noexcept { return items_; }

    /// \brief Takes the items made from the elements inside this one.
    ///
    /// \returns The items, leaving none behind.
    [[nodiscard]] Items takeItems() noexcept;

    /// \brief Returns what a shaper left on this element at enter.
    ///
    /// \returns The value, which descendants read through parent() and
    ///          ancestor(). Empty until something is stored.
    [[nodiscard]] std::any& data() noexcept { return data_; }

    /// \copydoc data()
    [[nodiscard]] const std::any& data() const noexcept { return data_; }

    /// \brief Returns a slot a language binding may keep working in.
    ///
    /// \returns The slot. Shapers use data() instead.
    [[nodiscard]] std::any& cache() noexcept { return cache_; }

    /// \brief Returns what the parser knows about this element beyond its
    ///        name, attributes and text.
    ///
    /// \returns The slot, filled by the driver for its own default treatment
    ///          and empty otherwise.
    ///
    /// \remarks The JSON walker records here whether an array holds an object
    ///          anywhere inside it, which is what decides whether the array
    ///          reads as a list of values or as a list of nodes. A fact like
    ///          that has no place in the element's attributes, because an
    ///          attribute is something the file said.
    [[nodiscard]] std::any& source() noexcept { return source_; }

    /// \copydoc source()
    [[nodiscard]] const std::any& source() const noexcept { return source_; }

    /// \brief Returns how this element is being handled.
    ///
    /// \returns The mode, which descendants inherit.
    [[nodiscard]] ShapeMode mode() const noexcept { return mode_; }

    /// \brief Returns a number that identifies this element within one parse.
    ///
    /// \returns The serial, which no other element of the same parse shares.
    [[nodiscard]] std::uint64_t serial() const noexcept { return serial_; }

private:
    friend class EnterControl;
    friend class ShapeSession;

    std::string name_;
    std::vector<Property> attributes_;
    std::string text_;
    SourceSpan textSpan_;
    SourceSpan span_;
    std::uint32_t depth_ = 0;
    std::uint32_t index_ = 0;
    std::uint32_t childCount_ = 0;
    std::uint32_t lastChildEnd_ = 0;
    bool closed_ = false;
    Element* parent_ = nullptr;
    Items items_;
    std::any data_;
    std::any cache_;
    std::any source_;
    ShapeMode mode_ = ShapeMode::Shaped;
    std::uint64_t serial_ = 0;
};

/// \brief What a shaper may decide when an element is entered.
///
/// \remarks Enter never emits anything. It steers: it can leave a value for
///          descendants on the element, and it can take the element and
///          everything inside it out of the shaper's hands. Emitting is
///          exit's job, where the children are already decided.
class EnterControl {
public:
    /// \brief Hands this element and everything inside it to the default
    ///        treatment.
    ///
    /// \remarks No further callback is made for the subtree, including this
    ///          element's own exit. Nothing is dropped: the default keeps
    ///          every element, attribute and text.
    void useDefault() noexcept { element_.mode_ = ShapeMode::Default; }

    /// \brief Keeps this element as one property holding its raw text.
    ///
    /// \remarks Nothing inside it is visited, and no further callback is made
    ///          for it. The property is named after the element and spans it.
    void opaque() noexcept { element_.mode_ = ShapeMode::Opaque; }

    /// \brief Returns the element being entered.
    ///
    /// \returns The element, whose data() may be set here.
    [[nodiscard]] Element& element() noexcept { return element_; }

private:
    friend class ShapeSession;
    explicit EnterControl(Element& element) : element_(element) {}
    Element& element_;
};

/// \brief A handle to a node the builder has emitted, for filling it in.
///
/// \remarks Valid only while the exit() call that made it is running.
class NodeBuilder {
public:
    /// \brief Adds every attribute of an element as a property.
    ///
    /// \param element The element whose attributes to take.
    /// \param except Attribute names to leave out.
    ///
    /// \returns This handle.
    NodeBuilder& attributes(const Element& element, AttributeNames except = {});

    /// \brief Adds an element's text content as the property named
    ///        kTextProperty.
    ///
    /// \param element The element whose text to take.
    ///
    /// \returns This handle. Nothing is added when there is no text.
    NodeBuilder& text(const Element& element);

    /// \brief Adds a property.
    ///
    /// \param name The property name.
    /// \param value The property value.
    /// \param span Where it sits in the source, or empty for none.
    ///
    /// \returns This handle.
    NodeBuilder& property(std::string name, std::string value, SourceSpan span = {});

    /// \brief Adds a property that already has its parts.
    ///
    /// \param property The property.
    ///
    /// \returns This handle.
    NodeBuilder& property(Property property);

    /// \brief Takes items in: nodes become children, properties become
    ///        properties.
    ///
    /// \param items The items, consumed.
    ///
    /// \returns This handle.
    NodeBuilder& adopt(Items&& items);

    /// \brief Takes one item in.
    ///
    /// \param item The item, consumed.
    ///
    /// \returns This handle.
    NodeBuilder& adopt(Item&& item);

    /// \brief Changes the node's kind.
    ///
    /// \param kind The new kind.
    ///
    /// \returns This handle.
    NodeBuilder& kind(std::string kind);

    /// \brief Records what makes this node the same node across versions.
    ///
    /// \param value The key. Empty means none.
    /// \param strong Whether the key may be matched across any distance.
    ///
    /// \returns This handle.
    NodeBuilder& identity(std::string value, bool strong);

    /// \brief Records how the node's card is titled.
    ///
    /// \param title The first line.
    /// \param subtitle The second line, or empty for none.
    ///
    /// \returns This handle.
    NodeBuilder& title(std::string title, std::string subtitle = {});

    /// \brief Records whether the order of the node's children carries
    ///        meaning.
    ///
    /// \param ordered `true` when reordering children is a change.
    ///
    /// \returns This handle.
    NodeBuilder& orderedChildren(bool ordered);

    /// \brief Accesses the node being built.
    ///
    /// \returns The staged node.
    [[nodiscard]] PendingNode& pending();

private:
    friend class Builder;
    NodeBuilder(Items& target, std::size_t index) : target_(&target), index_(index) {}
    Items* target_;
    std::size_t index_;
};

/// \brief A handle to a property the builder has emitted, for giving it
///        parts.
///
/// \remarks Valid only while the exit() call that made it is running. A
///          property with no parts is a value; one with parts is a record,
///          or a sequence once ordered() says so.
class PropertyBuilder {
public:
    /// \brief Sets the property's value.
    ///
    /// \param value The value.
    ///
    /// \returns This handle.
    PropertyBuilder& value(std::string value);

    /// \brief Adds a part.
    ///
    /// \param name The part's name.
    /// \param value The part's value.
    /// \param span Where it sits in the source, or empty for none.
    ///
    /// \returns This handle.
    PropertyBuilder& part(std::string name, std::string value, SourceSpan span = {});

    /// \brief Adds a part that already has parts of its own.
    ///
    /// \param part The part.
    ///
    /// \returns This handle.
    PropertyBuilder& part(Property part);

    /// \brief Adds every attribute of an element as a part.
    ///
    /// \param element The element whose attributes to take.
    /// \param except Attribute names to leave out.
    ///
    /// \returns This handle.
    PropertyBuilder& attributes(const Element& element, AttributeNames except = {});

    /// \brief Takes items in as parts.
    ///
    /// \param items The items, consumed. A node among them becomes a part
    ///        the way propertyFromNode() makes one.
    ///
    /// \returns This handle.
    PropertyBuilder& adopt(Items&& items);

    /// \brief Says whether the parts are a sequence rather than a record.
    ///
    /// \param ordered `true` when reordering the parts is a change.
    ///
    /// \returns This handle.
    PropertyBuilder& ordered(bool ordered);

    /// \brief Folds a single plain part down to the value.
    ///
    /// \returns This handle.
    ///
    /// \remarks See collapseSinglePart().
    PropertyBuilder& collapse();

    /// \brief Accesses the property being built.
    ///
    /// \returns The staged property.
    [[nodiscard]] Property& pending();

private:
    friend class Builder;
    PropertyBuilder(Items& target, std::size_t index) : target_(&target), index_(index) {}
    Items* target_;
    std::size_t index_;
};

/// \brief What exit() emits into.
///
/// \remarks Everything emitted lands in the list the parent element will see
///          as its items. Items the shaper neither adopts nor forwards are
///          forwarded for it once exit() returns, and an exit() that emits
///          nothing at all gets the default treatment, so the only way to
///          lose content is to say so with drop().
class Builder {
public:
    /// \brief Emits a node.
    ///
    /// \param kind What kind of node it is.
    /// \param source The element it stands for, whose span it takes.
    ///
    /// \returns A handle for filling the node in.
    NodeBuilder node(std::string kind, const Element& source);

    /// \brief Emits a node with an explicit span.
    ///
    /// \param kind What kind of node it is.
    /// \param span Where it sits in the source, or empty for none.
    ///
    /// \returns A handle for filling the node in.
    NodeBuilder node(std::string kind, SourceSpan span);

    /// \brief Emits a property.
    ///
    /// \param name The property name.
    /// \param value The property value.
    /// \param source The element it came from, whose span it takes.
    ///
    /// \returns A handle for giving the property parts.
    PropertyBuilder property(std::string name, std::string value, const Element& source);

    /// \brief Emits a property with an explicit span.
    ///
    /// \param name The property name.
    /// \param value The property value.
    /// \param span Where it sits in the source, or empty for none.
    ///
    /// \returns A handle for giving the property parts.
    PropertyBuilder property(std::string name, std::string value, SourceSpan span = {});

    /// \brief Emits a property that already has its parts.
    ///
    /// \param property The property.
    ///
    /// \returns A handle for giving the property more parts.
    PropertyBuilder property(Property property);

    /// \brief Passes items up unchanged.
    ///
    /// \param items The items, consumed.
    ///
    /// \remarks This is what a wrapper element does: the nodes inside it
    ///          belong to the node above, and the wrapper itself keeps nothing.
    void forward(Items&& items);

    /// \brief Passes one item up unchanged.
    ///
    /// \param item The item, consumed.
    void forward(Item&& item);

    /// \brief Applies the default treatment to an element.
    ///
    /// \param element The element, whose items are consumed.
    ///
    /// \remarks The default keeps everything: the element becomes a node
    ///          named after itself, its attributes and text become properties,
    ///          and its items are adopted.
    void defaultTreatment(Element& element);

    /// \brief Keeps nothing of this element or of anything inside it.
    ///
    /// \remarks The one deliberate way to lose content. Items already made
    ///          from the children are discarded with it.
    void drop() noexcept { dropped_ = true; touched_ = true; }

    /// \brief Reports whether anything has been asked of this builder.
    ///
    /// \returns `true` once something was emitted, forwarded or dropped.
    [[nodiscard]] bool touched() const noexcept { return touched_; }

    /// \brief Reports whether drop() was called.
    ///
    /// \returns `true` when the element is to be discarded.
    [[nodiscard]] bool dropped() const noexcept { return dropped_; }

private:
    friend class ShapeSession;
    Builder(Items& target, ShapeSession& session) : target_(&target), session_(&session) {}
    Items* target_;
    ShapeSession* session_;
    bool touched_ = false;
    bool dropped_ = false;
};

/// \brief Decides what the elements of a document mean.
///
/// \remarks Implemented by a compiled format, by the bridge that runs a
///          script, and by the default treatment itself. A shaper is created
///          per parse and may keep state, but it must not touch anything
///          shared, because parses run on worker threads.
class IShaper {
public:
    /// \brief Destroys the shaper.
    virtual ~IShaper() = default;

    /// \brief Called once, before the first element, with the session that
    ///        will drive this shaper.
    ///
    /// \param session The session.
    ///
    /// \remarks For a shaper that hands elements to something outside C++
    ///          and needs to check, later, that what comes back still names
    ///          an open element. The default does nothing.
    virtual void attach(ShapeSession& session) { (void)session; }

    /// \brief Called when an element starts, before anything inside it.
    ///
    /// \param element The element, with its name, attributes and ancestors
    ///        known and nothing else.
    /// \param control Where to leave a value for descendants or to take the
    ///        subtree away from the shaper.
    ///
    /// \remarks Never emits. The default does nothing.
    virtual void enter(Element& element, EnterControl& control) {
        (void)element;
        (void)control;
    }

    /// \brief Called when an element ends, after everything inside it.
    ///
    /// \param element The element, complete, with the items made from its
    ///        children in items().
    /// \param out Where to emit what the element becomes.
    virtual void exit(Element& element, Builder& out) = 0;
};

/// \brief The treatment an element gets when nobody says otherwise.
///
/// \remarks Every element becomes a node named after itself, every attribute
///          and the text become properties, and every item made inside it is
///          adopted. This is what the generic XML format is, and what a
///          scripted format on XML gets for anything it does not mention. A
///          driver whose documents mean something else, JSON with its arrays
///          of values, brings a default of its own to the session.
class DefaultShaper final : public IShaper {
public:
    void exit(Element& element, Builder& out) override;
};

/// \brief Runs one document through a shaper and builds the tree.
///
/// \remarks A parser drives it: open() for each element as it starts, close()
///          as it ends, and finish() once the document is done. The session
///          keeps the stack of open elements, calls the shaper at the right
///          moments, stages what it emits, and writes the arena out at the
///          end. Parsers never touch the shaper and shapers never touch the
///          parser.
class ShapeSession {
public:
    /// \brief Prepares to shape one document.
    ///
    /// \param shaper The shaper to consult.
    /// \param fallback The default treatment, applied to an element the
    ///        shaper says nothing about or hands over.
    /// \param text The whole document, for raw slices.
    ShapeSession(IShaper& shaper, IShaper& fallback, std::string_view text);

    /// \brief Opens an element.
    ///
    /// \param name The element's name.
    /// \param attributes Its attributes, in document order, spans included.
    /// \param span Where its start tag sits.
    ///
    /// \returns The element, which stays valid until it is closed.
    Element& open(std::string name, std::vector<Property> attributes, SourceSpan span);

    /// \brief Reports whether the parser should visit what is inside the
    ///        innermost open element.
    ///
    /// \returns `false` when the element is opaque.
    [[nodiscard]] bool descend() const noexcept;

    /// \brief Closes the innermost open element.
    ///
    /// \param text Its text content, or empty for none.
    /// \param textSpan Where that text sits, or empty.
    /// \param spanEnd Where the whole element ends.
    void close(std::string text, SourceSpan textSpan, std::uint32_t spanEnd);

    /// \brief Returns the innermost open element.
    ///
    /// \returns The element, or `nullptr` when none is open.
    [[nodiscard]] Element* current() noexcept;

    /// \brief Returns how many elements are open.
    ///
    /// \returns The count.
    [[nodiscard]] std::uint32_t depth() const noexcept {
        return static_cast<std::uint32_t>(frames_.size());
    }

    /// \brief Looks an open element up by depth and serial.
    ///
    /// \param depth The element's depth.
    /// \param serial The element's serial.
    ///
    /// \returns The element, or `nullptr` when no open element matches both.
    ///
    /// \remarks What a language binding checks before touching an element a
    ///          script may have kept longer than it should.
    [[nodiscard]] Element* frameAt(std::uint32_t depth, std::uint64_t serial) noexcept;

    /// \brief Returns the default treatment.
    ///
    /// \returns The shaper that keeps everything.
    [[nodiscard]] IShaper& defaultShaper() noexcept { return fallback_; }

    /// \brief Turns what was produced into a tree.
    ///
    /// \param formatName The provider name to record on the tree.
    ///
    /// \returns The tree, neither finalised nor hashed.
    ///
    /// \remarks If the document element produced exactly one node, that is
    ///          the root. Otherwise a root named after the document element is
    ///          made and everything produced hangs from it, so a document
    ///          always has somewhere to hang.
    [[nodiscard]] Tree finish(std::string formatName);

private:
    /// \brief Runs the shaper, or the treatment the mode asks for, on an
    ///        element that has just been closed.
    ///
    /// \param element The element.
    void shapeClosed(Element& element);

    /// \brief Returns the list an element's output goes into.
    ///
    /// \param element The element.
    ///
    /// \returns Its parent's items, or the document's own list for the
    ///          document element.
    [[nodiscard]] Items& targetFor(Element& element) noexcept;

    IShaper& shaper_;
    IShaper& fallback_;
    std::string_view text_;
    std::deque<Element> frames_;
    Items rootItems_;
    std::string rootName_;
    SourceSpan rootSpan_;
    std::uint64_t nextSerial_ = 1;
};

/// \brief Writes a staged tree into an arena.
///
/// \param root The staged root, consumed.
/// \param formatName The provider name to record.
///
/// \returns The tree, in depth-first document order, neither finalised nor
///          hashed.
[[nodiscard]] Tree materialize(PendingNode&& root, std::string formatName);

}  // namespace nmxd
