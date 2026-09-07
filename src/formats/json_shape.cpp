/// \file
/// \brief Implementation of the JSON shaping driver.

#include "formats/json_shape.h"

#include <any>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "core/hash.h"

namespace nmxd {

namespace {

namespace ondemand = simdjson::ondemand;

/// \brief How many values are opened between two checks of the stop token.
///
/// \remarks Checking every value would cost more than the check is worth on a
///          document small enough to finish before anyone could cancel it.
constexpr std::size_t kCancelCheckInterval = 4096;

/// \brief What the walker learned about an array before reporting it.
struct ArrayFacts {
    /// \brief Whether an object sits anywhere inside it, however deep.
    bool holdsObject = false;
};

/// \brief Reports whether a byte is JSON whitespace.
///
/// \param c The byte to test.
///
/// \returns `true` for space, tab, carriage return and line feed.
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// \brief Reports whether text begins with a prefix.
///
/// \param text The text to test.
/// \param prefix The prefix to look for.
///
/// \returns `true` when \p text starts with \p prefix.
bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// \brief Drops trailing whitespace from a token.
///
/// \param token The token as simdjson reported it.
///
/// \returns The same view with any trailing whitespace removed.
///
/// \remarks simdjson::ondemand::value::raw_json_token() runs from the start of
///          the token to the start of the next one, so it carries the
///          whitespace between them. A span has to stop at the value.
std::string_view trimRight(std::string_view token) {
    while (!token.empty() && isSpace(token.back())) {
        token.remove_suffix(1);
    }
    return token;
}

/// \brief Reports whether a JSON type is an object or an array.
///
/// \param type The type.
///
/// \returns `true` for a container, `false` for a scalar.
bool isContainer(ondemand::json_type type) {
    return type == ondemand::json_type::object || type == ondemand::json_type::array;
}

/// \brief Makes the attribute that records a value's JSON type.
///
/// \param type The type word, kJsonObjectType or kJsonArrayType.
///
/// \returns The attribute, with no span because the file never wrote it.
Property typeAttribute(std::string_view type) {
    Property attribute;
    attribute.name = std::string(kJsonTypeProperty);
    attribute.value = std::string(type);
    return attribute;
}

/// \brief Walks one parsed document and reports its values to a session.
///
/// \remarks Kept as a class rather than a set of free functions because every
///          step needs the same things: the session being reported to, the
///          buffer the spans are offsets into, the document whose cursor says
///          where parsing has reached, and the stop token.
///
///          Recursive on document depth, because On Demand parsing hands a
///          container out once and its iterator has to stay alive while what
///          is inside it is read.
class JsonWalker {
public:
    /// \brief Prepares to walk one document.
    ///
    /// \param session The session to report to.
    /// \param buffer The padded copy of the document being walked.
    /// \param origin Offset of \p buffer within the original file, which is
    ///        non-zero only when a byte order mark was skipped.
    /// \param document The document cursor, used to find where a container
    ///        ended.
    /// \param token Checked periodically while walking.
    JsonWalker(ShapeSession& session, std::string_view buffer, std::uint32_t origin,
               ondemand::document& document, const std::stop_token& token)
        : session_(session), buffer_(buffer), origin_(origin), document_(document), token_(token) {}

    /// \brief Walks the whole document from its root.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks A document holding nothing but a scalar is a valid JSON
    ///          document and becomes a single element, so that a file
    ///          consisting of one number still diffs instead of being refused.
    simdjson::error_code walkRoot() {
        ondemand::json_type type{};
        if (const auto error = document_.type().get(type)) {
            return error;
        }

        if (!isContainer(type)) {
            std::string_view raw;
            if (const auto error = document_.raw_json().get(raw)) {
                return error;
            }
            raw = trimRight(raw);
            const SourceSpan span{offsetOf(raw.data()), endOf(raw)};
            session_.open(std::string(kJsonRootKind), {}, SourceSpan{span.begin, span.begin});
            if (const auto error = validateScalar(type)) {
                return error;
            }
            session_.close(std::string(raw), span, span.end);
            return simdjson::SUCCESS;
        }

        ondemand::value root;
        if (const auto error = document_.get_value().get(root)) {
            return error;
        }
        return walkValue(std::string(kJsonRootKind), root.raw_json_token().data(), root);
    }

