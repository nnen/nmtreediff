/// \file
/// \brief Implementation of the generic JSON provider.

#include "formats/json_generic.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "core/builder.h"
#include "core/dom.h"
#include "core/hash.h"
#include "core/shape.h"

namespace nmtreediff {

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

/// \brief How many parser depths a document needs beyond its deepest nesting.
///
/// \remarks simdjson counts the document itself as depth one, its root
///          container's members as depth two, and so on, so a value inside
///          `n` open containers sits at depth `n + 1`. The parser's limit has
///          to be strictly greater than the deepest depth it visits.
constexpr std::size_t kDepthHeadroom = 2;

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

/// \brief Measures how many containers a document has open at its deepest.
///
/// \param text The document, which need not be well formed.
///
/// \returns The largest number of objects and arrays open at once.
///
/// \remarks simdjson's parser is allocated for a fixed depth, 1024 unless told
///          otherwise, and its On Demand iterator does not refuse a deeper
///          document: it records where each level started in an array of
///          that size and asserts, in a build with its development checks
///          on, that the level fits. Generated data nests deeper than that,
///          so the parser is sized from the document instead.
///
///          Brackets inside strings are skipped the way the parser's own
///          first stage skips them, which makes the answer exact for any
///          document that stage accepts. A malformed document may close more
///          than it opened, hence the guard against counting below zero.
std::size_t deepestNesting(std::string_view text) {
    std::size_t open = 0;
    std::size_t deepest = 0;
    bool inString = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            // An escaped byte is never the closing quote, so step over it.
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            deepest = std::max(deepest, ++open);
        } else if ((c == '}' || c == ']') && open > 0) {
            --open;
        }
    }
    return deepest;
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

/// \brief Reports whether a node is an object.
///
/// \param node The node to inspect.
///
/// \returns `true` when the node was built from a JSON object.
bool isObject(const Node& node) {
    const Property* type = node.findProperty(kTypeProperty);
    return type != nullptr && type->value == kObjectType;
}

/// \brief Subtitle candidates, in order of preference.
///
/// \remarks A scalar element's own value first, then the identifying members,
///          then the container type, so a card always has a second line and
///          it is the most telling one the node has.
constexpr std::array<std::string_view, 6> kSubtitleProperties{"#value", "id",  "name",
                                                              "type",   "key", "#type"};

/// \brief Walks one parsed document and fills a Tree from it, as written.
///
/// \remarks Every object and every array becomes a node and every array
///          element becomes an `item` node, scalar or not. Deciding which
///          arrays are really lists of values is shape()'s job, over the tree
///          this produces.
///
///          Kept as a class rather than a set of free functions because every
///          step needs the same four things: the tree being filled, the buffer
///          the spans are offsets into, the document whose cursor says where
///          parsing has reached, and the stop token.
class Reader {
public:
    /// \brief Prepares to read one document.
    ///
    /// \param tree The tree to fill.
    /// \param buffer The padded copy of the document being walked.
    /// \param origin Offset of \p buffer within the original file, which is
    ///        non-zero only when a byte order mark was skipped.
    /// \param document The document cursor, used to find where a container
    ///        ended.
    /// \param token Checked periodically while reading.
    Reader(Tree& tree, std::string_view buffer, std::uint32_t origin, ondemand::document& document,
           const std::stop_token& token)
        : tree_(tree), buffer_(buffer), origin_(origin), document_(document), token_(token) {}

    /// \brief Reads the whole tree from the document root.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks A document holding nothing but a scalar is a valid JSON
    ///          document and becomes a single node, so that a file consisting
    ///          of one number still diffs instead of being refused.
    simdjson::error_code readRoot() {
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
        if (const auto error =
                open(kInvalidNode, std::string(kRootKind), root.raw_json_token().data(), root, type)) {
            return error;
        }
        return walk();
    }

    /// \brief Reports whether the walk stopped because it was cancelled.
    ///
    /// \returns `true` when the stop token was signalled mid-walk.
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

private:
    /// \brief One container whose members are still being read.
    ///
    /// \remarks On Demand hands a container out as a forward-only iterator,
    ///          so a stack of these is the parser's own shape: the innermost
    ///          open container on top, its iterator advanced only once the
    ///          member it points at has been fully consumed.
    struct Frame {
        /// \brief The node standing for the container.
        NodeId id = kInvalidNode;
        /// \brief Whether the container is an object rather than an array.
        bool isObject = false;
        /// \brief The members of an object.
        ondemand::object_iterator objectIt;
        ondemand::object_iterator objectEnd;
        /// \brief The elements of an array.
        ondemand::array_iterator arrayIt;
        ondemand::array_iterator arrayEnd;
    };

