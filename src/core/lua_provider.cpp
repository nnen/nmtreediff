/// \file
/// \brief Implementation of the scripted format provider.

#include "core/lua_provider.h"

#include <algorithm>
#include <any>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "core/hash.h"
#include "core/lua_state.h"
#include "core/registry.h"
#include "core/shape.h"
#include "core/tree_shape.h"
#include "formats/xml_shape.h"

namespace nmxd {

namespace {

/// \brief Score returned when the file's extension is one the script claimed.
///
/// \remarks The same number generic XML uses for its own extensions. A scripted
///          format that wants to win over the format it is built on says so by
///          claiming the extension, not by outbidding it.
constexpr int kExtensionScore = 90;

/// \brief The function a script writes to be told an element has started.
constexpr const char* kEnter = "enter";
/// \brief The function a script writes to be told an element has ended.
constexpr const char* kExit = "exit";

/// \brief The function a script writes to say what counts as a node.
constexpr const char* kIsNode = "is_node";
/// \brief The function that says an element describes the node above it.
constexpr const char* kFoldIntoParent = "fold_into_parent";
/// \brief The function that names what sort of node this is.
constexpr const char* kKind = "kind";
/// \brief The function that says what makes a node the same node.
constexpr const char* kIdentity = "identity";
/// \brief The function that titles a node's card.
constexpr const char* kTitle = "title";

/// \brief The word a script returns to mean an identity may travel.
constexpr const char* kStrongWord = "strong";

/// \brief The attribute a folded element is named after, when it has one.
constexpr const char* kNameAttribute = "name";
/// \brief The attribute a folded element takes its value from.
constexpr const char* kValueAttribute = "value";

/// \brief The usertype a script sees an element as.
///
/// \remarks The usertype names are prefixed so that a script cannot mistake
///          them for something it declared, and so that an error message
///          names what it is about.
constexpr const char* kElementType = "nmxd.Element";
/// \brief The usertype a script sees an element's items as.
constexpr const char* kItemsType = "nmxd.Items";
/// \brief The usertype a script sees the builder as.
constexpr const char* kBuilderType = "nmxd.Builder";
/// \brief The usertype a script sees an emitted node as.
constexpr const char* kNodeType = "nmxd.Node";
/// \brief The usertype a script sees an emitted property as.
constexpr const char* kPropertyType = "nmxd.Property";
/// \brief The usertype a script sees the enter control as.
constexpr const char* kFrameType = "nmxd.Frame";

/// \brief Reads a string out of a protected call's result.
///
/// \param result The result.
/// \param index Which return value to read.
///
/// \returns The string, or nothing when that return value is not one.
[[nodiscard]] sol::optional<std::string> stringAt(const sol::protected_function_result& result,
                                                  int index) {
    if (!result.valid() || result.return_count() <= index ||
        result.get_type(index) != sol::type::string) {
        return sol::nullopt;
    }
    return result.get<std::string>(index);
}

// ------------------------------------------------------- the compatible form

/// \brief The five-question form, written as a shaper.
///
/// \remarks The form a script wrote against version 1: `is_node`,
///          `fold_into_parent`, `kind`, `identity` and `title`, each handed
///          a table with the element's name and attributes. It is kept as
///          the short way to say the common thing, and it is kept exact:
///          the tests hold it against the compiled behaviour-tree provider.
///
///          The two yes-or-no questions are asked at enter, because they
///          decide how everything below is read, and the three naming
///          questions at exit, where the element's text is known too.
class LegacyScriptShaper final : public IShaper {
public:
    /// \brief Prepares to shape with a script's table of functions.
    ///
    /// \param lua The interpreter the script is loaded in.
    /// \param shape The script's table.
    LegacyScriptShaper(sol::state& lua, sol::table shape) : lua_(lua), shape_(std::move(shape)) {}

    void enter(Element& element, EnterControl& control) override {
        (void)control;
        Answers answers;
        if (element.parent() == nullptr) {
            // The document's own root is always a node. A script decides what
            // is inside a document, not whether there is one.
            answers.isNode = true;
        } else if (!insideFold(element)) {
            // Under a folded element nothing is asked: everything there is
            // part of the property, whatever the script would have said.
            answers.isNode = ask(kIsNode, element, true);
            answers.fold = !answers.isNode && ask(kFoldIntoParent, element, false);
        }
        element.data() = answers;
    }

