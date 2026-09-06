NM Tree Diff — Implementation Plan
==================================

Derived from [REQUIREMENTS.md](REQUIREMENTS.md), whose numbered requirements
this document cites as R0 to R12, and written to
[CODE_GUIDELINES.md](CODE_GUIDELINES.md). Target: a C++20 Dear ImGui
tool that diffs tree-shaped data in XML and JSON, offers both a text view
and a node view of the same diff, starts instantly, never blocks its own frame
loop, and runs from the command line as a Perforce diff tool. It is aimed at
becoming the default way game studios diff asset files.

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

**Whether generic JSON should change is a decision inside M9.** A scalar array
is currently a node holding one node per element, which is what makes the node
count in the budget benchmark what it is. R7.10 makes another reading available:
one array property holding its values. The second is better for a reader, since
`tags` changing reads as one property rather than as a subtree of anonymous
`item` nodes, and it would cut the node count on array-heavy files. It is also a
change to what every existing JSON golden case produces, so it needs deciding
deliberately rather than falling out of the implementation. The provider table
above records today's behaviour, not the answer.

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
| Generic JSON | Every object, every array, and every array element | Scalar members of an object, plus `#value` for a scalar array element and `#type` on every container | Arrays ordered, object members unordered | Member key inside its parent object; weak. Array elements have none. |
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
| Parse and match, 100k nodes, JSON | under 2 s | 113 ms and 80 ms (M5) | M5 budget test |

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
the other. A list of recent pairs belongs on that pane and is left to M11, which
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
feature. M11 should decide whether to add it while it is writing those
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
| M9 | Properties with parts | Nested properties in the data model, hashing, matching and both views; record and sequence parts, so an array property reorders as a change and a record does not; the rule that anything not a node becomes a property; a way for a format to take both an element's attributes and its child elements as properties; the scripted surface and the golden corpus updated to match | A format can represent a transform, a colour or a list of tags as one property, reordering a list registers while reordering a record does not, and no provider can drop an element it does not recognise |
| M10 | Keys | Every action named, every shortcut settable from a configuration script, more than one binding allowed per action, the menus showing whatever is bound | A reader rebinds next-change to two keys of their own and the menu says so |
| M11 | Ship | Headless report, exit codes, a portable archive built in continuous integration from a tag and attached to a GitHub release, MIT licence and attribution for bundled dependencies, per-extension Perforce and Git setup docs verified against real clients, possibly a Git seven-argument mode, settings persistence | A technical artist can unzip it and configure it without help |
| M12 | Later | Three-way merge, further game asset formats | Out of initial scope |

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

M9 and M10 cover the requirements added after M8. They are two milestones
because they share nothing: one is a change to the data model that every
provider and both views are built on, and the other is a table of key bindings.

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

**M10 is small but not trivial**, because R16 asks for more than one binding per
action, which means a key table rather than a setting per key. The exit key
that M8 added becomes one row in it. Doing this before shipping matters more
than its size suggests: a keyboard map is the kind of thing people build habits
around, and changing it afterwards costs more than building it now.

**R11.3 was already satisfied**, which was checked rather than assumed: one
script may declare as many providers as it likes, because each declaration is
an ordinary call and nothing about the reader is limited to one.

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
  before M12, not during it.
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
  means a release is a git tag rather than a build someone ran, so M11 builds the
  archive in continuous integration from the tag and attaches it; it means the
  setup documents can name a download URL that does not change; and it means the
  attribution for the bundled dependencies ships in the archive rather than
  living on a page somewhere. There is no installer and no auto-update: a studio
  unzips a directory, which is also what makes it easy to keep two versions
  side by side.
