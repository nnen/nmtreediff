Writing a format provider
=========================

**Provider interface version 1.**

A format provider teaches NM Tree Diff to read one file format. It decides what
counts as a node, which two nodes are the same node across two versions of a
file, and how a node is titled and coloured. Everything above it, the matcher,
both views and the reports, works the same whatever you decide.

This document is the contract. Version 1 will keep working: changes to the
interface are additive, so a new method arrives with a default implementation
and nothing already declared changes shape or meaning. The number in the
heading goes up only when that promise is broken, which would be a decision
rather than an accident. `nmxmldiff --list-formats` prints the version the
build in front of you implements.

The short version
-----------------

1. Write a class deriving from `IFormatProvider` in `src/formats/`.
2. Implement six methods. Four more have defaults you can leave alone.
3. Add one line to `makeDefaultRegistry()` in `src/core/registry.cpp` and one
   to `src/core/CMakeLists.txt`.
4. Add a case to `testdata/golden/` so a future change cannot silently break
   what you built.

`src/formats/bt_xml.cpp` is a complete worked example, and the rest of this
document refers to it. It reads the behaviour-tree format below, in which only
`<node>` elements are nodes and the `<property>` elements describing them fold
into the node's property list.

```xml
<behaviortree version="2">
  <node id="a1b2" type="Sequence" name="Patrol">
    <property name="interruptible" value="true"/>
    <node id="c3d4" type="MoveTo" name="Go to waypoint">
      <property name="speed" value="1.0"/>
    </node>
  </node>
</behaviortree>
```

Read as generic XML that file is five nodes, two of which are called
`property` and tell a reviewer nothing. Read by its own provider it is three,
titled Sequence and MoveTo, and a changed speed is reported as a property of
the behaviour it belongs to rather than as an edit to an anonymous element.
Both readings are in the golden corpus, as
`behavior_tree_reorder_edit_and_insert` and `bt_reorder_edit_and_insert`, so
you can compare them.

What you are building
---------------------

A tree of nodes. A node has a **kind**, a list of **properties**, a list of
**children**, and a **span** saying where in the file's bytes it sits.

- **Kind** is what sort of node this is. The matcher compares kinds and never
  interprets them, and both views show it as the card title unless `style()`
  says otherwise. Two nodes of different kinds are never paired by any
  structural heuristic, so kind is a strong statement.
- **Properties** are name and value pairs. Matching treats them as an unordered
  set, so a reordered attribute list is never a change. Where they came from in
  the file is up to you: the behaviour-tree provider fills the list from both
  the node's own attributes and its `<property>` children, and nothing above
  learns that the format writes them two different ways.
- **Spans** are byte offsets into the file. They are what links the node view
  to the text view: selecting a node highlights its source, and putting the
  caret on a line selects the node containing it. A wrong span is worse than no
  span, so if you cannot compute one honestly, leave it empty.

Two property names are conventions rather than rules, defined in
`core/tree.h`. Use `#text` for a leaf's text content and `#value` for a node
whose whole content is one scalar. Naming your own synthetic properties with a
leading `#` keeps them from colliding with something in the file.

The six methods you have to write
---------------------------------

### name and displayName

```cpp
std::string_view name() const override { return "bt"; }
std::string_view displayName() const override { return "Behavior tree (XML)"; }
```

`name()` is what `--format` accepts and what a configuration file writes. Keep
it short, lower case and stable: it ends up in scripts and in diff-tool
configuration that nobody wants to edit again. `displayName()` is for people.

### defaultExtensions

```cpp
std::span<const std::string_view> defaultExtensions() const override {
    return kExtensions;  // {".bt", ".btree"}
}
```

Lower case, each with its leading dot, backed by storage that outlives the
call. This is declared rather than buried inside `score()` so that
`--list-formats` can tell a user what handles what.

### score

```cpp
int score(const SourceFile& source) const override;
```

