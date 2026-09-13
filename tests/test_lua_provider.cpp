#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/diff.h"
#include "core/log.h"
#include "core/lua_config.h"
#include "core/lua_provider.h"
#include "core/registry.h"
#include "core/source.h"
#include "formats/xml_generic.h"

// The bridge is judged against the compiled behaviour-tree provider rather than
// against a plausible answer. The two read the same files and must produce the
// same trees, the same matches and the same change list. If they disagree,
// either the bridge is losing something or the scripted surface cannot say
// something the compiled provider can, and the difference names which case.

namespace fs = std::filesystem;

using nmxd::ConfigProblem;
using nmxd::GraphDirection;
using nmxd::IFormatProvider;
using nmxd::ProviderConfig;
using nmxd::SourceFile;
using nmxd::Tree;

namespace {

fs::path samplePath(const char* name) { return fs::path(NMXD_TESTDATA_DIR) / "sample" / name; }

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// Builds a registry with the sample script's provider in it.
nmxd::ProviderRegistry registryWithScript(const std::string& script) {
    ProviderConfig config;
    std::vector<ConfigProblem> problems;
    REQUIRE(nmxd::runConfigScript(script, "test.lua", config, problems));

    auto registry = nmxd::makeDefaultRegistry();
    REQUIRE(nmxd::addScriptedProviders(registry, config).empty());
    return registry;
}

Tree parseWith(const nmxd::ProviderRegistry& registry, const char* format, const char* sample) {
    const auto* provider = registry.byName(format);
    REQUIRE(provider != nullptr);

    const auto source = SourceFile::load(samplePath(sample), sample);
    REQUIRE(source.ok());
    auto tree = provider->parse(source.value(), {});
    REQUIRE(tree.ok());
    return std::move(tree).value();
}

}  // namespace

TEST_CASE("a scripted provider builds the same tree as the compiled one", "[lua]") {
    const auto registry = registryWithScript(readFile(samplePath("behaviortree.lua")));

    const Tree compiled = parseWith(registry, "bt", "guard_before.bt");
    const Tree scripted = parseWith(registry, "bt-lua", "guard_before.bt");

    REQUIRE(compiled.size() == scripted.size());
    for (nmxd::NodeId id = 0; id < compiled.size(); ++id) {
        INFO("node " << id);
        const auto& left = compiled.node(id);
        const auto& right = scripted.node(id);
        CHECK(left.kind == right.kind);
        CHECK(left.children.size() == right.children.size());
        CHECK(left.span.begin == right.span.begin);
        CHECK(left.span.end == right.span.end);

        REQUIRE(left.properties.size() == right.properties.size());
        for (std::size_t i = 0; i < left.properties.size(); ++i) {
            CHECK(left.properties[i].name == right.properties[i].name);
            CHECK(left.properties[i].value == right.properties[i].value);
        }
    }
}

TEST_CASE("a scripted provider reports the same changes", "[lua]") {
    // The whole pipeline, not just the parse: identity, matching and the change
    // list all have to agree, which is what makes this worth more than a tree
    // comparison on its own.
    const auto registry = registryWithScript(readFile(samplePath("behaviortree.lua")));

    const Tree compiledLeft = parseWith(registry, "bt", "guard_before.bt");
    const Tree compiledRight = parseWith(registry, "bt", "guard_after.bt");
    const Tree scriptedLeft = parseWith(registry, "bt-lua", "guard_before.bt");
    const Tree scriptedRight = parseWith(registry, "bt-lua", "guard_after.bt");

    const auto* compiled = registry.byName("bt");
    const auto* scripted = registry.byName("bt-lua");
    REQUIRE(compiled != nullptr);
    REQUIRE(scripted != nullptr);

    const auto compiledDiff = nmxd::diffTrees(compiledLeft, compiledRight, *compiled);
    const auto scriptedDiff = nmxd::diffTrees(scriptedLeft, scriptedRight, *scripted);

    CHECK(compiledDiff.added == scriptedDiff.added);
    CHECK(compiledDiff.deleted == scriptedDiff.deleted);
    CHECK(compiledDiff.modified == scriptedDiff.modified);
    CHECK(compiledDiff.moved == scriptedDiff.moved);

    CHECK(nmxd::serializeChanges(compiledLeft, compiledRight, compiledDiff) ==
          nmxd::serializeChanges(scriptedLeft, scriptedRight, scriptedDiff));
}