    /// \brief Reports whether the walk stopped because it was cancelled.
    ///
    /// \returns `true` when the stop token was signalled mid-walk.
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

private:
    /// \brief Reports one value and everything inside it.
    ///
    /// \param name What the element is called.
    /// \param spanBegin Where the element starts in the buffer, which is the
    ///        key token for a member of an object and the value token
    ///        otherwise.
    /// \param value The value to report. Consumed by this call.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    simdjson::error_code walkValue(std::string name, const char* spanBegin, ondemand::value value) {
        if (++seen_ % kCancelCheckInterval == 0 && token_.stop_requested()) {
            cancelled_ = true;
            return simdjson::SUCCESS;
        }

        ondemand::json_type type{};
        if (const auto error = value.type().get(type)) {
            return error;
        }

        const std::uint32_t begin = offsetOf(spanBegin);
        switch (type) {
            case ondemand::json_type::object:
                return walkObject(std::move(name), begin, value);
            case ondemand::json_type::array:
                return walkArray(std::move(name), begin, value);
            default:
                return walkScalar(std::move(name), type, value);
        }
    }

    /// \brief Reports an object.
    ///
    /// \param name What the element is called.
    /// \param begin Where the element starts in the file.
    /// \param value The object. Consumed by this call.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks Two passes. The first reads the scalar members into
    ///          attributes, so that the element opens with them and a shaper
    ///          sees them at enter. The second walks into the object and
    ///          array members, which are the elements inside. The cursor is
    ///          forward only, so this is the only way to have the attributes
    ///          before anything inside is reported.
    simdjson::error_code walkObject(std::string name, std::uint32_t begin, ondemand::value& value) {
        ondemand::object object;
        if (const auto error = value.get_object().get(object)) {
            return error;
        }

        std::vector<Property> attributes;
        attributes.push_back(typeAttribute(kJsonObjectType));
        if (const auto error = collectScalarMembers(object, attributes)) {
            return error;
        }
        bool rewound = false;
        if (const auto error = object.reset().get(rewound)) {
            return error;
        }

        session_.open(std::move(name), std::move(attributes), SourceSpan{begin, begin});
        if (const auto error = walkContainerMembers(object, session_.descend())) {
            return error;
        }
        if (cancelled_) {
            return simdjson::SUCCESS;
        }
        session_.close(std::string{}, SourceSpan{}, cursor());
        return simdjson::SUCCESS;
    }

    /// \brief Reads an object's scalar members into attributes.
    ///
    /// \param object The object, left at its end afterwards.
    /// \param into Where to append the attributes.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks Object and array members are passed over structurally and
    ///          read on the second pass.
    simdjson::error_code collectScalarMembers(ondemand::object& object,
                                              std::vector<Property>& into) {
        for (auto memberResult : object) {
            ondemand::field member;
            if (const auto error = std::move(memberResult).get(member)) {
                return error;
            }

            // Both have to be read before the value is touched, because each is
            // recovered from where the cursor currently stands.
            const std::string_view key = member.escaped_key();
            const char* keyBegin = member.key_raw_json_token().data();

            ondemand::value memberValue = member.value();
            ondemand::json_type type{};
            if (const auto error = memberValue.type().get(type)) {
                return error;
            }
            if (isContainer(type)) {
                continue;
            }

            const std::string_view raw = trimRight(memberValue.raw_json_token());
            Property attribute;
            attribute.name = std::string(key);
            attribute.value = std::string(raw);
            attribute.span = SourceSpan{offsetOf(keyBegin), endOf(raw)};
            into.push_back(std::move(attribute));
            if (const auto error = validateScalar(type, memberValue)) {
                return error;
            }
        }
        return simdjson::SUCCESS;
    }

    /// \brief Walks into an object's object and array members.
    ///
    /// \param object The object, rewound to its start.
    /// \param descend Whether to report what is inside. When `false` the
    ///        members are passed over so that the cursor still reaches the
    ///        object's end.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    simdjson::error_code walkContainerMembers(ondemand::object& object, bool descend) {
        for (auto memberResult : object) {
            ondemand::field member;
            if (const auto error = std::move(memberResult).get(member)) {
                return error;
            }
            const std::string_view key = member.escaped_key();
            const char* keyBegin = member.key_raw_json_token().data();

            ondemand::value memberValue = member.value();
            ondemand::json_type type{};
            if (const auto error = memberValue.type().get(type)) {
                return error;
            }
            if (!isContainer(type) || !descend) {
                continue;
            }

            if (const auto error = walkValue(std::string(key), keyBegin, memberValue)) {
                return error;
            }
            if (cancelled_) {
                return simdjson::SUCCESS;
            }
        }
        return simdjson::SUCCESS;
    }

