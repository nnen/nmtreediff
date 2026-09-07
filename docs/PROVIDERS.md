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

Two ways to write one
---------------------

**In a script**, which needs no compiler and is where to start. A scripted
format sits on top of a built-in one: XML or JSON does the parsing, and your
script decides what the result means. That covers most studio formats, because
most studio formats are XML or JSON with rules about what the elements mean.
Read "A format in script" below.

**In C++**, when you need something a script cannot say: a format that is
neither XML nor JSON, spans computed in a way nothing else can, or a parse fast
enough to matter on files where it does. Read the rest of this document.

The two produce the same thing. `testdata/sample/behaviortree.lua` is the
compiled behaviour-tree provider rewritten in script, and the tests assert the
two produce identical trees and identical change lists, so the choice is about
what you need to say rather than about what you give up.

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

  is_node = function(element) return element.name == "node" end,
  fold_into_parent = function(element) return element.name == "property" end,

  kind     = function(element) return element.attr.type end,
  identity = function(element) return element.attr.id, "strong" end,
  title    = function(element) return element.attr.type, element.attr.name end,
}
```

This is the short form: five questions, each handed one element with `name`
holding what the file calls it and `attr` holding its attributes by name.
Everything is optional: a provider with only a `base` reads exactly like the
format it sits on.

| Entry | What it decides |
| --- | --- |
| `base` | Which built-in format does the parsing. `xml` or `json`. |
| `extensions` | Which suffixes this format claims. A claimed suffix beats the format underneath. |
| `graph_direction` | `top_down` or `left_to_right`. |
| `property_order` | Which properties sort first. Presentation only. |
| `is_node` | Whether an element becomes a node. Default: every element does. |
| `fold_into_parent` | Whether it becomes a name and value pair on the node above. An element that is neither still becomes a property, named after itself and holding a part per attribute, and the walk carries on inside it so any nodes it wraps still surface. |
| `kind` | What sort of node this is. Default: the element's name. |
| `identity` | What makes this the same node across versions. Return a second value of `"strong"` to say the key may be matched across any distance. |
| `title` | The card's first line, and optionally a second. |

**Nothing is dropped.** An element is a node or it is a property. There is no
third answer, so a format cannot lose content by failing to mention it. A
wrapper keeps both halves: the wrapper itself becomes a property, and the nodes
inside it attach to the nearest node above.

**Each function is asked once per node, while the document is open.** The
answers are kept with the tree, so nothing crosses into the interpreter while a
frame is being drawn. Write them as though they cost something, because they do,
but not per frame.

### The full form: enter and exit

The five questions decide each element from that element alone. Some formats
need more: a wrapper that should vanish while the nodes inside it are kept, a
child keyed by the node above it, a block of editor data that should stay one
opaque property. For those a script writes `enter` and `exit` instead, and the
five questions are ignored.

```lua
provider "tree" {
  base = "xml",

  enter = function(el, frame)
    if el.name == "node" then
      frame.data = el.attr.id     -- visible to every element inside
    elseif el.name == "editor" then
      frame:opaque()              -- one property holding the raw text
    end
  end,

  exit = function(el, out)
    if el.name == "node" then
      out:node("node", el):identity(el.attr.id, "strong"):attributes(el):adopt(el.items)
    elseif el.name == "child" then
      local owner = el:ancestor("node")
      out:node("child", el)
         :identity(owner.data .. "/" .. el.attr.name, "strong")
         :attributes(el):adopt(el.items)
    elseif el.name == "children" then
      out:forward(el.items)       -- a wrapper: what it held goes up, it keeps nothing
    end
  end,
}
```

**Exit decides, enter steers.** `enter` is called when an element starts,
before anything inside it, and never emits. It can leave a value on the element
for its descendants and it can take the subtree away: `frame:default()` gives
the element and everything inside it the default treatment with no further
callbacks, and `frame:opaque()` keeps it as one property holding its raw text.
`exit` is called when the element ends. By then every element inside it has
already become a node or a property, and those sit in `el.items` in document
order. That is the whole reason the decision is made at exit: a script can look
at what an element holds before saying what the element is.

**What exit can say.** `out:node(kind, el)` emits a node spanning the element;
`out:property(name, value, el)` emits a property; `out:forward(el.items)`
passes the items up unchanged; `out:default(el)` applies the default treatment;
`out:drop()` keeps nothing. A node handle takes `:attributes(el, ...)` with names
to leave out, `:text(el)`, `:property(name, value, el)`, `:adopt(el.items)`,
which makes nodes children and properties properties, `:kind(name)`,
`:identity(value, "strong")`, `:title(first, second)` and `:ordered(false)`. A
property handle takes `:value(v)`, `:part(name, value, el)`, `:attributes(el,
...)`, `:adopt(el.items)`, which makes every item a part, `:ordered(true)` for a
sequence, and `:collapse()`, which folds a single plain part into the value.

**Nothing is dropped here either.** An `exit` that says nothing about an element
gives it the default treatment. Items an `exit` neither adopts nor forwards are
forwarded for it. The only way to lose content is `out:drop()`, which is what
makes a forgotten branch harmless and a deliberate one visible.

**What an element shows.** `el.name`, `el.attr`, `el.text` (empty until exit,
and empty on an element that holds elements), `el.span` as `start` and `stop`
offsets, `el.depth`, `el.index` among its siblings, `el.child_count`,
`el.parent`, `el:ancestor(name)`, `el.data` and `el.items`. An element is
reachable while it or anything inside it is being visited. Nothing below an
element is reachable except through `el.items`, and nothing to its right at
all, which is what keeps the surface honest about what a parser reports as it
reads.

**Handles do not outlive their callback.** A builder or node handle kept past
the `exit` that made it, or an element kept past its close, raises a Lua error
when touched rather than reading memory that is gone. An error raised inside
`enter` or `exit` is read as no answer: what was emitted before it stays, and
the rules above keep the rest.

`testdata/sample/behaviortree_events.lua` is the behaviour tree written in
this form, and the tests hold it against the compiled provider the same way
they hold the short form. `testdata/sample/nested_children.lua` is the wrapper
case above, complete.

**A JSON base is read first.** XML has a walker of its own, so a script sees
the elements as the parser meets them. Any other base reads the file into its
own tree first, and the script sees that tree: a node's kind is the element's
name, its properties are the attributes, and its span is the span. Generic
JSON keeps every scalar exactly as written, quotes included, and so does what
the script sees.

**A script gets its own interpreter on each worker.** Two sides of a comparison
parse at once and a Lua state is not thread safe, so the states share nothing.
A script cannot carry anything between files, which also means a comparison
never depends on what was opened before it.

**A script that will not stop is stopped.** Switching format or reloading a file
cancels whatever is running, and that applies to your code too.

The short version
-----------------

1. Write a class deriving from `IFormatProvider` in `src/formats/`. (If a
   script would do, see above: it is a great deal less work.)
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
- **Properties** are name and value pairs, and a property may have parts of its
  own. A record's parts are named and unordered, so reordering a transform's
  fields is not a change; a sequence's parts are positional, so reordering a
  list of tags is. Set `ordered` to say which you built. Matching treats a
  node's properties as an unordered set either way, so a reordered attribute
  list is never a change. Where they came from in
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

The one method that does real work, and for an XML-based format it is two
lines, because the reading is done for you. Write a shaper, which decides what
each element becomes, and hand it to the XML driver:

```cpp
class MyShaper final : public IShaper {
    void exit(Element& el, Builder& out) override {
        if (el.name() == "node") {
            out.node(std::string(el.attributeValue("type")), el)
                .attributes(el)
                .adopt(el.takeItems());
        } else if (el.name() == "children") {
            out.forward(el.takeItems());
        }
        // Anything unmentioned gets the default treatment, and is kept.
    }
};

Result<Tree, ParseError> parse(const SourceFile& source, std::stop_token token) const override {
    MyShaper shaper;
    return shapeXmlDocument(source, shaper, *this, token);
}
```

`core/shape.h` is the contract: `exit()` is called when an element ends, with
the items already made from what it held, and `enter()` when it starts, for
steering and for leaving values that descendants read. The same rules as the
scripted full form apply, because the scripted form is this interface with a
Lua binding in front of it: an exit that says nothing gets the default, items
left behind go up, and `drop()` is the only way to lose content. Spans are the
driver's business, so a node made from an element covers the element and a
property made from an attribute covers the attribute, and nothing in a shaper
computes an offset. `src/formats/bt_xml.cpp` is the worked example, and
`tree_shape.h` drives the same interface from a tree another provider built,
which is how a format on top of JSON is written until JSON has a walker of its
own.

A format that is neither XML nor JSON builds the tree itself. Build it depth
first in document order, which gives the rest of the tool two invariants it
relies on: a parent has a lower index than its children, and a node's
descendants occupy a contiguous run of indices after it.

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
they claim, that identity is strong where you meant it to be, and that a node
survives the edit you built the provider to survive.