    void exit(Element& element, Builder& out) override {
        if (insideFold(element)) {
            out.property(deepProperty(element, std::string(element.name())));
            return;
        }

        const Answers answers = answersOf(element);
        if (answers.isNode) {
            shapeNode(element, out);
            return;
        }
        if (answers.fold) {
            out.property(deepProperty(element, propertyName(element)));
            return;
        }

        // Anything else keeps its own name and attributes, and whatever it
        // holds goes up to the node above, so a wrapper neither disappears
        // nor swallows the nodes it holds.
        out.property(shallowProperty(element));
        out.forward(element.takeItems());
    }

private:
    /// \brief What the script said about an element at enter.
    struct Answers {
        bool isNode = false;  ///< Whether the element is a node of its own.
        bool fold = false;    ///< Whether it folds into the node above.
    };

    /// \brief Reads the answers stored on an element.
    ///
    /// \param element The element.
    ///
    /// \returns The answers, or none when nothing was stored.
    [[nodiscard]] static Answers answersOf(const Element& element) {
        const Answers* answers = std::any_cast<Answers>(&element.data());
        return answers == nullptr ? Answers{} : *answers;
    }

    /// \brief Reports whether an element sits inside a folded element.
    ///
    /// \param element The element.
    ///
    /// \returns `true` when any ancestor folds into its parent.
    [[nodiscard]] static bool insideFold(const Element& element) {
        for (const Element* above = element.parent(); above != nullptr; above = above->parent()) {
            if (answersOf(*above).fold) {
                return true;
            }
        }
        return false;
    }

    /// \brief Turns an element into the table a script sees.
    ///
    /// \param element The element.
    ///
    /// \returns A table holding the element's name and its attributes by
    ///          name, with its text under kTextProperty once it is closed.
    [[nodiscard]] sol::table describe(const Element& element) {
        sol::table table = lua_.create_table();
        table["name"] = std::string(element.name());

        sol::table attributes = lua_.create_table();
        for (const Property& attribute : element.attributes()) {
            attributes[attribute.name] = attribute.value;
        }
        if (!element.text().empty()) {
            attributes[std::string(kTextProperty)] = std::string(element.text());
        }
        table["attr"] = attributes;
        return table;
    }

    /// \brief Calls one of the script's functions, if it wrote one.
    ///
    /// \param function The name of the function.
    /// \param element The element to describe to it.
    ///
    /// \returns The result, or nothing when the script did not supply the
    ///          function.
    [[nodiscard]] sol::optional<sol::protected_function_result> call(const char* function,
                                                                     const Element& element) {
        const sol::optional<sol::protected_function> callable = shape_[function];
        if (!callable) {
            return sol::nullopt;
        }
        return (*callable)(describe(element));
    }

    /// \brief Asks the script a yes-or-no question about an element.
    ///
    /// \param function The name of the function to call.
    /// \param element The element to ask about.
    /// \param whenAbsent What to answer when the script did not supply one.
    ///
    /// \returns The script's answer, or \p whenAbsent.
    [[nodiscard]] bool ask(const char* function, const Element& element, bool whenAbsent) {
        const auto result = call(function, element);
        if (!result || !result->valid() || result->get_type() == sol::type::nil) {
            return whenAbsent;
        }
        return result->get<bool>();
    }

    /// \brief Turns an element the script called a node into one.
    ///
    /// \param element The element, whose items are consumed.
    /// \param out Where to emit it.
    void shapeNode(Element& element, Builder& out) {
        std::string kind(element.name());
        if (const auto result = call(kKind, element)) {
            if (const auto named = stringAt(*result, 0)) {
                kind = *named;
            }
        }

        NodeBuilder node = out.node(std::move(kind), element).attributes(element).text(element);

        if (const auto result = call(kIdentity, element)) {
            if (const auto identity = stringAt(*result, 0)) {
                // A second return value naming strength, so the common case
                // stays one line and the strong case is deliberate.
                const auto strength = stringAt(*result, 1);
                node.identity(*identity, strength && *strength == kStrongWord);
            }
        }
        if (const auto result = call(kTitle, element)) {
            if (const auto title = stringAt(*result, 0)) {
                node.title(*title, stringAt(*result, 1).value_or(std::string{}));
            }
        }

        node.adopt(element.takeItems());
    }