    /// \brief Reads every container on the stack until it is empty.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks The loop looks at the top frame, reads its next member, and
    ///          either records a scalar and moves on, or opens a container as
    ///          a new frame above it. A frame with nothing left closes its
    ///          node's span, pops, and advances the iterator below it past the
    ///          container that was just consumed. No call frame per level.
    simdjson::error_code walk() {
        while (!stack_.empty()) {
            if (tree_.size() % kCancelCheckInterval == 0 && token_.stop_requested()) {
                cancelled_ = true;
                return simdjson::SUCCESS;
            }

            Frame& frame = stack_.back();
            const bool exhausted =
                frame.isObject ? !(frame.objectIt != frame.objectEnd) : !(frame.arrayIt != frame.arrayEnd);
            if (exhausted) {
                tree_.node(frame.id).span.end = cursor();
                stack_.pop_back();
                if (!stack_.empty()) {
                    advance(stack_.back());
                }
                continue;
            }

            const auto error = frame.isObject ? readMember(frame) : readElement(frame);
            if (error) {
                return error;
            }
        }
        return simdjson::SUCCESS;
    }

    /// \brief Steps a frame's iterator past the member just consumed.
    ///
    /// \param frame The frame to advance.
    static void advance(Frame& frame) {
        if (frame.isObject) {
            ++frame.objectIt;
        } else {
            ++frame.arrayIt;
        }
    }

    /// \brief Reads the member an object frame is pointing at.
    ///
    /// \param frame The object frame.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks A member holding an object or an array becomes a child node,
    ///          and a member holding a scalar becomes a property. That is what
    ///          keeps the node view showing structure rather than one card per
    ///          string.
    simdjson::error_code readMember(Frame& frame) {
        ondemand::field member;
        if (const auto error = (*frame.objectIt).get(member)) {
            return error;
        }

        // Both have to be read before the value is touched, because each is
        // recovered from where the cursor currently stands.
        const std::string_view key = member.escaped_key();
        const char* keyBegin = member.key_raw_json_token().data();

        ondemand::value value = member.value();
        ondemand::json_type type{};
        if (const auto error = value.type().get(type)) {
            return error;
        }

        if (type == ondemand::json_type::object || type == ondemand::json_type::array) {
            // The frame below advances once this container has been consumed,
            // which is when its own frame closes.
            return open(frame.id, std::string(key), keyBegin, value, type);
        }

        const std::string_view raw = trimRight(value.raw_json_token());
        tree_.addProperty(frame.id, std::string(key), std::string(raw),
                          SourceSpan{offsetOf(keyBegin), endOf(raw)});
        if (const auto error = validateScalar(type, value)) {
            return error;
        }
        advance(frame);
        return simdjson::SUCCESS;
    }

    /// \brief Reads the element an array frame is pointing at.
    ///
    /// \param frame The array frame.
    ///
    /// \returns simdjson::SUCCESS, or the first error the walk ran into.
    ///
    /// \remarks Every element becomes a node, scalar or not. A scalar
    ///          element's own value is all it has, so it becomes the one
    ///          property rather than a nameless node.
    simdjson::error_code readElement(Frame& frame) {
        ondemand::value element;
        if (const auto error = (*frame.arrayIt).get(element)) {
            return error;
        }
        const char* begin = element.raw_json_token().data();
        ondemand::json_type type{};
        if (const auto error = element.type().get(type)) {
            return error;
        }

        if (type == ondemand::json_type::object || type == ondemand::json_type::array) {
            return open(frame.id, std::string(kElementKind), begin, element, type);
        }

        const std::string_view raw = trimRight(element.raw_json_token());
        const NodeId id = tree_.add(frame.id, std::string(kElementKind),
                                    SourceSpan{offsetOf(raw.data()), endOf(raw)});
        tree_.addProperty(id, std::string(kValueProperty), std::string(raw),
                          SourceSpan{offsetOf(raw.data()), endOf(raw)});
        if (const auto error = validateScalar(type, element)) {
            return error;
        }
        advance(frame);
        return simdjson::SUCCESS;
    }

