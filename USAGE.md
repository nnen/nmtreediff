Using NM Tree Diff
==================

NM Tree Diff compares two files that hold tree-shaped data and shows you what
changed, either as text or as a graph of nodes. This is the user guide. If you
want to teach it a file format of your own, read
[docs/PROVIDERS.md](docs/PROVIDERS.md) instead.

Getting it
----------

There is no download yet, so build it. You need CMake 3.25 or newer and a C++20
compiler. Everything else is fetched and pinned by the build.

```bash
cmake -S . -B build
```

```bash
cmake --build build --config RelWithDebInfo
```

The program lands at `build/bin/RelWithDebInfo/nmxmldiff`. The rest of this
guide calls it `nmxmldiff`.

Comparing two files
-------------------

Give it two paths. The first is the older side, shown on the left.

```bash
nmxmldiff testdata/sample/tree_before.xml testdata/sample/tree_after.xml
```

The window opens immediately and the files are read behind it, so a large pair
never leaves you looking at nothing. Both sides are compared twice: once as
lines of text, and once as a tree of nodes. The text alignment usually finishes
first and the text view becomes usable while the tree is still being matched.

Both paths are optional. Launch with none and the window opens on a welcome
pane where you can choose them. Launch with one and that side is already
chosen, so the window asks only for the other.

Opening files from the window
-----------------------------

The **File** menu has **Open left** and **Open right**, and the welcome pane has
a button for each side. Both open your system's own file dialog, so recent
places, typed network paths and the usual sorting all work as they do
everywhere else.

Choosing a file for one side while the other is already chosen starts the
comparison at once. The dialog opens in the directory the other side came from,
which is usually where both versions live.

The dialog's type list is built from the formats the tool knows at that
moment. The first entry, selected by default, admits every extension any
format claims; after it comes one entry per format, under the format's own
name, listing its extensions and any a configuration file pointed at it. A
format defined in a script that claims `.blackboard` is offered as an entry of
its own, and the list follows a Reload. The last entry admits any file, for
the suffix nothing claims.

The window keeps drawing while the dialog is open, so a comparison already on
screen stays readable while you pick its replacement.

Titles for the two sides
------------------------

By default each side is titled with its path. When a version control system
hands you a temporary file, that path says nothing, so override it:

```bash
nmxmldiff --left-label "tree.xml #14" --right-label "tree.xml #15" old.xml new.xml
```

The two views
-------------

**Escape** closes the tool from anywhere, with no confirmation. That is
deliberate, and it is what other version control diff tools do: reviewing a
changelist means opening one file after another, and dismissing each with a
single key is what makes a long list bearable. If a menu is open, Escape closes
that first.

Switch with **Ctrl+1** for text and **Ctrl+2** for the node view, or from the
View menu. Both views show the same comparison and share one selection, so a
node picked in one is the node selected in the other. **Ctrl+R** reloads
everything: the configuration files are read again, a format defined in a
script is rebuilt from what is on disk now, and both files are read and
compared afresh. Edit a shape function, press Ctrl+R, and the tree is the new
script's answer, without restarting the tool.

**F8** goes to the next change and **Shift+F8** to the previous one, from
anywhere. Changed lines and changed nodes are two different lists, so this
follows whichever view you are looking at.

In the node view, **n** and **b** do the same thing without a modifier. They
walk the tree depth first, so a change deep inside a branch comes before the
next branch begins, and they carry on from whatever is selected rather than
from wherever you last stopped. The view glides to each one rather than
jumping, so you can see whether the next change was next door or halfway
across the document. Dragging or zooming stops the glide at once.

### The text view

A conventional side-by-side line diff. Within a line that was rewritten rather
than replaced, the words that actually changed are picked out, so a small edit
inside a long line does not read as a whole new line.

The gutter marks each changed, added and deleted line. The strip beside the
scrollbar shows the change density of the whole file, so you can see where the
edits are concentrated without scrolling through it.

### The node view