    /// \brief Reports an array.
    ///
    /// \param name What the element is called.
    /// \param begin Where the element starts in the file.
    /// \param value The array. Consumed by this call.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks Looks ahead for an object before reporting anything, and
    ///          leaves the answer on the element for the default treatment,
    ///          which is what decides whether the array is a list of values or
    ///          a list of nodes.
    simdjson::error_code walkArray(std::string name, std::uint32_t begin, ondemand::value& value) {
        ondemand::array array;
        if (const auto error = value.get_array().get(array)) {
            return error;
        }
        ArrayFacts facts;
        if (const auto error = arrayHoldsObject(array, facts.holdsObject)) {
            return error;
        }

        std::vector<Property> attributes;
        attributes.push_back(typeAttribute(kJsonArrayType));
        Element& element = session_.open(std::move(name), std::move(attributes),
                                         SourceSpan{begin, begin});
        element.source() = facts;

        if (const auto error = walkElements(array, session_.descend())) {
            return error;
        }
        if (cancelled_) {
            return simdjson::SUCCESS;
        }
        session_.close(std::string{}, SourceSpan{}, cursor());
        return simdjson::SUCCESS;
    }

    /// \brief Reports whether an array holds an object anywhere inside it.
    ///
    /// \param array The array to look at, left rewound afterwards.
    /// \param answer Set to `true` when an object is found at any depth.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks The recursive half is what makes a four by four transform
    ///          matrix one property with parts rather than sixteen anonymous
    ///          nodes four levels deep.
    ///
    ///          This costs a second pass over the array. On Demand parsing is
    ///          forward only, so the alternative was to report optimistically
    ///          and unpick it on meeting an object, which is more code and more
    ///          ways to be wrong.
    simdjson::error_code arrayHoldsObject(ondemand::array& array, bool& answer) {
        answer = false;
        for (auto elementResult : array) {
            ondemand::value element;
            if (const auto error = std::move(elementResult).get(element)) {
                return error;
            }
            ondemand::json_type type{};
            if (const auto error = element.type().get(type)) {
                return error;
            }

            if (type == ondemand::json_type::object) {
                answer = true;
            } else if (type == ondemand::json_type::array) {
                ondemand::array nested;
                if (const auto error = element.get_array().get(nested)) {
                    return error;
                }
                bool nestedAnswer = false;
                if (const auto error = arrayHoldsObject(nested, nestedAnswer)) {
                    return error;
                }
                answer = answer || nestedAnswer;
            }
        }

        // Walked to the end even once the answer is known, because leaving the
        // iterator part way through is what would make the rewind unreliable.
        bool rewound = false;
        return array.reset().get(rewound);
    }