    /// \brief Adds a container as a node and pushes it as the open frame.
    ///
    /// \param parent The parent node's id, or kInvalidNode for the root.
    /// \param kind The kind to give the new node.
    /// \param spanBegin Where the node starts in the buffer, which is the key
    ///        token for a member of an object and the value token otherwise.
    /// \param value The container. Consumed by the frame over time.
    /// \param type Whether it is an object or an array.
    ///
    /// \returns simdjson::SUCCESS, or the error obtaining the container gave.
    simdjson::error_code open(NodeId parent, std::string kind, const char* spanBegin,
                              ondemand::value& value, ondemand::json_type type) {
        const std::uint32_t begin = offsetOf(spanBegin);
        const NodeId id = tree_.add(parent, std::move(kind), SourceSpan{begin, begin});

        Frame frame;
        frame.id = id;
        frame.isObject = type == ondemand::json_type::object;
        if (frame.isObject) {
            // Reordering the members of an object changes nothing about the
            // document, so a reordering there is not a move and is not
            // reported as one. Reordering an array changes the document.
            tree_.node(id).childrenOrdered = false;
            tree_.addProperty(id, std::string(kTypeProperty), std::string(kObjectType));
            ondemand::object object;
            if (const auto error = value.get_object().get(object)) {
                return error;
            }
            if (const auto error = object.begin().get(frame.objectIt)) {
                return error;
            }
            if (const auto error = object.end().get(frame.objectEnd)) {
                return error;
            }
        } else {
            tree_.addProperty(id, std::string(kTypeProperty), std::string(kArrayType));
            ondemand::array array;
            if (const auto error = value.get_array().get(array)) {
                return error;
            }
            if (const auto error = array.begin().get(frame.arrayIt)) {
                return error;
            }
            if (const auto error = array.end().get(frame.arrayEnd)) {
                return error;
            }
        }
        stack_.push_back(frame);
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
    std::vector<Frame> stack_;
    bool cancelled_ = false;
};

/// \brief Reads JSON as written, then folds lists of values into properties.
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

    Result<Tree, ParseError> read(const SourceFile& source, std::stop_token token) const override {
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
        // The parser is sized for this document's own depth rather than left
        // at simdjson's default, which a generated file can nest past.
        if (parser.allocate(buffer.size(), deepestNesting(text) + kDepthHeadroom)) {
            return fail(ParseError::NotWellFormed);
        }
        if (parser.iterate(buffer).get(document)) {
            return fail(ParseError::NotWellFormed);
        }

        Tree tree;
        tree.setFormatName(std::string(name()));

        Reader reader(tree, std::string_view(buffer.data(), buffer.size()), origin, document, token);
        if (reader.readRoot() != simdjson::SUCCESS) {
            return fail(ParseError::NotWellFormed);
        }
        if (reader.cancelled()) {
            return fail(ParseError::Cancelled);
        }
        // On Demand parsing stops as soon as the root value is complete, so a
        // second document appended to the first would otherwise go unnoticed.
        if (!document.at_end()) {
            return fail(ParseError::NotWellFormed);
        }

        tree.finalize();
        computeHashes(tree, token);
        return tree;
    }

    /// \brief Folds arrays of values into sequence properties.
    ///
    /// \param context The document as read, and the builder.
    ///
    /// \remarks The rule, stated precisely because it decides what every JSON
    ///          file turns into: an array becomes a sequence property when
    ///          every element is a scalar or an array that is itself a
    ///          sequence property, and a node otherwise. A four by four
    ///          transform matrix is then one property with parts rather than
    ///          sixteen anonymous nodes four levels deep, while an array
    ///          holding an object stays a node, because an object has named
    ///          fields a reader will want matched against their counterparts.
    ///
    ///          The root array is no exception: it hangs from the root node as
    ///          its `#value`, so a root list and a nested one diff alike.
    ///
    ///          One pass in arena order. An element under a node is a node or
    ///          a sequence; an element under a sequence is an item. Whether an
    ///          array qualifies is read from a table filled backwards first,
    ///          so no element is looked at twice and nothing recurses.
    void shape(ShapeContext& context) const override {
        const Dom& dom = context.dom();
        const Tree& raw = dom.tree();
        TreeBuilder& out = context.out();
        if (raw.empty()) {
            return;
        }

        const std::vector<bool> holdsObject = objectsBelow(raw);
        std::vector<RefId> made(raw.size());
        for (DomId id = 0; id < raw.size(); ++id) {
            const DomNode element = dom.at(id);
            const Node& node = raw.node(id);
            const DomNode parent = element.parent();
            const bool foldable = isArray(node) && !holdsObject[id];

            Ref ref;
            if (!parent.valid()) {
                ref = placeRoot(out, element, node, foldable);
            } else {
                Ref above = out.at(made[parent.id()]);
                ref = above.isNode() ? placeUnderNode(above, element, node, raw, foldable)
                                     : placeItem(above, element, node);
            }
            made[id] = ref.id();
        }
    }