TEST_CASE("a scripted identity survives a change of kind", "[lua]") {
    // The thing no structural heuristic can do, and the reason the surface has
    // a word for a strong key rather than inferring one.
    const auto registry = registryWithScript(readFile(samplePath("behaviortree.lua")));
    const auto* provider = registry.byName("bt-lua");
    REQUIRE(provider != nullptr);

    const Tree left = parseWith(registry, "bt-lua", "guard_before.bt");
    const Tree right = parseWith(registry, "bt-lua", "guard_after.bt");
    const auto model = nmxd::diffTrees(left, right, *provider);

    // "Stand easy" is an Idle before and a LookAround after, and it keeps its
    // identifier. It has to survive as one modified node rather than becoming a
    // deletion beside an addition.
    const std::string changes = nmxd::serializeChanges(left, right, model);
    CHECK(changes.find("LookAround[0] [type") != std::string::npos);
    CHECK(changes.find("- /behaviortree/Selector[0]/Idle") == std::string::npos);
}

TEST_CASE("a scripted provider states its own graph direction", "[lua]") {
    const auto registry = registryWithScript(readFile(samplePath("behaviortree.lua")));
    const auto* provider = registry.byName("bt-lua");
    REQUIRE(provider != nullptr);
    CHECK(provider->graphDirection() == GraphDirection::LeftToRight);
    CHECK(provider->displayName() == "Behavior tree (script)");
}

TEST_CASE("a script may claim extensions and win them", "[lua]") {
    // Registered ahead of the built-ins, so a format that names an extension
    // gets it rather than losing a tie to the format it is built on.
    const auto registry = registryWithScript(
        "provider 'mine' {\n"
        "  base = 'xml',\n"
        "  extensions = { '.MINE' },\n"
        "}\n");

    const auto source = SourceFile::fromMemory("<r/>", "thing.mine", "thing.mine");
    REQUIRE(registry.resolve(source) != nullptr);
    CHECK(registry.resolve(source)->name() == "mine");
}

TEST_CASE("a provider built on a format that does not exist is reported", "[lua]") {
    ProviderConfig config;
    std::vector<ConfigProblem> problems;
    REQUIRE(nmxd::runConfigScript("provider 'x' { base = 'yaml' }\n", "test.lua", config,
                                  problems));

    auto registry = nmxd::makeDefaultRegistry();
    const auto unknown = nmxd::addScriptedProviders(registry, config);
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0] == "yaml");
    CHECK(registry.byName("x") == nullptr);
}

TEST_CASE("a script that keeps every element still parses", "[lua]") {
    // The smallest possible provider: one that changes nothing. It should read
    // exactly like the format it is built on, which is what says the bridge
    // adds nothing of its own.
    const auto registry = registryWithScript("provider 'passthrough' { base = 'xml' }\n");

    const auto source = SourceFile::fromMemory("<r><a x='1'/><b/></r>", "t.xml", "t.xml");
    const auto* scripted = registry.byName("passthrough");
    const auto* plain = registry.byName("xml");
    REQUIRE(scripted != nullptr);
    REQUIRE(plain != nullptr);

    auto shaped = scripted->parse(source, {});
    auto generic = plain->parse(source, {});
    REQUIRE(shaped.ok());
    REQUIRE(generic.ok());
    CHECK(shaped.value().size() == generic.value().size());
}