    /// \brief Reports an array's elements.
    ///
    /// \param array The array, rewound to its start.
    /// \param descend Whether to report what is inside. When `false` the
    ///        elements are passed over so that the cursor still reaches the
    ///        array's end.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    simdjson::error_code walkElements(ondemand::array& array, bool descend) {
        for (auto elementResult : array) {
            ondemand::value element;
            if (const auto error = std::move(elementResult).get(element)) {
                return error;
            }
            if (!descend) {
                continue;
            }
            const char* begin = element.raw_json_token().data();
            if (const auto error = walkValue(std::string(kJsonElementKind), begin, element)) {
                return error;
            }
            if (cancelled_) {
                return simdjson::SUCCESS;
            }
        }
        return simdjson::SUCCESS;
    }

    /// \brief Reports a scalar that stands on its own.
    ///
    /// \param name What the element is called.
    /// \param type The scalar's type.
    /// \param value The scalar. Consumed by this call.
    ///
    /// \returns simdjson::SUCCESS, or the error the value turned out to hold.
    ///
    /// \remarks Only an element of an array reaches here; a scalar member of
    ///          an object is an attribute of the object instead.
    simdjson::error_code walkScalar(std::string name, ondemand::json_type type,
                                    ondemand::value& value) {
        const std::string_view raw = trimRight(value.raw_json_token());
        const SourceSpan span{offsetOf(raw.data()), endOf(raw)};
        session_.open(std::move(name), {}, SourceSpan{span.begin, span.begin});
        if (const auto error = validateScalar(type, value)) {
            return error;
        }
        session_.close(std::string(raw), span, span.end);
        return simdjson::SUCCESS;
    }

    /// \brief Reads a scalar so that the parser checks it.
    ///
    /// \param type The scalar's type.
    /// \param value The value to read.
    ///
    /// \returns simdjson::SUCCESS, or the error the value turned out to hold.
    ///
    /// \remarks Only the raw text of a scalar is kept, so nothing else here
    ///          needs its parsed form. It is read anyway because On Demand
    ///          parsing is lazy: a value nobody reads is skipped over
    ///          structurally and never checked, and a file whose numbers are
    ///          malformed would otherwise diff as though it were sound.
    static simdjson::error_code validateScalar(ondemand::json_type type, ondemand::value& value) {
        switch (type) {
            case ondemand::json_type::string:
                return value.get_string().error();
            case ondemand::json_type::number:
                return value.get_double().error();
            case ondemand::json_type::boolean:
                return value.get_bool().error();
            case ondemand::json_type::null:
                return value.is_null().error();
            default:
                return simdjson::SUCCESS;
        }
    }

    /// \brief Reads the document's own scalar so that the parser checks it.
    ///
    /// \param type The scalar's type.
    ///
    /// \returns simdjson::SUCCESS, or the error the value turned out to hold.
    ///
    /// \remarks The same check as the two-argument overload, for the document
    ///          that holds nothing but a scalar. A document is not a value, so
    ///          the two cannot share one code path.
    simdjson::error_code validateScalar(ondemand::json_type type) {
        switch (type) {
            case ondemand::json_type::string:
                return document_.get_string().error();
            case ondemand::json_type::number:
                return document_.get_double().error();
            case ondemand::json_type::boolean:
                return document_.get_bool().error();
            case ondemand::json_type::null:
                return document_.is_null().error();
            default:
                return simdjson::SUCCESS;
        }
    }

    /// \brief Converts a pointer into the buffer to an offset in the file.
    ///
    /// \param pointer A pointer within the padded buffer.
    ///
    /// \returns The matching byte offset in the original file.
    [[nodiscard]] std::uint32_t offsetOf(const char* pointer) const {
        return origin_ + static_cast<std::uint32_t>(pointer - buffer_.data());
    }

    /// \brief Returns the offset just past a token.
    ///
    /// \param token A view into the buffer.
    ///
    /// \returns The offset one byte past the token's last byte.
    [[nodiscard]] std::uint32_t endOf(std::string_view token) const {
        return offsetOf(token.data() + token.size());
    }

    /// \brief Returns the offset just past whatever was last consumed.
    ///
    /// \returns The end of the container that has just been walked.
    ///
    /// \remarks This is the one thing simdjson provides that decided the choice
    ///          of parser. The cursor sits on the next token, which is the comma
    ///          or closing bracket that follows the container, so winding back
    ///          over the whitespace in between lands exactly on the container's
    ///          own closing bracket. A cursor that has run off the end means the
    ///          container was the last thing in the file.
    [[nodiscard]] std::uint32_t cursor() const {
        const char* location = nullptr;
        if (document_.current_location().get(location)) {
            std::string_view rest = trimRight(buffer_);
            return endOf(rest);
        }
        while (location > buffer_.data() && isSpace(location[-1])) {
            --location;
        }
        return offsetOf(location);
    }

    ShapeSession& session_;
    std::string_view buffer_;
    std::uint32_t origin_ = 0;
    ondemand::document& document_;
    const std::stop_token& token_;
    std::size_t seen_ = 0;
    bool cancelled_ = false;
};

/// \brief Finds a property of a staged node by name.
///
/// \param node The node.
/// \param name The property name.
///
/// \returns The property, or `nullptr` when the node has none of that name.
Property* findProperty(PendingNode& node, std::string_view name) {
    for (Property& property : node.properties) {
        if (property.name == name) {
            return &property;
        }
    }
    return nullptr;
}

/// \brief Turns a node made from an array element into a part.
///
/// \param node The node, consumed.
///
/// \returns A part with the element's value, or with ordered parts of its
///          own for a nested array.
///
/// \remarks The parts have no names, because a position in a list is not a
///          name. They are marked as a sequence, so reordering them is a
///          change while reordering a record's fields is not.
///
///          Recognises what the default treatment makes: a scalar element
///          carries kValueProperty and nothing else, and a nested array
///          carries kJsonTypeProperty. A node a shaper made instead is kept
///          whole, as a record named after its kind.
Property partFromElement(PendingNode&& node) {
    const Property* type = findProperty(node, kJsonTypeProperty);
    Property* value = findProperty(node, kValueProperty);
    if (type == nullptr && value != nullptr && node.properties.size() == 1 &&
        node.children.empty()) {
        Property part;
        part.span = node.span;
        part.value = std::move(value->value);
        return part;
    }
    if (type != nullptr && type->value == kJsonArrayType) {
        Property part;
        part.span = node.span;
        part.ordered = true;
        for (PendingNode& child : node.children) {
            part.children.push_back(partFromElement(std::move(child)));
        }
        return part;
    }
    return propertyFromNode(std::move(node));
}

