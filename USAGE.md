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
node picked in one is the node selected in the other. **Ctrl+R** reloads both
files from disk.

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

- **Drag** to pan and use the **wheel** to zoom.
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
knows it, the line. A configuration with any mistake in it stops the run rather
than being half applied, because a comparison read by the wrong format looks
like a working comparison.

Teaching it your own format
---------------------------

A format of your own needs no compiler. A script sits on top of XML or JSON:
that format does the parsing, and your script decides what the result means.

```lua
provider "bt" {
  display_name = "Behavior tree",
  base = "xml",
  extensions = { ".bt", ".btree" },

  exit = function(el, out)
    if el.name == "node" then
      -- What it does, not what the element is called; and the editor's GUID,
      -- which makes this the same node however far it moved.
      out:node(el.attr.type, el):identity(el.attr.id, "strong"):attributes(el):adopt(el.items)
    elseif el.name == "property" then
      -- <property name= value=/> describes the node it sits in.
      out:property(el.attr.name, el.attr.value, el)
    end
  end,
}
```

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
| `enter` | function | nothing is steered |
| `exit` | function | every element gets the default treatment |

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

### The shaping functions

The two function entries are the script's opinion about what the parsed file
means. Both are optional. `enter` is called once per element as it starts and
`exit` once as it ends, while the document is read, never once per frame.

**`enter(el, frame)`** is called when an element starts, before anything
inside it. Only `el.name`, `el.attr`, where the element starts and its
ancestors are known. It never emits. It may set `frame.data`, which every
element inside can read back as the ancestor's `data`, and it may steer:
`frame:default()` gives this element and everything inside it the default
treatment with no further callbacks, and `frame:opaque()` keeps the element as
one property, named after it, holding its raw text, with nothing inside it
visited.

**`exit(el, out)`** is called when the element ends, after everything inside
it. By then each element inside has already become a node or a property, and
those sit in `el.items` in document order. `exit` says what this element
becomes through `out`:

| Call | What it does |
| --- | --- |
| `out:node(kind, el)` | Emits a node spanning `el`, and returns a node handle. Pass `nil` for no span. |
| `out:property(name, value, el)` | Emits a property spanning `el`, and returns a property handle. |
| `out:forward(el.items)` | Passes the items up to the element above, unchanged. |
| `out:default(el)` | Applies the default treatment: a node named after the element, with its attributes, its text and its items. |
| `out:drop()` | Keeps nothing of this element and discards its items. |

A node handle takes these, each returning the handle so calls chain:

| Call | What it does |
| --- | --- |
| `:attributes(el, ...)` | Adds every attribute as a property, except the names listed. |
| `:text(el)` | Adds the element's text as the `#text` property, when there is any. |
| `:property(name, value, el)` | Adds one property, spanning `el` or nothing. |
| `:adopt(el.items)` | Takes the items in: nodes become children, properties become properties. |
| `:kind(name)` | Changes the node's kind, which is the word shown on the card and the word the colour is derived from. Two nodes of one kind always get one colour, so a script never chooses a colour. |
| `:identity(value, "strong")` | What makes this the same node across versions. With `"strong"` the identity may travel: two nodes carrying it are the same node however far apart they have moved, and a node survives even a change of kind. A strong key that appears more than once on either side identifies nothing and anchors nothing. Without `"strong"` the key is a hint the matcher is free to ignore, and today it does. |
| `:title(first, second)` | The card's title and optional subtitle. Absent, the card shows the kind. |
| `:ordered(false)` | Says the order of the node's children carries nothing, so a reordering is not a change. |

A property handle takes these:

| Call | What it does |
| --- | --- |
| `:value(v)` | Sets the value. |
| `:part(name, value, el)` | Adds one part. |
| `:attributes(el, ...)` | Adds every attribute as a part, except the names listed. |
| `:adopt(el.items)` | Takes the items in as parts. A node among them becomes a part named after its kind. |
| `:ordered(true)` | Says the parts are a sequence, so reordering them is a change. |
| `:collapse()` | Folds a single plain part into the value, so `<x a="1"/>` reads as `x = 1`. |

**Nothing is dropped.** An `exit` that says nothing about an element gives it
the default treatment. Items an `exit` neither adopts nor forwards are
forwarded for it. `out:drop()` is the only way to lose content.

**What an element shows.** `el.name`; `el.attr`, a table of attribute values by
name; `el.text`, empty until exit and empty on an element that holds elements;
`el.span`, a table with `start` and `stop` byte offsets; `el.depth`, zero for
the document element; `el.index` among its siblings; `el.child_count`;
`el.closed`; `el.parent`, or `nil`; `el:ancestor(name)`, the nearest ancestor
of that name or `nil`; `el.data`, what `enter` left there; and `el.items`.
Nothing below an element is reachable except through `el.items`, and nothing
to its right at all.

**What JSON looks like.** On a JSON base the document's outermost value is an
element named `$`. An object's `el.attr` holds `#type` and its scalar members,
each exactly as written, quotes included; its object and array members are the
elements inside it, named after their keys. An array is an element with `#type`
set to `array` holding one element named `item` per value, and a scalar inside
an array is an element whose `el.text` is the value as written. Left
unmentioned, an array under an object that holds no object anywhere inside
becomes one ordered property, exactly as generic JSON reads it.

**Handles do not outlive their callback.** A builder or a node handle used after
the `exit` that made it, or an element used after it closed, raises a Lua error
naming the problem. An error raised inside `enter` or `exit` is read as no
answer: what was emitted before it stays, and the rules above keep the rest.

```lua
provider "nested" {
  base = "xml",
  extensions = { ".nested" },

  enter = function(el, frame)
    if el.name == "node" then frame.data = el.attr.id end
    if el.name == "editor" then frame:opaque() end
  end,

  exit = function(el, out)
    if el.name == "node" or el.name == "child" then
      out:node(el.name, el):attributes(el):adopt(el.items)
    elseif el.name == "children" then
      out:forward(el.items)   -- the wrapper vanishes, the children stay
    end
  end,
}
```

`testdata/sample/nested_children.lua` is this example complete, and
`testdata/sample/behaviortree.lua` is the behaviour tree written in script,
held by the tests against the compiled provider.

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

```bash
nmxmldiff --headless testdata/sample/tree_before.xml testdata/sample/tree_after.xml
```

Add `--report json` for a machine-readable version of the same thing. Add
`--exit-code` to have the result decide the exit status.

| Exit status | Meaning |
| --- | --- |
| 0 | The files are identical, or `--exit-code` was not given |
| 1 | The files differ, and `--exit-code` was given |
| 2 | Something went wrong: a file would not open, would not parse, or an option was rejected |

The text report ends with a list of what happened to each node, one per line:

| Mark | Meaning |
| --- | --- |
| `+` | Added on the right |
| `-` | Deleted from the left |
| `~` | Modified, with the changed property names after it |
| `>` | Moved, with the old path and the new one |
| `~>` | Moved and modified |

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

**The report says the match was reduced.** A very large pair trims the more
expensive matching passes to stay responsive. Some moves will be reported as a
deletion next to an addition. The warning tells you when this happened, so a
comparison never quietly under-reports.