Return zero when you do not recognise the file at all, and higher when you do.
The registry picks the highest scorer, and the earliest registered provider
wins a tie.

The behaviour-tree provider returns 95 when the head of the file contains
`<behaviortree` and 85 when only the extension matches. The higher number is
deliberate: generic XML claims `.xml` at 90, so scoring the document element
above that is what lets a behaviour tree saved as `.xml` still be read as one.
That is the point of a score rather than an extension table.

Keep this cheap. It runs on every candidate, before anything is parsed, so look
at a few hundred bytes rather than parsing the document.

Rough guide to the range:

| Score | Means |
| --- | --- |
| 90 to 100 | The content says so. A document element or magic bytes only your format uses. |
| 70 to 89 | An extension only your format uses. |
| 30 to 69 | The content is consistent with your format but not distinctive. |
| 0 | Not yours. |

### parse

```cpp
Result<Tree, ParseError> parse(const SourceFile& source, std::stop_token token) const override;
```

The one method that does real work. Build the tree depth first in document
order, which gives the rest of the tool two invariants it relies on: a parent
has a lower index than its children, and a node's descendants occupy a
contiguous run of indices after it.

```cpp
Tree tree;
tree.setFormatName(std::string(name()));
const NodeId id = tree.add(parent, "Sequence", SourceSpan{begin, end});
tree.addProperty(id, "speed", "1.0", SourceSpan{propertyBegin, propertyEnd});
// ... children ...
tree.finalize();
computeHashes(tree, *this, token);
return tree;
```

`finalize()` and `computeHashes()` are both required. The first fills in depth
and descendant counts; the second fills in the subtree hashes the second
matching pass runs on, and it calls back into your `childrenOrdered()` while it
does.

Four rules:

- **It runs on a worker thread.** Providers must be stateless and safe to call
  from several threads at once, because the two sides of a diff parse in
  parallel.
- **Check the stop token.** Cancellation is the normal path, not the
  exception: switching format or reloading a changed file cancels whatever is
  in flight. Check on a bounded interval, per node batch rather than per node,
  and return `ParseError::Cancelled`.
- **Report failure rather than guessing.** A file you cannot read is a
  `ParseError`, and the interface shows the message. Half a tree is worse than
  none.
- **Do not normalise.** If the file says `1.0` and the other side says `1`,
  that is a difference, and deciding it is not worth showing is not the
  provider's decision to make. The generic JSON provider keeps scalars exactly
  as written for this reason.

### identity

```cpp
IdentityKey identity(const Tree& tree, NodeId id) const override;
```

This is where a provider earns its keep. Return `strong = true` only when the
key is genuinely stable, meaning the same key in two files really is the same
node however far it has moved. A strong key is honoured before any structural
heuristic runs, which is what makes a behaviour tree node follow a move to
anywhere in the tree:

```cpp
if (const Property* identifier = node.findProperty("id")) {
    if (!identifier->value.empty()) {
        return IdentityKey{true, identifier->value};
    }
}
return IdentityKey{};
```

Generic XML looks at exactly the same attribute and returns a weak key,
because in arbitrary XML an `id` might be a stable identifier or might be a
colour swatch name. If you know your schema, say so. If you do not, return
`IdentityKey{}` and let the structural passes work.

A strong key only anchors a pair when it appears exactly once on each side.
Duplicates anchor nothing, so a key you are not sure is unique costs you
nothing but gains you nothing either.

### style

```cpp
NodeStyle style(const Tree& tree, NodeId id) const override;
```

Title, optional subtitle, and an accent colour. Diff status is tinted on top,
so your palette can never hide whether a node changed.

Two requirements. It must be **deterministic**, because the two sides style
their nodes independently and have to agree; derive the colour from the
content, as every built-in provider does by hashing the kind. And it must be
**cheap**, because it is called per visible card per frame.

`NodeStyle::icon` is reserved. Neither view reads it yet.

The four with defaults
----------------------

### propertyRank

