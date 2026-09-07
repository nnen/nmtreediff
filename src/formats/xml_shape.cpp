/// \file
/// \brief Implementation of the XML shaping driver.

#include "formats/xml_shape.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "core/hash.h"
#include "formats/xml_spans.h"

namespace nmxd {

namespace {

/// \brief How many elements are opened between cancellation checks.
constexpr std::size_t kCancelCheckInterval = 1024;

/// \brief Reports whether text begins with a prefix.
///
/// \param text The text to test.
/// \param prefix The prefix to look for.
///
/// \returns `true` when \p text starts with \p prefix.
bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// \brief Returns where an element's start tag begins.
///
/// \param element The element.
///
/// \returns The offset of its opening angle bracket.
///
/// \remarks pugixml's offset points at the element name, one byte past the
///          bracket.
std::uint32_t startOf(const pugi::xml_node& element) {
    const auto nameOffset = static_cast<std::uint32_t>(element.offset_debug());
    return nameOffset > 0 ? nameOffset - 1 : 0;
}

/// \brief Turns an element's attributes into properties with spans.
///
/// \param element The element.
/// \param text The whole document.
/// \param begin Offset of the start tag's opening bracket.
/// \param startTagEnd Offset just past the start tag.
///
/// \returns One property per attribute, in document order.
///
/// \remarks pugixml reports offsets for nodes but not for attributes, so the
///          start tag is scanned once and its spans matched up with the parsed
///          attributes, which arrive in the same order.
std::vector<Property> attributesOf(const pugi::xml_node& element, std::string_view text,
                                   std::uint32_t begin, std::uint32_t startTagEnd) {
    const std::vector<SourceSpan> spans = scanAttributeSpans(text, begin, startTagEnd);
    std::vector<Property> attributes;
    std::size_t spanIndex = 0;
    for (const pugi::xml_attribute& attribute : element.attributes()) {
        Property property;
        property.name = attribute.name();
        property.value = attribute.value();
        property.span = spanIndex < spans.size() ? spans[spanIndex] : SourceSpan{};
        ++spanIndex;
        attributes.push_back(std::move(property));
    }
    return attributes;
}

/// \brief Finds the text content of an element that holds no elements.
///
/// \param element The element.
/// \param text Receives the text.
/// \param span Receives where the text sits.
///
/// \returns `true` when there is text, `false` when there is none.
bool leafText(const pugi::xml_node& element, std::string& text, SourceSpan& span) {
    const char* value = element.child_value();
    if (value == nullptr || *value == '\0') {
        return false;
    }
    for (const pugi::xml_node& child : element.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            const auto offset = static_cast<std::uint32_t>(child.offset_debug());
            text = value;
            span = SourceSpan{offset, offset + static_cast<std::uint32_t>(text.size())};
            return true;
        }
    }
    return false;
}

/// \brief Returns the last element inside another.
///
/// \param element The element to look in.
///
/// \returns The last element child, or an empty handle when there is none.
pugi::xml_node lastElementChild(const pugi::xml_node& element) {
    for (pugi::xml_node child = element.last_child(); child; child = child.previous_sibling()) {
        if (child.type() == pugi::node_element) {
            return child;
        }
    }
    return pugi::xml_node{};
}

/// \brief Finds where an element ends without having walked it.
///
/// \param element The element.
/// \param text The whole document.
///
/// \returns The offset just past its closing tag, or past its own tag when
///          it closes itself.
///
/// \remarks For a subtree the shaper asked not to visit. The closing tag is
///          searched for from the end of the last element inside, so a
///          descendant's closing tag is never mistaken for this one.
std::uint32_t unvisitedEnd(const pugi::xml_node& element, std::string_view text) {
    const std::uint32_t startTagEnd = endOfTag(text, startOf(element));
    if (isSelfClosing(text, startTagEnd)) {
        return startTagEnd;
    }
    const pugi::xml_node last = lastElementChild(element);
    return endOfClosingTag(text, last ? unvisitedEnd(last, text) : startTagEnd);
}

/// \brief Walks a parsed document and reports its elements to a session.
class XmlWalker {
public:
    /// \brief Prepares a walk.
    ///
    /// \param session The session to report to.
    /// \param text The whole document, for spans.
    /// \param token Checked on a bounded interval.
    XmlWalker(ShapeSession& session, std::string_view text, const std::stop_token& token)
        : session_(session), text_(text), token_(token) {}