TEST_CASE("a script finds children by name, first and every one", "[lua]") {
    // The record style the bt2 sample uses: a node's id and type are child
    // elements rather than attributes, and its children sit under a wrapper
    // whose repeated <child> elements are the ones worth a node each.
    const auto registry = registryWithScript(
        "provider 'record' {\n"
        "  base = 'xml',\n"
        "  shape = function(doc, out)\n"
        "    local function visit(element, owner)\n"
        "      local node = owner:child(element)\n"
        "      local id = element:child('id')\n"
        "      if id then node:set_identity(id.text, 'strong') end\n"
        "      node:property('type', element:child('type').text)\n"
        "      local wrapper = element:child('children')\n"
        "      if wrapper then\n"
        "        for child in wrapper:children('child') do out:next(visit, child, node) end\n"
        "      end\n"
        "      node:property('missing', tostring(element:child('nope') == nil))\n"
        "    end\n"
        "    local root = out:root(doc.root)\n"
        "    for child in doc.root:children('node') do out:next(visit, child, root) end\n"
        "  end,\n"
        "}\n");

    const auto source = SourceFile::fromMemory(
        "<r><skip/><node><id>n1</id><type>Seq</type><children>"
        "<child><id>n2</id><type>Act</type></child>"
        "<comment/>"
        "<child><id>n3</id><type>Act</type></child>"
        "</children></node><skip/></r>",
        "t.xml", "t.xml");
    const auto* provider = registry.byName("record");
    REQUIRE(provider != nullptr);

    auto parsed = provider->parse(source, {});
    REQUIRE(parsed.ok());
    const Tree& tree = parsed.value();

    // r, node, and its two <child> elements; <skip/> and <comment/> are passed over.
    REQUIRE(tree.size() == 4);
    const auto& top = tree.node(tree.root());
    REQUIRE(top.children.size() == 1);
    const auto& seq = tree.node(top.children[0]);
    CHECK(seq.findProperty("type")->value == "Seq");
    CHECK(seq.findProperty("missing")->value == "true");
    CHECK(provider->identity(tree, seq.id).value == "n1");
    REQUIRE(seq.children.size() == 2);
    CHECK(provider->identity(tree, seq.children[0]).value == "n2");
    CHECK(provider->identity(tree, seq.children[1]).value == "n3");
}

TEST_CASE("a script that will not stop is cancelled", "[lua][slow]") {
    // Constraint C makes no exception for code the user wrote. A script with a
    // loop in it would otherwise hold a worker for ever, and switching format
    // or reloading a file has to be able to take that worker back.
    const auto registry = registryWithScript(
        "provider 'spinner' {\n"
        "  base = 'xml',\n"
        "  shape = function(doc, out)\n"
        "    out:root(doc.root)\n"
        "    while true do end\n"
        "  end,\n"
        "}\n");

    const auto* provider = registry.byName("spinner");
    REQUIRE(provider != nullptr);

    const auto source = SourceFile::fromMemory("<r><a/></r>", "t.xml", "t.xml");

    std::stop_source stopping;
    std::thread canceller([&stopping] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stopping.request_stop();
    });

    const auto started = std::chrono::steady_clock::now();
    const auto parsed = provider->parse(source, stopping.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    canceller.join();

    // It gave up rather than running to the end, and it did so promptly.
    CHECK_FALSE(parsed.ok());
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 2000);
}

namespace {

/// A script that copies an XML document one to one, with the walk written
/// three ways: queued depth-first, queued breadth-first, or plain recursion.
std::string copyingScript(const char* how) {
    std::string script =
        "provider 'copy' {\n"
        "  base = 'xml',\n"
        "  shape = function(doc, out)\n"
        "    local function visit(element, parent)\n"
        "      local node = parent:child(element)\n"
        "      for attribute in element:properties() do node:property(attribute) end\n"
        "      for child in element:children() do\n";
    script += how;
    script +=
        "      end\n"
        "    end\n"
        "    local root = out:root(doc.root)\n"
        "    for child in doc.root:children() do out:next(visit, child, root) end\n"
        "  end,\n"
        "}\n";
    return script;
}

std::vector<std::string> kindsOf(const Tree& tree) {
    std::vector<std::string> kinds;
    for (const auto& node : tree.nodes()) {
        kinds.push_back(node.kind);
    }
    return kinds;
}

}  // namespace