    /// \brief Chooses what a folded element's property is called.
    ///
    /// \param element The element being folded.
    ///
    /// \returns The value of its `name` attribute where it has one, and the
    ///          element's own name otherwise.
    ///
    /// \remarks A format that writes `<property name="speed" .../>` means
    ///          the property to be called speed. One that writes
    ///          `<transform .../>` means it to be called transform.
    [[nodiscard]] static std::string propertyName(const Element& element) {
        const std::string_view named = element.attributeValue(kNameAttribute);
        return std::string(named.empty() ? element.name() : named);
    }

    /// \brief Turns an element and everything inside it into a property.
    ///
    /// \param element The element, whose items are consumed.
    /// \param name What to call the resulting property.
    ///
    /// \returns The property, with a part per attribute, the text, and each
    ///          element inside, which were turned into parts as they closed.
    ///
    /// \remarks A record rather than a sequence: these parts are named, so
    ///          their order carries nothing.
    [[nodiscard]] static Property deepProperty(Element& element, std::string name) {
        Property property;
        property.name = std::move(name);
        property.span = element.span();

        for (const Property& attribute : element.attributes()) {
            // The name and value attributes are the element's own bookkeeping
            // rather than part of what it describes.
            if (attribute.name == kNameAttribute || attribute.name == kValueAttribute) {
                continue;
            }
            property.children.push_back(attribute);
        }
        if (!element.text().empty()) {
            Property text;
            text.name = std::string(kTextProperty);
            text.value = std::string(element.text());
            text.span = element.textSpan();
            property.children.push_back(std::move(text));
        }
        for (Item& item : element.takeItems()) {
            if (!item.isNode()) {
                property.children.push_back(std::move(item.property()));
            }
        }

        if (property.children.empty()) {
            property.value = std::string(element.attributeValue(kValueAttribute));
        } else if (property.children.size() == 1 && !property.children.front().hasParts()) {
            // One attribute and nothing else is a value, not a record.
            property.value = property.children.front().value;
            property.children.clear();
        }
        return property;
    }

    /// \brief Keeps an element that is neither a node nor folded.
    ///
    /// \param element The element.
    ///
    /// \returns A property named after the element, holding its attributes
    ///          and text as parts, or as its value when there is one of them.
    ///
    /// \remarks Attributes only, deliberately. Whatever the element holds is
    ///          forwarded on its own account, so recording it here as well
    ///          would represent everything inside it twice.
    [[nodiscard]] static Property shallowProperty(const Element& element) {
        Property folded;
        folded.name = std::string(element.name());
        folded.span = element.span();
        for (const Property& attribute : element.attributes()) {
            folded.children.push_back(attribute);
        }
        if (!element.text().empty()) {
            Property text;
            text.name = std::string(kTextProperty);
            text.value = std::string(element.text());
            text.span = element.textSpan();
            folded.children.push_back(std::move(text));
        }
        if (folded.children.size() == 1) {
            folded.value = folded.children.front().value;
            folded.children.clear();
        }
        return folded;
    }

    sol::state& lua_;
    sol::table shape_;
};

// ------------------------------------------------------------ the new form

/// \brief What every handle a script holds points back to.
///
/// \remarks One per parse. A handle carries a pointer to this and enough to
///          check that what it names is still there, because a script may
///          keep a handle longer than the callback that made it, and touching
///          a closed element has to be an error rather than a crash.
struct ScriptContext {
    sol::state* lua = nullptr;         ///< The interpreter.
    ShapeSession* session = nullptr;   ///< The session driving the shaper.
    Builder* builder = nullptr;        ///< The builder of the exit in progress.
    EnterControl* control = nullptr;   ///< The control of the enter in progress.
    std::uint64_t epoch = 0;           ///< Counts callbacks, to date handles.
};

/// \brief A script's view of one open element.
struct ElementHandle {
    ScriptContext* context = nullptr;  ///< What the handle points back to.
    std::uint32_t depth = 0;           ///< The element's depth.
    std::uint64_t serial = 0;          ///< The element's serial.