/// \brief Reports whether an array element should become one property.
///
/// \param element The array element.
///
/// \returns `true` for an array under an object that holds no object
///          anywhere inside it.
///
/// \remarks An array holding an object stays a node, because an object has
///          named fields a reader will want matched against their
///          counterparts, and matching is what nodes are for. An array under
///          an array, or the document's own array, stays a node too, because
///          its elements are the things being compared.
bool arrayBecomesProperty(const Element& element) {
    const Element* parent = element.parent();
    if (parent == nullptr || parent->attributeValue(kJsonTypeProperty) != kJsonObjectType) {
        return false;
    }
    const ArrayFacts* facts = std::any_cast<ArrayFacts>(&element.source());
    return facts != nullptr && !facts->holdsObject;
}

}  // namespace

void JsonDefaultShaper::exit(Element& element, Builder& out) {
    const std::string_view type = element.attributeValue(kJsonTypeProperty);

    // A scalar on its own: an element of an array, or the whole document. Its
    // value is all it has, so it becomes the one property rather than a
    // nameless node.
    if (type.empty()) {
        out.node(std::string(element.name()), element)
            .property(std::string(kValueProperty), std::string(element.text()),
                      element.textSpan());
        return;
    }

    // A list of values is one property with parts, made from the nodes the
    // elements became. The span covers the key and the whole array.
    if (type == kJsonArrayType && arrayBecomesProperty(element)) {
        PropertyBuilder property = out.property(std::string(element.name()), std::string{}, element);
        property.ordered(true);
        for (Item& item : element.takeItems()) {
            if (item.isNode()) {
                property.part(partFromElement(std::move(item.node())));
            } else {
                property.part(std::move(item.property()));
            }
        }
        return;
    }

    // An object, or an array of nodes: a node whose attributes are its type
    // and its scalar members, and whose items are its children and its
    // properties with parts.
    out.node(std::string(element.name()), element).attributes(element).adopt(element.takeItems());
}

Result<Tree, ParseError> shapeJsonDocument(const SourceFile& source, IShaper& shaper,
                                           const IFormatProvider& provider, std::stop_token token) {
    if (source.empty()) {
        return fail(ParseError::Empty);
    }

    // Offsets are what spans are made of, so a document whose bytes do not
    // line up with the text is refused rather than silently mis-linked.
    std::string_view text = source.text();
    if (startsWith(text, "\xFF\xFE") || startsWith(text, "\xFE\xFF")) {
        return fail(ParseError::UnsupportedEncoding);
    }

    // A UTF-8 byte order mark is common on files written on Windows and is
    // not part of the document. Skipping it here rather than stripping it
    // from the text keeps every span an offset into the file as it sits on
    // disk.
    std::uint32_t origin = 0;
    if (startsWith(text, "\xEF\xBB\xBF")) {
        origin = 3;
        text.remove_prefix(3);
    }
    if (trimRight(text).empty()) {
        return fail(ParseError::Empty);
    }

    // simdjson reads past the end of the document by up to a fixed padding,
    // so it needs a buffer with room to spare. The copy keeps every offset
    // the same, which is what lets a span computed here point into the
    // original file.
    simdjson::padded_string buffer(text.data(), text.size());
    ondemand::parser parser;
    ondemand::document document;
    if (parser.iterate(buffer).get(document)) {
        return fail(ParseError::NotWellFormed);
    }

    // The walk reports values and the session decides what they become.
    JsonDefaultShaper fallback;
    ShapeSession session(shaper, fallback, source.text());
    JsonWalker walker(session, std::string_view(buffer.data(), buffer.size()), origin, document,
                      token);
    if (walker.walkRoot() != simdjson::SUCCESS) {
        return fail(ParseError::NotWellFormed);
    }
    if (walker.cancelled()) {
        return fail(ParseError::Cancelled);
    }
    // On Demand parsing stops as soon as the root value is complete, so a
    // second document appended to the first would otherwise go unnoticed.
    if (!document.at_end()) {
        return fail(ParseError::NotWellFormed);
    }

    Tree tree = session.finish(std::string(provider.name()));
    if (tree.empty()) {
        return fail(ParseError::Empty);
    }
    tree.finalize();
    computeHashes(tree, provider, token);
    if (token.stop_requested()) {
        return fail(ParseError::Cancelled);
    }
    return tree;
}

}  // namespace nmxd
