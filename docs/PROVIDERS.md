Writing a format provider
=========================

**Provider interface version 2.**

A format provider teaches NM Tree Diff to read one file format. It decides what
counts as a node, which two nodes are the same node across two versions of a
file, and how a node is titled and coloured. Everything above it, the matcher,
both views and the reports, works the same whatever you decide.

This document is the contract. `nmxmldiff --list-formats` prints the version
the build in front of you implements, and the number in the heading changes
when something already declared changes shape or meaning. Version 2 replaced
the shaping functions of version 1 with the document and the builder described
here; nothing written against version 1 shapes a document in version 2.

Two ways to write one
---------------------

**In a script**, which needs no compiler and is where to start. A scripted
format sits on top of a built-in one: XML or JSON reads the bytes, and your
script decides what the result means. That covers most studio formats, because
most studio formats are XML or JSON with rules about what the elements mean.
Read "A format in script" below.

**In C++**, when you need something a script cannot say: a format that is
neither XML nor JSON, spans computed in a way nothing else can, or a shape fast
enough to matter on files where it does. Read the rest of this document.

The two produce the same thing, through the same builder.
`testdata/sample/behaviortree.lua` is the compiled behaviour-tree provider
rewritten in script, and the tests assert the two produce identical trees, span
for span, and identical change lists. The choice is about what you need to say
rather than about what you give up.

A format in script
------------------

Put this in `~/.nmtreediff.lua`, or in a file you pass with `--config`:

```lua
provider "bt" {
  display_name = "Behavior tree",
  base = "xml",
  extensions = { ".bt", ".btree" },
  graph_direction = "left_to_right",
  property_order = { "id", "type", "name" },

  shape = function(doc, out)
    local function visit(element, parent)
      if element.name == "node" then
        local node = parent:child(element):set_name(element.attr.type or "node")
        node:set_identity(element.attr.id, "strong")
        for attribute in element:properties() do node:property(attribute) end
        for child in element:children() do out:next(visit, child, node) end
      else
        parent:property(element):set_name(element.attr.name or element.name)
              :set_value(element.attr.value or element.text)
      end
    end
    local root = out:root(doc.root)
    for child in doc.root:children() do out:next(visit, child, root) end
  end,
}
```

`doc` is the document as the base format read it, and `out` is the tree you
are building. The script owns the walk. Rather than have `visit` call itself
for each child, it hands the call to `out:next`, and the tool runs what was
queued once the current call returns. Depth then costs memory rather than the
stack. Replacing `out:next(visit, child, node)` with `visit(child, node)` is a
valid script that recurses, and builds the same tree.

Everything a script may say is listed in USAGE.md under "Lua API reference".
The names there map one for one onto the C++ types below, so the rest of this
document is worth reading for a script too.

The short version
-----------------

1. Write a class deriving from `IFormatProvider` in `src/formats/`. (If a
   script would do, see above: it is a great deal less work.)
2. Implement five methods. Five more have defaults you can leave alone.
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
  interprets them, and both views show it as the card title unless the node
  was given one. Two nodes of different kinds are never paired by any
  structural heuristic, so kind is a strong statement.
- **Properties** are name and value pairs, and a property has a **form**. A
  *scalar* is a value and nothing else. A *record* has named parts whose order
  means nothing, so reordering a transform's fields is not a change. A
  *sequence* has positional parts, named or not, so reordering a list of tags
  is. Any form may carry a value as well as parts. Names may repeat: two
  properties of one node called `tag` are two properties, and everything that
  compares a property list treats it as a multiset. Matching treats a node's
  properties as unordered either way, so a reordered attribute list is never a
  change.
- **Spans** are byte offsets into the file. They are what links the node view
  to the text view: selecting a node highlights its source, and putting the
  caret on a line selects the node containing it. A span arrives with the
  element or property a handle was made from, so you rarely compute one. A
  wrong span is worse than no span, so a synthetic node gets none.

Two property names are conventions rather than rules, defined in
`core/tree.h`. `#text` is a leaf's text content and `#value` is a node whose
whole content is one scalar. Generic JSON also records `#type`, which says
whether a node was an object or an array. Naming your own synthetic properties
with a leading `#` keeps them from colliding with something in the file.

The five methods you have to write
----------------------------------

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

### read

```cpp
Result<Tree, ParseError> read(const SourceFile& source, std::stop_token token) const override;
```

Bytes to structure and nothing more: every element a node, every attribute a
property, in the shape the file has. What the document *means* is the next
method's question. A format built on XML or JSON does not write this at all;
it borrows the built-in one:

```cpp
Result<Tree, ParseError> read(const SourceFile& source, std::stop_token token) const override {
    return xml_->read(source, std::move(token));  // xml_ = makeGenericXmlProvider()
}
```

