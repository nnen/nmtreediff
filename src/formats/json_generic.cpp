/// \file
/// \brief Implementation of the generic JSON provider.

#include "formats/json_generic.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include <simdjson.h>

#include "core/hash.h"

namespace nmxd {

namespace {

namespace ondemand = simdjson::ondemand;

/// \brief The kind given to the document's outermost node.
///
/// \remarks Borrowed from the JSONPath spelling of the root, so that a path
///          printed by a report reads the way someone used to JSON tooling
///          expects it to.
constexpr std::string_view kRootKind = "$";

/// \brief The kind given to every element of an array.
///
/// \remarks Deliberately the same for every element of every array. An element
///          has no name of its own, and putting its index in the kind would
///          make moving it look like turning it into a different sort of node,
///          which is exactly the mistake a tree diff exists to avoid.
constexpr std::string_view kElementKind = "item";

/// \brief The property recording whether a node is an object or an array.
///
/// \remarks A node's kind carries its member key, which is what makes a report
///          path readable, so it cannot also carry the JSON type. Without this
///          property an empty object replaced by an empty array under the same
///          key would hash the same and be reported as unchanged.
constexpr std::string_view kTypeProperty = "#type";

/// \brief The value of kTypeProperty on an object node.
constexpr std::string_view kObjectType = "object";

/// \brief The value of kTypeProperty on an array node.
constexpr std::string_view kArrayType = "array";

/// \brief Member names worth seeing first on a node card.
///
/// \remarks Anything unlisted keeps document order behind them. The two
///          synthetic properties sort last because they describe the node
///          rather than identify it.
constexpr std::array<std::string_view, 4> kLeadingProperties{"id", "name", "type", "key"};

/// \brief Extensions this format claims outright.
///
/// \remarks A studio asset with an unfamiliar suffix still reaches the sniffing
///          path, so this list does not have to be exhaustive.
constexpr std::array<std::string_view, 3> kExtensions{".json", ".geojson", ".webmanifest"};

/// \brief How many nodes are built between two checks of the stop token.
///
/// \remarks Checking every node would cost more than the check is worth on a
///          document small enough to finish before anyone could cancel it.
constexpr std::size_t kCancelCheckInterval = 4096;

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

/// \brief Returns a file's extension in lower case.
///
/// \param source The file to inspect.
///
/// \returns The extension including its leading dot, or an empty string when the
///          file has none.
std::string lowerExtension(const SourceFile& source) {
    std::string extension = source.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

/// \brief Reports whether a node is an array.
///
/// \param node The node to inspect.
///
/// \returns `true` when the node was built from a JSON array.
bool isArray(const Node& node) {
    const Property* type = node.findProperty(kTypeProperty);
    return type != nullptr && type->value == kArrayType;
}

/// \brief Walks one parsed document and fills a Tree from it.
///
/// \remarks Kept as a class rather than a set of free functions because every
///          step needs the same four things: the tree being filled, the buffer
///          the spans are offsets into, the document whose cursor says where
///          parsing has reached, and the stop token.
class Builder {
public:
    /// \brief Prepares to build one tree.
    ///
    /// \param tree The tree to fill.
    /// \param buffer The padded copy of the document being walked.
    /// \param origin Offset of \p buffer within the original file, which is
    ///        non-zero only when a byte order mark was skipped.
    /// \param document The document cursor, used to find where a container
    ///        ended.
    /// \param token Checked periodically while building.
    Builder(Tree& tree, std::string_view buffer, std::uint32_t origin, ondemand::document& document,
            const std::stop_token& token)
        : tree_(tree), buffer_(buffer), origin_(origin), document_(document), token_(token) {}

    /// \brief Builds the whole tree from the document root.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks A document holding nothing but a scalar is a valid JSON
    ///          document and becomes a single node, so that a file consisting
    ///          of one number still diffs instead of being refused.
    simdjson::error_code buildRoot() {
        ondemand::json_type type{};
        if (const auto error = document_.type().get(type)) {
            return error;
        }

        if (type != ondemand::json_type::object && type != ondemand::json_type::array) {
            std::string_view raw;
            if (const auto error = document_.raw_json().get(raw)) {
                return error;
            }
            raw = trimRight(raw);
            const NodeId id = tree_.add(kInvalidNode, std::string(kRootKind),
                                        SourceSpan{offsetOf(raw.data()), endOf(raw)});
            tree_.addProperty(id, std::string(kValueProperty), std::string(raw),
                              SourceSpan{offsetOf(raw.data()), endOf(raw)});
            return validateScalar(type);
        }

        ondemand::value root;
        if (const auto error = document_.get_value().get(root)) {
            return error;
        }
        return build(kInvalidNode, std::string(kRootKind), root.raw_json_token().data(), root);
    }

    /// \brief Reports whether the walk stopped because it was cancelled.
    ///
    /// \returns `true` when the stop token was signalled mid-walk.
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

private:
    /// \brief Builds one node and everything below it.
    ///
    /// \param parent The parent node's id, or kInvalidNode for the root.
    /// \param kind The kind to give the new node.
    /// \param spanBegin Where the node starts in the buffer, which is the key
    ///        token for a member of an object and the value token otherwise.
    /// \param value The value to convert. Consumed by this call.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    simdjson::error_code build(NodeId parent, std::string kind, const char* spanBegin,
                               ondemand::value value) {
        if (tree_.size() % kCancelCheckInterval == 0 && token_.stop_requested()) {
            cancelled_ = true;
            return simdjson::SUCCESS;
        }

        ondemand::json_type type{};
        if (const auto error = value.type().get(type)) {
            return error;
        }

        const std::uint32_t begin = offsetOf(spanBegin);
        const NodeId id = tree_.add(parent, std::move(kind), SourceSpan{begin, begin});

        switch (type) {
            case ondemand::json_type::object: {
                tree_.addProperty(id, std::string(kTypeProperty), std::string(kObjectType));
                if (const auto error = buildObject(id, value)) {
                    return error;
                }
                break;
            }
            case ondemand::json_type::array: {
                tree_.addProperty(id, std::string(kTypeProperty), std::string(kArrayType));
                ondemand::array array;
                if (const auto error = value.get_array().get(array)) {
                    return error;
                }
                if (const auto error = buildArrayElements(id, array)) {
                    return error;
                }
                break;
            }
            default: {
                // A scalar array element. Its own value is all it has, so it
                // becomes the one property rather than a nameless node.
                const std::string_view raw = trimRight(value.raw_json_token());
                tree_.addProperty(id, std::string(kValueProperty), std::string(raw),
                                  SourceSpan{offsetOf(raw.data()), endOf(raw)});
                tree_.node(id).span.end = endOf(raw);
                return validateScalar(type, value);
            }
        }

        // Only now that every child is placed, because adding one invalidates
        // any reference taken before it.
        tree_.node(id).span.end = cursor();
        return simdjson::SUCCESS;
    }

    /// \brief Adds an object's members to a node.
    ///
    /// \param id The node standing for the object.
    /// \param value The object.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks A member holding an object or an array becomes a child node,
    ///          and a member holding a scalar becomes a property. That is what
    ///          keeps the node view showing structure rather than one card per
    ///          string.
    simdjson::error_code buildObject(NodeId id, ondemand::value& value) {
        ondemand::object object;
        if (const auto error = value.get_object().get(object)) {
            return error;
        }

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
            ondemand::json_type memberType{};
            if (const auto error = memberValue.type().get(memberType)) {
                return error;
            }

            if (memberType == ondemand::json_type::array) {
                ondemand::array array;
                if (const auto error = memberValue.get_array().get(array)) {
                    return error;
                }
                bool asProperty = false;
                if (const auto error = arrayBecomesProperty(array, asProperty)) {
                    return error;
                }

                if (asProperty) {
                    Property property;
                    property.name = std::string(key);
                    if (const auto error = collectArrayProperty(property, array)) {
                        return error;
                    }
                    property.span = SourceSpan{offsetOf(keyBegin), cursor()};
                    tree_.addProperty(id, std::move(property));
                } else if (const auto error =
                               buildArrayNode(id, std::string(key), keyBegin, array)) {
                    return error;
                }
                if (cancelled_) {
                    return simdjson::SUCCESS;
                }
            } else if (memberType == ondemand::json_type::object) {
                if (const auto error = build(id, std::string(key), keyBegin, memberValue)) {
                    return error;
                }
                if (cancelled_) {
                    return simdjson::SUCCESS;
                }
            } else {
                const std::string_view raw = trimRight(memberValue.raw_json_token());
                tree_.addProperty(id, std::string(key), std::string(raw),
                                  SourceSpan{offsetOf(keyBegin), endOf(raw)});
                if (const auto error = validateScalar(memberType, memberValue)) {
                    return error;
                }
            }
        }
        return simdjson::SUCCESS;
    }