    /// \brief Resolves the element.
    ///
    /// \returns The element.
    ///
    /// \remarks Throws when the element is no longer open, which sol2 turns
    ///          into a Lua error at the point of use.
    [[nodiscard]] Element& element() const {
        Element* found = context->session->frameAt(depth, serial);
        if (found == nullptr) {
            throw std::runtime_error("this element is no longer open");
        }
        return *found;
    }
};

/// \brief Makes a handle for an element.
///
/// \param context The context the handle belongs to.
/// \param element The element.
///
/// \returns The handle.
[[nodiscard]] ElementHandle handleFor(ScriptContext& context, const Element& element) {
    return ElementHandle{&context, element.depth(), element.serial()};
}

/// \brief A script's view of the items made inside one element.
struct ItemsHandle {
    ElementHandle owner;  ///< The element the items belong to.

    /// \brief Resolves the items.
    ///
    /// \returns The owning element's items.
    [[nodiscard]] Items& items() const { return owner.element().items(); }
};

/// \brief A script's view of the builder handed to exit.
struct BuilderHandle {
    ScriptContext* context = nullptr;  ///< What the handle points back to.
    std::uint64_t epoch = 0;           ///< The callback the handle was made in.

    /// \brief Resolves the builder.
    ///
    /// \returns The builder.
    ///
    /// \remarks Throws once the exit it was made for has returned.
    [[nodiscard]] Builder& builder() const {
        if (context->builder == nullptr || context->epoch != epoch) {
            throw std::runtime_error("the builder belongs to an exit that has finished");
        }
        return *context->builder;
    }
};

/// \brief A script's handle on a node it emitted.
struct NodeHandle {
    ScriptContext* context = nullptr;  ///< What the handle points back to.
    std::uint64_t epoch = 0;           ///< The callback the handle was made in.
    NodeBuilder node;                  ///< The builder for the node.

    /// \brief Resolves the node builder.
    ///
    /// \returns The builder.
    ///
    /// \remarks Throws once the exit it was made in has returned.
    [[nodiscard]] NodeBuilder builder() const {
        if (context->epoch != epoch) {
            throw std::runtime_error("the node belongs to an exit that has finished");
        }
        return node;
    }
};

/// \brief A script's handle on a property it emitted.
struct PropertyHandle {
    ScriptContext* context = nullptr;  ///< What the handle points back to.
    std::uint64_t epoch = 0;           ///< The callback the handle was made in.
    PropertyBuilder property;          ///< The builder for the property.

    /// \brief Resolves the property builder.
    ///
    /// \returns The builder.
    ///
    /// \remarks Throws once the exit it was made in has returned.
    [[nodiscard]] PropertyBuilder builder() const {
        if (context->epoch != epoch) {
            throw std::runtime_error("the property belongs to an exit that has finished");
        }
        return property;
    }
};

/// \brief A script's handle on the control handed to enter.
struct FrameHandle {
    ElementHandle owner;      ///< The element being entered.
    std::uint64_t epoch = 0;  ///< The callback the handle was made in.

