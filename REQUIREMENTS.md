NM Tree Diff Requirements
=========================

- *R0*: NM Tree Diff is a light-weight GUI tool for diffing tree-shaped data in
  XML and possibly other formats (e.g. JSON).
- *R1*: "Light-weight" in this case means mainly that it's quick to start and
  responsive. If there are any operations that could be blocking, they should
  be done asynchronously, without blocking the GUI update loop. That said, ease
  of maintenance is also a factor.
- *R2*: It should be implemented in C++20 (using modules is allowed).
- *R3*: Code should follow code guidelines specified in `CODE_GUIDELINES.md`.
- *R4*: It should be invocable from command line so that it can be used as a
  diff tool by Perforce or other version control systems (the specific argument
  order doesn't matter, as long as the necessary information can be passed
  through the command line - Perforce allows users to specify the order for
  custom diff tools).
- *R5*: It uses Dear ImGui library to render GUI.
- *R6*: It should offer both text view and also node-based view of the diff.
  User should be able to switch between those two at will.
- *R7*: By default, the tool should consider XML elements "nodes" and their
  attributes "node properties", but there should be a way to define custom
  XML-based, JSON-based, etc. formats that:
  - *R7.1*: Have custom logic for what is "node". E.g. a custom XML-based
    behavior tree format may consider only `<node>` elements to be "nodes" and
    any `<property>` elements that are children of a `<node>` element are the
    "node properties".
  - *R7.2*: Can have custom logic for which two "nodes" are considered to have
    the same identity. E.g. in an XML-based behavior tree format, `<node>`
    elements may have an `id` attribute that contains the BT node's GUID. If
    two nodes in two different version of the tree have the same GUID, then
    they should be considered the same node regardless of how similar or
    distant they are from each other.
  - *R7.3*: Have custom logic for what the title, color, etc. of a node in a
    node view is.
  - *R7.4*: Initially, the custom formats will be specified as C++ classes
    implementing a format provider interface. Alternative method using embedded
    scripting (Lua) will be implemented later.
  - *R7.5*: The custom formats should make it possible to define node property
    order.
  - *R7.6*: Defines which extension it applies to by default. However, which
    actual format provider is used should be overridable through the command
    line.
- *R8*: Three-way merge should not be initially implemented, but it should
  remain possible to implement it later.
- *R9*: Although it has humble beginnings, this tool has the ambition to become
  the gamedev industry standard for performing XML and tree data diffs on game
  asset files.
- *R10*: It should be possible to launch the app without any CLI arguments and
  then select the file(s) to compare using the GUI.
- *R11*: The app should be configurable through config files. The config files
  should be Lua scripts.
  - *R11.1*: The config file should be applied in the following order:
    1. `~/.nmtreediff.lua`
    2. `~/.nmtreediff/config.lua`
    3. config file passed to the tool through the `--config` command line option
  - *R11.2*: When Lua custom format providers are implemented, it should be
    possible to specify them inside the config files.
- *R12*: It should be possible to choose the direction of the tree node graph
  (top-down or left-to-right). There should be global default (configurable
  through config files when *R11* is implemented), but also specifiable
  per-format through the format providers.
- *R13*: `README.md` should not refer to any requirements mentioned in this
  file or any milestone or other item mentioned in the implementation plan. It
  should only describe the current state of the repository and the app.
- *R14*: `USAGE.md` should be the primary user documentation. It should be
  referenced at the top of `README.md`.

