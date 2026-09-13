NM Tree Diff — Implementation Plan
==================================

Derived from [REQUIREMENTS.md](REQUIREMENTS.md), whose numbered requirements
this document cites as R0 to R12, and written to
[CODE_GUIDELINES.md](CODE_GUIDELINES.md). Target: a C++20 Dear ImGui
tool that diffs tree-shaped data in XML and JSON, offers both a text view
and a node view of the same diff, starts instantly, never blocks its own frame
loop, and runs from the command line as a Perforce diff tool. It is aimed at
becoming the default way game studios diff asset files.

Contents
--------

1. [Three constraints that shape everything](#1-three-constraints-that-shape-everything)
2. [What the ambition changes](#2-what-the-ambition-changes)
3. [Guiding decisions](#3-guiding-decisions)
4. [Pipeline and module layout](#4-pipeline-and-module-layout)
5. [Data model](#5-data-model)
6. [The format provider interface](#6-the-format-provider-interface)
   - [The configuration file](#the-configuration-file)
   - [Why configuration is not searched for](#why-configuration-is-not-searched-for)
   - [Providers written in script](#providers-written-in-script)
   - [The built-in providers](#the-built-in-providers)
7. [Matching, in four passes](#7-matching-in-four-passes)
   - [Size guard, and admitting to it](#size-guard-and-admitting-to-it)
8. [Staying responsive](#8-staying-responsive)
   - [Budgets](#budgets)
9. [The two views](#9-the-two-views)
   - [Text view](#text-view)
   - [Node view](#node-view)
10. [Command line and version control](#10-command-line-and-version-control)
11. [Milestones](#11-milestones)
12. [Testing](#12-testing)
13. [Risks and open questions](#13-risks-and-open-questions)
14. [Field review, and what it filed](#14-field-review-and-what-it-filed)
    - [Second pass: an engineering review](#second-pass-an-engineering-review)
15. [The DOM provider interface](#15-the-dom-provider-interface)
    - [Reading and shaping](#reading-and-shaping)
    - [The output handle](#the-output-handle)
    - [The work queue](#the-work-queue)
    - [Properties: forms, values and repeated names](#properties-forms-values-and-repeated-names)
    - [What generic JSON builds, and what YAML would](#what-generic-json-builds-and-what-yaml-would)
    - [What goes missing, and saying so](#what-goes-missing-and-saying-so)
    - [The Lua surface](#the-lua-surface)
    - [Order of work](#order-of-work)
    - [What landed, and where it differs from the above](#what-landed-and-where-it-differs-from-the-above)

1. Three constraints that shape everything
------------------------------------------

Most of the requirements are ordinary features that can be added in any order.
Three are architectural. Each is cheap to honour now and expensive to retrofit,
so all three are settled before any code is written.

**A. Formats are pluggable.** Format-specific knowledge never leaks into the
diff engine or the views. It lives behind one provider interface, implemented
by C++ classes and, from M8, by a Lua bridge. The engine never learns that a
behavior tree has property elements or identifier attributes.

**B. Merge stays possible.** The engine produces a *matching* between two
trees, not a one-sided edit script. A three-way merge is then the composition
of a base-to-left matching with a base-to-right matching. No engine rewrite, no
data model change.

**C. The frame loop never blocks.** Reading, parsing, matching, and layout all
run off the UI thread and publish immutable snapshots back to it.
Responsiveness is a requirement, and code written to block is not made
non-blocking later without being rewritten.

2. What the ambition changes
----------------------------

The goal of becoming a standard tool in game development is not decoration on
this plan. It moves four things from nice-to-have into scope.

- **Performance targets become product requirements.** Game asset files are
  machine-generated and large. A tool that stalls on a twenty-megabyte scene
  file does not get adopted, whatever its diff quality. The budgets in section
  8 are acceptance criteria, not aspirations.
- **Degraded results must be visible.** When the size guard trims the matching
  passes, the interface says so in plain words. A diff tool that quietly
  under-reports differences loses trust once, permanently.
- **Format authorship is the adoption path.** Every studio has its own asset
  formats, so the provider interface is a public surface, versioned and
  documented from the first release. The Lua bridge is what lets a technical
  artist add a format without a compiler, and since format authorship is the
  adoption path it belongs in the first release rather than after it. That is
  why it moved ahead of shipping, into M8. It also means the C++ interface must
  stay narrow enough to mirror in script, which is a claim M8 finally tests
  rather than asserts.
- **Installation must be trivial.** A portable build that a technical artist
  can unzip and point Perforce at, with a one-page setup document. Anything
  that needs help from IT will not spread through a studio.

3. Guiding decisions
--------------------

| Decision | Choice | Rationale |
| --- | --- | --- |
| Language | C++20, headers | Settled. Modules are permitted, but a codebase this small gains little compile time from them while still paying for uneven toolchain and package-manager support, and maintenance ease counts. Keep one interface header per translation unit so the layout stays module-shaped, and reopen the question only if build times become measurably painful. Note that `std::expected` is C++23, so core carries its own `Result<T, E>` of the same shape. |
| Concurrency | UI thread plus a small `jthread` pool | Coarse cancellable jobs and immutable published snapshots. C++20 stop tokens give cancellation without a bespoke mechanism. |
| GUI | Dear ImGui, docking branch | Docking is what the text, node, and details panel layout needs. |
| Backend | GLFW + OpenGL 3.3 | One code path on Windows, Linux, and macOS. A DirectX 11 backend can sit behind the same interface if Windows startup time demands it. |
| Build | CMake 3.25+ with pinned FetchContent | Dependencies are pinned by exact git ref rather than taken from a package manager, so a contributor needs only CMake, Ninja and a compiler. Revisit vcpkg if binary caching in CI becomes worth the extra prerequisite. |
| XML parser | pugixml | Small, fast, and preserves document order. Its offsets are only half the story: it reports where a node starts but not where it ends, and reports nothing for attributes, so spans are recovered by scanning the original bytes with quote awareness. Worth knowing before assuming a parser swap is cheap. |
| JSON parser | simdjson, on-demand API | The deciding factor is byte offsets, not speed: node spans are what link the two views, and simdjson exposes a source location for every value. nlohmann/json does not, which rules it out despite being the obvious default. That turned out to be the right reason to pick it, because the speed is not what arrives. The On Demand API is header-inline against whichever kernel the build can always run, and on the Microsoft toolchain without an instruction-set baseline that is the scalar fallback. Compiling the provider with `/arch:AVX2` moved a 100k-node parse from 113 ms to 111 ms, so the SIMD kernel is not worth raising the hardware requirement for: building the tree costs far more than scanning the bytes. See the note on comment-bearing JSON in section 6. |
| Argument parsing | CLI11 | Plain named options are enough, because Perforce lets the user define the argument order for a custom diff tool. No tolerance hacks needed. |
| Licence | MIT | Permissive enough to clear a studio legal review without a conversation, which is a precondition for the adoption this tool is aiming at. It also lets a studio vendor the core library into an internal tool. |
| Distribution | A portable archive, attached to a tagged release on GitHub or an equivalent | No installer, because adoption inside a studio depends on someone being able to unzip it and edit a Perforce setting. Building it in continuous integration from the tag is what stops a release being whatever happened to be on one machine. |
| Configuration and scripting | Lua 5.4 with sol2 | R11 asks for configuration written in Lua and R7.4 asks for format providers in Lua, and M8 does both, so one binding has to serve a table of settings and a set of callbacks on a worker thread. sol2 binds a C++ class to a Lua table with no code generator, which is what the bridge needs; hand-rolling against the C API is cheap for the configuration file and expensive for the providers. Lua rather than a larger runtime because a provider is called per node, and an interpreter that starts in under a millisecond and embeds in one translation unit is what a 200 ms startup budget can afford. |
| File dialog | nativefiledialog-extended | R10 needs a file picker and Dear ImGui has none. A native dialog is what an artist expects: recent places, a typed network path, and the shell's own sorting. Writing one per platform is three backends; this is one pinned MIT dependency with a CMake build, and it leaves the portable archive portable. |
| Code style | CODE_GUIDELINES.md | R3. Doxygen on every entity is enforced by the `docs` target. The rest, a summary comment per block, named constants, and short functions, is reviewed rather than tooled, so it is a standing item in review rather than a build step. |
| Tests | Catch2 v3 | Golden-file friendly. |
| Documentation | Doxygen, MSDN reference style | Required by CODE_GUIDELINES.md, which R3 makes binding. Every file, type and function carries a brief, its parameters, its return value and the remarks that explain why. The Doxyfile treats an undocumented entity as an error, so the `docs` target fails rather than letting the standard decay. |

4. Pipeline and module layout
-----------------------------

The core library links against no GUI target and knows nothing about threads
beyond its own job queue. Layout lives there rather than in the interface,
because positioning a tree is arithmetic on a snapshot and runs on a worker.
Entries carrying a milestone number are the ones still to write; everything
else exists. That is what makes the diff engine testable in
continuous integration and reusable later by a merge tool.

```
  left ─┐                          ┌─ 1 Identity anchors ─┐
        ├─ Format provider ─┬─ Tree A ─┐  2 Identical subtrees │
  right ┘  parse, identity  └─ Tree B ─┴─ 3 Similarity ────────┴─ Diff
                                          4 Classify              snapshot
                                                                    │
        ── worker pool, cancellable, publishes in stages ──         │
                                                                    ▼
        ── UI thread, 16 ms budget ──────────────  Text view / Node view
```

Everything up to and including the snapshot runs on a worker. The views only
ever read a finished snapshot, so a frame never waits on a parse or a match. A
future three-way merge adds a second run of the same pipeline, base against
each side, and composes the two matchings.

```
nmxmldiff/
  CMakeLists.txt
  cmake/Dependencies.cmake   pinned FetchContent declarations
  LICENSE              MIT
  src/
    core/          # no GUI, no I/O beyond reading files
      result.h             Result<T, E>, standing in for std::expected
      jobs.h/.cpp          worker pool, stop tokens, cancellation
      snapshot.h           DiffSnapshot, the one thing the UI reads
      session.h/.cpp       owns the pool and the snapshot box
      source.h/.cpp        SourceFile: bytes, line index, label
      tree.h/.cpp          Node, Tree, NodeId, Property, SourceSpan
      provider.h/.cpp      IFormatProvider and its value types
      registry.h/.cpp      registration, sniffing, explicit override
      hash.h/.cpp          content hashing of subtrees
      match.h/.cpp         identity, bottom-up, top-down passes
      diff.h/.cpp          Matching to DiffModel
      textdiff.h/.cpp      Myers line diff, intra-line word diff
      layout_tree.h/.cpp   tidy-tree positioning, in either direction
      config.h/.cpp        the resolved configuration, and where it lives
      lua_config.h/.cpp    reads one configuration script
      lua_provider.h/.cpp  a format provider written in script
      lua_state.h/.cpp     one interpreter per worker, and cancellation
    formats/
      xml_generic.h/.cpp   default XML provider (M2)
      json_generic.h/.cpp  default JSON provider (M5)
      bt_xml.h/.cpp        sample behavior-tree provider (M6)
      xml_spans.h/.cpp     span recovery shared by the XML providers (M6)
    ui/
      app_window.h/.cpp    docking layout, menu, details panel, keyboard map
      text_view.h/.cpp
      node_view.h/.cpp     canvas, interaction, drawing, minimap
      selection.h          the selection both views share
      screenshot.h/.cpp    writing a frame out, for timing runs and docs
      file_picker.h/.cpp   the native open dialog, off the frame loop
      welcome.h/.cpp       what the window shows before a file is chosen
    app/
      main.cpp             headless or windowed dispatch
      cli.h/.cpp           options
      report.h/.cpp        headless text and JSON output
  tests/  docs/  testdata/
```

5. Data model
-------------

A parsed document is a tree of node values held in a flat arena and addressed
by a 32-bit identifier. The arena avoids pointer chasing during matching, keeps
subtree ranges contiguous, makes memory use predictable on large files, and
makes a finished tree trivially cheap to hand between threads. Leaf text
content is modelled as a property, so the views and the engine need one concept
rather than two.

```cpp
struct SourceSpan { uint32_t begin, end; };   // byte offsets into SourceFile

struct Property {
    std::string name;
    std::string value;
    SourceSpan  span;
    std::vector<Property> children;   // M9: a property may hold properties
    bool        ordered = false;      // M9: true when those parts are a sequence
};

struct Node {
    NodeId                id;
    NodeId                parent;
    std::vector<NodeId>   children;
    std::string           kind;            // element name, or provider-defined
    std::vector<Property> properties;      // document order; display is ranked
    SourceSpan            span;            // full extent, links to the text view
    uint64_t              contentHash;     // filled by the hashing pass
    uint32_t              depth;
    uint32_t              descendantCount;
};
```

Every node keeps its source span. That single field is what lets a click in the
node view scroll the text view, and a click in the text view select a node.
Retrofitting it later would touch every provider.

**Properties nest, from M9.** R7.7 asks for it, and the reason is that a game
asset format does not restrict itself to flat name and value pairs: a transform,
a colour, a bounding box are each one thing with parts, and flattening them into
`transform.position.x` would turn one changed number into a changed string with
a made-up name. A nested property keeps the shape the file had.

**A property's parts are either a record or a sequence.** R7.10 asks for array
properties, and an array differs from a record in the way that matters most to a
diff: position means something and names do not. So a property carries the same
distinction a node already carries through `childrenOrdered()`. A record's parts
compare as a set by name and hash over sorted pairs, exactly as a node's
properties do today. A sequence's parts compare by position and hash in order,
so reordering a list of tags is a change and reordering a transform's fields is
not. This is the ordering hook JSON earned at M5, one level further down, and it
is the same rule rather than a second one.

That is the one data model change still ahead, and it reaches further than it
looks: hashing walks properties, matching compares them as a set, the details
panel lists them, and the change list names them. Each of those has to decide
what it means for a property with parts. The plan is that a property's hash
covers its children, that two properties differ when their subtrees differ, and
that the change list names the outermost property that changed rather than a
path to a leaf. All three keep the existing behaviour exactly when nothing
nests, which is what makes the change safe to land against the golden corpus.

6. The format provider interface
--------------------------------

One interface, free of templates and of any ImGui type, so that the scripted
implementation arriving later is a plain subclass rather than a redesign.
Parsing is where a provider collapses format detail: the behavior-tree case
from the requirements builds a tree holding only its node elements, folds each
child property element into the parent's property list, and returns the
identifier attribute as a strong identity key.

```cpp
struct NodeStyle {
    std::string title;        // shown on the node card
    std::string subtitle;     // optional second line
    Color       accent;       // provider colour, before diff-status tinting
    std::string icon;         // optional glyph key
};

struct IdentityKey {
    bool        strong = false;   // true: match across arbitrary distance
    std::string value;
};

class IFormatProvider {
public:
    virtual ~IFormatProvider() = default;

    virtual std::string_view name() const = 0;
    virtual std::string_view displayName() const = 0;

    // Ranked sniffing: extension plus a cheap look at the head of the file.
    virtual int score(const SourceFile&) const = 0;

    // Called on a worker; must not touch shared mutable state.
    virtual Result<Tree, ParseError>
        parse(const SourceFile&, std::stop_token) const = 0;

    // Consulted by the matcher before any structural heuristic runs.
    virtual IdentityKey identity(const Tree&, NodeId) const = 0;

    virtual NodeStyle style(const Tree&, NodeId) const = 0;

    // Display order for a node's properties; lower ranks sort first and equal
    // ranks keep document order. RankFromList covers the common case of a
    // fixed leading order with everything else trailing.
    virtual int propertyRank(const Tree&, NodeId, std::string_view name) const { return 0; }

    // Ordered children mean sibling position is meaningful and a reordering
    // is a move. Unordered means position changes are ignored.
    virtual bool childrenOrdered(const Tree&, NodeId) const { return true; }

    // Reserved for the merge milestone; the default reports unsupported.
    virtual Result<std::string, SerializeError>
        serialize(const Tree&) const {
        return fail(SerializeError::NotSupported);
    }
};
```

**A scalar array becomes one property, from M9.** Today it is a node holding one
anonymous `item` node per element, and a changed tag reads as a subtree of
things with no names. As a property it reads as one line, `tags`, which is what
the file says and what a reviewer is looking at. It also cuts the node count on
array-heavy files, which is where most of the budget benchmark's nodes come
from.

The rule has to be stated precisely, because it decides what every JSON file
turns into:

> An array becomes a property when every element is a scalar, or is itself an
> array that becomes a property. An array holding an object stays a node.

The recursive half is not tidiness. A four by four transform matrix is an array
of arrays of numbers, and it is exactly the kind of thing R7.7 was asked for: one
property with parts, not sixteen anonymous nodes four levels deep. An array with
an object in it is a different animal, because an object has named fields that a
reader will want to match against their counterparts, and that is what nodes are
for.

Being a sequence rather than a record, an array property compares by position, so
reordering a list of tags registers as a change. That is the behaviour arrays
already have as nodes, kept rather than lost.

**A scripted JSON format decides for itself.** The `is_node` hook already means
what is needed: answer `true` and the array is a node, answer `false` and R7.8
makes it a property. What M9 has to add is the information a script needs to
answer, because a JSON element is not an XML element. It should see what kind of
container it is looking at and whether the elements inside are scalars, so a
studio whose spawn lists want to be nodes and whose tag lists want to be
properties can say so in one line each.

**This moves every JSON golden case,** which is the cost and the reason it is
worth doing at the start of M9 rather than the end. Regenerating them is the
point at which the change is reviewable: the expected output is the record of
what the tool believes, and a smaller, better-named set of changes is what the
diff of that record should show.

**Nothing a provider does not recognise may be dropped, from M9.** R7.8 settles
what was an open question: everything that is not a node is a property. A
provider used to have three answers about an element, the third being to walk
through it and keep nothing of it, and that third answer is what made it
possible to lose content by accident. It is gone. An element is a node or it is
a property, and a property may have parts, so there is nowhere for content to
fall out of the tree.

One case needs deciding rather than assuming: an element that is not a node but
contains nodes, such as a `<children>` wrapper. Two readings are available. The
wrapper becomes a property and everything under it becomes property content,
which is simple and loses the nodes inside. Or the wrapper becomes a property
recording its own name and attributes while the nodes under it attach to the
nearest ancestor node, which keeps both and is what the walk-through case was
useful for. The second is the one to build, because the first reintroduces the
problem R7.8 exists to remove.

**Graph direction is the version-1 promise being tested.** R12 asks for a
top-down or left-to-right node graph, a global default with a per-format
override. That is a new method with a default implementation and nothing already
declared changes shape, which is exactly what additive was defined to mean, so
`kProviderInterfaceVersion` stays at 1 and every provider written against M6
keeps working untouched. The default returns `Inherit` rather than `TopDown`: a
provider with no opinion must not overrule the user's global choice, and a
provider that returns a direction is stating one.

R12 lands across two milestones, which the requirement itself anticipates by
saying the global default is configurable "when R11 is implemented". M7 builds
the layout in both directions, adds the provider hook, and puts the global
default on the View menu. M8 adds the configuration key that makes the default
stick between runs.

**Property order is presentation, never matching.** Matching compares
properties as an unordered set, so a reordered attribute list never registers
as a change. The provider's ranking decides what the node card, the details
panel, and the change list show first, which also makes the change list
deterministic across runs.

**Serialization is declared now and left unimplemented.** It is the hook a
merge will need in order to write a merged tree back out, and declaring it
early forces every provider to be written with round-tripping in mind.

The registry resolves a provider by an explicit format option first, then by a
configured extension, then by the highest sniffing score, then falls back to the
generic XML provider. Those four are in order of how deliberate they are: the
command line is a person correcting a guess right now, a configured extension is
a studio's standing decision, and sniffing is the guess.
Registration is an explicit list rather than a static initialiser per format.
In a static library the linker drops a translation unit nothing references,
taking its self-registration with it, and a format that silently vanishes from
a release build is far worse than one list that has to be edited. Since this interface is the
studio-facing surface, it carries a version number from the first release and
changes to it are additive. That number is `kProviderInterfaceVersion`, it is
printed by `--list-formats`, and it heads docs/PROVIDERS.md, which is the
document someone outside the project writes a provider from.

### The configuration file

A studio names its asset files whatever it likes, and pointing an extension at a
provider should not need a rebuild. M6 shipped that as one `key = value` per
line. R11 replaces it with a Lua script, and M8 makes that change.

Two formats would be worse than either, so the `key = value` reader is retired
rather than kept alongside. What it expressed, an extension map and a fallback,
the script expresses as a table.

```lua
formats {
  [".bt"]        = "bt",
  [".btree"]     = "bt",
  [".leveldata"] = "json",
}

fallback "xml"
graph_direction "left_to_right"
exit_key "escape"
```

**The exit key is settable because it is a habit, not a preference.** Escape
closes the window with no confirmation, which is what other version control diff
tools do: reviewing a changelist is a run of files opened one after another, and
dismissing each with one key is what makes a long list bearable. Confirming
would ask the same question dozens of times in a row. But someone whose muscle
memory says Escape means "undo what I just typed" will lose a window to it, so
the key is a setting and `none` is one of its values. `--exit-key` overrides it
for one run, the way `--format` overrides a resolved format.

Three files are read, each overriding what came before, so the most specific
statement wins:

1. `~/.nmtreediff.lua`
2. `~/.nmtreediff/config.lua`
3. whatever `--config` names

Then `--format`, which is a person correcting a guess right now and beats every
standing decision. A file that is absent is not an error. A file that is present
and wrong is, and it names itself, its line and what it said, because a diff read
by the wrong provider looks like a working diff.

### Why configuration is not searched for

An earlier draft of R11 had the tool walk up from each compared file looking for
a script, so that a team could keep one in the repository and have it picked up
by whoever opened those files. It is worth recording why that is gone, because
it is the obvious design and someone will propose it again.

**A version control system usually hands over temporary files.** A Perforce diff
of a workspace file against the depot passes one real path and one temporary
extract; a diff of two revisions passes two extracts. Walking up from those
paths finds the temporary directory, not the repository, so the file the team
wrote would not be read in exactly the case it was written for. What survives a
temporary file is the *label*, which the tool already accepts for each side and
which a client fills in with the depot or repository path. Any future attempt at
project configuration should match on that rather than on where the bytes
happen to sit.

**A script that arrives with the files is code that runs because you opened
something you were sent.** Serving that safely needs a sandbox and a record of
what the user has agreed to run, which is a real feature and not a small one.

Dropping the search removes both problems together, and it removes the reason
for the sandbox with them: all three files above are the user's own, two of them
in their home directory and the third named on the command line, which is the
same trust a shell gives a startup file. The interpreter is therefore built with
the standard libraries present and no instruction budget, and none of the
trust machinery is written.

What that does not solve is the case the search was for. A team still has no way
to share configuration except by putting a file somewhere everyone can reach and
naming it with `--config`, which means the setting has to be repeated in every
person's diff-tool command. That is a real gap and it is left open below rather
than papered over.

### Providers written in script

R7.4 wants a format defined without a compiler, and M8 delivers it. This is the
claim constraint A was written for: a scripted provider should be a plain
subclass rather than a redesign. If the bridge needs the interface to change,
the interface was wrong, and it is better to find that out now than after a
studio has written against it.

**A scripted provider shapes a tree; it does not parse bytes.** R7 asks for
"custom XML-based, JSON-based, etc. formats", so there is always an underlying
format the tool already reads. A script declares which one, and the built-in
provider for it does the parsing. What the script decides is what the generic
tree means: which elements are nodes, which children fold into properties, what
a node is called and coloured, and what makes two nodes the same node.

```lua
provider "bt" {
  display_name = "Behavior tree (XML)",
  base = "xml",
  extensions = { ".bt", ".btree" },

  is_node = function(element) return element.name == "node" end,
  fold_into_parent = function(element) return element.name == "property" end,

  kind = function(node) return node.attr.type end,
  identity = function(node) return node.attr.id, "strong" end,
  title = function(node) return node.attr.type, node.attr.name end,
}
```

That keeps every hot loop in C++ and leaves the script deciding only what a
person actually wants to decide. It also keeps the scripted surface far smaller
than `IFormatProvider`, which matters because the surface is now a published one
in two languages rather than one.

**The cost model is what shapes the rest.** A provider's methods are not called
once each. `identity()` and `childrenOrdered()` run per node during matching,
and `style()` runs per visible card per frame, which at sixty frames a second is
the one call that cannot cross into an interpreter at all. So the bridge
computes kind, identity and ordering once while the tree is being built, and
caches style per node rather than per frame. A script is asked a question once
per node in the document, never once per node per frame.

**One interpreter per worker.** A Lua state is not thread-safe and the interface
contract says providers must be callable from several threads at once, because
the two sides of a diff parse in parallel. Each worker therefore holds its own
state with the same script loaded. The states share nothing, which also means a
script cannot accumulate state between files and quietly make a diff depend on
what was opened before it.

**Cancellation still has to work.** A script with a loop in it would otherwise
hold a worker past a cancel, so a Lua provider runs under an instruction-count
hook that checks the stop token. The mechanism is the one the sandbox would have
used, arriving for a different reason: not because the script is untrusted, but
because constraint C does not make exceptions for code the user wrote.

**No interface change is expected.** A Lua provider is a C++ class implementing
`IFormatProvider` and delegating, so `kProviderInterfaceVersion` should stay at
1. If it does not, that is the most interesting result M8 can produce and it
belongs in the plan rather than in a commit message.

### The built-in providers

XML ships first and JSON second. Both are generic providers with no knowledge
of any particular schema, and between them they are what proves the interface
is not XML-shaped. A third, the sample behavior-tree provider, arrived at M6 and
proves the opposite half: that a provider which does know its schema can collapse
detail the generic ones have to keep.

| | What is a node | What is a property | Child order | Default identity |
| --- | --- | --- | --- | --- |
| Generic XML | Every element | Every attribute, plus `#text` for a leaf element's text | Ordered | Element name and sibling index; weak |
| Generic JSON | Every object, and every array that holds an object | Scalar members of an object, plus `#type` on every container, plus one ordered array property per array of scalars (M9) | Arrays ordered, object members unordered | Member key inside its parent object; weak. Array elements have none. |
| Behavior tree | Only `<node>` elements, plus the document element | Every attribute, plus each `<property name= value=>` child folded in | Ordered, because sibling order is execution order | The `id` attribute; **strong** |

**JSON is where the ordering hook earns its place.** Reordering the members of
an object changes nothing about the document, while reordering an array does.
The generic provider reports the first as unordered and the second as ordered,
and the matcher reports moves only where a move is real. An XML-only design
would never have surfaced that distinction.

**A node's kind is its member key.** The root is `$`, a member holding a
container takes the key it appears under, and an array element is `item`,
which is deliberately the same for every element of every array: an element has
no name of its own, and putting its index in the kind would make moving it look
like turning it into a different sort of node. That leaves nowhere for the JSON
type to live, which is why every container carries `#type`. Without it an empty
object replaced by an empty array under the same key would hash the same and be
reported as unchanged. The gain is that a reported path reads as
`$/spawns[0]/item[2]` rather than as a column of the word "object".

**Scalars are kept exactly as written**, quotes, escapes, numeric formatting and
all. A provider that normalised them would be deciding on the reader's behalf
that a change is not worth seeing, and the two changes it would hide, a number
becoming a string and `1.0` becoming `1`, are both real in a game asset
pipeline. It also means a value is a slice of the source rather than a
re-rendering of it.

**Every scalar is read even though only its text is kept.** On Demand parsing
is lazy: a value nobody asks for is skipped structurally and never checked, so
a file containing `1.2.3` would otherwise diff as though it were sound.

**The sample provider is where the interface pays for itself.** Read as generic
XML, the behaviour tree from the requirements is five nodes, two of them
called `property`, and a changed movement speed is reported as an edit to an
anonymous element. Read by its own provider it is three nodes titled Sequence
and MoveTo, and the same change is reported as a property of the behaviour it
belongs to. Both readings are in the golden corpus so the difference is
reviewable rather than asserted.

The strong identity key is the other half. A `<node>` carries a GUID its editor
generated, so two nodes with the same one are the same node however far apart
they have moved. Generic XML looks at the same attribute and returns a weak key,
because in arbitrary XML an `id` might be a stable identifier or might be a
colour swatch name. The difference shows up as a single reported move where the
generic reading would report a deletion next to an addition, and as a surviving
match when a node's `type` changes, which no structural heuristic could
recover because a kind mismatch stops all of them.

**One note on comment-bearing JSON.** simdjson accepts strict JSON only, so
files with comments or trailing commas, which do turn up in game
configuration, will not parse. If that proves common in the target corpus, the
fallback is a small hand-written scanner rather than a different library,
because the offset tracking is the only part that is hard and we would be
writing it either way.

7. Matching, in four passes
---------------------------

The passes run cheapest first, and each one only sees what the previous ones
left unmatched.

1. **Identity anchors.** Collect strong identity keys from both sides. A key
   appearing exactly once on each side becomes a fixed anchor pair. This is
   what makes a behavior-tree node with a stable identifier match its
   counterpart no matter how far it moved.
2. **Identical subtrees, bottom up.** Hash every subtree over its kind, its
   sorted property pairs, and its child hashes. Equal hashes with equal
   descendant counts pair up, largest first. This pass is what makes the common
   case, a small edit in a big file, fast.
3. **Similarity, top down.** For still-unmatched nodes whose parents are
   already matched, pair by a score combining kind equality, property overlap,
   and the fraction of already-matched descendants, using a Dice coefficient.
   This follows the shape of the GumTree algorithm, the established approach to
   tree differencing, which is worth staying close to rather than reinventing.
4. **Classification.** A matched pair whose parents are not themselves matched,
   or whose sibling index changed under an ordered parent, is a move. A matched
   pair with differing properties is modified, and the changed property names
   are recorded. Unmatched left nodes are deletions, unmatched right nodes are
   additions.

Statuses: added, deleted, modified, moved, unchanged.

The result keeps the correspondence separate from its interpretation, which is
the single choice that keeps three-way merge open.

```cpp
struct Matching { /* left NodeId <-> right NodeId, both directions */ };

struct DiffModel {
    Matching            matching;
    std::vector<Change> changes;      // ordered, for next/previous navigation
    MatchQuality        quality;      // full, or which passes the guard trimmed
    NodeStatus statusOf(Side, NodeId) const;
};
```

### Size guard, and admitting to it

The first two passes are near linear. The third is the quadratic risk. Bound it
by comparing only candidates under a matched parent, capping candidate set
size, and trimming the pass above a configurable node count, degrading to
identity plus exact-subtree matching.

When that happens the result carries a reduced quality value and the status bar
says which pass was trimmed and offers to run the full match anyway. Silence
here would mean showing a user fewer matches than exist without telling them,
which is the one failure a diff tool does not recover from.

8. Staying responsive
---------------------

The UI thread runs the ImGui loop and nothing else. Everything with an
unbounded cost happens on a small pool of worker threads, sized to hardware
concurrency minus one and capped low, because the work is a handful of coarse
jobs rather than fine-grained parallelism.

- **Jobs are coarse and chained.** Read, parse each side in parallel, hash,
  line diff, match, classify, lay out. Each job carries a stop token and checks
  it on a bounded interval, per node batch or per line chunk.
- **Results are immutable snapshots.** A worker builds a `DiffSnapshot` and
  publishes it as a `shared_ptr<const DiffSnapshot>` through a single atomic.
  The UI takes one reference at the top of a frame and renders from it. No
  locks in the render path, no torn state, and the previous snapshot stays
  alive as long as a frame still holds it.
- **Publishing happens in stages.** The line diff finishes long before the tree
  match on a large file, so it is published first and the text view becomes
  usable while matching continues. The node view shows progress rather than an
  empty window, and the interface never presents partial results as complete.
- **Cancellation is the normal path, not the exception.** Switching format,
  toggling normalisation, or reloading a changed file cancels the job in flight
  and starts another. Because that is routine, every long loop is written
  around a stop token from the first line rather than retrofitted with one.
- **Startup does no work.** The window appears before any file is opened. Heavy
  static initialization is avoided, only the glyph ranges actually used are
  baked, and the first file read is queued after the first frame has been
  presented.

### Budgets

| Measure | Target | Measured | Verified by |
| --- | --- | --- | --- |
| Window visible, cold start | under 200 ms | 264-289 ms (over) | `--max-frames` timing run |
| Frame time, steady state | under 16 ms | 1.1-1.7 ms (M3) | `--max-frames` timing run |
| Text view usable, 20 MB pair | under 800 ms | 145 ms (M1) | M1 budget test |
| Full match, 100k nodes | under 2 s | 84 ms (M3) | M3 budget test |
| Cancellation acknowledged | under 50 ms | 3.2 ms (M5) | M5 cancellation test |
| Parse and match, 100k nodes, JSON | under 2 s | 258 ms and 161 ms (M9) | M9 budget test |

Frame time is measured in steady state, after the window has settled. Showing
a window costs a compositor round trip of roughly two vsync intervals, landing
around the thirtieth frame, and it measures the same whether the pair is four
hundred bytes or twenty megabytes. Folding that one-time cost into the frame
budget would hide every real stall smaller than it, so it is reported alongside
rather than inside.

The remaining targets are still estimates to design against. The point of
writing them down is that missing one is a visible failure rather than a vague
sense that the tool feels slow.

9. The two views
----------------

Both views render one snapshot and share one selection model. Selecting a node
highlights its source span; putting the caret on a line selects the innermost
node whose span contains it.

### Text view

- **Myers line diff** with word-level highlighting inside modified line pairs,
  rendered through the ImGui list clipper so line count does not affect frame
  time.
- **Side by side by default,** unified as a toggle, with scrolling synchronised
  by the line correspondence rather than by a pixel ratio.
- **Gutter and overview.** The gutter marks changed, added, and deleted lines;
  the scrollbar overview shows the whole file's change density.
- **A normalise-formatting toggle** re-serialises both sides through the
  provider before diffing, so whitespace or attribute-order churn stops
  drowning real edits. Off by default, because a diff tool that silently
  reformats its input is one people stop trusting.
- **Two guards, both of which announce themselves.** An edit-distance ceiling,
  and a step budget on the alignment. The step budget is the one that bounds
  time: alignment costs roughly the edit distance squared, so capping the
  distance alone still allows billions of steps on two files that share
  nothing.

### Node view

- **Drawn through the ImGui draw list,** not with widgets, so panning, zooming,
  and culling stay under our control.
- **Either direction.** Top-down suits a wide, shallow tree and left-to-right
  suits a deep one, which is the shape a behaviour tree usually has. The
  positioning pass works in breadth and depth rather than in x and y, and the
  direction decides which is which; card sizes stay in screen space, because
  text does not rotate. Direction lives in the layout rather than in the
  drawing, so hit testing, the minimap and the ghost edges cannot disagree with
  what is on screen. Changing it re-runs the layout job, which is the ordinary
  cancel-and-restart path.
- **Top-down layout,** computed on a worker once per snapshot. Each subtree
  gets a width, then fills the span its parent allotted it, which is linear and
  never overlaps. A tighter packing that interleaves subtrees of different
  depths would save horizontal space and is a refinement, not a correctness
  fix.
- **One unified tree by default,** holding the union of both sides and coloured
  by status, with a ghost edge from a moved node back to its former parent. Two
  synchronised side-by-side canvases are the alternative.
- **Node cards** take their title, subtitle, and accent colour from the
  provider, list properties in the provider's rank order, and get tinted by
  diff status so status survives any provider palette.
- **Scale.** Unchanged subtrees collapse into a chip showing how many nodes are
  hidden, off-screen nodes are culled by bounding box, and below a zoom
  threshold cards degrade to coloured boxes. A minimap and next-change
  navigation keep large trees usable.

10. Command line and version control
------------------------------------

```
nmxmldiff [options] <left> <right>

  --format <name>             override provider sniffing
  --left-label, --right-label titles a VCS wants shown
  --view text|node            initial view
  --config <file>             a configuration script, read after the found ones
  --exit-key <name>|none      which key closes the window, default escape
  --list-formats              what this build reads, and what it resolves
  --headless                  no window
  --report text|json
  --exit-code                 0 identical, 1 different
```

Both paths are optional (R10). With neither, the window opens on a welcome pane
offering the file picker; with one, it opens with that side chosen and asks for
the other. A list of recent pairs belongs on that pane and is left to M12, which
is where anything the tool writes back out is dealt with. Headless mode still
requires two paths, because there is nobody there to answer.

Because Perforce lets a user define the argument order for a custom diff tool,
the parser needs no tolerance for unusual argument shapes. Plain named options
and two positional paths are enough.

**This is a diff tool for particular file types, not a replacement for the
default one.** It has nothing useful to say about source code, so registering it
for everything would be worse for its users than not registering it at all. That
turns out to change the setup instructions more than expected, and the two
systems differ:

- **Perforce** does per-extension diff applications in P4V's preferences, one
  entry per extension with `%1` and `%2` for the two files. There is no
  command-line equivalent: `P4DIFF` names one program for every text file. So
  the Perforce setup document is a sequence of screens, not a string to paste.
- **Git** does it with `.gitattributes` naming a diff driver and
  `diff.<driver>.command` defining it, both settable with `git config`. But an
  external diff receives seven arguments rather than two, with the files second
  and fifth, so it needs a wrapper script. `git difftool` is a different
  mechanism with one tool for everything and is the wrong door for this.

The wrapper is worth removing. An option that reads Git's seven-argument shape
directly would turn the Git setup into two commands with nothing to install
alongside them, and it is a small piece of argument handling rather than a
feature. M12 should decide whether to add it while it is writing those
documents against real clients.

Subversion gets the same treatment, documented rather than special-cased.

Headless mode with a JSON report and an exit code runs the whole pipeline with
no window. That serves scripting and continuous integration checks on asset
submissions, and it is also how the end-to-end tests run.

11. Milestones
--------------

| # | Milestone | Contents | Done when |
| --- | --- | --- | --- |
| M0 &check; | Skeleton and job system | CMake with pinned FetchContent, ImGui window with docking, worker pool with stop tokens, snapshot publishing, argument parsing, headless reporting | Done, except that startup measures 207-211 ms against the 200 ms target; file loading is off the frame loop and the worst frame is 2.1 ms |
| M1 &check; | Text diff | SourceFile, Myers line diff, word highlighting, synchronised scrolling, gutter and overview, staged publishing with progress | A 20 MB pair is readable inside the budget with the frame loop never stalling |
| M2 &check; | Model and generic XML | Tree arena, spans, provider interface, property ranking, registry, generic XML provider, subtree hashing | A parsed tree round-trips its spans and hashes deterministically |
| M3 &check; | Diff engine | The four passes, DiffModel, size guard with visible degraded mode, cancellation, golden-file tests, performance tests | Golden tests pass and the hundred-thousand-node case meets its budget |
| M4 &check; | Node view | Canvas, tidy-tree layout on a worker, node cards, status colouring, collapsing, view switching, shared selection | Both views show the same snapshot and cross-select |
| M5 &check; | JSON | Generic JSON provider on simdjson, spans from source locations, ordered arrays and unordered object members, sniffing between the two built-ins | Done, and no interface change was needed: the provider is a new file, one line in the registry and one in the build. A 100k-node JSON pair parses in 113 ms a pair and matches in 80 ms, against 84 ms for the XML case of the same size |
| M6 &check; | Custom formats | Sample behavior-tree provider, format override, provider config, versioned provider documentation | Done. A `<node>` follows its GUID from one branch of the tree to another and is reported as one move, and survives a change of `type` that no structural heuristic could. docs/PROVIDERS.md carries interface version 1 |
| M7 &check; | Standing on its own | File picker and a welcome pane, graph direction in the layout with a per-format override and a View menu default, a pass over the existing code against CODE_GUIDELINES.md | Done. The window opens with no arguments and both files are chosen in it; the behaviour tree draws itself left to right without being asked, and the interface version stayed at 1 |
| M8 &check; | Formats without a compiler | Lua configuration from the home directory and the command line, retiring the M6 reader, the Lua provider bridge, the sample behaviour tree reimplemented in script, the graph direction and exit key settings | Done. The scripted behaviour tree produces the same tree and the same change list as the compiled one, and `kProviderInterfaceVersion` stayed at 1 |
| M9 &check; | Properties with parts | Nested properties in the data model, hashing, matching and both views; record and sequence parts, so an array property reorders as a change and a record does not; generic JSON reading a scalar array as one property, with a scripted format able to choose otherwise; the rule that anything not a node becomes a property; a way for a format to take both an element's attributes and its child elements as properties; the scripted surface and the golden corpus updated to match | Done. A list of scalars is one property, a matrix is one property with parts, reordering a list registers while reordering a record does not, and neither built-in format nor the bridge can drop an element it does not recognise |
| M10 &check; | Output and reload | A GUI launch that opens no console window while a headless run from a shell still prints and pipes; one log sink behind every line the program writes, shown in an Output pane and forwarded to whatever console or pipe is attached; Reload re-running every configuration file, rebuilding the provider registry, re-reading both files and comparing again; every Lua error written in full, with its traceback, to standard error; the open dialog's type list built from the registry, every known extension first and one entry per format under its own name | Done. The binary is GUI-subsystem and finds its console or pipe at startup; Ctrl+R rebuilds a scripted format from disk and a held snapshot keeps the old one alive; a raised `error()` reaches the Output pane and standard error with a traceback that names the script file and line; a scripted format claiming `.blackboard` is offered in the dialog as "Blackboard". P4V and Git remain to be checked by hand |
| M11 | Keys | Every action named, every shortcut settable from a configuration script, more than one binding allowed per action, the menus showing whatever is bound | A reader rebinds next-change to two keys of their own and the menu says so |
| M12 | Ship | Headless report, exit codes, a portable archive built in continuous integration from a tag and attached to a GitHub release, MIT licence and attribution for bundled dependencies, per-extension Perforce and Git setup docs verified against real clients, possibly a Git seven-argument mode, settings persistence | A technical artist can unzip it and configure it without help |
| M13 | Later | Three-way merge, further game asset formats | Out of initial scope |

M0 through M4 were the critical path. JSON sat at M5, deliberately ahead of the
milestone that publishes the provider interface as a documented surface: it was
the cheapest way to find out whether that interface accidentally assumed XML,
and finding out afterwards would have meant breaking a published contract. The
answer is that it did not. Adding a second format took a new file, one line in
the registry and one in the build, and the two hooks generic XML never
exercised, per-node child ordering and a synthetic property standing in for a
node's own content, both worked as declared. The one thing the exercise did
change is the golden harness, which had `left.xml` written into it and now
takes the format from whatever extension a case directory holds.

M6 published that interface. Writing a third provider against it needed no
change to it either, and it surfaced one thing worth extracting: recovering a
span from XML bytes, which pugixml does not do, is now `formats/xml_spans` and
is shared rather than copied. The one interface change M6 did make is elsewhere:
`SourceFile::fromMemory` gained an optional path, because the format a file
resolves to is read from its extension and content in memory previously had
nowhere to carry one. That made the configured-extension path testable without
touching the disk.

The requirements added after M6, R3, R10, R11 and R12, are two milestones rather
than one. They were briefly written as one, and the reason for splitting them is
that only half of the work shares anything.

**M7 is what the tool does for itself.** A file picker answers R10, and R10 is
what makes the tool usable by someone who has not yet wired it into a version
control system: shipping a diff tool that can only be launched by another
program would be shipping to the people who already have one. Graph direction
answers most of R12, since a deep tree read left to right is the case the
current layout serves worst. Neither needs an interpreter, neither depends on
the other, and both are visible the moment they land. The code guidelines pass
sits here too, deliberately before the largest new subsystem rather than after
it, so that the code M8 is written against already reads the way the guidelines
ask.

**M8 is one piece wearing two names.** R11 wants configuration in Lua and R7.4
wants providers in Lua, and R11.2 ties them together by asking for providers to
be declared inside config files. Splitting those would mean designing the same
surface twice, building the interpreter twice and writing two sets of error
messages. So the configuration, the bridge, and the sample behaviour tree
rewritten in script are one milestone, and the graph direction setting joins
them because R12 says the global default becomes configurable "when R11 is
implemented", which is here.

Both sit ahead of shipping. R11 replaces the configuration file M6 shipped, and
a format that reached a release would have to be supported afterwards. The
provider bridge is ahead of shipping for the reason in section 2: format
authorship is the adoption path, so a bridge arriving after the first release
arrives after the people it was for have already decided.

The order matters in one direction only. M7 does not need M8, but M8's graph
direction setting needs M7's graph direction, so running them the other way
around would leave a configuration key with nothing behind it.

M7 landed as planned, and the part worth recording is what it proved about the
interface. Graph direction was the first additive change made against the
published version 1: a new method with a default, nothing already declared
altered, and `kProviderInterfaceVersion` untouched. The three built-in providers
needed no edit at all except the one that wanted the new behaviour, which is
what additive was supposed to mean.

Two things came out differently from the sketch. The positioning pass was
rewritten in breadth and depth rather than gaining a second code path, and while
doing that it started aligning each level on the deepest card in it. Ragged
levels were tolerable top down and would have looked broken left to right, where
card widths vary most, so the fix serves both. And the file dialog turned out to
need a thread: a native dialog blocks whoever opens it for as long as someone is
looking at it, and constraint C makes no exception for waiting on a person.

The guidelines pass took the worst offender from 101 lines to about 25 by
extracting four named steps, split the card drawing out of its loop, gave the
node view's twenty-odd drawing literals names, and separated the two report
writers. What is left sits between 45 and 65 lines: the Myers middle snake, the
similarity score, a clipper loop and the frame loop. Those are single algorithms
rather than several things in one function, and cutting them up would hide their
shape rather than reveal it, so they were left alone deliberately.

R3 is not a feature but a pass. The existing code already carries Doxygen on
every entity, which the `docs` target enforces, so what is left to review is
block-level comments, named constants, and functions long enough to want
breaking up.

M8 landed, and the thing it was written to test came out well. Constraint A
said a scripted provider should be a plain subclass rather than a redesign, and
it is: `kProviderInterfaceVersion` is still 1, no method changed shape, and the
bridge is a class implementing the same interface as everything else.

The design that made that work is the one sketched above: a script shapes a tree
rather than parsing bytes. The base format reads the file and the script decides
what the result means, which keeps every hot loop compiled and makes the scripted
surface nine entries rather than ten methods. It also made the cost model easy to
honour, because each of those entries is asked once per node while the document
is open and the answers travel with the tree.

That last part needed somewhere to put them. A provider must be stateless, so a
cache inside the provider was out, and synthetic properties would have joined
the hash and changed which nodes pair up. Trees gained a side table instead,
indexed by node and empty unless a provider fills it, so the built-in formats
pay nothing for it.

Two things the corpus caught that review would not have. Comparing the scripted
behaviour tree against the compiled one turned up a difference in the order of
one change's property names, which was the scripted surface having no way to say
property display order: R7.5 asks for that, and it was simply missing. And the
cancellation hook, which the sandbox would have provided before the search was
dropped, came back for its real reason: a script with a loop in it holds a
worker, and constraint C makes no exception for code the user wrote.

Lua is compiled as C++ so that a script error unwinds as an exception rather
than through longjmp, which would step over the destructors of everything the
bridge holds while a callback is running. That is worth knowing before anyone
tries to swap in a system Lua, which would be built as C.

M9 and M11 cover the requirements added after M8. They are two milestones
because they share nothing: one is a change to the data model that every
provider and both views are built on, and the other is a table of key bindings.
M10 was added after both were planned and takes the number between them
because it goes first; it is described below, after M9.

**M9 is the last data model change.** Nested properties reach into hashing,
matching, the details panel and the change list, and each of those has to decide
what a property with parts means. It goes before shipping for the ordinary
reason: the tree is what every provider is written against, and changing it
after a release means changing it under people who have written providers.

It also settles the open question about how much a provider may drop, and
settles it more firmly than the question asked. The question was whether the
interface should make dropping content harder to do by accident. R7.8 answers
that it should be impossible: an element is a node or a property, and there is
no third answer. What the milestone has to get right is the wrapper case above,
where an element that is not a node contains nodes.

**M11 is small but not trivial**, because R16 asks for more than one binding per
action, which means a key table rather than a setting per key. The exit key
that M8 added becomes one row in it. Doing this before shipping matters more
than its size suggests: a keyboard map is the kind of thing people build habits
around, and changing it afterwards costs more than building it now.

**R11.3 was already satisfied**, which was checked rather than assumed: one
script may declare as many providers as it likes, because each declaration is
an ordinary call and nothing about the reader is limited to one.

M9 landed, and the corpus is what makes that claim worth anything: only two
JSON cases moved, and both moved the way they were meant to. A change buried in
an anonymous `item` node became a named property on the node above it, which is
the whole point of the milestone in one line of expected output.

Three things were decided while building rather than before, and each is worth
keeping.

**The boundary is "contains an object", not "is empty".** An empty array
becoming a property means an empty object and an empty array flip
representation, which shows up as a deletion beside an addition rather than as a
changed type. That is churn on a rare edit. Putting the boundary at emptiness
instead would have moved the churn onto adding the first element to a list,
which is a common edit, so the rarer boundary is the right one.

**Deciding needs a second pass over the array.** On Demand parsing is forward
only, so the choice was between looking twice and building optimistically then
unpicking it on meeting an object. The parser's own rewind makes the first cheap
and exact, and the second would have been more code and more ways to be wrong.

**The benchmark had to grow.** Turning tag lists into properties took the
hundred-thousand-node case down to forty thousand for the same file, so the test
would have gone on passing while measuring a smaller problem than its name
claims. The entity count went up to keep it honest, which is why parse and match
now read higher: the file is six megabytes rather than three, not the code
slower.

The rule that nothing may be dropped reached the compiled behaviour-tree
provider too, not only the bridge. It had the same walk-through path, and the
sample provider being the worked example is exactly why it should not be the one
place the rule is broken.

It had a second hole, found by someone editing the sample rather than by any
test. A `<property>` element whose content is elements rather than a value was
read as text, found nothing there, and lost everything inside it. The fixed
version distinguishes the two cases by who visits the children: a folded
element's content is read once, there and nowhere else, so it nests; an element
the format does not recognise is walked into separately, so only its attributes
are recorded and recording its children as well would represent them twice. The
bridge makes the same distinction through `fold_into_parent`.

The corpus had no case that reached a property with parts at all, which is why
neither hole showed up in it. Three cases now do: a nested property whose change
is reported at the outermost name, a wrapper element whose own attribute changed
while the node inside it stayed put, and a JSON pair with a tag list and a
matrix. Those are the cases the milestone turns on, and they were the last thing
it was missing.

**M10 covers R17 through R20**, which arrived together and turn out to be one
piece of work seen from four sides: where the program's output goes, and what
happens to a configuration once the window is open. It goes before Keys because
every milestone after it that touches configuration is tested by editing a
script and pressing Reload, and because a scripting error that vanishes is the
failure a format author can least afford while writing a provider.

**R20 reverses a decision the build records.** `src/app/CMakeLists.txt` keeps
the binary a console application on purpose, with the reason written beside it:
a GUI subsystem binary loses standard output, and the headless path and every
version control integration depend on it. Both requirements stand, so the
resolution is a GUI subsystem binary that finds its output rather than a console
binary that hides its window. At startup, before anything is written, the
program looks at what it was given: a standard output or error handle that is
already valid, because the caller redirected it to a file or a pipe, is used as
it is; otherwise it attaches to the parent process's console, when there is one,
and reopens the two streams on it. Launched from the desktop or by a version
control client there is neither, and nothing is attached, which is the whole of
R20. There is one known cost, and it is why this is a milestone item rather
than a flag: a command shell does not wait for a GUI subsystem process, so
`nmxmldiff --headless a b` typed at a prompt returns the prompt before the
report, with the two interleaved. Version control tools wait on the process
handle and are unaffected; a person at a shell is. The alternative is two
executables built from one object library, differing only in subsystem, which
is what Python does with `python.exe` and `pythonw.exe`. That doubles what an
integration guide has to say and leaves the GUI-launched one unable to print
at all, so the attach approach is the recommendation and the two-binary one is
the fallback if the interleaving proves unlivable. The done-when condition
checks it against `cmd.exe`, PowerShell, Git Bash, P4V and Git, because this is
the kind of behaviour that differs between callers rather than between builds.

**R17 and R20 are the same sink.** Once a GUI launch has no console, standard
output and standard error of that process go nowhere, and an Output pane is not
a mirror of them but the only place they exist. So the program gets one log
sink, in core rather than in the window, and every line the program writes goes
through it: the configuration problems `main.cpp` prints, the glfw error
callback, the screenshot failure, the headless report's warnings. The sink
keeps a bounded buffer the pane draws from and forwards each line to standard
output or standard error when one is attached, which is how the headless path
and the pane see the same text. Which stream a line belongs to is kept, so the
pane can show error lines as such and a script can still tell `2>` from `>`.
`print()` inside a configuration or provider script is rebound to the sink for
the same reason: the tool is telling the author what their script said, and a
`print` that reaches a console nobody can see has told them nothing. Everything
the process itself writes is covered by that; a third-party library writing to
the C runtime's streams directly is not, and capturing at the file descriptor
level with a reader thread is the fallback if one turns up. The pane is a
docked window like the details panel, off by default in a fresh layout so a
person reviewing a changelist is not shown a log, and opened from the View menu
or by the first error, which is what a log pane is for.

**R18 makes Reload mean what it says.** Today Ctrl+R hands the session the same
request again, which re-reads both files and compares them, and nothing else:
the configuration was loaded once in `main()` before the window existed, the
scripted providers were added to the session's registry once from it, and a
provider script edited on disk is not consulted again until the process
restarts. A format author iterating on a shape function is restarting the tool
for every edit, which is a slower loop than the compiled provider had. Reload
becomes: cancel and wait for the comparison in flight, since the workers hold
provider pointers into the registry that is about to go; run the same
`loadConfiguration()` the startup ran, over the same three files in the same
order, with the `--config` path fixed at what the launch said; build a fresh
registry from the default one plus what the configuration adds, exactly as the
session does now; if all of that succeeded, swap the registry in, re-apply the
settings the window took from configuration, the graph direction default and
the exit key among them, and open the request again. If any of it failed, the
problems go to the sink and the previous registry stays, which is the startup
rule that a bad configuration is not partly applied, carried into the running
program. The step worth taking out of `main.cpp` is the loading and checking,
so that startup and Reload are one function called twice rather than two copies
that drift. The `LuaState` a scripted provider keeps alive through `keepAlive()`
belongs to the tree it shaped, not to the registry, so a tree built by the
previous provider remains valid to draw until the new comparison replaces it.

**R19 wants the whole error where a person can read it.** Two places truncate
today and each had a reason. A configuration error is trimmed to its first line
before it becomes a `ConfigProblem`, so the line-per-problem listing stays a
listing. A failure inside a shape job keeps its first line only, by decision in
section 15, because the message lands in every report line and every card
tooltip for one wrong element. Both stay as they are for those uses, and the
full text, with the traceback sol2 produces when asked for one, goes to the
sink as the error is raised, once. `ShapeFailure` gains a `detail` beside its
`message` for that purpose, written when the drain records the failure, and the
configuration reader writes its detail at the point it complains. The shape
case is the important one: in the window, a shape failure today marks a span
violet and a card corner, and the message exists only as a tooltip, so a format
author reads their stack trace by hovering. After this it is on standard error
when a console is attached and in the Output pane always, and the mark in the
views points at where it happened.

**Order of work.** The sink first, with the existing writes moved onto it and a
test that a line written on either stream comes back from the buffer with its
stream. Then the subsystem change with the attach logic, tested by hand against
each caller named above and recorded in this section. Then Reload as the loading
function extracted from `main.cpp` and called from the window, with a test that
a provider script changed between two loads shapes differently on the second.
Then the full error text, with a test that a shape job calling `error()` leaves
a traceback in the sink and a one-line message on the failure. The pane last,
since it draws what the rest produced.

**M10 landed** in five commits, in that order, and three things came out
differently from the paragraphs above.

*Reload does not wait.* The reason to cancel and wait was that workers hold
provider pointers into the registry about to go. Instead the session captures
the registry a comparison starts with and the snapshot holds it, so a frame
drawing a tree the old provider shaped may still ask it for a title while the
new comparison runs, and the frame loop waits for nothing. `configureProviders()`
builds a fresh registry from the built-ins every time, which is what makes
calling it twice mean Reload rather than accumulation. The session test holds
the first snapshot across the swap and asks its provider a question.

*Where the failure text is written.* The plan had the default `parse()` write
it, with the file label to hand. It is written by the drain instead, as the
job fails, because a script that no longer runs at all fails before any node
exists and then there is no tree to carry the detail through; the label
travels to the context for the purpose. The chunk name was the surprise: sol2
already appended a traceback to every message, but the chunk was the script's
own first line in quotes, so a traceback read `[string "provider "bt" {..."]:6`.
Both readers now load a script under its file name, and the name alone rather
than the path, because a Windows path carries a colon where Lua's format puts
the line number after the first one.

*Where the pane docks.* Docking it in the one-time layout did nothing, because
a saved layout skips that step and a window absent when the layout was saved
opens floating. The pane asks to join the status bar's node on its first
appearance instead, and asks for focus on three consecutive frames, since a
window joins its node's tab bar the frame after it first exists and a focus
given before that selects no tab.

What the done-when condition asked for was checked from Git Bash, cmd.exe and
PowerShell through pipes and files, and from PowerShell with no handles at
all, where the report landed in PowerShell's own console buffer. P4V and Git
were not to hand and remain to be checked. The shell-does-not-wait cost is
real and is documented in USAGE.md rather than worked around; the two-binary
fallback was not needed.

**R21 joined M10 after the rest had landed**, and it is small because the
registry already knew everything the dialog needs. The type list was a
constant in the picker, four entries written by hand, so a format a script
defined was exactly the file the dialog hid. `fileFiltersFor()` in core builds
the list from the registry instead: every extension any provider claims or a
configuration pointed at it, first and selected by default, then one entry
per provider that claims anything, in registry order and under the provider's
display name. The registry gained an `overrides()` accessor for the configured
extensions, and an extension is credited to the provider `overrideFor()` would
name, so the last mapping wins in the dialog as it does in resolution. The
list is built when the dialog opens, from the session's registry as it is then,
so a Reload that changes the scripts changes the list. The dialog library adds
the entry admitting every file. Tested on the registry alone, since the dialog
is the operating system's.

12. Testing
-----------

- **Golden-file diff tests.** Each case is a directory holding a left file, a
  right file, and an expected serialised change list. The extension decides the
  format, so a case in a new format is two files and an expectation with nothing
  else to edit, and the corpus asserts that every built-in format is present
  rather than trusting that it is. A case that needs a particular provider adds
  a `format.txt` naming it, which is what lets the corpus hold one document read
  two ways and makes the difference between a generic and a schema-aware
  provider reviewable. This is the main defence against matching
  regressions, and it makes a change in matching quality reviewable as a diff of
  expected output.
- **Unit tests** for hash stability, span correctness, property ranking,
  registry resolution, and the Myers implementation against known cases.
- **Concurrency tests** that cancel a job mid-pass and assert the pipeline
  settles, and that run the whole thing under the thread and address sanitizers
  in continuous integration.
- **Budget tests** for startup time, staged publishing latency, full match
  time, and cancellation latency, so the numbers in section 8 cannot silently
  rot.
- **Configuration tests** that write the three files, assert which setting
  wins where they disagree, and assert that a broken script names its own line
  rather than failing silently or half applying itself.
- **The scripted behaviour tree against the compiled one.** The corpus already
  holds what the C++ behaviour-tree provider produces, so a Lua reimplementation
  of it has an exact expected answer rather than a plausible one. If the two
  disagree, either the bridge is lossy or the scripted surface is missing
  something, and the corpus says which case differs. This is the main test that
  the bridge is faithful and not merely working.
- **End-to-end tests** through headless JSON reporting.

The user interface is not unit tested. Keeping all the logic in the core
library is what makes that acceptable.

13. Risks and open questions
----------------------------

- **Risk: move detection is the product.** Weak move detection makes the node
  view worse than a plain text diff, and it is the thing a studio will judge
  the tool on in its first ten minutes. Budget tuning time in M3 against real
  asset files, and treat the golden corpus as the definition of good.
- **Risk: async correctness.** Not blocking has a cost, and it is paid in
  lifetime and cancellation bugs. Mitigated by a single publish point,
  immutable snapshots, and no shared mutable state between a worker and the
  frame loop. Sanitizers run in CI from M0, not later.
- **Risk: docking branch churn.** Dear ImGui docking is not a release branch.
  Pin an exact commit in the manifest and upgrade deliberately.
- **Risk: round-tripping for merge.** Serialization is declared but
  unimplemented. If preserving original formatting in merged output matters,
  the tree must retain more source detail than it does now. M9 moves the model
  in that direction by giving properties parts, but does not settle it. Decide
  before M13, not during it.
- **Settled: JSON spans.** Node spans are what link the two views, and most
  JSON libraries expose no byte offsets at all. simdjson was chosen for that one
  capability and it delivered: a container's extent comes from the parser's
  cursor after the container is consumed, which is a constant-time read rather
  than a rescan of the bytes. A change of parser later is still not a swap but a
  rewrite of the provider.
- **Open: what a scripted provider costs on a large file.** The cost model
  landed as designed, with each script function asked once per node and the
  answers kept with the tree, so nothing crosses into the interpreter while a
  frame is drawn. What has not been measured is the parse itself: the samples it
  has been run against are tens of nodes, not the hundred thousand the budgets
  are written for. If the gap against a compiled provider turns out to be large,
  the honest answer is a documented size limit rather than pretending scripted
  formats are free.
- **Risk: startup is over budget and the measurement is noisy.** The window is
  visible somewhere between 205 and 290 ms against a 200 ms target, and where in
  that range depends on what else the machine is doing: the same binary measured
  210 ms earlier in a session and 270 ms later in it. Neither M7 nor M8 added to
  it, which was checked rather than assumed by measuring the build from before
  M8 and getting the same range. The file dialog is built only when someone asks
  for one and an interpreter only when a script is found, so a run with no
  configuration builds neither. What the number needs is a quiet machine and
  repeated runs, not another guess: most of it is window and OpenGL context
  creation, and until that is measured properly there is nothing to act on. It belongs off the critical path: the window is shown first, and
  the interpreter is created when a configuration file is found rather than at
  startup, which costs nothing in the common case of having none.
- **Open: how a team shares configuration and providers.** Dropping the
  directory search left no answer to the question it was for, and the provider
  bridge sharpens it: a studio format written in script is exactly the thing a
  team wants to keep in one place, and it is code rather than settings. A file on a shared drive named with
  `--config` works, at the cost of every person repeating it in their diff-tool
  command, and a studio that deploys the tool by script can write the home
  directory file instead. Whether that is enough depends on how studios actually
  install it, which is not known yet. If it is not, the note above on matching
  labels rather than paths is where a second attempt should start.
- **Open: which real asset format to validate against.** The sample
  behavior-tree provider is the format from the requirements, not one a studio
  actually exports, so it proves the interface without proving the tool against
  a real pipeline. Picking one concrete format gives the performance work a
  realistic corpus instead of synthetic trees, and decides who can try the tool
  on day one.
- **Settled: properties do not move.** A property is compared inside the node
  that owns it and is never matched against a property somewhere else, so a
  transform block cut from one entity and pasted into another reads as a
  deletion beside an addition rather than as one move. Making it read as a move
  would mean giving properties identity and matching of their own, which is most
  of what a node is, and the cheapest way to get there would be to make
  properties nodes outright. That was considered and refused on two grounds.
  Counting properties per node across the samples and the benchmark gives 2.2 to
  2.4, so the hundred-thousand-node budget case would become roughly three
  hundred and twenty thousand nodes for the same input file, and the headline
  number would stop describing the file a person opened. And properties compare
  as an unordered set while nodes match by identity, hash and similarity, so
  unifying them would either put that distinction straight back inside the
  matcher or make a reordered attribute list read as a move, which the plan
  promises it never will. Reopen this only if reporting a moved property block
  becomes something a studio actually asks for; the numbers above are what to
  weigh it against.
- **Settled: how much a provider may drop.** Nothing. R7.8 makes everything that
  is not a node a property, so the walk-through answer that let content fall out
  of a tree is gone at M9. The question had been whether to make dropping harder
  to do by accident; the answer is that it stops being possible. What remains is
  a design decision inside M9, not an open question about the interface: an
  element that is not a node but contains nodes has to keep both, which means
  recording the wrapper as a property while its nodes attach to the nearest
  ancestor node.
- **Settled: release channel.** A tagged archive on GitHub, or on something
  that works the way GitHub does. That decides more than where a file sits. It
  means a release is a git tag rather than a build someone ran, so M12 builds the
  archive in continuous integration from the tag and attaches it; it means the
  setup documents can name a download URL that does not change; and it means the
  attribution for the bundled dependencies ships in the archive rather than
  living on a page somewhere. There is no installer and no auto-update: a studio
  unzips a directory, which is also what makes it easy to keep two versions
  side by side.
- **Risk: the presentation is behind the engine.** The first outside reading of
  the tool found the matching correct and fast on real-sized files and the node
  view unreadable on them, which is the more dangerous way round: a reader
  judges what is drawn. Section 14 carries the findings and their priorities,
  and five of them are marked as owed before M12 rather than in it.
- **Risk: the engine is trusted more than it has earned.** A second reading, by
  an engineer running the binary against constructed inputs rather than
  samples, found that the tool can report no change where one exists, can die
  without a message, and takes its headless verdict from the line diff instead
  of the tree diff. Those are in section 14 as F15 to F17, and they matter more
  than anything in the first pass: a tool that draws badly is abandoned, and a
  tool that under-reports is believed. Nine findings in total are now owed
  before M12.

14. Field review, and what it filed
-----------------------------------

A senior designer read the tool the way a studio would, against files a studio
actually has rather than the samples: a five thousand entry localization table,
the same table after a tool re-sorted it, a Tiled map with a CSV tile blob, and
an eleven megabyte weapon table of two hundred thousand rows. The engine came
out well and the presentation did not, which is worth stating plainly because
the plan has spent its attention on the half that is already good.

What held: matching is correct and fast, attribute order and entity spelling
and empty-tag form are all correctly not a change, and a scripted provider for
an unfamiliar format took nine lines of Lua and worked first time. The eleven
megabyte pair with one edited row found the row and nothing else in 1.24
seconds. That is the adoption path in section 2 doing its job.

What did not hold is everything between a correct diff and a reader seeing it.
Two findings change scope rather than adding polish, and they are the first two
rows below. The rest are defects with a known cause.

| # | Finding | Priority | Lands in |
| --- | --- | --- | --- |
| F1 | Node cards drop their text below a zoom of 0.55, and fitting a twenty-six node tree already lands under it, so the node view opens on unlabelled boxes. `kTextZoomThreshold` in `src/ui/node_view.cpp`. | P0 | Before M12 |
| F2 | A wide, flat document degenerates: fifteen thousand nodes draw as a one pixel smear and the minimap with it. Most studio XML is a table, not a tree. Collapsing unchanged subtrees should be the default when changes are sparse against the node count. | P0 | Before M12 |
| F3 | Sibling order cannot be declared unordered from a script. `childrenOrdered()` is on the C++ interface and not on the Lua surface, so a re-sorted string table reports 4860 moves and the fix needs a compiler. This contradicts the claim that a studio format needs no compiler. | P0 | Before M12 |
| F4 | There is no search or filter in either view. Finding one entry in a five thousand row table means scrolling to it. | P0 | Before M12 |
| F5 | The JSON report carries counts only. The text report lists every changed node and the machine-readable one does not, so automation gets strictly less than a person does. `src/app/report.cpp`. USAGE.md calls it "a machine-readable version of the same thing", which is not true today. | P0 | Before M12 |
| F6 | Report paths are positional, so a changed string reads as `/StringTable/Entry[501]` even when the provider supplies both an identity and a title. That makes headless output near useless in a review comment. | P1 | M12 |
| F7 | UTF-16 is refused outright, and older exporters still write it. | P1 | M12 |
| F8 | A pair whose format changed between revisions reports "the document is not well formed" without naming the format it tried, because the format is resolved from one side and applied to both. | P1 | M12 |
| F9 | An unrecognised command-line option exits 109, the CLI11 default, rather than the documented 2. | P1 | M12 |
| F10 | Nothing can be copied to the clipboard: not a path, not a value, not a node. | P1 | M12 |
| F11 | Diff colours are hard coded with no on-screen legend, and added against deleted is carried by red against green alone. | P1 | M12 |
| F12 | Comments are dropped from the node tree while the text view still counts them, so an edited comment prints a node summary of "identical" beside four changed lines. Decide which of the two is wrong rather than leaving them to disagree. | P2 | M13 |
| F13 | Duplicate identity keys are handled without a crash and without a warning. A copy-pasted GUID is a real authoring bug the tool is in a position to name. | P2 | M13 |
| F14 | Escape closes with no confirmation. Right for reading a changelist, surprising on a first launch that was not started by a version control system. | P2 | M11 |

F1 through F5 are grouped as P0 because each one alone is enough for a reader
to conclude the tool does not work on their files, and four of the five are
about the node view, which is the thing the tool is for. They belong before
M12 rather than in it: shipping an archive that a technical artist can unzip is
worth nothing if what they see when they open it is a field of blank
rectangles.

F3 is the one that costs more than it looks. Putting `childrenOrdered` on the
scripted surface is small, but it is the first entry that is a property of a
node rather than a property of the format, so the bridge has to ask the script
per node and keep the answer with the tree the way `is_node` already does. That
is the established cost model rather than a new one, which is why this is a
defect and not a design question.

F2 has a decision inside it. Collapsing unchanged subtrees by default is a
change to what the tool shows without being asked, and section 2 says a diff
tool that quietly under-reports loses trust permanently. The distinction that
makes it safe is that a collapsed subtree is still counted and still reachable,
and the chip says how many nodes it stands for. Anything that hides a change
rather than a body of unchanged nodes is out.


### Second pass: an engineering review

The first review read the tool the way a designer would and found the engine
good and the presentation bad. A second review read the code the way a
principal engineer at the studio adopting it would, and ran the shipped binary
against constructed inputs rather than against samples. It reached the opposite
conclusion in one respect that matters: the engine is not as sound as the first
pass concluded, because a correct matching is not the same thing as a correct
report of what it found. Every finding below was reproduced against the built
binary rather than inferred from reading.

What held, again: the passes are correct and fast on the shapes they were
designed for, the matching is a matching rather than an edit script so merge
stays open, the provider interface carries no GUI type and no template, the
union tree with ghost edges is the right visual model for a move, and hashing
walks the arena backwards so it has no recursion to overflow. All 184
registered tests pass.

What did not hold is that the tool can silently report no change where a change
exists, can die without a message, and answers the one question automation asks
using the wrong half of its own output.

| # | Finding | Priority | Lands in |
| --- | --- | --- | --- |
| F15 | A change to a property whose name is already present is reported as identical. `changedPropertyNames()` in `src/core/diff.cpp` puts the right-hand properties in a map keyed by name and probes the left with `findProperty()`, and both collapse duplicates. Adding a second `<property name="cooldown">` to a `<node>` yields no node change at all; removing it is caught, so the miss is asymmetric. The content hash is correct, so the pair never matches by hash: it anchors by strong identity and is then classified as unchanged. Every format that folds repeated child elements into properties is exposed, which is the shape R7.9 exists for. Properties have to compare as a multiset, matched by name and then greedily by value. | P0 | Before M12 |
| F16 | A document nested about six thousand levels deep crashes the process with no message and no usable exit status. Parsing, `pairIdenticalSubtree()`, `addSubtree()`, `walkPair()` and the layout's own `addSubtree()` all recurse on document depth, while `computeHashes()` deliberately does not. Either convert those walks to explicit stacks or refuse past a declared depth with a real `ParseError`. Generated data reaches this and hand-authored data does not, which is why no sample caught it. | P0 | Before M12 |
| F17 | `--exit-code` and the `identical` field are taken from the line diff, not the tree diff. `writeReport()` in `src/app/report.cpp` computes `identical` from `TextDiff::identical()`, so the whitespace-only golden pair prints `identical` for the tree and still exits 1. A submit trigger wired to this gets exactly the answer the tool exists to correct. The tree verdict must decide whenever a provider resolved, with the line verdict as the fallback. The JSON report already admits the problem by reporting `"comparison": "lines"`, but honesty is not the behaviour a build job needs. | P0 | Before M12 |
| F18 | The similarity step budget aborts the pass for the whole document rather than for the subtree that overspent it. `matchChildrenOf()` returning false ends `matchBySimilarity()` outright, so one wide container degrades the matching everywhere else in the file. Four thousand JSON entities with renumbered identifiers, 850 KB a side, take about 4.9 seconds and come back with the guard tripped. That is a re-export, not a pathological input. Budget per parent pair and let the rest of the tree finish clean. | P0 | Before M12 |
| F19 | The budget tests never run. They carry Catch2's hidden `[.slow]` tag, so `catch_discover_tests` does not register them and `ctest` lists none of them. The numbers in section 8 are documented and unenforced, which is how F18 survived. Register them as their own labelled suite, and let continuous integration run them on a schedule if they are too slow for every commit. | P1 | Before M12 |
| F20 | Weak identity keys are computed and discarded. `identity()` is read in exactly one place, `anchorByIdentity()`, which skips anything not marked strong. Generic XML builds a key from the element name and its `id` attribute on every node and nothing ever reads it. Either consume a weak key as a tiebreaker inside `similarity()` or take it off the interface, because a provider author writing against section 6 will reasonably expect it to do something. | P1 | M12 |
| F21 | A script sees a flattened view of a node: its element name and a name-to-value table, rebuilt for each of the five questions asked per node. It cannot see children, parent context, or the nested and array parts that R7.7 and R7.10 added to the model, and duplicate names collapse in that table the same way F15 collapses them. A format definition that needs to look one level down, which most real schemas do, cannot be written in Lua today. This is the same class of gap as F3 and should be fixed alongside it. | P1 | M12 |
| F22 | Provider scripts run with `io`, `os` and `package` open, reasoned in `src/core/lua_state.cpp` as the trust a shell gives a startup file. A studio rollout inverts that assumption: the shared provider script lands in the depot, every engineer's configuration points at it, and it then executes on every workstation and build agent on every diff, twice per diff because `loadShape()` re-runs the whole script once per side. Sandbox provider scripts to base, string, table and math, and keep the full set for the top-level user configuration only. Section 6 should say which of the two a given file is. | P1 | M12 |
| F23 | Text content is dropped from any element that also has element children. `GenericXmlProvider::build()` folds `#text` on a leaf only, so an edit inside `<text>Hello <b>world</b></text>` is invisible in the node view. This is the same disagreement as F12 one level down, and the two should be decided together: R7.8 says everything that is not a node is a property, and mixed content is currently neither. | P2 | M13 |
| F24 | Paths given on the command line are decoded through the active code page, because `main()` takes narrow `argv` and the Microsoft toolchain converts it with the ACP. A workspace under a name outside that code page cannot be opened at all. Take `wmain()` on Windows and carry a `std::filesystem::path` from there. | P2 | M13 |
| F25 | `nodePath()` names a deep node by its full ancestry, so one changed node in a deeply nested document prints a path thousands of segments long and the change list becomes unreadable. F6 already replaces positional paths with provider identity, and that fix should cap or elide depth as well. | P2 | M13 |

F15, F16 and F17 are the three that block putting this in front of the team,
and they are of a kind the first review could not have found: each needs an
input a designer would not think to construct, and two of them stay invisible
unless the tool's answer is compared against the truth rather than against its
own other answer. F15 is the worst of the three by some distance. A diff tool
that is slow is annoying and a diff tool that crashes gets reported; a diff
tool that says nothing changed is believed.

F17 deserves stating in the terms section 2 uses. The adoption argument is that
a reformat is not a change, headless mode is where a studio cashes that
argument in, and headless mode currently answers with the line diff. The tool
disagrees with itself inside eight lines of its own output. Fixing it is small.
Leaving it is the difference between a tool a build engineer wires in and one
they read the output of once and give up on.

F18 and F19 belong together and in that order. The budget going unenforced is
why a five second case reached a review at all, and re-registering the tests
before fixing the abort would only mean the suite fails.

F20, F21 and F22 are the studio-facing surface rather than the engine, and they
are what the second format author will hit in their first week. F21 in
particular is the same finding as F3 seen from another side: the scripted
surface was built to carry the sample behaviour tree and has not been widened
since the data model grew properties with parts. Widening it once, deliberately,
is cheaper than answering it one function at a time.

15. The DOM provider interface
------------------------------

**Provider interface version 2.** This section supersedes four statements made
earlier in this plan, and says so here rather than editing them, because the
reasoning that led to each is worth keeping. Section 6 says no interface change
is expected and `kProviderInterfaceVersion` stays at 1; it goes to 2. Section 6
says nothing a provider does not recognise may be dropped; a provider may now
drop content, and the tool shows where. Section 5 gives a property a bool
`ordered`; it gets a three-way form and may carry a value in any form. And
section 6's scripted surface, the five shaping functions, is replaced whole.

Four findings from section 14 drive this, and they are one finding seen from
four sides. F21: a script sees a flattened view of one element and cannot look
one level down. F3: a script cannot say a node's children are unordered. F15:
repeated property names collapse in the change list. F16: a document six
thousand levels deep crashes the process, because the parsers, the shaper and
several matching passes recurse on depth. The five shaping functions were a
visitor: the tool walked the document and asked the script a question per
element. Every one of the four is a limit of who holds the walk. So the walk
moves to the provider, the provider is handed the document rather than an
element, and the tool's own part becomes non-recursive by construction.

### Reading and shaping

`parse()` splits in two. `read()` turns bytes into the document as written, and
`shape()` says what that document means. The default `parse()` is `read()`,
then `shape()`, then `finish()`, and a provider rarely overrides it.

```cpp
class IFormatProvider {
    // Bytes to DOM. Only a format that really reads bytes implements this.
    virtual Result<Dom, ParseError> read(const SourceFile&, std::stop_token) const;

    // DOM to tree. This is where a format says what its elements mean.
    virtual void shape(ShapeContext&) const;

    // read, shape, finish. The default is what every provider wants.
    virtual Result<Tree, ParseError> parse(const SourceFile&, std::stop_token) const;

    // No longer virtual. Each reads what shape() recorded on the node.
    IdentityKey identity(const Tree&, NodeId) const;
    NodeStyle style(const Tree&, NodeId) const;
    bool childrenOrdered(const Tree&, NodeId) const;
};
```

A scripted provider is then the same shape as a compiled one: it names a base
format whose `read()` it borrows and supplies its own `shape()`. The "base
provider" special case in `lua_provider.cpp` goes away, and so does the test
that a compiled and a scripted behaviour-tree provider produce identical trees
being a test of two parsers. It becomes a test of two shapers over one DOM,
which is what it was always trying to be.

**The DOM is a façade, not a copy.** `Dom` is a read-only view over the tree
the base `read()` produced, and `DomNode` is an index into that arena with
navigation mapped onto what `Node` already carries. Nothing is built alongside
it. That matters because a million-element document is already a memory
problem as a tree, and holding a second representation of it during shaping
would double the problem for no gain. The consequence is that a script sees the
base format's reading of the file, `#text`, `#value` and `#type` included, and
the documentation names that rather than hiding it.

```cpp
class DomNode {
    bool valid() const;
    DomId id() const;
    std::string_view name() const;
    SourceSpan span() const;
    std::string_view text() const;

    DomNode parent() const;
    DomNode firstChild() const;
    DomNode lastChild() const;
    DomNode nextSibling() const;
    DomNode prevSibling() const;
    std::size_t childCount() const;
    DomNode childAt(std::size_t) const;
    ChildRange children() const;

    std::size_t propertyCount() const;
    DomProperty propertyAt(std::size_t) const;        // document order, repeats included
    DomProperty property(std::string_view) const;     // the first with that name
    PropRange properties() const;
    PropRange properties(std::string_view) const;     // every one with that name
};

class DomProperty {
    std::string_view name() const;
    std::string_view value() const;
    PropertyForm form() const;
    SourceSpan span() const;
    std::size_t partCount() const;
    DomProperty partAt(std::size_t) const;
    PartRange parts() const;
};

class Dom {
    DomNode root() const;
    DomNode at(DomId) const;
    std::size_t size() const;
    std::string_view baseFormat() const;
};
```

There is deliberately no traversal helper on the DOM. Navigation is by handle
and the provider owns the walk. The tool's side of the bargain is that nothing
it does with a document recurses: `read()` in each built-in, the renumbering in
`finish()`, hashing, the matching passes F16 names and the layout. Every one is
an explicit stack. USAGE.md now says so, in the section on when a script runs,
and the corpus gets a case a few thousand levels deep so the sentence has a
test behind it.

**Every built-in moves onto this.** All three parsers recurse today, not only
the behaviour tree: `xml_generic.cpp` per element, `json_generic.cpp` through
`build`, `buildObject` and `buildArrayNode` and again in the scalar-array
lookahead, and `bt_xml.cpp` in three places. pugixml's own parser is iterative
and its nodes carry parent and sibling links, so the XML walk is a stack of
`xml_node`. simdjson On Demand is a forward-only cursor, so the JSON walk is a
stack of open containers, which is the shape that parser wants anyway. The
behaviour-tree provider stops parsing bytes at all and becomes a `shape()` over
the XML `read()`, which is also the worked example in docs/PROVIDERS.md.

**Visitor-style providers stay possible.** `shape()` is the one virtual. A
provider that would rather answer questions than hold a walk derives from an
adapter that owns a stack-driven pre-order over the DOM and calls `enter()` and
`leave()` with whatever context travels down the branch. It builds through the
same handle and never touches the queue. The five functions this section
retires were such a visitor, and if a studio wants the short form back it is one
adapter and one binding on top of the same DOM.

### The output handle

One handle type, for nodes and properties alike. That is the whole point: a
caller holding a handle does not track whether it stands on a node or inside a
property, because `child()` does the right thing wherever it stands.

```cpp
enum class RefKind { Node, Property };
enum class PropertyForm { Scalar, Record, Sequence };
enum class Identity { Weak, Strong };

class Ref {
    bool valid() const;
    RefKind kind() const;
    bool isNode() const;
    PropertyForm form() const;             // property only
    RefId id() const;                      // savable across queued jobs

    // The polymorphic one. See the table below.
    Ref child(std::string_view name = {});
    Ref child(const DomNode&);             // kind, span and source from the element

    // Always a property: of a node, or a part of a property.
    Ref property(std::string_view name, std::string_view value = {});
    Ref property(const DomProperty&);      // name, value, form, parts, span, source
    Ref record(std::string_view name);
    Ref sequence(std::string_view name);
    Ref item(std::string_view value = {}); // sequence only, appends

    Ref parent() const;                    // the enclosing node or property
    Ref owner() const;                     // nearest enclosing node, itself when one

    Ref& setName(std::string_view);        // kind on a node, name on a property
    Ref& setValue(std::string_view);
    Ref& setSpan(SourceSpan);              // compiled providers only; Lua has no span type
    Ref& setChildrenOrdered(bool);         // node only; answers F3
    Ref& setIdentity(std::string_view value, Identity = Identity::Weak);
    Ref& setTitle(std::string_view title, std::string_view subtitle = {});
    Ref& setAccent(std::uint32_t rgb);
};

class TreeBuilder {
    TreeBuilder(std::string formatName, std::stop_token);
    Ref root(std::string_view kind, SourceSpan = {});
    Ref at(RefId) const;
    std::size_t nodeCount() const;
    Result<Tree, ParseError> finish();     // renumber, flatten, finalize, hash
};
```

What `child()` does depends on where the handle stands and on whether a name
was given, and nothing else:

| Standing on | `child("name")` | `child()` |
| --- | --- | --- |
| Node | a child node of that kind | error: a node needs a kind |
| Scalar property | promotes to a record, adds a named part | promotes to a sequence, adds an item |
| Record | a named part | error |
| Sequence | a named item | an unnamed item |

Promotion keeps the scalar's value, because any form may carry one; nothing is
dropped by building on top of it. `identity()`, `style()` and
`childrenOrdered()` on the provider become plain readers of what the handle
recorded, with the kind as the fallback title and a hash of the kind as the
fallback accent, which is what every built-in derives anyway. One mechanism for
one fact rather than two.

**Build order is free.** The builder keeps nodes and properties in its own
arenas with parent links and per-parent child lists. Sibling order is the order
of `child()` and `property()` calls on that one parent handle and nothing else,
so two jobs building under different parents cannot affect each other. Handles
stay valid because nothing lives in the nested `Property::children` vectors
until `finish()`, which renumbers nodes into document order in one stack-based
pass, folds the property arena into the tree by walking it backwards, then
finalizes and hashes as now. The tree's arena invariants survive and a provider
never learns they exist. This property is load-bearing for everything below
and is tested on its own, with a deliberately shuffled build.

### The work queue

The provider controls the walk completely. There is no way to stop one from
recursing if it insists, and a Lua function that calls itself gets whatever
depth Lua's own stack affords. What the tool provides is a way to queue a call
instead of making it, and a drain that runs the queue until it is empty,
checking the stop token between jobs.

```cpp
using Job = std::function<void(ShapeContext&)>;

class ShapeContext {
    const Dom& dom() const;
    TreeBuilder& out();

    void later(Job);          // append: breadth-first drain
    void next(Job);           // prepend: depth-first drain
    std::size_t pending() const;
    bool cancelled() const;   // true once the token is signalled; the drain stops
};
```

The queue belongs to the context, not the builder. A visitor adapter never uses
it and the builder never knows it exists.

`next` is the documented idiom and the one the sample scripts use. Because the
build order is free the two drains produce identical trees, and the difference
is the peak size of the queue: depth-first holds about one root-to-leaf path of
pending jobs, breadth-first holds a whole level. A pending job costs a closure
and two handles, only while pending, where a DOM element costs its arena record
plus an attribute record each plus the strings, for the whole pass. The
pathological case of one parent with a million children makes the two drains
equal, and there the DOM already lost, so the queue is not the thing to
optimise.

Cancellation lands between jobs, which replaces the every-thousand-elements
check the shaper does today. A script that queues one job for a whole document
still needs the instruction-count hook, which stays.

### Properties: forms, values and repeated names

This is the part that reaches beyond the provider interface, and it lands
first because the corpus has to absorb it before anything else moves.

**Three forms, not a flag.** The `ordered` bool says how parts compare and says
nothing when there are none, so today `[]`, `{}` as a property and `""` are
one `Property`. Worse, `hashProperty` ignores `ordered` on a property with no
parts while `propertiesDiffer` checks it, so an array turning into a record
hashes identically and then reports as modified.

```cpp
struct Property {
    std::string name;
    std::string value;                   // allowed in every form
    PropertyForm form = PropertyForm::Scalar;
    std::vector<Property> children;      // empty for Scalar
    SourceSpan span;

    bool hasParts() const noexcept;
    const Property* find(std::string_view) const noexcept;   // the first
    PartRange findAll(std::string_view) const noexcept;      // all, in order
};
```

The form is folded into the hash before the parts, so the three empties stop
colliding. A record with a value and no parts therefore hashes differently
from a scalar with the same value, which is the same rule that separates `[]`
from `""` and only bites when one provider writes one thing two ways, which is
that provider's bug rather than a false change.

**Any form may carry a value.** A record holding `speed="1.0"` and a `range`
part is what `<property name="speed" value="1.0"><range min="0"/></property>`
means, and today the shaper has to choose one half. Hashing and comparison
already fold name, then value, then parts, so this costs them nothing. It costs
the details panel: `drawProperty` in `app_window.cpp` treats parts as meaning
no value, and `drawPropertyParts` writes a header of `name [3]` or `name {}`
with nowhere for a value or a before-and-after arrow. The header renders the
value the way the scalar path does, then the parts underneath.

**Repeated names, at every level.** A node may hold two properties named `tag`
and a record two parts named `tag`; a sequence property holding two items is
structurally distinct from that, and so is a node holding two `item` children.
Hashing and the matcher's fingerprints already treat a property list as a
multiset of hashes and need nothing. `changedPropertyNames()` in `diff.cpp` is
F15 exactly: it maps name to one property and keeps the first, so the fix
groups both sides by name and compares each group as a multiset of property
hashes, reporting the name once when the groups differ. `propertiesDiffer`
has the same bug one level down in its record branch. So does `propertyDiffers`
in `app_window.cpp`, which is a copy of the former; it becomes one function
exported from core so the panel cannot disagree with the change list, which
is the promise its own comment makes. `findProperty` stays as "the first one
named this", since that is what every caller of it wants.

The change list stays keyed by name, so it says `tag` changed rather than
which of three did. Saying which means a changed property carrying an index,
and that is a details-panel question left for later.

### What generic JSON builds, and what YAML would

Section 6's rule stands: an array becomes a sequence property when every
element is a scalar or an array that is itself a sequence property, and a node
otherwise. What changes is where it lives. `read()` for JSON produces the
document as written, every array a node with `#type` of array and one `item`
child per element, and generic JSON's own `shape()` applies the rule. A
scripted provider with `base = "json"` replaces that `shape()`, sees the raw
form, and folds per key or not at all, which is the control a studio needs
when one array is a tag list and another a list of entities. The lookahead
stops being a walk: a node's descendants are a contiguous run of the arena, so
"contains no object" is a linear scan of that run.

The root array stops being an exception. At the raw level it is a node like any
other, and generic JSON's `shape()` gives the root one `#value` property holding
the sequence, so a root array and a nested one diff identically. That moves the
golden cases for a root array and is the one corpus change this rule brings.

An empty array becomes reachable as a difference for the first time:
`"tags": []` is an empty sequence, distinct from `"tags": ""`.

**YAML would reuse all of it.** The raw DOM for YAML is the same shape as for
JSON, so a script written against `base = "json"` shapes a YAML file
unchanged and the scalar-sequence rule is one shared `shape()` rather than two.
Four YAML features have no JSON counterpart and each is a `read()` decision:
anchors and aliases stay as written, the alias a scalar `*name` and the anchor
a `#anchor` property, because expanding them normalises and gives two nodes one
span; a multi-document stream is a `$` root with one `document` child per
section; tags are a `#tag` property; a complex key serialises to its source
text. Duplicate mapping keys survive because the model now allows them. The
parser has to be non-recursive and report byte offsets, which points at
rapidyaml rather than yaml-cpp, but that is a dependency choice and not a model
one.

### What goes missing, and saying so

Two things can now remove content from the tree that could not before, and
both are shown rather than refused.

**Dropped content.** A provider may leave an element out. The tool records
which DOM elements no handle was ever made from, which is why `child()` and
`property()` take a DOM element or property directly: the link is made where
the handle is, so it cannot be forgotten, and the shorter call is the common
case. There is no `drop()`: a computed complement answers the reader's
question, what in this file the format does not show, whether the omission was
deliberate or not, and intent only matters to the script author.

Granularity is the element, not the attribute. A script that lifts `type` into
the kind and `name` into the title has used both without making a property of
either, and an attribute-level highlight would flag them. An attribute is
dropped when its element is. An XML node span covers the whole element, so a
dropped wrapper whose inner nodes were kept must have every represented
descendant's span subtracted, which `finish()` does with a sort and a sweep and
hands the text view a sorted, disjoint list.

**Failed jobs.** An error inside a job does not fail the parse. The drain
catches it at the job boundary; whatever the job built stays, whatever it never
queued is simply unrepresented, and the failure is recorded against a span. The
job's arguments supply that span: the first DOM element or property among them
gives the location and the first handle gives the node the missing content
would have hung from, so the idiom `out:next(visit, element, parent)` yields
both for free. A failure inside a function a job called directly fails that job,
so a script that recurses fails at the granularity it recursed from. The
`cancelled` error is the exception and aborts the drain as now. If `shape` and
the drain end with no root there is nothing to show, and the parse fails with
the first recorded message.

```cpp
struct ShapeFailure {
    SourceSpan span;         // empty when the job named no element
    DomId element;           // kInvalidDom when none
    RefId owner;             // the handle the job was building under, if any
    std::string message;     // the Lua error, with line where Lua knows it
};

const std::vector<SourceSpan>& Tree::unrepresented() const;
const std::vector<ShapeFailure>& Tree::failures() const;
```

Collection is always on; a bit per DOM element and one sweep at `finish()` is
small next to the DOM. The choice is the reader's: the text view marks dropped
and failed spans under two toggles and two colours, since "this format ignores
comments" and "this script broke here" are different news. The node view puts a
marker on a failure's owner card, because a change reported under that node may
be an artefact of the failure. A headless report lists each failure with line
and message and prints both counts, and fails the run on the failed count only.
Dropping is a format's decision; failing is a bug. A script error at
configuration time still stops the run with exit code 2, since that is a broken
script rather than a broken element.

### The Lua surface

The five shaping functions become one `shape` function handed the document and
the builder. The behaviour-tree provider, rewritten:

```lua
provider "bt" {
  display_name = "Behavior tree",
  base = "xml",
  extensions = { ".bt" },
  property_order = { "id", "type", "name" },

  shape = function(doc, out)
    local function visit(element, parent)
      if element.name == "node" then
        local node = parent:child(element)
        node:set_name(element.attr.type or element.name)
        node:set_identity(element.attr.id, "strong")
        node:set_title(element.attr.type, element.attr.name)
        for child in element:children() do
          out:next(visit, child, node)
        end
      else
        local prop = parent:property(element.attr.name or element.name,
                                     element.attr.value)
        for child in element:children() do
          out:next(visit, child, prop)
        end
      end
    end

    visit(doc.root, out:root("behaviortree"))
  end,
}
```

Replacing `out:next` with `visit` is a valid script that recurses; replacing it
with `out:later` walks breadth-first. All three build the same tree.

```
doc.root, doc.size, doc.base_format, doc:at(id)

element.name, element.span, element.text, element.id
element.attr[name]              -- value of the first with that name, or nil
element:property(name)          -- handle to the first, or nil
element:properties(name)        -- iterator over every one, in document order
element:properties()            -- iterator over all, repeats included
element.parent, element.first_child, element.last_child,
element.next_sibling, element.prev_sibling, element.child_count
element:child_at(i), element:children()

prop.name, prop.value, prop.form, prop.span, prop.part_count
prop:part_at(i), prop:parts()

out:root(kind), out:at(id), out.node_count, out.pending, out.cancelled
out:later(fn, ...), out:next(fn, ...)

ref:child(name), ref:child(element)
ref:property(name, value), ref:property(prop)
ref:record(name), ref:sequence(name), ref:item(value)
ref.kind, ref.form, ref.is_node, ref.parent, ref.owner, ref.id
ref:set_name(s), ref:set_value(s), ref:set_children_ordered(b)
ref:set_identity(value, "strong"), ref:set_title(title, subtitle), ref:set_accent(rgb)
```

`element.attr` is kept as a first-wins shortcut because every script in the
corpus reads attributes that never repeat, and it is honest for records with a
value; the accessors beside it are for everything else. There is no `set_span`
and no span type in Lua: a span arrives only with the element or property a
handle was made from, and a synthetic node has none, which docs/PROVIDERS.md
already says is the right answer for a span that cannot be computed honestly.

A queued job pins its arguments for the length of the drain. That is the cost
the section on the queue already accounts for, and it is why `next` is the
idiom.

### Order of work

Each step leaves a green build with the golden corpus passing, and the corpus
moves are named where they happen.

1. `PropertyForm`, values in every form, repeated names, the shared comparison,
   and the form in the hash. Corpus moves where `[]` stops colliding with `""`.
   Closes F15.
2. `TreeBuilder` with any-order construction and `finish()`, tested alone with a
   shuffled build and a deep synthetic tree.
3. `Dom` as a façade over `Tree`, `ShapeContext`, and the drain.
4. The three built-in parsers as non-recursive `read()` plus `shape()`, the
   behaviour tree as a `shape()` over XML, and the JSON root array through
   `#value`. Corpus moves for root arrays. Closes the parse half of F16; the
   matching and layout half is its own work and stays under F16.
5. The Lua surface, the sample script rewritten, and the test that it matches
   the compiled behaviour tree over one DOM. Closes F3 and F21.
6. Dropped and failed spans, the two text-view toggles, the node-view marker,
   and the report counts with the exit rule.
7. docs/PROVIDERS.md and the scripting section of USAGE.md rewritten,
   `kProviderInterfaceVersion` to 2.

### What landed, and where it differs from the above

The seven steps landed in seven commits on `feature/dom-format-provider`,
each with the corpus passing. Four things came out differently from the
design as written, and each is worth knowing before reading the code against
this section.

**`read()` returns a `Tree`, not a `Dom`.** A `Dom` is a view over a tree and
owns nothing, so the thing `read()` hands back has to be the tree itself. The
default `parse()` holds that tree for the length of the pass and builds the
view over it. Nothing else about the split changed.

**`next()` batches.** Prepending one job at a time reversed every sibling list,
because a job that queues one call per child put the last child first. What
one job queues with `next()` now goes to the front as a block, in queued
order, once that job finishes. The claim that the two drains build identical
trees holds because of this, not despite it.

**No span in Lua at all.** `element.span` was listed and is not there; a span
travels only with the element or property a handle is made from. Two
overloads arrived to make that sufficient: `out:root(element)` and
`ref:property(element)`, each setting the name, the span and the source. The
compiled behaviour-tree provider and the script build the same tree span for
span, which is the test that says the surface is complete.

**Generic XML skips its shape.** Its shape is the identity, and copying a
million-node tree to change nothing was the wrong default for the most common
file the tool opens, so it returns `read()` from `parse()`. The default
`parse()` is what every other provider uses. `style()` stayed non-virtual as
decided, but with a declarative `subtitleProperties()` beside it so that a
format which copies a document pays nothing per node for its cards.

Two smaller ones. `Property` gained a destructor that flattens before
destroying, because the compiler's destroyed a nested property one call frame
per level and the deep test found it. And a Lua failure keeps the first line
of its message rather than sol2's traceback, since the message lands in every
report line and every mark.

The corpus did not move. No golden case held a root array of scalars or an
empty array, so the two moves this section predicted were never exercised;
the unit tests cover both instead. Step 4 closed the parse half of F16; the
matching and layout passes named there still recurse and stay under F16.