Both documents are drawn as a single graph, coloured by what happened to each
node: added, deleted, modified, moved, or unchanged.

- **Drag** to pan and use the **wheel** to zoom. Labels scale with the zoom
  and fade out only when they would be too small to read.
- **Double-click** a node to collapse or expand it. A collapsed node is
  replaced by a chip saying how many nodes it stands for.
- **Right-click** for a menu: collapse or expand the node under the pointer,
  collapse everything unchanged, expand everything, fit the graph to the
  window, or change the direction. Right-clicking empty canvas gives the same
  menu without the item for a single node.
- **Fit** frames the whole graph. **Expand all** opens every collapsed group.
  **Collapse unchanged** hides the subtrees with nothing to report.
- A **dashed edge** traces a node that moved, running from where it used to be
  to where it is now.
- The **minimap** in the corner shows the whole graph with the visible area
  marked on it.

The graph runs **top down** by default, which suits a wide, shallow tree.
**View, Graph direction**, or the same item in the right-click menu, switches it
to **left to right**, which suits a deep one. A format may choose for itself: behaviour trees are drawn left to right
whatever the menu says, because a behaviour tree is deep and narrow and reading
it left to right matches the execution order it describes. Changing the
direction lays the graph out again, which is why the graph blinks.

### The details panel

Beside the two views sits a panel with the size of each side in bytes, lines and
nodes, the format that was chosen, and an outline of the newer document. Every
entry is coloured by what happened to it.

The properties under each node say what changed and what it changed from, which
is the one thing the node graph cannot show you:

| Row | Meaning |
| --- | --- |
| `speed 1.0 -> 1.4` in amber | The value changed |
| `fresh new` in green | The property is new |
| `doomed yes ->` in red | The property was removed |
| `transform {}` or `tags [3]` | A property with parts, which opens |
| plain | Unchanged |

A property with parts opens by itself when something inside it changed, so you
do not have to go looking. A name in braces is a record, whose parts are named
and whose order means nothing. A name with a count in brackets is a list, whose
parts are positional, so reordering one is a change.

A removed property is listed after the others. It has no row of its own in the
newer document, so without this the one thing you could not see would be the
thing that was taken away.

### The Output pane

Everything the program writes, on standard output and standard error, is kept
and shown in a pane beside the status bar: configuration problems, what a
script's `print` said, and the whole of any error a script raised, traceback
included. Launched from the desktop or by a version control client the tool
has no console, so this pane is the only place that text exists.

It is closed in a fresh layout, so a reader going through a changelist is not
shown a log. **View, Output** opens it, and the first error the pane has not
shown yet opens it too. Error lines are coloured. **Clear** empties the log,
**Copy** puts the whole of it on the clipboard, and **Follow** keeps the
newest line in view.

Formats
-------

Three formats are built in. Ask the program what it has:

```bash
nmxmldiff --list-formats
```

- **XML**, where every element is a node and every attribute is a property.
- **JSON**, where objects are nodes and so is an array holding objects. A list
  of scalars is one property rather than a subtree, so a changed tag reads as
  one line. Members of an object are unordered, so reordering them is not a
  change, while reordering a list is.
- **Behaviour tree**, a worked example of a format that knows its own schema.

A format is chosen by extension first, and by a look at the first bytes when
the extension is unfamiliar. Override that when it guesses wrong:

```bash
nmxmldiff --format xml odd_suffix.dat other.dat
```

There are two behaviour trees in `testdata/sample`. The small one, `tree_before`
and `tree_after`, shows why a format that knows its own schema is worth having. Read by its own provider it is four nodes, titled by
what each one does. Read as generic XML with `--format xml` it is eight, half
of them called `property`, and a changed speed reads as an edit to an anonymous
element rather than as a property of the behaviour it belongs to.

The larger pair, `guard_before.bt` and `guard_after.bt`, is a guard's brain
twenty-six nodes deep in places. It is there to try the node view on something
that does not fit on screen at once, and it contains one of every kind of
change: an edited property, an added one, a node inserted, a node deleted, a
node moved to a new parent, and a node whose type changed. That last one still
matches, because the format anchors on the identifier rather than on the shape
of the tree.