    /// \brief Resolves the control.
    ///
    /// \returns The control.
    ///
    /// \remarks Throws once the enter it was made for has returned.
    [[nodiscard]] EnterControl& control() const {
        if (owner.context->control == nullptr || owner.context->epoch != epoch) {
            throw std::runtime_error("the frame belongs to an enter that has finished");
        }
        return *owner.context->control;
    }
};

/// \brief Reads an element handle out of an argument that may be nil.
///
/// \param argument What the script passed.
///
/// \returns The element, or `nullptr` for nil or anything else.
[[nodiscard]] Element* elementArgument(const sol::object& argument) {
    if (!argument.is<ElementHandle>()) {
        return nullptr;
    }
    return &argument.as<ElementHandle>().element();
}

/// \brief Reads an items handle out of an argument.
///
/// \param argument What the script passed.
///
/// \returns The items.
///
/// \remarks Throws when the argument is not an items handle, so a script
///          that passes the wrong thing hears about it.
[[nodiscard]] Items& itemsArgument(const sol::object& argument) {
    if (!argument.is<ItemsHandle>()) {
        throw std::runtime_error("expected the items of an element");
    }
    return argument.as<ItemsHandle>().items();
}

/// \brief Reads a span out of an argument that may name an element.
///
/// \param argument What the script passed.
///
/// \returns The element's span, or an empty span for nil or anything else.
[[nodiscard]] SourceSpan spanArgument(const sol::object& argument) {
    const Element* from = elementArgument(argument);
    return from == nullptr ? SourceSpan{} : from->span();
}

/// \brief Reads a list of attribute names to leave out.
///
/// \param names What the script passed after the element.
///
/// \returns The names, as strings a call can point at.
[[nodiscard]] std::vector<std::string> namesArgument(const sol::variadic_args& names) {
    std::vector<std::string> collected;
    for (const auto& name : names) {
        if (name.is<std::string>()) {
            collected.push_back(name.as<std::string>());
        }
    }
    return collected;
}

/// \brief Hands over an element's attributes with some left out.
///
/// \param source The element whose attributes to take.
/// \param skip The names to leave out, as a script gave them.
/// \param add Called with each attribute that is kept.
template <class Add>
void attributesExcept(const Element& source, const std::vector<std::string>& skip, Add&& add) {
    for (const Property& attribute : source.attributes()) {
        if (std::find(skip.begin(), skip.end(), attribute.name) == skip.end()) {
            add(attribute);
        }
    }
}

/// \brief Returns the value a script stored on an element.
///
/// \param element The element.
/// \param lua The interpreter, for nil.
///
/// \returns The value, or nil when nothing was stored.
[[nodiscard]] sol::object dataOf(const Element& element, sol::state& lua) {
    const sol::object* stored = std::any_cast<sol::object>(&element.data());
    return stored == nullptr ? sol::make_object(lua, sol::lua_nil) : *stored;
}

/// \brief Builds, once, the table of an element's attributes.
///
/// \param element The element.
/// \param lua The interpreter to build it in.
///
/// \returns The table, kept on the element so that a script reading it
///          twice pays once.
[[nodiscard]] sol::table attributeTable(Element& element, sol::state& lua) {
    if (const sol::table* cached = std::any_cast<sol::table>(&element.cache())) {
        return *cached;
    }
    sol::table table = lua.create_table();
    for (const Property& attribute : element.attributes()) {
        table[attribute.name] = attribute.value;
    }
    element.cache() = table;
    return table;
}

/// \brief Installs the usertypes the new form uses into an interpreter.
///
/// \param lua The interpreter.
///
/// \remarks Every entry resolves its handle on each use, so a stale handle
///          raises a Lua error naming the problem rather than reading memory
///          that is no longer there.
void installHandles(sol::state& lua) {
    lua.new_usertype<ElementHandle>(
        kElementType, sol::no_constructor,
        "name", sol::property([](const ElementHandle& h) { return std::string(h.element().name()); }),
        "text", sol::property([](const ElementHandle& h) { return std::string(h.element().text()); }),
        "depth", sol::property([](const ElementHandle& h) { return h.element().depth(); }),
        "index", sol::property([](const ElementHandle& h) { return h.element().index(); }),
        "child_count", sol::property([](const ElementHandle& h) { return h.element().childCount(); }),
        "closed", sol::property([](const ElementHandle& h) { return h.element().closed(); }),
        "span", sol::property([](const ElementHandle& h) {
            const SourceSpan span = h.element().span();
            sol::table table = h.context->lua->create_table();
            table["start"] = span.begin;
            table["stop"] = span.end;
            return table;
        }),
        "attr", sol::property([](const ElementHandle& h) {
            return attributeTable(h.element(), *h.context->lua);
        }),
        "parent", sol::property([](const ElementHandle& h) -> sol::object {
            const Element* parent = h.element().parent();
            if (parent == nullptr) {
                return sol::make_object(*h.context->lua, sol::lua_nil);
            }
            return sol::make_object(*h.context->lua, handleFor(*h.context, *parent));
        }),
        "items", sol::property([](const ElementHandle& h) { return ItemsHandle{h}; }),
        "data", sol::property(
            [](const ElementHandle& h) { return dataOf(h.element(), *h.context->lua); },
            [](const ElementHandle& h, sol::object value) { h.element().data() = value; }),
        "ancestor", [](const ElementHandle& h, const std::string& name) -> sol::object {
            const Element* found = h.element().ancestor(name);
            if (found == nullptr) {
                return sol::make_object(*h.context->lua, sol::lua_nil);
            }
            return sol::make_object(*h.context->lua, handleFor(*h.context, *found));
        });

    lua.new_usertype<ItemsHandle>(
        kItemsType, sol::no_constructor,
        sol::meta_function::length, [](const ItemsHandle& h) { return h.items().size(); });

    lua.new_usertype<NodeHandle>(
        kNodeType, sol::no_constructor,
        "attributes", [](NodeHandle& h, const ElementHandle& source, sol::variadic_args skip) {
            NodeBuilder node = h.builder();
            attributesExcept(source.element(), namesArgument(skip),
                             [&node](const Property& attribute) { node.property(attribute); });
            return h;
        },
        "text", [](NodeHandle& h, const ElementHandle& source) {
            h.builder().text(source.element());
            return h;
        },
        "property", [](NodeHandle& h, std::string name, std::string value, sol::object source) {
            h.builder().property(std::move(name), std::move(value), spanArgument(source));
            return h;
        },
        "adopt", [](NodeHandle& h, sol::object items) {
            h.builder().adopt(std::move(itemsArgument(items)));
            return h;
        },
        "kind", [](NodeHandle& h, std::string kind) {
            h.builder().kind(std::move(kind));
            return h;
        },
        "identity", [](NodeHandle& h, sol::object value, sol::optional<std::string> strength) {
            if (value.is<std::string>()) {
                h.builder().identity(value.as<std::string>(), strength && *strength == kStrongWord);
            }
            return h;
        },
        "title", [](NodeHandle& h, sol::object title, sol::optional<std::string> subtitle) {
            if (title.is<std::string>()) {
                h.builder().title(title.as<std::string>(), subtitle.value_or(std::string{}));
            }
            return h;
        },
        "ordered", [](NodeHandle& h, bool ordered) {
            h.builder().orderedChildren(ordered);
            return h;
        });

    lua.new_usertype<PropertyHandle>(
        kPropertyType, sol::no_constructor,
        "value", [](PropertyHandle& h, std::string value) {
            h.builder().value(std::move(value));
            return h;
        },
        "part", [](PropertyHandle& h, std::string name, sol::object value, sol::object source) {
            h.builder().part(std::move(name),
                             value.is<std::string>() ? value.as<std::string>() : std::string{},
                             spanArgument(source));
            return h;
        },
        "attributes", [](PropertyHandle& h, const ElementHandle& source, sol::variadic_args skip) {
            PropertyBuilder property = h.builder();
            attributesExcept(source.element(), namesArgument(skip),
                             [&property](const Property& attribute) { property.part(attribute); });
            return h;
        },
        "adopt", [](PropertyHandle& h, sol::object items) {
            h.builder().adopt(std::move(itemsArgument(items)));
            return h;
        },
        "ordered", [](PropertyHandle& h, bool ordered) {
            h.builder().ordered(ordered);
            return h;
        },
        "collapse", [](PropertyHandle& h) {
            h.builder().collapse();
            return h;
        });

    lua.new_usertype<BuilderHandle>(
        kBuilderType, sol::no_constructor,
        "node", [](const BuilderHandle& h, std::string kind, sol::object source) {
            NodeBuilder node = h.builder().node(std::move(kind), spanArgument(source));
            return NodeHandle{h.context, h.epoch, node};
        },
        "property", [](const BuilderHandle& h, std::string name, sol::object value,
                       sol::object source) {
            PropertyBuilder property = h.builder().property(
                std::move(name), value.is<std::string>() ? value.as<std::string>() : std::string{},
                spanArgument(source));
            return PropertyHandle{h.context, h.epoch, property};
        },
        "forward", [](const BuilderHandle& h, sol::object items) {
            h.builder().forward(std::move(itemsArgument(items)));
        },
        "default", [](const BuilderHandle& h, const ElementHandle& element) {
            h.builder().defaultTreatment(element.element());
        },
        "drop", [](const BuilderHandle& h) { h.builder().drop(); });

    lua.new_usertype<FrameHandle>(
        kFrameType, sol::no_constructor,
        "default", [](const FrameHandle& h) { h.control().useDefault(); },
        "opaque", [](const FrameHandle& h) { h.control().opaque(); },
        "data", sol::property(
            [](const FrameHandle& h) {
                return dataOf(h.owner.element(), *h.owner.context->lua);
            },
            [](const FrameHandle& h, sol::object value) { h.owner.element().data() = value; }));
}

/// \brief The enter-and-exit form, written as a shaper.
///
/// \remarks A script's `enter` is told an element has started and may leave
///          a value for its descendants or take the subtree away. Its `exit`
///          is told the element has ended, with the items made inside it,
///          and says what it becomes through the builder. Either may be
///          absent: no `enter` steers nothing, and no `exit` on an element
///          gives it the default treatment.
///
///          An error raised inside either is read as no answer. Whatever
///          `exit` emitted before the error stays, and what it did not adopt
///          goes up as it always does.
class ScriptShaper final : public IShaper {
public:
    /// \brief Prepares to shape with a script's table of functions.
    ///
    /// \param lua The interpreter the script is loaded in.
    /// \param shape The script's table.
    ScriptShaper(sol::state& lua, sol::table shape) : shape_(std::move(shape)) {
        context_.lua = &lua;
        enter_ = shape_[kEnter];
        exit_ = shape_[kExit];
    }