```cpp
int propertyRank(const Tree&, NodeId, std::string_view propertyName) const override {
    return rankFromList(kLeadingProperties, propertyName);
}
```

Lower sorts first, equal ranks keep document order. This is presentation only
and never affects matching. `rankFromList` covers the usual case of a fixed
leading order with everything else trailing. The behaviour-tree provider leads
with `id`, because that is what makes a node the same node across versions and
the first thing to check when a match looks wrong.

### childrenOrdered

```cpp
bool childrenOrdered(const Tree& tree, NodeId id) const override;
```

Return `true`, the default, when sibling position is meaningful, so a
reordering is a move. Return `false` when position carries nothing, so a
reordering is not a change at all.

It is asked per node rather than per format, and generic JSON is why: an array
is ordered and an object is not, and one document holds both. Behaviour-tree
children are ordered because sibling order is execution order.

### serialize

Reserved for the merge milestone and unimplemented. Declared now so that
providers are written with round-tripping in mind rather than discovering later
that they threw away what a merge needs.

### claimsExtension

Not virtual. It answers `defaultExtensions()` for you, and `score()` usually
calls it.

Registering it
--------------

Registration is an explicit list, in `makeDefaultRegistry()`:

```cpp
ProviderRegistry makeDefaultRegistry() {
    ProviderRegistry registry;
    registry.add(makeGenericXmlProvider());
    registry.add(makeGenericJsonProvider());
    registry.add(makeBehaviorTreeProvider());
    registry.setFallback("xml");
    return registry;
}
```

There is no self-registering static initialiser on purpose. In a static library
the linker drops a translation unit nothing references, taking its
self-registration with it, and a format that silently vanishes from a release
build is far worse than one list that has to be edited.

Then add your `.cpp` to `src/core/CMakeLists.txt`.

How a file finds its provider
-----------------------------

Three ways, in order of how deliberate they are:

1. **`--format <name>`** on the command line. Always wins. A name the registry
   does not know is an error rather than a silent fall back to sniffing,
   because it is usually a typo in a diff-tool configuration that would
   otherwise go unnoticed for a long time.
2. **A configured extension**, from `--config`. A studio's standing decision.
3. **The highest `score()`**, then the fallback provider if nothing scores.

The configuration file
----------------------

A studio names its asset files whatever it likes, and pointing an extension at
a provider should not need a rebuild. The file is one `key = value` per line,
`#` starts a comment, and blank lines are ignored:

```
# Our exporter writes behaviour trees with this suffix.
.bt      = bt
.btree   = bt

# And our level data is JSON under another name.
.leveldata = json

# What to use for a file nothing else claims.
fallback = xml
```

A key beginning with a dot is an extension, matched without regard to case. The
only other key is `fallback`. Anything else is reported with its line number,
and so is a provider name the build does not know. Every problem in the file is
listed at once rather than one per run, and a file with any problem stops the
run rather than being partly applied: a diff read by the wrong provider looks
like a working diff, which is the failure that goes unnoticed longest.

Pass it with `--config`, which for a version control system means adding it to
the command string once. There is no automatic search of the working directory,
because a diff tool is launched by another program from a directory nobody
chose, and configuration that depends on where you were standing is not
configuration.

Testing it
----------

A golden case is a directory under `testdata/golden/` holding `left.<ext>`,
`right.<ext>` and `expected.txt`. The extension decides the provider, so a case
in your format needs nothing else. Add `format.txt` naming a provider to pin a
case rather than sniffing, which is how the corpus holds one document read two
ways.

Generate the expectations, then read the diff before committing it:

```
NMXD_UPDATE_GOLDEN=1 ctest --test-dir build -C RelWithDebInfo
```

This is the main defence against a change in matching quality, and it makes
such a change reviewable rather than only a pass or a fail. `test_bt_provider.cpp`
shows the unit tests worth writing alongside: that spans slice back to the text
they claim, that identity is strong where you meant it to be, and that a node
survives the edit you built the provider to survive.