Configuration
-------------

Configuration is a Lua script. Three files are read, each overriding what came
before:

1. `~/.nmtreediff.lua`
2. `~/.nmtreediff/config.lua`
3. whatever you pass with `--config`

A missing file in your home directory is fine. A missing `--config` is an error,
because asking for a file and silently not getting it is the kind of failure
noticed weeks later.

```lua
-- Point your own suffixes at a format the tool already knows.
formats {
  [".bt"]        = "bt",
  [".leveldata"] = "json",
}

fallback "xml"
graph_direction "left_to_right"
exit_key "escape"
```

`--format` still beats all of it, because that is you correcting a guess now
rather than a standing decision. A copy to start from is at
`testdata/sample/providers.lua`.

Because it is a script rather than a list, a studio with many suffixes writes a
loop:

```lua
local map = {}
for _, suffix in ipairs{ ".mesh", ".anim", ".mat" } do
  map[suffix] = "json"
end
formats(map)
```

Every mistake in a script is reported at once, with the file and, where Lua
knows it, the line, and an error a script raised carries its traceback below
that. A configuration with any mistake in it stops the run rather than being
half applied, because a comparison read by the wrong format looks like a
working comparison.

The same rule holds in the window. **Ctrl+R** reads all three files again and
rebuilds every format they define; if any of them now has a mistake, the
problems go to the Output pane and the formats you had stay in force, while the
two files are still read again. So editing a script and pressing Ctrl+R is the
whole loop, and a mistake costs one look at the pane rather than a restart.

Teaching it your own format
---------------------------

A format of your own needs no compiler. A script sits on top of XML or JSON:
that format reads the file, and your script decides what the result means by
building the tree it stands for.

```lua
provider "bt" {
  display_name = "Behavior tree",
  base = "xml",
  extensions = { ".bt", ".btree" },

  shape = function(doc, out)
    local function visit(element, parent)
      if element.name == "node" then
        -- What it does, not what the element is called, and the editor's
        -- GUID, which makes this the same node however far it moved.
        local node = parent:child(element):set_name(element.attr.type or "node")
        node:set_identity(element.attr.id, "strong")
        for attribute in element:properties() do node:property(attribute) end
        for child in element:children() do out:next(visit, child, node) end
      else
        -- <property name= value=/> describes the node it sits in.
        parent:property(element):set_name(element.attr.name or element.name)
              :set_value(element.attr.value or element.text)
      end
    end
    local root = out:root(doc.root)
    for child in doc.root:children() do out:next(visit, child, root) end
  end,
}
```