    std::span<const std::string_view> subtitleProperties() const override {
        return kSubtitleProperties;
    }

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        if (propertyName == kTypeProperty || propertyName == kValueProperty) {
            return static_cast<int>(kLeadingProperties.size()) + 1;
        }
        return rankFromList(kLeadingProperties, propertyName);
    }

private:
    /// \brief Works out, for every node, whether an object sits at or below it.
    ///
    /// \param raw The tree as read.
    ///
    /// \returns One answer per node, indexed by id.
    ///
    /// \remarks Children always have a higher index than their parent, so one
    ///          backward pass answers every node from its children's answers.
    static std::vector<bool> objectsBelow(const Tree& raw) {
        std::vector<bool> holds(raw.size(), false);
        for (std::size_t i = raw.size(); i-- > 0;) {
            const Node& node = raw.node(static_cast<NodeId>(i));
            bool any = isObject(node);
            for (const NodeId child : node.children) {
                any = any || holds[child];
            }
            holds[i] = any;
        }
        return holds;
    }

    /// \brief Places the document's outermost value.
    ///
    /// \param out The builder.
    /// \param element The root element.
    /// \param node The same, as the tree holds it.
    /// \param foldable Whether the root is an array that qualifies as a
    ///        sequence.
    ///
    /// \returns The handle the root's children attach to: the root node, or
    ///          its `#value` sequence when the root is a list of values.
    static Ref placeRoot(TreeBuilder& out, const DomNode& element, const Node& node,
                         bool foldable) {
        Ref root = out.root(element.name(), element.span());
        root.setSource(element.id());
        copyProperties(root, element);
        root.setChildrenOrdered(node.childrenOrdered);
        if (!foldable) {
            return root;
        }
        Ref sequence = root.sequence(kValueProperty);
        sequence.setSpan(element.span());
        return sequence;
    }

    /// \brief Places an element whose parent became a node.
    ///
    /// \param above The parent's handle.
    /// \param element The element to place.
    /// \param node The same, as the tree holds it.
    /// \param raw The tree as read, for the parent's kind.
    /// \param foldable Whether the element is an array that qualifies as a
    ///        sequence.
    ///
    /// \returns The new node, or the new sequence property.
    static Ref placeUnderNode(Ref& above, const DomNode& element, const Node& node,
                              const Tree& raw, bool foldable) {
        if (foldable) {
            Ref sequence = above.sequence(element.name());
            sequence.setSpan(element.span());
            sequence.setSource(element.id());
            return sequence;
        }

        Ref made = above.child(element);
        copyProperties(made, element);
        made.setChildrenOrdered(node.childrenOrdered);
        // An element's own kind is the same throughout the document, so it
        // borrows its parent's key for colour; otherwise every array in a file
        // would be painted alike.
        if (node.kind == kElementKind) {
            made.setAccent(accentForKind(raw.node(node.parent).kind));
        }
        return made;
    }

    /// \brief Places an element whose parent became a sequence property.
    ///
    /// \param above The sequence.
    /// \param element The element to place, which is an item of the array.
    /// \param node The same, as the tree holds it.
    ///
    /// \returns The new item: a scalar with the element's value, or a nested
    ///          sequence.
    static Ref placeItem(Ref& above, const DomNode& element, const Node& node) {
        Ref item;
        if (isArray(node)) {
            item = above.child();
            item.setForm(PropertyForm::Sequence);
        } else {
            const Property* value = node.findProperty(kValueProperty);
            item = above.item(value != nullptr ? std::string_view(value->value) : std::string_view{});
        }
        item.setSpan(element.span());
        item.setSource(element.id());
        return item;
    }

    /// \brief Copies an element's properties onto its node.
    static void copyProperties(Ref& into, const DomNode& element) {
        for (const DomProperty property : element.properties()) {
            into.property(property);
        }
    }
};

}  // namespace

std::unique_ptr<IFormatProvider> makeGenericJsonProvider() {
    return std::make_unique<GenericJsonProvider>();
}

}  // namespace nmtreediff