    /// \brief Reports whether an array should become one property.
    ///
    /// \param array The array to look at, left rewound afterwards.
    /// \param answer Set to `true` when the array holds no object.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks An array becomes a property when every element is a scalar,
    ///          or is itself an array that becomes one. An array holding an
    ///          object stays a node, because an object has named fields a reader
    ///          will want matched against their counterparts, and matching is
    ///          what nodes are for.
    ///
    ///          The recursive half is what makes a four by four transform matrix
    ///          one property with parts rather than sixteen anonymous nodes four
    ///          levels deep.
    ///
    ///          This costs a second pass over the array. On Demand parsing is
    ///          forward only, so the alternative was to build optimistically and
    ///          unpick it on meeting an object, which is more code and more ways
    ///          to be wrong.
    simdjson::error_code arrayBecomesProperty(ondemand::array& array, bool& answer) {
        answer = true;
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
                answer = false;
            } else if (type == ondemand::json_type::array) {
                ondemand::array nested;
                if (const auto error = element.get_array().get(nested)) {
                    return error;
                }
                bool nestedAnswer = true;
                if (const auto error = arrayBecomesProperty(nested, nestedAnswer)) {
                    return error;
                }
                answer = answer && nestedAnswer;
            }
        }

        // Walked to the end even once the answer is known, because leaving the
        // iterator part way through is what would make the rewind unreliable.
        bool rewound = false;
        return array.reset().get(rewound);
    }

    /// \brief Collects an array into one property with parts.
    ///
    /// \param into The property to fill in.
    /// \param array The array to read.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks The parts have no names, because a position in a list is not
    ///          a name. They are marked as a sequence, so reordering them is a
    ///          change while reordering a record's fields is not.
    simdjson::error_code collectArrayProperty(Property& into, ondemand::array& array) {
        into.ordered = true;
        for (auto elementResult : array) {
            ondemand::value element;
            if (const auto error = std::move(elementResult).get(element)) {
                return error;
            }
            ondemand::json_type type{};
            if (const auto error = element.type().get(type)) {
                return error;
            }

            Property part;
            if (type == ondemand::json_type::array) {
                ondemand::array nested;
                if (const auto error = element.get_array().get(nested)) {
                    return error;
                }
                const char* begin = element.raw_json_token().data();
                if (const auto error = collectArrayProperty(part, nested)) {
                    return error;
                }
                part.span = SourceSpan{offsetOf(begin), cursor()};
            } else {
                const std::string_view raw = trimRight(element.raw_json_token());
                part.value = std::string(raw);
                part.span = SourceSpan{offsetOf(raw.data()), endOf(raw)};
                if (const auto error = validateScalar(type, element)) {
                    return error;
                }
            }
            into.children.push_back(std::move(part));
        }
        return simdjson::SUCCESS;
    }

    /// \brief Adds an array that stays a node, given the array itself.
    ///
    /// \param parent The node the array hangs from.
    /// \param kind The member key the array appeared under.
    /// \param spanBegin Where the member starts in the source.
    /// \param array The array, already obtained by the caller.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks Separate from build() because deciding whether an array is a
    ///          property means obtaining it first, and On Demand hands a value
    ///          out once.
    simdjson::error_code buildArrayNode(NodeId parent, std::string kind, const char* spanBegin,
                                        ondemand::array& array) {
        const std::uint32_t begin = offsetOf(spanBegin);
        const NodeId id = tree_.add(parent, std::move(kind), SourceSpan{begin, begin});
        tree_.addProperty(id, std::string(kTypeProperty), std::string(kArrayType));
        if (const auto error = buildArrayElements(id, array)) {
            return error;
        }
        tree_.node(id).span.end = cursor();
        return simdjson::SUCCESS;
    }

    /// \brief Adds an array's elements to a node.
    ///
    /// \param id The node standing for the array.
    /// \param value The array.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks Every element becomes a node, scalar or not, because position
    ///          in an array is meaningful and a value that only exists as a
    ///          property cannot be reported as having moved.
    simdjson::error_code buildArrayElements(NodeId id, ondemand::array& array) {
        for (auto elementResult : array) {
            ondemand::value element;
            if (const auto error = std::move(elementResult).get(element)) {
                return error;
            }
            const char* begin = element.raw_json_token().data();
            if (const auto error = build(id, std::string(kElementKind), begin, element)) {
                return error;
            }
            if (cancelled_) {
                return simdjson::SUCCESS;
            }
        }
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

    Tree& tree_;
    std::string_view buffer_;
    std::uint32_t origin_ = 0;
    ondemand::document& document_;
    const std::stop_token& token_;
    bool cancelled_ = false;
};

