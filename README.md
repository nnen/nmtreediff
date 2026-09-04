NM Tree Diff
============

A lightweight GUI tool for diffing tree-shaped data. It shows the same diff two
ways, as text and as a node graph, and it runs from the command line so it can
serve as the diff tool for Perforce or another version control system.

**Status: early. Milestone M0 has landed.** The window opens, docks its
panels, reads both files on a worker thread and shows them side by side, and
the headless path reports and returns exit codes. There is no diff engine yet,
so what it reports today is a byte comparison. The sections below describe the
tool that is being built, not one that is finished.

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
  identifier are the same node however far apart they have moved.
- **Stays responsive.** Parsing, matching, and layout run off the frame loop,
  publish results in stages, and can be cancelled. A large file does not freeze
  the window.

Planned stack
-------------

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

Compare two files:

```
build/bin/RelWithDebInfo/nmxmldiff testdata/sample/tree_before.xml testdata/sample/tree_after.xml
```

Add `--headless --report json --exit-code` to run without a window, which is
also how a script or a continuous integration check would use it.

Repository contents
-------------------

| File | What it is |
| --- | --- |
| [REQUIREMENTS.md](REQUIREMENTS.md) | What the tool has to do. The source of truth. |
| [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) | Architecture, data model, provider interface, matching algorithm, milestones, and open questions. |

Roadmap
-------

Milestones M0 through M4 are the critical path: a skeleton with a job system, a
text diff, the data model with a generic XML provider, the diff engine, and the
node view. JSON follows at M5, custom format providers at M6, and a shippable
release at M7. A Lua bridge for writing format providers without a compiler,
and three-way merge, are deliberately out of initial scope but the architecture
keeps both open.

Licence
-------

MIT.
