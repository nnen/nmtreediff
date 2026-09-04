NM Tree Diff — Implementation Plan
==================================

Derived from [REQUIREMENTS.md](REQUIREMENTS.md). Target: a C++20 Dear ImGui
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
by C++ classes now and by a Lua bridge later. The engine never learns that a
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
  documented from the first release. The later Lua bridge is what lets a
  technical artist add a format without a compiler, which means the C++
  interface must stay narrow enough to mirror in script.
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
| XML parser | pugixml | Small, fast, preserves document order, and exposes byte offsets, which the node view needs to link back to the text view. |
| JSON parser | simdjson, on-demand API | The deciding factor is byte offsets, not speed: node spans are what link the two views, and simdjson exposes a source location for every value. nlohmann/json does not, which rules it out despite being the obvious default. See the note on comment-bearing JSON in section 6. |
| Argument parsing | CLI11 | Plain named options are enough, because Perforce lets the user define the argument order for a custom diff tool. No tolerance hacks needed. |
| Licence | MIT | Permissive enough to clear a studio legal review without a conversation, which is a precondition for the adoption this tool is aiming at. It also lets a studio vendor the core library into an internal tool. |
| Distribution | Portable archive, no installer | Adoption inside a studio depends on someone being able to unzip it and edit a Perforce setting. |
| Tests | Catch2 v3 | Golden-file friendly. |

4. Pipeline and module layout
-----------------------------

The core library links against no GUI target and knows nothing about threads
beyond its own job queue. That is what makes the diff engine testable in
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
      provider.h           IFormatProvider and its value types
      registry.h/.cpp      registration, sniffing, explicit override
      hash.h/.cpp          content hashing of subtrees
      match.h/.cpp         identity, bottom-up, top-down passes
      diff.h/.cpp          Matching to DiffModel
      textdiff.h/.cpp      Myers line diff, intra-line word diff
    formats/
      xml_generic.h/.cpp   default XML provider (M2)
      json_generic.h/.cpp  default JSON provider (M5)
      bt_xml.h/.cpp        sample behavior-tree provider (M6)
    ui/
      app_window.h/.cpp    docking layout, menu, keyboard map
      text_view.h/.cpp
      node_view.h/.cpp     canvas, interaction, drawing
      layout_tree.h/.cpp   tidy-tree positioning
      details_panel.h/.cpp property table for the selection
      status_bar.h/.cpp    progress, cancellation, degraded-mode notice
      theme.h/.cpp         status colours, light and dark
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

**Property order is presentation, never matching.** Matching compares
properties as an unordered set, so a reordered attribute list never registers
as a change. The provider's ranking decides what the node card, the details
panel, and the change list show first, which also makes the change list
deterministic across runs.

**Serialization is declared now and left unimplemented.** It is the hook a
merge will need in order to write a merged tree back out, and declaring it
early forces every provider to be written with round-tripping in mind.

The registry resolves a provider by an explicit format option first, then by
the highest sniffing score, then falls back to the generic XML provider.
Registration is a static initializer per format, so adding a format is one
translation unit and no edits anywhere else. Since this interface is the
studio-facing surface, it carries a version number from the first release and
changes to it are additive.

### The two built-in providers

XML ships first and JSON second. Both are generic providers with no knowledge
of any particular schema, and between them they are what proves the interface
is not XML-shaped.

| | What is a node | What is a property | Child order | Default identity |
| --- | --- | --- | --- | --- |
| Generic XML | Every element | Every attribute, plus `#text` for a leaf element's text | Ordered | Element name and sibling index; weak |
| Generic JSON | Every object, every array, and every array element | Scalar members of an object, plus `#value` for a scalar array element | Arrays ordered, object members unordered | Member key inside its parent object; weak. Array elements have none. |

**JSON is where the ordering hook earns its place.** Reordering the members of
an object changes nothing about the document, while reordering an array does.
The generic provider reports the first as unordered and the second as ordered,
and the matcher reports moves only where a move is real. An XML-only design
would never have surfaced that distinction.

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
| Window visible, cold start | under 200 ms | 207-211 ms (M0, over) | `--max-frames` timing run |
| Frame time, any state | under 16 ms | 1.8-2.1 ms (M0) | `--max-frames` timing run |
| Text view usable, 20 MB pair | under 800 ms | not yet | M1 performance test |
| Full match, 100k nodes | under 2 s | not yet | M3 performance test |
| Cancellation acknowledged | under 50 ms | not yet | M3 cancellation test |