    /// \brief Walks everything under the document element.
    ///
    /// \param root The document element.
    ///
    /// \returns `false` when the walk was cancelled part way.
    bool walk(const pugi::xml_node& root) {
        openElement(root);
        while (!stack_.empty()) {
            if (cancelled_) {
                return false;
            }

            // Step into the next element child, if the shaper wants it.
            Visit& top = stack_.back();
            const pugi::xml_node child = nextElement(top.next);
            if (child && session_.descend()) {
                top.next = child.next_sibling();
                top.hasElementChild = true;
                openElement(child);
                continue;
            }

            // Nothing left inside, so the element ends here.
            const Visit finished = top;
            stack_.pop_back();
            closeElement(finished);
        }
        return !cancelled_;
    }

private:
    /// \brief One element the walk is inside of.
    struct Visit {
        pugi::xml_node element;   ///< The element itself.
        pugi::xml_node next;      ///< The next child to look at.
        std::uint32_t startTagEnd = 0;  ///< Offset just past its start tag.
        bool hasElementChild = false;   ///< Whether an element was found inside.
    };

    /// \brief Skips to the next sibling that is an element.
    ///
    /// \param from The node to start at, inclusive.
    ///
    /// \returns That element, or an empty handle at the end.
    static pugi::xml_node nextElement(pugi::xml_node from) {
        while (from && from.type() != pugi::node_element) {
            from = from.next_sibling();
        }
        return from;
    }

    /// \brief Reports an element's start to the session.
    ///
    /// \param element The element.
    void openElement(const pugi::xml_node& element) {
        if (++seen_ % kCancelCheckInterval == 0 && token_.stop_requested()) {
            cancelled_ = true;
            return;
        }

        const std::uint32_t begin = startOf(element);
        const std::uint32_t startTagEnd = endOfTag(text_, begin);
        session_.open(element.name(), attributesOf(element, text_, begin, startTagEnd),
                      SourceSpan{begin, startTagEnd});
        stack_.push_back(Visit{element, element.first_child(), startTagEnd, false});
    }

    /// \brief Reports an element's end to the session.
    ///
    /// \param visit The element, as the walk left it.
    void closeElement(const Visit& visit) {
        const Element* open = session_.current();

        // Text only becomes content on an element with no elements inside.
        // Mixed content is not what any of the target formats mean.
        std::string text;
        SourceSpan textSpan;
        if (!visit.hasElementChild) {
            leafText(visit.element, text, textSpan);
        }

        std::uint32_t end = visit.startTagEnd;
        if (!isSelfClosing(text_, visit.startTagEnd)) {
            if (open != nullptr && open->childCount() > 0) {
                // Searching from the last child rather than from the start
                // tag, so a descendant's closing tag is not taken for this one.
                end = endOfClosingTag(text_, open->lastChildEnd());
            } else if (open != nullptr && open->mode() == ShapeMode::Opaque) {
                end = unvisitedEnd(visit.element, text_);
            } else {
                end = endOfClosingTag(text_, visit.startTagEnd);
            }
        }

        session_.close(std::move(text), textSpan, end);
    }

    ShapeSession& session_;
    std::string_view text_;
    const std::stop_token& token_;
    std::vector<Visit> stack_;
    std::size_t seen_ = 0;
    bool cancelled_ = false;
};

}  // namespace

Result<Tree, ParseError> shapeXmlDocument(const SourceFile& source, IShaper& shaper,
                                          const IFormatProvider& provider, std::stop_token token) {
    if (source.empty()) {
        return fail(ParseError::Empty);
    }

    // Offsets are what spans are made of, so a document whose bytes do not
    // line up with the text is refused rather than silently mis-linked.
    const std::string_view text = source.text();
    if (startsWith(text, "\xFF\xFE") || startsWith(text, "\xFE\xFF")) {
        return fail(ParseError::UnsupportedEncoding);
    }

    pugi::xml_document document;
    const pugi::xml_parse_result result =
        document.load_buffer(text.data(), text.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!result) {
        return fail(ParseError::NotWellFormed);
    }
    const pugi::xml_node root = document.first_child();
    if (root.type() != pugi::node_element) {
        return fail(ParseError::NotWellFormed);
    }

    // The walk reports elements and the session decides what they become.
    DefaultShaper fallback;
    ShapeSession session(shaper, fallback, text);
    XmlWalker walker(session, text, token);
    if (!walker.walk(root)) {
        return fail(ParseError::Cancelled);
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