TEST_CASE("a script builds the same tree queued, breadth-first or recursing", "[lua][queue]") {
    // The script owns the walk. Handing a call to out:next, to out:later, or
    // making it directly are three spellings of one walk, and the builder
    // does not care which, because sibling order is call order on one parent.
    const auto source = SourceFile::fromMemory(
        "<r><a x='1'><a1/><a2 y='2'/></a><b/><c><c1><c11/></c1></c></r>", "t.xml", "t.xml");
    const auto plain = nmxd::makeGenericXmlProvider()->parse(source, {});
    REQUIRE(plain.ok());
    const std::vector<std::string> expected = kindsOf(plain.value());

    for (const char* how : {"        out:next(visit, child, node)\n",
                            "        out:later(visit, child, node)\n",
                            "        visit(child, node)\n"}) {
        INFO(how);
        const auto registry = registryWithScript(copyingScript(how));
        const auto* provider = registry.byName("copy");
        REQUIRE(provider != nullptr);
        auto shaped = provider->parse(source, {});
        REQUIRE(shaped.ok());
        CHECK(kindsOf(shaped.value()) == expected);
        CHECK(shaped.value().node(0).contentHash == plain.value().node(0).contentHash);
        CHECK(shaped.value().unrepresented().empty());
        CHECK(shaped.value().failures().empty());
    }
}

TEST_CASE("an error in one job is recorded and the rest of the document is built",
          "[lua][failure]") {
    // A failure does not fail the parse. What the job built stays, what it
    // never queued is missing, and the tree says where and why.
    const auto registry = registryWithScript(
        "provider 'fragile' {\n"
        "  base = 'xml',\n"
        "  shape = function(doc, out)\n"
        "    local function visit(element, parent)\n"
        "      if element.name == 'bad' then error('no bad elements here') end\n"
        "      local node = parent:child(element)\n"
        "      for child in element:children() do out:next(visit, child, node) end\n"
        "    end\n"
        "    local root = out:root(doc.root)\n"
        "    for child in doc.root:children() do out:next(visit, child, root) end\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("fragile");
    REQUIRE(provider != nullptr);

    const std::string text = "<r><good/><bad><inside/></bad><after/></r>";
    nmxd::Log& log = nmxd::Log::instance();
    log.forwardTo(nullptr, nullptr);
    const std::uint64_t logBefore = log.lastSequence();
    auto shaped = provider->parse(SourceFile::fromMemory(text, "t.xml", "t.xml"), {});
    log.forwardTo(stdout, stderr);
    REQUIRE(shaped.ok());
    const Tree& tree = shaped.value();

    CHECK(kindsOf(tree) == std::vector<std::string>{"r", "good", "after"});
    REQUIRE(tree.failures().size() == 1);
    CHECK(tree.failures()[0].message.find("no bad elements here") != std::string::npos);
    CHECK(tree.failures()[0].owner == tree.root());

    // One line for the report and the card; the whole thing, traceback
    // included, as the detail and in the log, written once and naming the file.
    CHECK(tree.failures()[0].message.find('\n') == std::string::npos);
    CHECK(tree.failures()[0].detail.find("stack traceback") != std::string::npos);
    CHECK(tree.failures()[0].detail.find("no bad elements here") != std::string::npos);
    const auto logged = log.linesAfter(logBefore);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].stream == nmxd::LogStream::Err);
    CHECK(logged[0].text.find("t.xml") != std::string::npos);
    CHECK(logged[0].text.find("stack traceback") != std::string::npos);
    const auto& where = tree.failures()[0].span;
    CHECK(text.substr(where.begin, where.end - where.begin) == "<bad><inside/></bad>");

    // The bad element and everything inside it is unrepresented, as one span.
    REQUIRE(tree.unrepresented().size() == 1);
    CHECK(tree.unrepresented()[0] == where);
}