/// \brief Treats every object, array and array element as a node.
class GenericJsonProvider final : public IFormatProvider {
public:
    std::string_view name() const override { return "json"; }
    std::string_view displayName() const override { return "JSON (generic)"; }

    std::span<const std::string_view> defaultExtensions() const override { return kExtensions; }

    int score(const SourceFile& source) const override {
        const std::string extension = lowerExtension(source);
        if (extension == ".json") {
            return 90;
        }
        if (claimsExtension(source)) {
            return 80;
        }

        // No familiar extension, so look at the head of the file. A game
        // configuration file called .cfg or .asset is still JSON underneath,
        // and an opening brace or bracket is a strong enough signal that no
        // other built-in format competes for it.
        std::string_view head = source.text().substr(0, std::min<std::size_t>(256, source.size()));
        if (startsWith(head, "\xEF\xBB\xBF")) {
            head.remove_prefix(3);
        }
        while (!head.empty() && isSpace(head.front())) {
            head.remove_prefix(1);
        }
        if (!head.empty() && (head.front() == '{' || head.front() == '[')) {
            return 60;
        }
        return 0;
    }

    Result<Tree, ParseError> parse(const SourceFile& source, std::stop_token token) const override {
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

        Tree tree;
        tree.setFormatName(std::string(name()));

        Builder builder(tree, std::string_view(buffer.data(), buffer.size()), origin, document,
                        token);
        if (builder.buildRoot() != simdjson::SUCCESS) {
            return fail(ParseError::NotWellFormed);
        }
        if (builder.cancelled()) {
            return fail(ParseError::Cancelled);
        }
        // On Demand parsing stops as soon as the root value is complete, so a
        // second document appended to the first would otherwise go unnoticed.
        if (!document.at_end()) {
            return fail(ParseError::NotWellFormed);
        }

        tree.finalize();
        computeHashes(tree, *this, token);
        return tree;
    }