These numbers are first estimates to design against and to measure early, not
measurements. The point of writing them down now is that missing one is a
visible failure rather than a vague sense that the tool feels slow.

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

### Node view

- **Drawn through the ImGui draw list,** not with widgets, so panning, zooming,
  and culling stay under our control.
- **Tidy-tree layout,** Reingold-Tilford with Walker's linear-time refinement,
  computed on a worker once per snapshot and cached. Top-down or left-to-right,
  switchable, because deep behavior trees read better left to right.
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
  --config <file>
  --headless                  no window
  --report text|json
  --exit-code                 0 identical, 1 different
```

Because Perforce lets a user define the argument order for a custom diff tool,
the parser needs no tolerance for unusual argument shapes. Plain named options
and two positional paths are enough, and the setup document supplies the exact
string to paste into the client. Git and Subversion get the same treatment,
documented rather than special-cased.

Headless mode with a JSON report and an exit code runs the whole pipeline with
no window. That serves scripting and continuous integration checks on asset
submissions, and it is also how the end-to-end tests run.

11. Milestones
--------------

| # | Milestone | Contents | Done when |
| --- | --- | --- | --- |
| M0 &check; | Skeleton and job system | CMake with pinned FetchContent, ImGui window with docking, worker pool with stop tokens, snapshot publishing, argument parsing, headless reporting | Done, except that startup measures 207-211 ms against the 200 ms target; file loading is off the frame loop and the worst frame is 2.1 ms |
| M1 | Text diff | SourceFile, Myers line diff, word highlighting, synchronised scrolling, gutter and overview, staged publishing with progress | A 20 MB pair is readable inside the budget with the frame loop never stalling |
| M2 | Model and generic XML | Tree arena, spans, provider interface, property ranking, registry, generic XML provider, subtree hashing | A parsed tree round-trips its spans and hashes deterministically |
| M3 | Diff engine | The four passes, DiffModel, size guard with visible degraded mode, cancellation, golden-file tests, performance tests | Golden tests pass and the hundred-thousand-node case meets its budget |
| M4 | Node view | Canvas, tidy-tree layout on a worker, node cards, status colouring, collapsing, view switching, shared selection | Both views show the same snapshot and cross-select |
| M5 | JSON | Generic JSON provider on simdjson, spans from source locations, ordered arrays and unordered object members, sniffing between the two built-ins | A JSON pair diffs correctly and no interface change was needed to get there |
| M6 | Custom formats | Sample behavior-tree provider, format override, provider config, versioned provider documentation | The behavior-tree case matches by identifier across a move, and someone outside the project can write a provider from the docs |
| M7 | Ship | Headless report, exit codes, portable archive, MIT licence and attribution for bundled dependencies, one-page Perforce and Git setup docs verified against real clients, settings persistence | A technical artist can unzip it and configure it without help |
| M8 | Later | Lua provider bridge, three-way merge, further game asset formats | Out of initial scope |

M0 through M4 are the critical path. JSON sits at M5, deliberately ahead of the
milestone that publishes the provider interface as a documented surface: it is
the cheapest way to find out whether that interface accidentally assumes XML,
and finding out afterwards would mean breaking a published contract.

12. Testing
-----------

- **Golden-file diff tests.** Each case is a directory holding a left file, a
  right file, an optional format name, and an expected serialised change list,
  with parallel cases in both built-in formats. This is the main defence against matching regressions, and it makes a change
  in matching quality reviewable as a diff of expected output.
- **Unit tests** for hash stability, span correctness, property ranking,
  registry resolution, and the Myers implementation against known cases.
- **Concurrency tests** that cancel a job mid-pass and assert the pipeline
  settles, and that run the whole thing under the thread and address sanitizers
  in continuous integration.
- **Budget tests** for startup time, staged publishing latency, full match
  time, and cancellation latency, so the numbers in section 8 cannot silently
  rot.
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
  the tree must retain more source detail than it does now. Decide before M8,
  not during it.
- **Risk: JSON spans depend on the parser.** Node spans are what link the two
  views, and most JSON libraries do not expose byte offsets per value. The
  choice of simdjson turns on that one capability, so a change of parser later
  is not a swap but a rewrite of the provider.
- **Open: which real asset format to validate against.** Generic XML and JSON
  cover the shape of the problem, but not a real studio pipeline. Picking one
  concrete format early gives the performance work a realistic corpus instead
  of synthetic trees, and decides who can try the tool on day one.
- **Open: release channel.** The licence is settled; how builds reach studios
  is not. A tagged archive on a public repository is the cheap answer, and it
  is worth deciding before the first person asks where to get it.
