/// \file
/// \brief Implementation of the Lua shaping surface.

#include "core/lua_shape.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sol/sol.hpp>

#include "core/builder.h"
#include "core/dom.h"
#include "core/shape.h"

namespace nmxd {

namespace {

/// \brief The word a script passes to say an identity may travel.
constexpr std::string_view kStrongWord = "strong";

/// \brief Names a form for a script.
///
/// \param form The form to name.
///
/// \returns `scalar`, `record` or `sequence`.
[[nodiscard]] const char* formWord(PropertyForm form) noexcept {
    switch (form) {
        case PropertyForm::Scalar:
            return "scalar";
        case PropertyForm::Record:
            return "record";
        case PropertyForm::Sequence:
            return "sequence";
    }
    return "scalar";
}

/// \brief Turns a one-based script index into a zero-based one.
///
/// \param index What the script passed.
///
/// \returns The zero-based index, or one past anything for an index below
///          one so that the lookup answers nothing rather than wrapping.
[[nodiscard]] std::size_t fromLuaIndex(int index) noexcept {
    return index < 1 ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(index - 1);
}

/// \brief What `element.attr` is: a view answering the first property of a
///        name, or nil.
///
/// \remarks A plain table cannot hold a repeated key, so the shortcut is a
///          view rather than a copy. It is honest for a scalar and for a
///          record or sequence carrying a value; the accessors beside it are
///          for everything else.
struct AttributeView {
    DomNode element;
};

/// \brief Makes a Lua iterator function over a counted sequence.
///
/// \param lua The interpreter to make the function in.
/// \param step Returns the item at an index, or nil past the end.
///
/// \returns A function the generic for calls until it answers nil.
template <class Step>
sol::object makeIterator(sol::this_state lua, Step step) {
    auto index = std::make_shared<std::size_t>(0);
    std::function<sol::object(sol::this_state)> next = [step, index](sol::this_state state) {
        return step(state, (*index)++);
    };
    return sol::make_object(lua, next);
}

/// \brief Hands a handle to Lua, or nil when it is invalid.
///
/// \param state The interpreter to make the object in.
/// \param handle A DOM handle that may be invalid.
///
/// \returns The handle as a userdata, or nil.
///
/// \remarks Every lookup that can miss goes through here, so that a script
///          tests for a missing parent, child, property or part with `== nil`
///          and never sees an invalid handle it would have to ask about.
template <class Handle>
sol::object orNil(sol::this_state state, const Handle& handle) {
    return handle.valid() ? sol::make_object(state, handle) : sol::object(sol::lua_nil);
}

/// \brief Raises a Lua error from a failed script call, keeping its message.
///
/// \param result The failed call.
///
/// \remarks The first line is the message. Lua's message carries the chunk
///          and line, which is what a report line and a card tooltip need,
///          and the traceback sol2 appends below it would put a stack dump
///          into both for one wrong element. The traceback is not thrown
///          away: it travels as the error's detail and the drain writes it to
///          the log, which is where a person fixing the script reads it.
[[noreturn]] void rethrow(const sol::protected_function_result& result) {
    const sol::error error = result;
    const std::string message = error.what();
    const std::size_t lineEnd = message.find('\n');
    throw ShapeError(message.substr(0, lineEnd),
                     lineEnd == std::string::npos ? std::string{} : message);
}

/// \brief Queues a script function with its arguments.
///
/// \param context The pass to queue into.
/// \param front Whether to queue at the front rather than the back.
/// \param function The function to call later.
/// \param args What to call it with.
///
/// \remarks The first element and the first builder handle among the
///          arguments are what a failure inside the job is recorded against,
///          which is why the idiom is `out:next(visit, element, parent)`.
void queueCall(ShapeContext& context, bool front, sol::protected_function function,
               sol::variadic_args args) {
    std::vector<sol::object> held;
    DomId element = kInvalidDom;
    RefId owner;
    for (const sol::object argument : args) {
        if (element == kInvalidDom && argument.is<DomNode>()) {
            element = argument.as<DomNode>().id();
        }
        if (owner.index == RefId::kNone && argument.is<Ref>()) {
            owner = argument.as<Ref>().id();
        }
        held.push_back(argument);
    }

    ShapeContext::Job job = [function, held](ShapeContext&) {
        const sol::protected_function_result result = function(sol::as_args(held));
        if (!result.valid()) {
            rethrow(result);
        }
    };
    if (front) {
        context.next(std::move(job), element, owner);
    } else {
        context.later(std::move(job), element, owner);
    }
}

/// \brief Binds the document handle and its element and property handles.
void bindDocument(sol::state& lua) {
    lua.new_usertype<Dom>(
        "Document", sol::no_constructor,
        "root", sol::property([](const Dom& dom) { return dom.root(); }),
        "size", sol::property([](const Dom& dom) { return dom.size(); }),
        "base_format", sol::property([](const Dom& dom) { return dom.baseFormat(); }),
        "at", [](const Dom& dom, DomId id, sol::this_state s) { return orNil(s, dom.at(id)); });

    lua.new_usertype<AttributeView>(
        "Attributes", sol::no_constructor,
        sol::meta_function::index,
        [](const AttributeView& view, std::string_view name, sol::this_state state) {
            const auto value = view.element.attribute(name);
            return value.has_value() ? sol::make_object(state, *value) : sol::lua_nil;
        });

    lua.new_usertype<DomNode>(
        "Element", sol::no_constructor,
        "name", sol::property([](const DomNode& e) { return e.name(); }),
        "text", sol::property([](const DomNode& e) { return e.text(); }),
        "id", sol::property([](const DomNode& e) { return e.id(); }),
        "depth", sol::property([](const DomNode& e) { return e.depth(); }),
        "attr", sol::property([](const DomNode& e) { return AttributeView{e}; }),
        "parent",
        sol::property([](const DomNode& e, sol::this_state s) { return orNil(s, e.parent()); }),
        "first_child",
        sol::property([](const DomNode& e, sol::this_state s) { return orNil(s, e.firstChild()); }),
        "last_child",
        sol::property([](const DomNode& e, sol::this_state s) { return orNil(s, e.lastChild()); }),
        "next_sibling",
        sol::property([](const DomNode& e, sol::this_state s) { return orNil(s, e.nextSibling()); }),
        "prev_sibling",
        sol::property([](const DomNode& e, sol::this_state s) { return orNil(s, e.prevSibling()); }),
        "child_count", sol::property([](const DomNode& e) { return e.childCount(); }),
        "property_count", sol::property([](const DomNode& e) { return e.propertyCount(); }),
        "child_at",
        [](const DomNode& e, int index, sol::this_state s) {
            return orNil(s, e.childAt(fromLuaIndex(index)));
        },
        "child",
        [](const DomNode& e, std::string_view name, sol::this_state s) {
            return orNil(s, e.child(name));
        },
        "children",
        [](const DomNode& e, sol::optional<std::string> name, sol::this_state lua) {
            // Filtered by name when one is given, every child otherwise. The
            // cursor walks the range once, so a filtered pass over a long
            // sibling list costs what an unfiltered one does. The filter is
            // owned alongside the cursor because the range keeps a view into
            // it and the script's string would not outlive this call.
            struct Cursor {
                std::string filter;
                DomChildRange::iterator at;
                DomChildRange::iterator end;
            };
            auto cursor = std::make_shared<Cursor>();
            cursor->filter = name.value_or("");
            const DomChildRange range = e.children(cursor->filter);
            cursor->at = range.begin();
            cursor->end = range.end();
            std::function<sol::object(sol::this_state)> next = [cursor](sol::this_state state) {
                if (cursor->at == cursor->end) {
                    return sol::object(sol::lua_nil);
                }
                const DomNode child = *cursor->at;
                ++cursor->at;
                return sol::make_object(state, child);
            };
            return sol::make_object(lua, next);
        },
        "property_at",
        [](const DomNode& e, int index, sol::this_state s) {
            return orNil(s, e.propertyAt(fromLuaIndex(index)));
        },
        "property",
        [](const DomNode& e, std::string_view name, sol::this_state s) {
            return orNil(s, e.property(name));
        },
        "properties",
        [](const DomNode& e, sol::optional<std::string> name, sol::this_state lua) {
            // Filtered by name when one is given, every property otherwise.
            // The filter is copied into the closure because a view into the
            // script's string would not outlive this call.
            const std::string filter = name.value_or("");
            return makeIterator(lua, [e, filter](sol::this_state state, std::size_t index) {
                // Walk to the index'th property that passes the filter.
                std::size_t seen = 0;
                for (std::size_t i = 0; i < e.propertyCount(); ++i) {
                    const DomProperty property = e.propertyAt(i);
                    if (!filter.empty() && property.name() != filter) {
                        continue;
                    }
                    if (seen++ == index) {
                        return sol::make_object(state, property);
                    }
                }
                return sol::object(sol::lua_nil);
            });
        });

    lua.new_usertype<DomProperty>(
        "Property", sol::no_constructor,
        "name", sol::property([](const DomProperty& p) { return p.name(); }),
        "value", sol::property([](const DomProperty& p) { return p.value(); }),
        "form", sol::property([](const DomProperty& p) { return formWord(p.form()); }),
        "part_count", sol::property([](const DomProperty& p) { return p.partCount(); }),
        "part_at",
        [](const DomProperty& p, int index, sol::this_state s) {
            return orNil(s, p.partAt(fromLuaIndex(index)));
        },
        "part",
        [](const DomProperty& p, std::string_view name, sol::this_state s) {
            return orNil(s, p.part(name));
        },
        "parts", [](const DomProperty& p, sol::this_state lua) {
            return makeIterator(lua, [p](sol::this_state state, std::size_t index) {
                const DomProperty part = p.partAt(index);
                return part.valid() ? sol::make_object(state, part) : sol::lua_nil;
            });
        });
}

/// \brief Binds the builder handle.
///
/// \remarks Every setter returns the handle, so a script can chain them. A
///          BuildError raised inside is turned into a Lua error by sol2 and
///          reaches the job that made the call.
void bindHandle(sol::state& lua) {
    lua.new_usertype<RefId>("Id", sol::no_constructor);

    lua.new_usertype<Ref>(
        "Ref", sol::no_constructor,
        "kind", sol::property([](const Ref& r) { return r.isNode() ? "node" : "property"; }),
        "form", sol::property([](const Ref& r) { return formWord(r.form()); }),
        "is_node", sol::property([](const Ref& r) { return r.isNode(); }),
        "id", sol::property([](const Ref& r) { return r.id(); }),
        "parent", sol::property([](const Ref& r) { return r.parent(); }),
        "owner", sol::property([](const Ref& r) { return r.owner(); }),
        "child",
        sol::overload([](Ref& r) { return r.child(); },
                      [](Ref& r, std::string_view name) { return r.child(name); },
                      [](Ref& r, const DomNode& element) { return r.child(element); }),
        "property",
        sol::overload([](Ref& r, std::string_view name) { return r.property(name); },
                      [](Ref& r, std::string_view name, std::string_view value) {
                          return r.property(name, value);
                      },
                      [](Ref& r, const DomProperty& source) { return r.property(source); },
                      [](Ref& r, const DomNode& element) { return r.property(element); }),
        "record", [](Ref& r, std::string_view name) { return r.record(name); },
        "sequence", [](Ref& r, std::string_view name) { return r.sequence(name); },
        "item",
        sol::overload([](Ref& r) { return r.item(); },
                      [](Ref& r, std::string_view value) { return r.item(value); }),
        "set_name", [](Ref& r, std::string_view name) { return r.setName(name); },
        "set_value", [](Ref& r, std::string_view value) { return r.setValue(value); },
        "set_form",
        [](Ref& r, std::string_view word) {
            if (word == "record") {
                return r.setForm(PropertyForm::Record);
            }
            if (word == "sequence") {
                return r.setForm(PropertyForm::Sequence);
            }
            if (word == "scalar") {
                return r.setForm(PropertyForm::Scalar);
            }
            throw BuildError("a form is scalar, record or sequence");
        },
        "set_children_ordered",
        [](Ref& r, bool ordered) { return r.setChildrenOrdered(ordered); },
        "set_identity",
        [](Ref& r, sol::optional<std::string_view> value, sol::optional<std::string_view> word) {
            // A nil value is no identity, so `set_identity(element.attr.id)`
            // reads naturally on an element without one.
            if (!value.has_value()) {
                return r;
            }
            const Identity strength =
                word.has_value() && *word == kStrongWord ? Identity::Strong : Identity::Weak;
            return r.setIdentity(*value, strength);
        },
        "set_title",
        [](Ref& r, sol::optional<std::string_view> title, sol::optional<std::string_view> subtitle) {
            return r.setTitle(title.value_or(std::string_view{}),
                              subtitle.value_or(std::string_view{}));
        },
        "set_accent", [](Ref& r, std::uint32_t rgb) { return r.setAccent(rgb); });
}

/// \brief Binds the builder and its queue, which a script sees as `out`.
void bindBuilder(sol::state& lua) {
    lua.new_usertype<ShapeContext>(
        "Builder", sol::no_constructor,
        "node_count", sol::property([](ShapeContext& c) { return c.out().nodeCount(); }),
        "pending", sol::property([](const ShapeContext& c) { return c.pending(); }),
        "cancelled", sol::property([](const ShapeContext& c) { return c.cancelled(); }),
        "root",
        sol::overload([](ShapeContext& c, std::string_view kind) { return c.out().root(kind); },
                      [](ShapeContext& c, const DomNode& element) { return c.out().root(element); }),
        "at", [](ShapeContext& c, RefId id) { return c.out().at(id); },
        "later",
        [](ShapeContext& c, sol::protected_function function, sol::variadic_args args) {
            queueCall(c, false, std::move(function), args);
        },
        "next", [](ShapeContext& c, sol::protected_function function, sol::variadic_args args) {
            queueCall(c, true, std::move(function), args);
        });
}

}  // namespace

void bindShapeApi(sol::state& lua) {
    bindDocument(lua);
    bindHandle(lua);
    bindBuilder(lua);
}

void runShapeFunction(sol::state& lua, sol::protected_function shape, ShapeContext& context) {
    (void)lua;
    const sol::protected_function_result result = shape(&context.dom(), &context);
    if (!result.valid()) {
        rethrow(result);
    }
}

}  // namespace nmxd
