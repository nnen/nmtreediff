NM Tree Diff
============

A lightweight GUI tool for diffing tree-shaped data. It shows the same diff two
ways, as text and as a node graph, and it runs from the command line so it can
serve as the diff tool for Perforce or another version control system.

**Status: usable. Milestones M0 to M6 have landed.** Both views work and share
a selection. The text view aligns the two files and picks out the changed words
within a rewritten line. The node view draws both trees as one graph coloured
by what happened to each node, with unchanged subtrees collapsed and a ghost
edge showing where a moved node came from. Clicking in either view selects in
the other. XML and JSON are both built in and are told apart by extension or,
failing that, by a look at the first bytes. A format of your own is a C++ class
and one line in a list, documented in [docs/PROVIDERS.md](docs/PROVIDERS.md).
The headless report lists the changes for scripting.

Missing so far: everything under Roadmap below.

Why
---

Line diffs are a poor fit for tree-shaped data. Moving a subtree in an XML file
reads as a large deletion next to a large insertion, and the one thing a
reviewer wants to know, that nothing inside it changed, is the thing a text
diff cannot say. This tool matches nodes between two documents first and
reports what actually happened to each one: added, deleted, modified, or moved.

What it does
------------

- **Two views of one diff.** A text view with a conventional line diff, and a
  node view drawing the tree as a graph coloured by change status. Switch
  between them at will; selection is shared, so a node in one view is the same
  node in the other.
- **Understands your formats.** Out of the box, XML elements are nodes and
  attributes are properties, and JSON objects, arrays, and array elements are
  nodes. Beyond that, a format provider defines what counts as a node, which
  two nodes are the same node across versions, and how a node is titled and
  coloured.
- **Matches by identity, not just position.** When a format has stable
  identifiers, such as a GUID on a behavior tree node, two nodes with the same
  identifier are the same node however far apart they have moved. The sample
  behavior-tree provider does exactly that, and a node it has anchored survives
  even a change of type, which no structural heuristic could recover from.
- **Stays responsive.** Parsing, matching, and layout run off the frame loop,
  publish results in stages, and can be cancelled. A large file does not freeze
  the window.

Stack
-----

C++20 with Dear ImGui on GLFW and OpenGL 3.3, built with CMake. Dependencies
are fetched and pinned by exact git ref, so you need only CMake, a generator
and a compiler. pugixml for XML, simdjson for JSON, Catch2 for tests. See the
implementation plan for why each was chosen.

Building
--------

Needs CMake 3.25 or newer and a C++20 compiler. Dependencies are fetched and
pinned by the build, so nothing else has to be installed. On Windows the
Microsoft toolchain is the tested one; Clang needs version 19 or newer to
match the Microsoft standard library it compiles against.

```
cmake -S . -B build
cmake --build build --config RelWithDebInfo
```

Run the tests:

```
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Build the API reference, which needs Doxygen on the path:

```
cmake --build build --target docs
```

Compare two files:

```
build/bin/RelWithDebInfo/nmxmldiff testdata/sample/tree_before.xml testdata/sample/tree_after.xml
```

Add `--headless --report json --exit-code` to run without a window, which is
also how a script or a continuous integration check would use it.

The format comes from the extension, or from the first bytes when the extension
is unfamiliar, so a JSON pair needs nothing extra:

```
build/bin/RelWithDebInfo/nmxmldiff testdata/sample/level_before.json testdata/sample/level_after.json
```

Pass `--format` to override that, and `--list-formats` to see what this build
reads:

```
build/bin/RelWithDebInfo/nmxmldiff --list-formats
```

The behaviour tree in `testdata/sample` shows what a format that knows its own
schema buys. Read by its own provider it is four nodes titled by behaviour;
read as generic XML with `--format xml` it is eight, half of them called
`property`.

If your studio's asset files use their own suffixes, point them at a provider
in a configuration file rather than rebuilding:

```
build/bin/RelWithDebInfo/nmxmldiff --config testdata/sample/providers.conf --list-formats
```

Open straight into the node view:

```
build/bin/RelWithDebInfo/nmxmldiff --view node testdata/sample/tree_before.xml testdata/sample/tree_after.xml
```

Repository contents
-------------------

| File | What it is |
| --- | --- |
| [REQUIREMENTS.md](REQUIREMENTS.md) | What the tool has to do. The source of truth. |
| [Doxyfile](Doxyfile) | Configuration for the API reference. Undocumented code is an error, so the `docs` target fails rather than quietly producing a thinner reference. |
| [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) | Architecture, data model, provider interface, matching algorithm, milestones, and open questions. |
| [docs/PROVIDERS.md](docs/PROVIDERS.md) | How to teach the tool a format of your own. Carries the provider interface version, which is 1. |
| [testdata/sample/providers.conf](testdata/sample/providers.conf) | A sample configuration pointing file extensions at providers. |

Roadmap
-------

Milestones M0 through M6 are done: a skeleton with a job system, a text diff,
the data model with a generic XML provider, the diff engine, the node view, a
generic JSON provider, and custom formats with a worked example. A shippable
release follows at M7: a portable archive, dependency attribution, and one-page
setup documents for Perforce and Git verified against real clients. A Lua bridge
for writing format providers without a compiler, and three-way merge, are
deliberately out of initial scope but the architecture keeps both open.

Licence
-------

MIT.
