#pragma once

/// \file
/// \brief Turning a matching into something the views can read.

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "core/match.h"
#include "core/provider.h"
#include "core/tree.h"

namespace nmxd {

/// \brief Which of the two documents a node belongs to.
enum class Side : std::uint8_t {
    Left,   ///< The left, usually older, document.
    Right,  ///< The right, usually newer, document.
};

/// \brief What happened to one node.
enum class NodeStatus : std::uint8_t {
    Unchanged,  ///< Matched, with the same properties and the same position.
    Added,      ///< Present on the right only.
    Deleted,    ///< Present on the left only.
    Modified,   ///< Matched, but its properties differ.
    Moved,      ///< Matched and unchanged, but it sits somewhere else now.
};

/// \brief Converts a status into a word suitable for a message.
///
/// \param status The status to describe.
///
/// \returns A single lower-case word, never null.
[[nodiscard]] const char* describe(NodeStatus status) noexcept;

/// \brief The index meaning "this node has no change recorded".
inline constexpr std::uint32_t kNoChange = 0xFFFFFFFFu;

/// \brief One reported change.
struct Change {
    /// \brief What happened to the node.
    NodeStatus status = NodeStatus::Unchanged;

    /// \brief Whether the node also changed position.
    ///
    /// \remarks Set alongside NodeStatus::Modified when a node both moved and
    ///          changed, so that neither half of what happened is lost.
    bool moved = false;

    /// \brief The node's id in the left tree, or kInvalidNode when added.
    NodeId left = kInvalidNode;
    /// \brief The node's id in the right tree, or kInvalidNode when deleted.
    NodeId right = kInvalidNode;

    /// \brief Names of the properties that differ, for a modified node.
    ///
    /// \remarks Ordered by the provider's property ranking, so the list reads
    ///          the same way the node card does.
    std::vector<std::string> changedProperties;
};

/// \brief The matching plus what it means, ready for the views to render.
struct DiffModel {
    /// \brief Which node on the left is which node on the right.
    Matching matching;
    /// \brief How complete the matching is.
    MatchQuality quality = MatchQuality::Full;

    /// \brief Containers the similarity pass gave up on for want of budget.
    ///
    /// \remarks Zero unless \ref quality says the pass was trimmed. The
    ///          children of such a container that no earlier pass had paired
    ///          are reported added and deleted, and this says how many
    ///          containers that applies to, so the report can name the scale
    ///          of what it could not do.
    std::uint32_t trimmedParents = 0;

    /// \brief Every change, in the order a reader should be walked through them.
    ///
    /// \remarks Produced by walking the union of both trees, so the order
    ///          follows the document rather than the arena.
    std::vector<Change> changes;

    /// \brief Status of every node in the left tree, indexed by NodeId.
    std::vector<NodeStatus> leftStatus;
    /// \brief Status of every node in the right tree, indexed by NodeId.
    std::vector<NodeStatus> rightStatus;

    /// \brief Where each left node's change sits in \ref changes.
    ///
    /// \remarks Indexed by NodeId, holding kNoChange for a node that did not
    ///          change. An index rather than a search, because the details panel
    ///          asks this per visible node per frame and scanning the change
    ///          list would make that quadratic in the size of the diff.
    std::vector<std::uint32_t> leftChangeIndex;
    /// \brief Where each right node's change sits in \ref changes.
    std::vector<std::uint32_t> rightChangeIndex;

    std::uint32_t added = 0;      ///< Nodes present on the right only.
    std::uint32_t deleted = 0;    ///< Nodes present on the left only.
    std::uint32_t modified = 0;   ///< Nodes whose properties differ.
    std::uint32_t moved = 0;      ///< Nodes that only changed position.
    std::uint32_t unchanged = 0;  ///< Nodes that did not change at all.

    /// \brief Wall-clock milliseconds spent matching and classifying.
    double elapsedMillis = 0.0;
    /// \brief Whether the work stopped early because it was cancelled.
    bool cancelled = false;

    /// \brief Looks up what happened to one node.
    ///
    /// \param side Which document the node belongs to.
    /// \param id The node to look up.
    ///
    /// \returns The node's status, or NodeStatus::Unchanged for an id out of
    ///          range.
    [[nodiscard]] NodeStatus statusOf(Side side, NodeId id) const;