A format that really reads bytes builds a `Tree` directly, depth first in
document order, then calls `finalize()` and `computeHashes()`. Four rules:

- **It runs on a worker thread.** Providers must be stateless and safe to call
  from several threads at once, because the two sides of a diff parse in
  parallel.
- **Check the stop token.** Cancellation is the normal path, not the
  exception: switching format or reloading a changed file cancels whatever is
  in flight. Check on a bounded interval and return `ParseError::Cancelled`.
- **Report failure rather than guessing.** A file you cannot read is a
  `ParseError`, and the interface shows the message. Half a tree is worse than
  none.
- **Do not recurse on the document's depth.** A file nested thousands of
  levels deep is read with an explicit stack, so its depth costs memory rather
  than the process. Both built-in readers show how: XML keeps a stack of open
  elements, JSON a stack of open containers.

The reading is never normalised. If the file says `1.0` and the other side says
`1`, that is a difference, and deciding it is not worth showing is not the
reader's decision to make. The generic JSON reader keeps scalars exactly as
written for this reason.

### shape

```cpp
void shape(ShapeContext& context) const override;
```

The one method that does real work. It is handed the document, the builder
and a queue, and builds the tree the document stands for. The default copies
the document one to one, which is what a generic format means by it, so a
provider that only reads bytes leaves this alone.

This is where a provider collapses format detail: the behaviour-tree provider
builds a tree holding only its `<node>` elements and folds the rest into
properties, and nothing above learns that this happened. The three parts of
the context are described in the next three sections.

The document
------------

`context.dom()` is the document as `read()` produced it, seen through
handles. A `DomNode` is one element: it has a name, a span, its text content,
its parent, its children and siblings, and its properties. A `DomProperty` is
one property: name, value, form, span, and parts. Both are small values you
can copy and keep.

```cpp
const Dom& dom = context.dom();
DomNode root = dom.root();
for (DomNode child : root.children()) { ... }
child.parent(); child.nextSibling(); child.childAt(0); child.childCount();
child.attribute("type");            // the first property of that name, or nothing
child.property("type");             // the same, as a handle
for (DomProperty p : child.properties()) { ... }       // every one, repeats included
for (DomProperty p : child.properties("tag")) { ... }  // every one of that name
p.form(); p.partCount(); p.partAt(0); p.part("x");
```

The document is a view over the tree `read()` produced, not a copy, so a
million-element file is one copy in memory during shaping rather than two.
The consequence is that you see the base format's reading of the file,
`#text`, `#value` and `#type` included.

There is deliberately no traversal on the document. Navigation is by handle
and the provider owns the walk.

The handle
----------

`context.out()` is the `TreeBuilder`, and everything it hands out is a `Ref`:
one handle type for a node and for a property, deliberately. A caller holding
one does not track which it is, because `child()` does the right thing
wherever it stands:

| Standing on | `child("name")` | `child()` |
| --- | --- | --- |
| A node | a child node of that kind | error: a node needs a kind |
| A scalar property | promotes it to a record and adds a named part | promotes it to a sequence and adds an item |
| A record | a named part | error |
| A sequence | a named item | an unnamed item |

Promotion keeps the scalar's value. The `property()` family always makes a
property, wherever it is called: a property of a node, or a part of a
property.

```cpp
Ref root = out.root(dom.root());          // named after the element, with its span
Ref node = root.child(element);            // kind, span and source from the element
node.setName("Sequence");
node.property("speed", "1.0");             // a scalar; call twice for two of them
node.property(domProperty);                // a copy, parts and all
node.property(element);                    // a scalar named after an element, with its span
Ref list = node.sequence("tags"); list.item("a"); list.item("b");
Ref rec = node.record("transform"); rec.property("x", "1");
node.setChildrenOrdered(false);           // JSON object members, say
node.setIdentity("guid-1", Identity::Strong);
node.setTitle("Patrol", "Sequence");
node.setAccent(0x4080C0);
node.parent(); node.owner();               // the enclosing handle; the nearest node
```

The overloads that take a `DomNode` or a `DomProperty` set the name, the span
and the *source* in one call. The source is what makes the element count as
represented rather than dropped; see "What goes missing" below.

**Build order is free.** Sibling order is the order of `child()` and
`property()` calls on that one parent handle, and nothing else. Two jobs
building under different parents cannot affect each other, and a breadth-first
walk, a depth-first walk and a hand-written recursion all build the same
tree. The builder renumbers into document order at the end, and a provider
never learns the tree's arena invariants exist.

**Identity, title and colour are recorded here**, not asked for later.
`identity()` and `style()` on the provider are not virtual: they read what the
handle recorded, with the kind as the fallback title and a colour derived from
the kind as the fallback accent. A format whose cards need a second line from
a property that is usually there names it once instead:

```cpp
std::span<const std::string_view> subtitleProperties() const override {
    return kSubtitleProperties;  // {"name"}: the first of these a node has
}
```