`doc` is the file as XML read it and `out` is the tree going out. The script
walks the document itself; `out:next` queues a call instead of making it, so a
deeply nested file costs memory rather than the stack.
`testdata/sample/behaviortree.lua` is a complete worked example. Every entry a
script may write is listed under [Lua API reference](#lua-api-reference), and
[docs/PROVIDERS.md](docs/PROVIDERS.md) covers writing a format in C++.

Changing the exit key
---------------------

Escape closes the window. If that fights your habits, change it:

```lua
exit_key "q"
```

Or turn it off with `exit_key "none"`. `--exit-key` overrides the script for one
run. Single letters, the function keys and `escape` are the names it knows.

Lua API reference
-----------------

This is the whole of what a script may say. Nothing else is exposed, and an
entry not listed here is ignored rather than being an error.

A script runs in a plain Lua interpreter with the base, `string`, `table`,
`math`, `os`, `io` and `package` libraries open. The `coroutine` and `debug`
libraries are not opened. The tool trusts these files the way a shell trusts a
startup file, because they are the user's own: two under the home directory,
and one named on the command line.

### Global functions

Five functions are defined, and only while a configuration script is being
read. All five may be called any number of times.

| Call | Argument | What it does |
| --- | --- | --- |
| `formats(table)` | extension to format name | Points suffixes at a format |
| `fallback(name)` | format name | Chooses the format for a file nothing claims |
| `graph_direction(word)` | `"top_down"` or `"left_to_right"` | Sets the direction the graph starts in |
| `exit_key(name)` | key name | Sets the key that closes the window |
| `provider(name)` | format name | Declares a format, called again with its body |

**`formats`** takes a table whose keys are extensions and whose values are
format names. A key must start with a dot and have something after it, or the
run stops with a message naming it. Keys are lower-cased, so `.BT` and `.bt`
are one entry. Later entries win over earlier ones, across files as well as
within one, so the same suffix named twice behaves the way a reader expects.

**`fallback`** names the format used when no format claims a file. Leaving it
unset keeps the built-in choice, which is generic XML.

**`graph_direction`** takes one of two words and nothing else. It sets the
direction the graph starts in, and the view's own control changes it later. A
format that states a direction of its own wins over both.

**`exit_key`** takes `escape` or `esc`, a single letter `a` through `z`, a
function key `f1` through `f12`, or `none` to leave no key bound. Names are
matched without regard to case. An unknown name binds no key. `--exit-key`
overrides it for one run.

**`provider`** is described below.

A format name given to any of these must be one the tool knows, either built in
or declared by a script. An unknown name stops the run rather than being passed
over, because it is nearly always a typo in a diff-tool configuration.

### Declaring a format

`provider` is called twice, once with the name and once with the body:

```lua
provider "name" { ... }
```

That is ordinary Lua, not special syntax. The first call returns a function
that takes the table, so the name reads before the body it names. The name is
what `--format` accepts, and an empty one is an error.

The body may hold these entries. Every one is optional.

| Entry | Type | Default |
| --- | --- | --- |
| `display_name` | string | the provider's name |
| `base` | string | `"xml"` |
| `extensions` | list of strings | none |
| `graph_direction` | string | inherit |
| `property_order` | list of strings | none |
| `shape` | function | the document is copied one to one |

**`display_name`** is the name shown to a person, in the format list and in the
window.

**`base`** names the format that does the parsing, matched without regard to
case. It must be a format already registered, in practice `xml` or `json`. A
base the tool does not know stops the run.

**`extensions`** are the suffixes this format claims, dots included and
lower-cased on load. Anything in the list that is not a string is skipped. A
scripted format is registered ahead of the built-ins, so claiming a suffix a
built-in already owns wins the tie. That is worth being deliberate about: it is
also why the shipped behaviour-tree script claims none and is asked for by
name.

**`graph_direction`** takes the same two words as the global function and
applies to this format alone. An unknown word is reported and the entry
ignored.

**`property_order`** lists property names that sort first, in the order they
sort. Names are compared as written, without case folding. This is presentation
only. It decides what the node card, the details panel and the change list show
first, and it never affects matching, which compares properties as an unordered
set whatever this says.

### The shape function

`shape(doc, out)` is the script's opinion about what the parsed file means.
It is called once per document while the document is read, never once per
frame, with the document as the base format read it and the builder the tree
goes out through. A declaration with no `shape` reads exactly like the format
it sits on.

**The script owns the walk.** Nothing here walks the document for you, and
nothing stops a function calling itself for each child if you write it that
way. What the builder offers instead is a queue: `out:next(fn, ...)` runs `fn`
with those arguments once the current call returns and before anything queued
earlier, and `out:later(fn, ...)` runs it after everything queued earlier.
Everything one call queues with `next` runs in the order it was queued, so
queueing one call per child visits the children first to last. A document
nested thousands of levels deep then costs memory rather than the interpreter's
stack, which is why `next` is the idiom every example uses.

**Build order is free.** Sibling order is the order of `child` and `property`
calls on one parent handle, and nothing else, so a queued walk, a breadth-first
one and a plain recursion all build the same tree.

**An error in one call is not the end of the document.** Whatever the call
built stays, whatever it never queued is missing, and the failure is recorded
against the first element and the first handle among the call's arguments,
which is why the idiom is `out:next(visit, element, parent)`. The text view
marks the element, the node view marks the card, and a headless report lists
the message and exits 2. An error inside `shape` itself before any node exists
fails the parse, since a tree of zero nodes is not a partial result. The card
and the report carry one line; the whole error, traceback included, goes to
standard error and to the Output pane, naming the script file and line.

**`print` is yours.** What a script prints goes to the same place, on standard
output, so a `print(element.name)` while writing a shape function reads
beside whatever went wrong with it, and a window launched from the desktop,
which has no console, still shows it.

**Content may be left out, and the tool says so.** An element no handle was
made from is dropped, its bytes are marked in the text view and counted in the
report, and the run is not failed for it. Making a handle from an element,
through `out:root(element)`, `ref:child(element)` or `ref:property(element)`,
is what counts the element as represented.

#### The document

| Entry | What it is |
| --- | --- |
| `doc.root` | The outermost element |
| `doc.size` | How many elements the document has |
| `doc.base_format` | The name of the format that read it |
| `doc:at(id)` | The element with that id, or nil |

An element is what the base format read: the tag name for XML; for JSON the
member key, `$` for the outermost value and `item` for every element of an
array, with every array a node and every element of it an `item`. Generic
JSON's own folding of scalar arrays into properties is not applied for a
script, which folds what it wants.

| Entry | What it is |
| --- | --- |
| `element.name` | The element's name as the base format read it |
| `element.text` | Its text content, or empty |
| `element.id` | Its id, for `doc:at` |
| `element.depth` | Distance from the root, which is zero |
| `element.attr[name]` | The value of the first property with that name, or nil |
| `element.parent` | The enclosing element, or nil for the root |
| `element.first_child`, `element.last_child` | The first and last child, or nil |
| `element.next_sibling`, `element.prev_sibling` | The neighbours, or nil |
| `element.child_count` | How many children it has |
| `element:child_at(i)` | The i-th child, one-based |
| `element:child(name)` | The first child with that name, or nil |
| `element:children()` | An iterator over the children, in document order |
| `element:children(name)` | An iterator over every child with that name |
| `element.property_count` | How many properties it has, repeats included |
| `element:property_at(i)` | The i-th property, one-based |
| `element:property(name)` | The first property with that name, or nil |
| `element:properties()` | An iterator over every property, repeats included |
| `element:properties(name)` | An iterator over every property with that name |

`element.attr` is a shortcut and it is honest only for a scalar and for a
record or sequence that carries a value; a property that is only parts answers
with an empty string. Names may repeat, and the shortcut answers the first.
XML records a leaf's text under the property `#text`, so a loop over
`properties()` sees it among the attributes.

| Entry | What it is |
| --- | --- |
| `property.name` | The property's name |
| `property.value` | Its value, in any form |
| `property.form` | `"scalar"`, `"record"` or `"sequence"` |
| `property.part_count` | How many parts it has |
| `property:part_at(i)` | The i-th part, one-based |
| `property:part(name)` | The first part with that name, or nil |
| `property:parts()` | An iterator over the parts |

#### The builder

| Entry | What it does |
| --- | --- |
| `out:root(kind)` or `out:root(element)` | Creates the root node, once |
| `out:at(id)` | Rehydrates a handle from `ref.id` |
| `out.node_count` | How many nodes exist so far |
| `out.pending` | How many queued calls are waiting |
| `out.cancelled` | Whether the comparison was cancelled |
| `out:next(fn, ...)` | Queues a call to run before what is already queued |
| `out:later(fn, ...)` | Queues a call to run after what is already queued |

#### The handle

Every node and every property the builder hands out is one kind of handle, and
`child` does the right thing wherever the handle stands:

| Standing on | `ref:child(name)` | `ref:child()` |
| --- | --- | --- |
| A node | a child node of that kind | an error: a node needs a kind |
| A scalar property | promotes it to a record and adds a named part | promotes it to a sequence and adds an item |
| A record | a named part | an error |
| A sequence | a named item | an unnamed item |

Promotion keeps the scalar's value. The `property` family always makes a
property, wherever it is called: a property of a node, or a part of a
property. Every setter returns the handle, so calls chain.

| Entry | What it does |
| --- | --- |
| `ref:child(name)`, `ref:child()` | See the table above |
| `ref:child(element)` | The same, named after the element, with its span and source |
| `ref:property(name, value)` | A scalar property; call twice for two of one name |
| `ref:property(property)` | A copy of a document property, parts and all |
| `ref:property(element)` | An empty scalar named after an element, with its span and source |
| `ref:record(name)` | A property that will hold named parts |
| `ref:sequence(name)` | A property that will hold positional parts |
| `ref:item(value)` | An unnamed item; a sequence only |
| `ref.kind` | `"node"` or `"property"` |
| `ref.form` | The property's form, or `"scalar"` for a node |
| `ref.is_node` | Whether it is a node |
| `ref.id` | An id `out:at` accepts, for carrying across a queued call |
| `ref.parent` | The enclosing node or property, or nil for the root |
| `ref.owner` | The nearest enclosing node, itself when it is one |
| `ref:set_name(s)` | The kind of a node, or the name of a property |
| `ref:set_value(s)` | A property's value; an error on a node |
| `ref:set_form(word)` | `"scalar"`, `"record"` or `"sequence"` outright, for a property with no parts to promote through |
| `ref:set_children_ordered(b)` | Whether sibling order under a node means anything; an error on a property |
| `ref:set_identity(value, "strong")` | What makes a node the same node across versions; nil for none |
| `ref:set_title(title, subtitle)` | The card's two lines; the kind when unset |
| `ref:set_accent(rgb)` | The card's colour as a number, `0xRRGGBB`; derived from the kind when unset |

**`set_identity`** returns `"strong"` as the second value to say the identity
may travel: two nodes carrying it are the same node however far apart they have
moved, and a node survives even a change of kind. The reverse holds too: two
nodes carrying different strong keys are never the same node, however alike
they look, so a sibling replaced under a new id is a deletion and an
insertion rather than an edit. A node with a key may still pair with one that
has none. A strong key that appears more than once on either side identifies
nothing and anchors nothing, rather than being guessed at. Any other second
value, or none, gives a weak key,
which is only a hint the matcher is free to ignore, and today it does. A nil
value sets no identity, so `set_identity(element.attr.id, "strong")` reads
naturally on an element without one.

There is no span type in a script. A span arrives only with the element or
property a handle was made from, and a node made from a name alone has none,
which is the right answer for a span that cannot be computed honestly.

```lua
provider "bt-lua" {
  display_name = "Behavior tree (script)",
  base = "xml",
  extensions = {},
  graph_direction = "left_to_right",
  property_order = { "id", "type", "name" },

  shape = function(doc, out)
    local function visit(element, owner)
      if element.name == "node" then
        local node = owner:child(element):set_name(element.attr.type or "node")
        node:set_identity(element.attr.id, "strong")
        for attribute in element:properties() do node:property(attribute) end
        for child in element:children() do out:next(visit, child, node) end
      elseif element.name == "property" then
        owner:property(element):set_name(element.attr.name)
             :set_value(element.attr.value or element.text)
      else
        owner:property(element)
        for child in element:children() do out:next(visit, child, owner) end
      end
    end
    local root = out:root(doc.root)
    for child in doc.root:children() do out:next(visit, child, root) end
  end,
}
```

### When a script runs

A configuration script runs once at startup, in the order the three files are
read. It also runs again, in a fresh interpreter, each time a file is parsed in
that format, once per side of a comparison and on a worker thread rather than
the frame loop. The text read at startup is what runs, so editing the file
halfway through a comparison cannot change what it is doing.

Two consequences are worth planning for. A script should be free of side
effects outside its own declarations, because running it twice must mean the
same as running it once. And during a reread only `provider` has any effect:
`formats`, `fallback`, `graph_direction` and `exit_key` are bound to do nothing,
so a script cannot move the window's settings from a worker thread.

A long-running script is interrupted when the comparison is cancelled, roughly
every ten thousand Lua instructions. That surfaces as an error named
`cancelled`, which a script should not try to catch.

Nothing in the tool walks a document by recursion. A file nested thousands of
levels deep is read, shaped, hashed and drawn with an explicit stack or queue
at every step, so its depth costs memory rather than the process. A script is
held to the same rule: when it has work to do for each element under another,
it queues that work rather than calling itself, and the tool drains the queue.

### Where mistakes are reported

Every mistake in a script is reported at once, with the file and, where Lua
knows it, the line. A script that fails to run and a script that runs but asks
for something impossible are both failures. Either stops the run with exit code
2 rather than being half applied, because a comparison read by the wrong format
looks like a working comparison.

Running without a window
------------------------

`--headless` runs the whole comparison and writes a report to standard output.
This is how a script or a build job would use it.

On Windows the program is built so that a launch from the desktop opens no
console window, and it finds its output rather than assuming one: output you
redirect to a file or a pipe goes there, and otherwise it writes to the console
it was started from. One thing follows from that. A command prompt does not
wait for such a program, so a headless run typed at a prompt hands the prompt
back before the report appears, and the two interleave. Pipe the output, send
it to a file, or run it from a script, and none of that applies; a version
control tool waits on the process and is unaffected.

```bash
nmxmldiff --headless testdata/sample/tree_before.xml testdata/sample/tree_after.xml
```

Add `--report json` for a machine-readable version of the same thing. Add
`--exit-code` to have the result decide the exit status.

The result is the tree's whenever a format resolved. A reformat is not a
change, and the tree is what knows that, so a pair that differs only in
whitespace exits 0 however many lines moved. The line diff decides only for a
file no format claims, and the report says which one answered under
`comparison`. What a format leaves out of the tree is left out of the verdict
too: a change inside dropped content is the format's decision not to see.

| Exit status | Meaning |
| --- | --- |
| 0 | Nothing changed, by the deciding comparison, or `--exit-code` was not given |
| 1 | Something changed, and `--exit-code` was given |
| 2 | Something went wrong: a file would not open, would not parse, an option was rejected, or a scripted format raised an error while shaping |

A format may leave content out of the tree, and a scripted format may fail on
an element. The text report says so after the change list, one `warning:`
line per side for dropped content and one `failed:` line per failure with the
file, the line and the message, and the JSON report carries the same under
`dropped` and `failures`. Dropping never changes the exit status; a failure
always makes it 2, whatever `--exit-code` says, because a script that raised
is a bug and a build job must not read the comparison as sound.

The text report begins with both files and the line diff, labelled `lines:`.
When a format resolved, the node counts follow and then the change list, one
node per line, and the change list is the verdict: `identical` when it is
empty, whatever the lines did. A `warning:` line before it says when the
similarity pass gave up on a container too wide to finish inside its budget;
the children of that container that nothing else paired are then reported
added and deleted rather than matched, and nothing elsewhere in the file is
affected.

| Mark | Meaning |
| --- | --- |
| `+` | Added on the right |
| `-` | Deleted from the left |
| `~` | Modified, with the changed property names after it |
| `>` | Moved, with the old path and the new one |
| `~>` | Moved and modified |

The JSON report carries the same list under `tree.changes`, one entry per
changed node with its `status`, whether it `moved`, its path on each side
(`null` on the side it is not on) and the `properties` that differ, so a
script gets what a person gets. `tree.trimmedContainers` counts the containers
the similarity pass gave up on, zero when the matching is complete.

Using it from version control
-----------------------------

**This is not a replacement for your usual diff tool.** It reads tree-shaped
data and has nothing useful to say about source code, so point it at the file
types it understands and leave everything else alone. Both of the systems below
can do that, but neither does it the way the rest of this guide sets things up,
and the details differ more than you would hope.

### Perforce

Per-extension diff applications are a P4V setting, in **Preferences, Diff**.
Add an entry, choose the extension, browse to `nmxmldiff.exe`, and leave the
arguments field to pass the two files. Perforce substitutes `%1` and `%2` for
them.

There is no command-line equivalent. The `P4DIFF` environment variable names one
diff program for every text file, so setting it to this tool would send your
source code here too. Set it only if that is genuinely what you want.

### Git

Git does this through `.gitattributes` and a named diff driver, which is all
settable from the command line. Two steps. First, say which files the driver
handles:

```bash
echo '*.bt diff=treediff' >> .gitattributes
```

Then define the driver:

```bash
git config diff.treediff.command /path/to/git-treediff.sh
```

The wrapper is needed because Git hands an external diff seven arguments, of
which the two files are the second and the fifth:

```bash
#!/bin/sh
# $1 path, $2 old file, $3 old hash, $4 old mode, $5 new file, ...
exec nmxmldiff --left-label "$1 (old)" --right-label "$1" "$2" "$5"
```

This drives `git diff`. `git difftool` is a separate mechanism with one tool for
everything, so it is the wrong door for a tool that only handles some files.

### Not verified yet

None of the above has been run against a real client. The mechanisms are what
the Perforce and Git documentation describe, and the exact strings still need
checking on a real installation.

Neither system passes this tool the name a file has in the repository, except
through the wrapper above, so a Perforce diff of two revisions shows two
temporary paths as its titles. `--left-label` and `--right-label` are how you
improve on that where the system gives you somewhere to put them.

Every option
------------

| Option | What it does |
| --- | --- |
| `--format <name>` | Use this format instead of guessing |
| `--left-label <text>`, `--right-label <text>` | Title each side, instead of using its path |
| `--view text\|node` | Which view to open on |
| `--config <file>` | Read extension mappings from this file |
| `--list-formats` | Print the formats this build reads, then exit |
| `--headless` | Run with no window and write a report |
| `--report text\|json` | Which report to write |
| `--exit-code` | Let the result decide the exit status |
| `--version` | Print the version, then exit |
| `--help` | Print the options, then exit |

Two more exist for development. `--max-frames` renders a fixed number of frames,
prints timings and exits, and `--screenshot` writes the last frame to a bitmap.

When something goes wrong
-------------------------

**The window opens empty.** You passed no files. Give it two paths.

**"two files are required".** You passed one. It needs both sides.

**The wrong format was chosen.** Check what it picked with `--headless`, which
names the format in its report, then override it with `--format` or map the
extension in a configuration file.

**A file will not parse.** The message says why. The tool reports a file it
cannot read rather than showing you half of it, because half a tree compared
against a whole one is worse than an error.

**Part of the file is marked grey in the text view.** The format left it out
of the tree. That is the format's decision, made in its script or its code,
and the mark is there so you can see what the node view is not showing. Turn
the marks off under View if the format is one you trust.

**Part of the file is marked violet, and a card has a violet corner.** A
scripted format raised an error on that element. What the script built before
the error is shown and what it never got to is missing, so a change reported
under that card may be an artefact of the failure. The status bar counts these,
the Output pane has the error with its traceback, and a headless report prints
the message.

**Nothing appears in the terminal.** Launched from the desktop the tool has no
console, and what it writes goes to the Output pane instead. Launched from a
prompt it writes to that prompt's console, but the prompt does not wait for it,
so the report can land after the prompt has come back; pipe or redirect the
output and it arrives where you sent it.

**A script edit had no effect.** Press Ctrl+R. The configuration is read at
launch and again only on Reload; if the edited script now has a mistake, the
Output pane says so and the previous version stays in force.

**The report says the match was reduced.** A very large pair trims the more
expensive matching passes to stay responsive. Some moves will be reported as a
deletion next to an addition. The warning tells you when this happened, so a
comparison never quietly under-reports.