    /// \brief Looks up the full record of what happened to one node.
    ///
    /// \param side Which document the node belongs to.
    /// \param id The node to look up.
    ///
    /// \returns The change, or null when the node did not change.
    ///
    /// \remarks statusOf() answers what happened; this answers the rest,
    ///          including which properties differ. Kept separate because most
    ///          callers only need the status and paying for a pointer chase to
    ///          get it would be wasteful.
    [[nodiscard]] const Change* changeFor(Side side, NodeId id) const;

    /// \brief Reports whether the two trees are the same.
    ///
    /// \returns `true` when nothing was added, deleted, modified or moved.
    [[nodiscard]] bool identical() const noexcept {
        return added == 0 && deleted == 0 && modified == 0 && moved == 0;
    }

    /// \brief Returns how many nodes changed.
    ///
    /// \returns The sum of added, deleted, modified and moved nodes.
    [[nodiscard]] std::uint32_t changedNodes() const noexcept {
        return added + deleted + modified + moved;
    }
};

/// \brief Reports whether two properties differ, parts and all.
///
/// \param left The property on one side.
/// \param right The property on the other.
///
/// \returns `true` when anything about them differs: value, form, or any
///          part at any depth.
///
/// \remarks Defined by hashProperty(), so the change list, the details panel
///          and matching all agree on what "the same property" means. The
///          name is compared too; callers pairing properties by name first
///          get the same answer either way.
[[nodiscard]] bool propertiesDiffer(const Property& left, const Property& right);

/// \brief Finds the property on the other side that one property stands
///        against, pairing repeated names by occurrence.
///
/// \param own The properties on this side.
/// \param index Which of them to pair; must be less than `own.size()`.
/// \param other The properties on the other side.
///
/// \returns The property in \p other with the same name and the same
///          occurrence among same-named siblings, or null when the other side
///          has fewer of that name.
///
/// \remarks Names may repeat, and the first with a name is the wrong answer
///          for the second: it reads the second's value as a change from the
///          first's. Nothing can say which of two same-named properties is
///          "the same one", so document order is the rule, the k-th here
///          against the k-th there. This is what the details panel pairs with
///          and what its removed-property listing counts from; matching does
///          not pair at all, it compares as a multiset.
[[nodiscard]] const Property* counterpartByOccurrence(const std::vector<Property>& own,
                                                      std::size_t index,
                                                      const std::vector<Property>& other) noexcept;

/// \brief Matches two trees and classifies the result.
///
/// \param left The left, usually older, tree.
/// \param right The right, usually newer, tree.
/// \param provider The format provider.
/// \param token Checked during matching.
/// \param options The guards bounding how much work matching may do.
///
/// \returns The classified diff. DiffModel::cancelled is set when the token
///          stopped the work, in which case the other fields are empty.
[[nodiscard]] DiffModel diffTrees(const Tree& left, const Tree& right,
                                  const IFormatProvider& provider, std::stop_token token = {},
                                  MatchOptions options = {});

/// \brief Classifies an existing matching without running the matcher.
///
/// \param left The left tree.
/// \param right The right tree.
/// \param provider The format provider.
/// \param match The matching to classify, consumed by this call.
///
/// \returns The classified diff.
///
/// \remarks For tests that supply their own matching.
[[nodiscard]] DiffModel classify(const Tree& left, const Tree& right,
                                 const IFormatProvider& provider, MatchResult match);

/// \brief Renders a change list as stable text.
///
/// \param left The left tree.
/// \param right The right tree.
/// \param model The diff to render.
///
/// \returns One line per change, or the word `identical` when there are none.
///
/// \remarks Used by the golden-file tests and by the headless report. Lines are
///          prefixed `+` added, `-` deleted, `~` modified, `>` moved, `~>` both,
///          and `!` for a note about reduced quality.
[[nodiscard]] std::string serializeChanges(const Tree& left, const Tree& right,
                                           const DiffModel& model);

/// \brief Builds the path used to name a node in serialized output.
///
/// \param tree The tree the node belongs to.
/// \param id The node to name.
///
/// \returns A path such as `/tree/node[1]/property[0]`, or `?` for an id out of
///          range.
///
/// \remarks Position is counted among siblings of the same kind, so inserting a
///          node of a different kind nearby does not renumber this one.
[[nodiscard]] std::string nodePath(const Tree& tree, NodeId id);

}  // namespace nmxd