Return `Identity::Strong` only when the key is genuinely stable, meaning the
same key in two files really is the same node however far it has moved. A
strong key is honoured before any structural heuristic runs, which is what
makes a behaviour-tree node follow a move to anywhere in the tree. It cuts
the other way as well: the similarity pass never pairs two nodes whose strong
keys differ, so a sibling replaced under a new id reads as a deletion and an
insertion, not an edit, while a node with a key may still pair with one that
has none. A strong key only anchors a pair when it appears exactly once on
each side. Generic
XML records no identity at all: in arbitrary XML an `id` might be a stable
identifier or might be a colour swatch name.

The queue
---------

The provider controls the walk completely, and nothing stops it recursing if
it insists. What the context offers is a way to queue a call instead of making
it:

```cpp
context.next(job, element.id(), parent.id());   // runs before what is already queued
context.later(job, element.id(), parent.id());  // runs after
```

A job is any callable taking the context. The drain runs the queue until it is
empty, checking the stop token between jobs. Everything one job queues with
`next()` goes to the front as a block, in the order it was queued, so a job
that queues one call per child sees its children run first to last.

`next()` is the idiom. Because build order is free the two drains produce the
same tree, and depth-first holds about one root-to-leaf path of pending jobs
where breadth-first holds a whole level.

The built-in providers use neither: they walk the document in arena order,
which is pre-order, so a parent's handle always exists before its children
need it, and that is one loop with no queue at all. The behaviour-tree
provider is the example. The queue exists for a walk that cannot be written
as one pass, and for scripts, where it is the only way to avoid recursion.

What goes missing
-----------------

A provider may leave content out now, and two things can remove content from
the tree. Both are shown rather than refused.

**Dropped content.** An element no handle was made from is unrepresented, and
the tree carries the source bytes of every such element, less any represented
element inside it. The text view marks them, the status bar counts them, and a
headless report warns about them. There is no `drop()` call: the complement is
computed, so an element cannot be forgotten. Granularity is the element, not
the attribute; an attribute is dropped when its element is. A provider that
folds a whole subtree into one property marks the rest of that subtree through
`out.represent(id)`, since the property has one source and the subtree has
many.

**Failed jobs.** An error inside a job does not fail the parse. The drain
catches it at the job boundary; whatever the job built stays, whatever it never
queued is simply missing, and the failure is recorded against the element and
handle the job was queued with. A `BuildError` from the handle is one such
error: a child with no kind under a node, a value on a node, an item outside
a sequence. The text view marks the element, the node view marks the card the
missing content would have hung from, and a headless report lists each one
and exits 2. If shaping ends with no root at all, the parse fails with
`ParseError::ShapeFailed`, because a tree of zero nodes is not a partial
result.

Dropping is a format's decision and never fails a run. Failing is a bug.

The five with defaults
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

### subtitleProperties

Described under "The handle". The default names nothing, so a card's second
line is whatever `setTitle()` gave it.

### graphDirection

Return `GraphDirection::LeftToRight` or `TopDown` to say which way this
format's graph reads best, or `Inherit`, the default, to accept the reader's
choice. Returning `Inherit` is not the same as returning `TopDown`: a provider
with no opinion must not overrule a reader who has one.

### parse

`read()`, then `shape()` over the result, then the finished tree. A provider
whose `shape()` is the identity may return `read()` directly rather than copy
a tree to change nothing, which is what generic XML does.

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

A studio names its asset files whatever it likes, and pointing an extension at a
format should not need a rebuild. Configuration is a Lua script, which is also
where a scripted format is declared:

```lua
formats {
  [".bt"]        = "bt",
  [".leveldata"] = "json",
}

fallback "xml"
graph_direction "top_down"
exit_key "escape"
```

Three files are read, each overriding what came before, so the most specific
statement wins:

1. `~/.nmtreediff.lua`
2. `~/.nmtreediff/config.lua`
3. whatever `--config` names

Then `--format`, which is a person correcting a guess right now and beats every
standing decision. A missing home file is not an error; a missing `--config` is.

A script is a script, so a studio with twenty suffixes writes a loop rather than
twenty lines. That is the reason configuration is not a table of pairs.

**Configuration is deliberately not searched for beside the files being
compared.** That would be the obvious design and it does not work. A version
control system usually hands over temporary extracts rather than the files in
your checkout, so a walk upwards from them finds a temporary directory instead
of the repository, in exactly the case the search was written for. And a script
arriving next to a file someone sent you is code you did not choose to run. If
project configuration is ever attempted again, it should match on the label a
client supplies, which survives a temporary file, rather than on where the bytes
happen to sit.

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
they claim, that identity is strong where you meant it to be, that a node
survives the edit you built the provider to survive, and that a document
nested a few thousand levels deep still shapes.
