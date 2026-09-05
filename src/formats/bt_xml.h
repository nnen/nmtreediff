#pragma once

/// \file
/// \brief A sample provider for an XML behavior-tree format.

#include <memory>

#include "core/provider.h"

namespace nmxd {

/// \brief Creates the sample behavior-tree provider.
///
/// \returns A provider that treats only `<node>` elements as nodes and folds
///          their `<property>` children into the property list.
///
/// \remarks This is the worked example the provider documentation refers to,
///          and it is deliberately a real provider rather than a toy: it is
///          registered, it is reachable from the command line, and the golden
///          corpus holds cases in it.
///
///          It exists to show the three things a schema-aware format does that
///          a generic one cannot. It decides what counts as a node, so the
///          `<property>` elements that clutter a generic XML diff disappear
///          into the node they describe. It returns a strong identity key from
///          the `id` attribute, so a node keeps its identity across a move to
///          anywhere in the tree. And it titles a card by the node's `type`
///          rather than by the element name, so a graph of behaviour reads as
///          Sequence and MoveTo rather than as a wall of the word "node".
///
///          The format it reads is the one from the requirements: a document
///          element holding `<node>` elements, each with `id`, `type` and
///          `name` attributes and `<property name="..." value="..."/>`
///          children. Elements that are neither are walked through rather than
///          represented, which is a simplification a studio provider would
///          revisit.
[[nodiscard]] std::unique_ptr<IFormatProvider> makeBehaviorTreeProvider();

}  // namespace nmxd
