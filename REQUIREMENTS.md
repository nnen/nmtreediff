NM Tree Diff Requirements 
=========================

- NM Xml Diff is a light-weight GUI tool for diffing tree-shaped data in XML
  and possibly other formats (e.g. JSON).
- "Light-weight" in this case means mainly that it's quick to start and
  responsive. If there are any operations that could be blocking, they should
  be done asynchronously with the GUI update loop. That said, ease of
  maintenance is also a factor.
- It should be implemented in C++20 (using modules is allowed).
- All code should be documented using Doxygen documentation comments using
  MSDN API reference style.
- It should be invocable from command line so that it can be used as diff tool
  by Perforce or other version control systems (the specific argument order
  doesn't matter, as long as the necessary information can be passed through
  the command line - Perforce allows users to specify the order for custom diff
  tools).
- It uses Dear ImGui library to render GUI.
- It should offer both text view and also node-based view of the diff. User
  should be able to switch between those two at will.
- By default, the tool should consider XML elements "nodes" and their
  attributes "node properties", but there should be a way to define custom
  XML-based, JSON-based, etc formats that:
  - Have custom logic for what is "node". E.g. a custom XML-based behavior tree
    format may consider only `<node>` elements to be "nodes" and any
    `<property>` elements that are children of a `<node>` element are the "node
    properties".
  - Can have custom logic for which two "nodes" are considered to have the same
    identity. E.g. in an XML-based behavior tree format, `<node>` elements may
    have an `id` attribute that contains the BT node's GUID. If two nodes in
    two differnt version of the tree have the same GUID, then they should be
    considered the same node regardless of otherwise similar or distant they
    are from each other.
  - Have custom logic for what the title, color, etc. of a node in a node view
    is.
  - Initially, the custom formats will be specified as C++ classes implementing
    a foramt provider interface. Alternative method using embedded scripting
    (Lua) will be implemented later. 
  - The custom formats should make it possible to define node property order.
  - Defines which extension it applies to by default. However, which actual 
    format provider is used should be overridable through the command line.
- Three-way merge should not be initially implemented, but it should remain
  possible to imlement it later.
- Although it has humble beginnings, this tool has the ambition to become the
  gamedev industry standard for performing XML and tree data diffs on game 
  asset files.

