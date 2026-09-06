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

Switch with **Ctrl+1** for text and **Ctrl+2** for the node view, or from the
View menu. Both views show the same comparison and share one selection, so a
node picked in one is the node selected in the other. **Ctrl+R** reloads both
files from disk.

**F8** goes to the next change and **Shift+F8** to the previous one. Changed
lines and changed nodes are two different lists, so this follows whichever view
you are looking at.

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
- **Fit** frames the whole graph. **Expand all** opens every collapsed group.
  **Collapse unchanged** hides the subtrees with nothing to report, each one
  replaced by a chip saying how many nodes it stands for.
- A **dashed edge** traces a node that moved, running from where it used to be
  to where it is now.
- The **minimap** in the corner shows the whole graph with the visible area
  marked on it.

The graph runs **top down** by default, which suits a wide, shallow tree.
**View, Graph direction** switches it to **left to right**, which suits a deep
one. A format may choose for itself: behaviour trees are drawn left to right
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
| plain | Unchanged |

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
- **JSON**, where objects, arrays and array elements are nodes. Members of an
  object are treated as unordered, so reordering them is not a change, while
  reordering an array is.
- **Behaviour tree**, a worked example of a format that knows its own schema.

A format is chosen by extension first, and by a look at the first bytes when
the extension is unfamiliar. Override that when it guesses wrong:

```bash
nmxmldiff --format xml odd_suffix.dat other.dat
```

The behaviour tree in `testdata/sample` shows why a format that knows its own
schema is worth having. Read by its own provider it is four nodes, titled by
what each one does. Read as generic XML with `--format xml` it is eight, half
of them called `property`, and a changed speed reads as an edit to an anonymous
element rather than as a property of the behaviour it belongs to.

Pointing your own extensions at a format
----------------------------------------

If your asset files use suffixes of their own, map them in a configuration file
rather than rebuilding. One `key = value` per line, `#` starts a comment, a key
beginning with a dot is an extension, and the only other key is `fallback`.

```
# Our exporter writes behaviour trees with this suffix.
.bt        = bt
.btree     = bt

# And our level data is JSON under a name of its own.
.leveldata = json

# What to use for a file nothing else claims.
fallback   = xml
```

Pass it with `--config`:

```bash
nmxmldiff --config providers.conf level_before.leveldata level_after.leveldata
```

Extensions are matched without regard to case. Every problem in the file is
reported with its line number, and a file with any problem stops the run rather
than being half applied, because a comparison read by the wrong format looks
like a working comparison. A copy you can start from is at
`testdata/sample/providers.conf`.

An explicit `--format` always beats the configuration file.

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

Nothing here has been verified against a real client yet, so treat it as the
shape of the command rather than as tested instructions.

**Perforce** takes the path of a diff program and appends the two file names to
it, which is the argument order this tool already expects:

```bash
p4 set P4DIFF=C:\tools\nmtreediff\nmxmldiff.exe
```

Perforce does not pass the depot path, so both sides are titled with the
temporary files it created. There is no way to improve on that from here today.

**Git** lets you build the whole command, so it can pass real titles:

```bash
git config --global difftool.nmtreediff.cmd 'nmxmldiff --left-label "$BASE (old)" --right-label "$BASE" "$LOCAL" "$REMOTE"'
```

```bash
git config --global diff.tool nmtreediff
```

Then compare with `git difftool`.

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