    void attach(ShapeSession& session) override { context_.session = &session; }

    void enter(Element& element, EnterControl& control) override {
        if (!enter_) {
            return;
        }
        ++context_.epoch;
        context_.control = &control;
        const ElementHandle handle = handleFor(context_, element);
        (void)(*enter_)(handle, FrameHandle{handle, context_.epoch});
        context_.control = nullptr;
    }

    void exit(Element& element, Builder& out) override {
        if (!exit_) {
            return;
        }
        ++context_.epoch;
        context_.builder = &out;
        (void)(*exit_)(handleFor(context_, element), BuilderHandle{&context_, context_.epoch});
        context_.builder = nullptr;
    }

private:
    ScriptContext context_;
    sol::table shape_;
    sol::optional<sol::protected_function> enter_;
    sol::optional<sol::protected_function> exit_;
};

/// \brief Reports whether a script's table uses the enter-and-exit form.
///
/// \param shape The script's table.
///
/// \returns `true` when it defines `enter` or `exit`.
[[nodiscard]] bool usesEventForm(const sol::table& shape) {
    return shape[kEnter].valid() || shape[kExit].valid();
}

// ------------------------------------------------------------ the provider

/// \brief A format whose shape is decided by a script.
class ScriptedProvider final : public IFormatProvider {
public:
    /// \brief Builds a provider from a declaration.
    ///
    /// \param spec What the script asked for.
    /// \param base The provider that does the parsing.
    /// \param script The script to reload in each worker.
    ScriptedProvider(ScriptedProviderSpec spec, const IFormatProvider& base, std::string script)
        : spec_(std::move(spec)), base_(base), script_(std::move(script)) {
        for (const std::string& extension : spec_.extensions) {
            extensionViews_.push_back(extension);
        }
        for (const std::string& property : spec_.propertyOrder) {
            propertyViews_.push_back(property);
        }
    }