    IdentityKey identity(const Tree& tree, NodeId id) const override {
        // Generic JSON has no identifier it can trust. A member key is a hint
        // and nothing more, and an array element has not even that: its
        // position is the only thing naming it, and position is what the
        // matcher is trying to work out.
        const Node& node = tree.node(id);
        if (node.kind == kElementKind) {
            return IdentityKey{};
        }
        return IdentityKey{false, node.kind};
    }

    NodeStyle style(const Tree& tree, NodeId id) const override {
        const Node& node = tree.node(id);

        NodeStyle style;
        style.title = node.kind;

        if (const Property* value = node.findProperty(kValueProperty)) {
            style.subtitle = value->value;
        } else {
            for (const auto candidate : kLeadingProperties) {
                if (const Property* p = node.findProperty(candidate)) {
                    style.subtitle = p->value;
                    break;
                }
            }
            if (style.subtitle.empty()) {
                style.subtitle = isArray(node) ? std::string(kArrayType) : std::string(kObjectType);
            }
        }

        // Colour derived from the member key, so every node under "enemies"
        // looks alike and one under "props" looks different. Array elements
        // borrow their parent's key, because their own kind is the same
        // throughout the document and would otherwise paint every array in a
        // file the same colour.
        std::string_view palette = node.kind;
        if (node.kind == kElementKind && node.parent != kInvalidNode) {
            palette = tree.node(node.parent).kind;
        }
        const std::uint64_t h = hashBytes(palette);
        style.accent = Color{static_cast<std::uint8_t>(110 + (h & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 8) & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 16) & 0x3F)), 255};
        return style;
    }

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        if (propertyName == kTypeProperty || propertyName == kValueProperty) {
            return static_cast<int>(kLeadingProperties.size()) + 1;
        }
        return rankFromList(kLeadingProperties, propertyName);
    }

    bool childrenOrdered(const Tree& tree, NodeId id) const override {
        // The hook that generic XML never exercises. Reordering the members of
        // an object changes nothing about the document, so a reordering there
        // is not a move and should not be reported as one. Reordering an array
        // changes the document, so it is.
        return isArray(tree.node(id));
    }
};

}  // namespace

std::unique_ptr<IFormatProvider> makeGenericJsonProvider() {
    return std::make_unique<GenericJsonProvider>();
}

}  // namespace nmxd
