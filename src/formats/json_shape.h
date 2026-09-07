#pragma once

/// \file
/// \brief Driving a shaper from a JSON document.

#include <stop_token>
#include <string_view>

#include "core/provider.h"
#include "core/shape.h"

namespace nmxd {

/// \brief The kind given to the document's outermost value.
///
/// \remarks Borrowed from the JSONPath spelling of the root, so that a path
///          printed by a report reads the way someone used to JSON tooling
///          expects it to.
inline constexpr std::string_view kJsonRootKind = "$";

/// \brief The name given to every element of an array.
///
/// \remarks Deliberately the same for every element of every array. An element
///          has no name of its own, and putting its index in the kind would
///          make moving it look like turning it into a different sort of node,
///          which is exactly the mistake a tree diff exists to avoid.
inline constexpr std::string_view kJsonElementKind = "item";

/// \brief The attribute recording whether a value is an object or an array.
///
/// \remarks A node's kind carries its member key, which is what makes a report
///          path readable, so it cannot also carry the JSON type. Without this
///          property an empty object replaced by an empty array under the same
///          key would hash the same and be reported as unchanged.
inline constexpr std::string_view kJsonTypeProperty = "#type";

/// \brief The value of kJsonTypeProperty on an object.
inline constexpr std::string_view kJsonObjectType = "object";

/// \brief The value of kJsonTypeProperty on an array.
inline constexpr std::string_view kJsonArrayType = "array";

/// \brief The treatment a JSON element gets when nobody says otherwise.
///
/// \remarks What the generic JSON format is. An object is a node whose
///          scalar members are its properties. An array under an object that
///          holds no object anywhere inside it is one property with ordered
///          parts; any other array is a node whose elements are nodes. A
///          scalar element, which only an array or the document itself can
///          hold, is a node carrying its value as kValueProperty.
class JsonDefaultShaper final : public IShaper {
public:
    void exit(Element& element, Builder& out) override;
};

/// \brief Parses a JSON document and runs its values through a shaper.
///
/// \param source The file to parse.
/// \param shaper What decides the meaning of each value.
/// \param provider The provider the tree belongs to, for its name and for
///        the hashing pass, which asks it about child order.
/// \param token Checked on a bounded interval while walking.
///
/// \returns The tree, finalised and hashed, or a ParseError.
///
/// \remarks What a shaper sees. The document's outermost value is an element
///          named kJsonRootKind. An object is an element carrying
///          kJsonTypeProperty and one attribute per scalar member, each with
///          the value exactly as written and a span from the key to the end
///          of the value; its object and array members are the elements
///          inside it, named after their keys. An array is an element
///          carrying kJsonTypeProperty, holding one element named
///          kJsonElementKind per value in it. A scalar inside an array, or a
///          document that is one scalar, is an element whose text is the
///          value as written.
///
///          Scalar members reach the shaper as attributes rather than as
///          elements, because they are what a script keys and titles a node
///          by, and an attribute is visible at enter where an item is not.
///          That costs one extra pass over each object, which the parser's
///          forward-only cursor makes the only way to know the attributes
///          before anything inside the object is reported.
[[nodiscard]] Result<Tree, ParseError> shapeJsonDocument(const SourceFile& source, IShaper& shaper,
                                                         const IFormatProvider& provider,
                                                         std::stop_token token);

}  // namespace nmxd
