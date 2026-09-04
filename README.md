NM Tree Diff
============

A lightweight GUI tool for diffing tree-shaped data. It shows the same diff two
ways, as text and as a node graph, and it runs from the command line so it can
serve as the diff tool for Perforce or another version control system.

**Status: planning. There is no code in this repository yet.** What exists is
the requirements and a plan for building against them. The sections below
describe the tool that is being built, not one that runs today.

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

C++20 with Dear ImGui on GLFW and OpenGL 3.3, built with CMake and vcpkg.
pugixml for XML, simdjson for JSON, Catch2 for tests. See the implementation
plan for why each was chosen.

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