    std::string_view name() const override { return spec_.name; }
    std::string_view displayName() const override { return spec_.displayName; }

    std::span<const std::string_view> defaultExtensions() const override {
        return extensionViews_;
    }

    int score(const SourceFile& source) const override {
        return claimsExtension(source) ? kExtensionScore : 0;
    }

    GraphDirection graphDirection() const override { return spec_.direction; }

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        return rankFromList(propertyViews_, propertyName);
    }

    Result<Tree, ParseError> parse(const SourceFile& source,
                                   std::stop_token token) const override {
        // One interpreter per call, because this runs on a worker and a Lua
        // state is not thread safe. Two sides of a diff parse at once.
        LuaState state;
        state.watchForCancellation(token);
        installHandles(state.get());
        sol::optional<sol::table> shape = loadShape(state.get());
        if (!shape) {
            return fail(ParseError::NotWellFormed);
        }

        std::unique_ptr<IShaper> shaper;
        if (usesEventForm(*shape)) {
            shaper = std::make_unique<ScriptShaper>(state.get(), *shape);
        } else {
            shaper = std::make_unique<LegacyScriptShaper>(state.get(), *shape);
        }

        // XML has a walker of its own, so the script sees the elements as the
        // parser meets them. Any other base reads the file first, and the
        // script sees that reading's tree.
        if (base_.name() == "xml") {
            return shapeXmlDocument(source, *shaper, *this, token);
        }
        auto parsed = base_.parse(source, token);
        if (!parsed.ok()) {
            return fail(parsed.error());
        }
        return shapeTree(parsed.value(), source, *shaper, *this, token);
    }