TEST_CASE("a script's print reaches the log as standard output", "[lua][log]") {
    // A window launched from the desktop has no console, so a print that went
    // to the C runtime's stdout would tell its author nothing.
    const auto registry = registryWithScript(
        "provider 'talkative' {\n"
        "  base = 'xml',\n"
        "  shape = function(doc, out)\n"
        "    print('shaping', doc.size, nil, true)\n"
        "    out:root(doc.root)\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("talkative");
    REQUIRE(provider != nullptr);

    nmxd::Log& log = nmxd::Log::instance();
    log.forwardTo(nullptr, nullptr);
    const std::uint64_t before = log.lastSequence();
    auto shaped = provider->parse(SourceFile::fromMemory("<r><a/></r>", "t.xml", "t.xml"), {});
    log.forwardTo(stdout, stderr);
    REQUIRE(shaped.ok());

    const auto logged = log.linesAfter(before);
    REQUIRE(logged.size() == 1);
    CHECK(logged[0].stream == nmxd::LogStream::Out);
    CHECK(logged[0].text == "shaping\t2\tnil\ttrue");
}

TEST_CASE("a script that leaves elements out has their bytes reported", "[lua][dropped]") {
    // Dropping is allowed now, and the price of that freedom is that the
    // tree says what was dropped so the text view can show it.
    const auto registry = registryWithScript(
        "provider 'picky' {\n"
        "  base = 'xml',\n"
        "  shape = function(doc, out)\n"
        "    local root = out:root(doc.root)\n"
        "    for child in doc.root:children() do\n"
        "      if child.name == 'keep' then root:child(child) end\n"
        "    end\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("picky");
    REQUIRE(provider != nullptr);

    const std::string text = "<r><keep/><skip a='1'/><keep/></r>";
    auto shaped = provider->parse(SourceFile::fromMemory(text, "t.xml", "t.xml"), {});
    REQUIRE(shaped.ok());
    CHECK(shaped.value().size() == 3);
    REQUIRE(shaped.value().unrepresented().size() == 1);
    const auto& gone = shaped.value().unrepresented()[0];
    CHECK(text.substr(gone.begin, gone.end - gone.begin) == "<skip a='1'/>");
}

TEST_CASE("a script shapes a document thousands of levels deep through the queue",
          "[lua][deep]") {
    // The whole reason the queue exists: the script's visit never calls
    // itself, so depth costs memory rather than the interpreter's stack.
    constexpr int kDepth = 5000;
    std::string text;
    for (int level = 0; level < kDepth; ++level) {
        text += "<e>";
    }
    for (int level = 0; level < kDepth; ++level) {
        text += "</e>";
    }

    const auto registry =
        registryWithScript(copyingScript("        out:next(visit, child, node)\n"));
    const auto* provider = registry.byName("copy");
    REQUIRE(provider != nullptr);
    auto shaped = provider->parse(SourceFile::fromMemory(text, "t.xml", "t.xml"), {});
    REQUIRE(shaped.ok());
    CHECK(shaped.value().size() == static_cast<std::size_t>(kDepth));
    CHECK(shaped.value().node(shaped.value().size() - 1).depth ==
          static_cast<std::uint32_t>(kDepth - 1));
}

TEST_CASE("a script on JSON sees every array as a node and folds what it wants", "[lua][dom]") {
    // The raw reading keeps every array a node, and a script folds per key.
    // This one folds a list of tags into a sequence and leaves the rest as
    // the reading had it, so a list of entities stays a list of nodes.
    const auto registry = registryWithScript(
        "provider 'tags' {\n"
        "  base = 'json',\n"
        "  shape = function(doc, out)\n"
        "    local function visit(element, parent)\n"
        "      if element.name == 'tags' then\n"
        "        local list = parent:sequence('tags')\n"
        "        for item in element:children() do\n"
        "          list:item(item:property('#value').value)\n"
        "        end\n"
        "        return\n"
        "      end\n"
        "      local node = parent:child(element)\n"
        "      for attribute in element:properties() do node:property(attribute) end\n"
        "      for child in element:children() do out:next(visit, child, node) end\n"
        "    end\n"
        "    local root = out:root(doc.root)\n"
        "    for attribute in doc.root:properties() do root:property(attribute) end\n"
        "    for child in doc.root:children() do out:next(visit, child, root) end\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("tags");
    REQUIRE(provider != nullptr);

    auto shaped = provider->parse(
        SourceFile::fromMemory(R"({"tags": ["a", "b"], "spawns": [{"x": 1}]})", "t.json",
                               "t.json"),
        {});
    REQUIRE(shaped.ok());
    const Tree& tree = shaped.value();
    const auto* tags = tree.node(tree.root()).findProperty("tags");
    REQUIRE(tags != nullptr);
    CHECK(tags->form == nmxd::PropertyForm::Sequence);
    REQUIRE(tags->children.size() == 2);
    CHECK(tags->children[1].value == "\"b\"");
    // The spawns array stayed a node with an item node under it.
    CHECK(tree.size() == 3);
    CHECK(tree.node(1).kind == "spawns");
    CHECK(tree.node(2).kind == "item");
}