    IdentityKey identity(const Tree& tree, NodeId id) const override {
        // Answered while parsing, because asking a script per node during
        // matching would cross into an interpreter on the hot path.
        const NodeAnnotation& annotation = tree.annotation(id);
        if (annotation.identity.empty()) {
            return IdentityKey{};
        }
        return IdentityKey{annotation.strongIdentity, annotation.identity};
    }

    bool childrenOrdered(const Tree& tree, NodeId id) const override {
        return !tree.annotation(id).childrenUnordered;
    }

    NodeStyle style(const Tree& tree, NodeId id) const override {
        const Node& node = tree.node(id);
        const NodeAnnotation& annotation = tree.annotation(id);

        NodeStyle style;
        style.title = annotation.title.empty() ? node.kind : annotation.title;
        style.subtitle = annotation.subtitle;

        // Derived from the kind rather than asked for, so two nodes of one kind
        // always agree and a script never has to think about colour.
        const std::uint64_t h = hashBytes(node.kind);
        style.accent = Color{static_cast<std::uint8_t>(110 + (h & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 8) & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 16) & 0x3F)), 255};
        return style;
    }

private:
    /// \brief Runs the script and finds the table this provider was declared
    ///        with.
    ///
    /// \param lua The interpreter to run in.
    ///
    /// \returns The table of shaping functions, or nothing when the script
    ///          failed or no longer declares this provider.
    ///
    /// \remarks The other configuration functions are bound to do nothing. The
    ///          script is being reread for its provider declarations, and
    ///          setting the graph direction a second time from a worker thread
    ///          would be a surprise.
    [[nodiscard]] sol::optional<sol::table> loadShape(sol::state& lua) const {
        sol::optional<sol::table> found;

        lua.set_function("formats", [](sol::object) {});
        lua.set_function("fallback", [](sol::object) {});
        lua.set_function("graph_direction", [](sol::object) {});
        lua.set_function("exit_key", [](sol::object) {});
        lua.set_function("provider", [this, &found](std::string declared) {
            return [this, &found, declared](sol::table body) {
                if (declared == spec_.name) {
                    found = body;
                }
            };
        });

        const sol::protected_function_result result =
            lua.safe_script(script_, sol::script_pass_on_error);
        if (!result.valid()) {
            return sol::nullopt;
        }
        return found;
    }

    ScriptedProviderSpec spec_;
    const IFormatProvider& base_;
    std::string script_;
    std::vector<std::string_view> extensionViews_;
    std::vector<std::string_view> propertyViews_;
};

}  // namespace

std::unique_ptr<IFormatProvider> makeScriptedProvider(const ScriptedProviderSpec& spec,
                                                      const IFormatProvider& base,
                                                      std::string script) {
    return std::make_unique<ScriptedProvider>(spec, base, std::move(script));
}

std::vector<std::string> addScriptedProviders(ProviderRegistry& registry,
                                              const ProviderConfig& config) {
    std::vector<std::string> unknown;
    for (const ScriptedProviderSpec& spec : config.providers) {
        const IFormatProvider* base = registry.byName(spec.base);
        if (base == nullptr) {
            unknown.push_back(spec.base);
            continue;
        }
        // Registered before the built-ins already there, so a scripted format
        // that claims an extension wins the tie against the format it is built
        // on. Claiming it is the deliberate act; the tie is not.
        registry.addFirst(makeScriptedProvider(spec, *base, spec.source));
    }
    return unknown;
}

}  // namespace nmxd
