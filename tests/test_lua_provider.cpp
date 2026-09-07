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
#include "core/lua_config.h"
#include "core/lua_provider.h"
#include "core/registry.h"
#include "core/source.h"

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

Tree parseText(const IFormatProvider& provider, const char* text, const char* name) {
    auto parsed = provider.parse(SourceFile::fromMemory(text, name, name), {});
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
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
        CHECK(left.span == right.span);

        REQUIRE(left.properties.size() == right.properties.size());
        for (std::size_t i = 0; i < left.properties.size(); ++i) {
            CHECK(left.properties[i].name == right.properties[i].name);
            CHECK(left.properties[i].value == right.properties[i].value);
            CHECK(left.properties[i].span == right.properties[i].span);
            CHECK(left.properties[i].children.size() == right.properties[i].children.size());
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

TEST_CASE("a script reads a nested property and a wrapper like the compiled provider",
          "[lua]") {
    // The two shapes that are easiest to get subtly wrong: a property whose
    // content is elements, and a wrapper that carries attributes of its own
    // around the nodes it holds.
    const auto registry = registryWithScript(readFile(samplePath("behaviortree.lua")));
    const char* xml =
        "<behaviortree>\n"
        "  <node id=\"s1\" type=\"Sequence\">\n"
        "    <property name=\"position\">\n"
        "      <entityPosition><target name=\"enemy\" offset=\"1.5\"/></entityPosition>\n"
        "    </property>\n"
        "    <children policy=\"all\" retries=\"2\">\n"
        "      <node id=\"w1\" type=\"Wait\"><property name=\"seconds\" value=\"1\"/></node>\n"
        "    </children>\n"
        "  </node>\n"
        "</behaviortree>\n";

    const Tree compiled = parseText(*registry.byName("bt"), xml, "t.bt");
    const Tree scripted = parseText(*registry.byName("bt-lua"), xml, "t.bt");
    REQUIRE(compiled.size() == scripted.size());
    for (nmxd::NodeId id = 0; id < compiled.size(); ++id) {
        INFO("node " << id);
        CHECK(compiled.node(id).contentHash == scripted.node(id).contentHash);
    }
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

TEST_CASE("a script that says nothing reads like the format it is built on", "[lua]") {
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

TEST_CASE("a wrapper can be dropped and its children kept", "[lua]") {
    // The case the surface was designed around.
    const auto registry = registryWithScript(readFile(samplePath("nested_children.lua")));
    const auto source = SourceFile::fromMemory(
        "<node id=\"root\">\n"
        "  <children>\n"
        "    <child id=\"a\"/>\n"
        "    <child id=\"b\"><children><child id=\"c\"/></children></child>\n"
        "  </children>\n"
        "  <editor><layout x=\"1\"/></editor>\n"
        "</node>\n",
        "t.nested", "t.nested");

    const auto* provider = registry.resolve(source);
    REQUIRE(provider != nullptr);
    CHECK(provider->name() == "nested");

    auto parsed = provider->parse(source, {});
    REQUIRE(parsed.ok());
    const Tree& tree = parsed.value();

    REQUIRE(tree.size() == 4);
    const auto& root = tree.node(tree.root());
    REQUIRE(root.children.size() == 2);
    CHECK(root.findProperty("children") == nullptr);

    // The editor block is one opaque property holding its raw text.
    REQUIRE(root.findProperty("editor") != nullptr);
    CHECK(root.findProperty("editor")->value == "<editor><layout x=\"1\"/></editor>");

    // Enter left the owner's id, and exit keyed the child by it.
    const auto& b = tree.node(root.children[1]);
    CHECK(provider->identity(tree, b.id).value == "root/b");
    CHECK(provider->identity(tree, b.id).strong);
    CHECK(provider->identity(tree, b.children[0]).value == "b/c");
    CHECK(provider->style(tree, b.id).title == "b");
}

TEST_CASE("a script can say a node's children are unordered", "[lua]") {
    // Sibling order that carries nothing, so a re-sorted table is not a move
    // per row.
    const auto registry = registryWithScript(
        "provider 'table' {\n"
        "  base = 'xml',\n"
        "  exit = function(el, out)\n"
        "    out:node(el.name, el):attributes(el):ordered(el.name ~= 'rows'):adopt(el.items)\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("table");
    REQUIRE(provider != nullptr);

    const Tree left = parseText(*provider, "<rows><r k='1'/><r k='2'/></rows>", "l.xml");
    const Tree right = parseText(*provider, "<rows><r k='2'/><r k='1'/></rows>", "r.xml");

    CHECK_FALSE(provider->childrenOrdered(left, left.root()));
    CHECK(provider->childrenOrdered(left, left.node(left.root()).children[0]));
    CHECK(left.node(left.root()).contentHash == right.node(right.root()).contentHash);

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.moved == 0);
    CHECK(model.modified == 0);
}

TEST_CASE("an element the script does not mention is kept", "[lua]") {
    // The exit below has no else branch. The unmentioned element must still
    // be in the tree, with everything it carried.
    const auto registry = registryWithScript(
        "provider 'partial' {\n"
        "  base = 'xml',\n"
        "  exit = function(el, out)\n"
        "    if el.name == 'keep' then out:node('kept', el):adopt(el.items) end\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("partial");
    REQUIRE(provider != nullptr);

    const Tree tree = parseText(*provider, "<keep><other a='1'>text</other></keep>", "t.xml");
    REQUIRE(tree.size() == 2);
    const auto& other = tree.node(tree.node(tree.root()).children[0]);
    CHECK(other.kind == "other");
    CHECK(other.findProperty("a")->value == "1");
    CHECK(other.findProperty(nmxd::kTextProperty)->value == "text");
}

TEST_CASE("a handle kept past its callback raises an error rather than crashing", "[lua]") {
    // The script stashes the builder and the element and uses them from a
    // later exit. Both are stale by then. The uses fail as Lua errors, which
    // pcall catches, and the parse still finishes with nothing lost.
    const auto registry = registryWithScript(
        "local stale_out, stale_el\n"
        "provider 'stale' {\n"
        "  base = 'xml',\n"
        "  exit = function(el, out)\n"
        "    if stale_out ~= nil then\n"
        "      local ok = pcall(function() stale_out:node('late', nil) end)\n"
        "      assert(not ok)\n"
        "      ok = pcall(function() return stale_el.name end)\n"
        "      assert(not ok)\n"
        "    end\n"
        "    stale_out, stale_el = out, el\n"
        "    out:node(el.name, el):adopt(el.items)\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("stale");
    REQUIRE(provider != nullptr);

    const Tree tree = parseText(*provider, "<a><b/><c/></a>", "t.xml");
    CHECK(tree.size() == 3);
    CHECK(tree.node(tree.root()).kind == "a");
}

TEST_CASE("a script that will not stop is cancelled", "[lua][slow]") {
    // Constraint C makes no exception for code the user wrote. A script with a
    // loop in it would otherwise hold a worker for ever, and switching format
    // or reloading a file has to be able to take that worker back.
    const auto registry = registryWithScript(
        "provider 'spinner' {\n"
        "  base = 'xml',\n"
        "  exit = function(el, out)\n"
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

// ---------------------------------------------------------------------------
// Scripts on a JSON base.

TEST_CASE("a script on JSON sees objects with their scalars and arrays with their items",
          "[lua][json]") {
    const auto registry = registryWithScript(
        "provider 'entities' {\n"
        "  base = 'json',\n"
        "  extensions = { '.ent' },\n"
        "  exit = function(el, out)\n"
        "    if el.name == 'item' then\n"
        "      out:node(el.attr.type or 'item', el):identity(el.attr.id, 'strong')"
        ":attributes(el):adopt(el.items)\n"
        "    elseif el.name == 'entities' then\n"
        "      out:forward(el.items)\n"
        "    end\n"
        "  end,\n"
        "}\n");
    const auto source = SourceFile::fromMemory(
        "{\"entities\": [{\"id\": \"e1\", \"type\": \"Door\"}, {\"id\": \"e2\", \"type\": \"Key\"}]}",
        "w.ent", "w.ent");
    const auto* provider = registry.resolve(source);
    REQUIRE(provider != nullptr);
    CHECK(provider->name() == "entities");

    auto parsed = provider->parse(source, {});
    REQUIRE(parsed.ok());
    const Tree& tree = parsed.value();
    REQUIRE(tree.size() == 3);
    const auto& root = tree.node(tree.root());
    CHECK(root.kind == "$");
    CHECK(root.findProperty("entities") == nullptr);

    // Generic JSON keeps every scalar exactly as written, quotes included,
    // and the script sees what the parser produced.
    CHECK(tree.node(root.children[0]).kind == "\"Door\"");
    CHECK(provider->identity(tree, root.children[1]).value == "\"e2\"");
    CHECK(source.slice(tree.node(root.children[1]).span) == "{\"id\": \"e2\", \"type\": \"Key\"}");
}

TEST_CASE("on JSON an object's scalar members are attributes and array values are text",
          "[lua][json]") {
    // What the JSON walker reports: an object carries its scalars as
    // attributes, each spanning key and value; an array element that is a
    // scalar is an element whose text is the value as written.
    const auto registry = registryWithScript(
        "provider 'j' {\n"
        "  base = 'json',\n"
        "  exit = function(el, out)\n"
        "    if el.name == 'item' and el.attr['#type'] == nil then\n"
        "      out:node('v', el):property('raw', el.text, el)\n"
        "    elseif el.name == '$' then\n"
        "      out:node('doc', el):attributes(el, '#type'):adopt(el.items)\n"
        "    end\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("j");
    REQUIRE(provider != nullptr);

    const char* text = "{\"hp\": 3, \"name\": \"x\", \"list\": [1, \"two\"]}";
    const auto source = SourceFile::fromMemory(text, "t.json", "t.json");
    auto parsed = provider->parse(source, {});
    REQUIRE(parsed.ok());
    const Tree& tree = parsed.value();

    const auto& root = tree.node(tree.root());
    CHECK(root.kind == "doc");
    REQUIRE(root.findProperty("hp") != nullptr);
    CHECK(root.findProperty("hp")->value == "3");
    CHECK(source.slice(root.findProperty("hp")->span) == "\"hp\": 3");
    CHECK(root.findProperty("#type") == nullptr);

    // The list held no object, and the script shaped its elements but not
    // the list itself, so the list took the default: one property whose
    // parts came from what the script made.
    const nmxd::Property* list = root.findProperty("list");
    REQUIRE(list != nullptr);
    CHECK(list->ordered);
    CHECK(source.slice(list->span) == "\"list\": [1, \"two\"]");
    REQUIRE(list->children.size() == 2);
    CHECK(list->children[0].name == "v");
    REQUIRE(list->children[1].children.size() == 1);
    CHECK(list->children[1].children[0].value == "\"two\"");
}

TEST_CASE("on JSON an unmentioned array of values is one property", "[lua][json]") {
    // The script says nothing about tags or pos, so both get the generic
    // JSON reading: a list of values is one property, nested lists are parts
    // with parts, and reordering any of it is a change.
    const auto registry = registryWithScript(
        "provider 'j' {\n"
        "  base = 'json',\n"
        "  exit = function(el, out)\n"
        "    if el.name == '$' then out:node('doc', el):attributes(el):adopt(el.items) end\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("j");
    REQUIRE(provider != nullptr);

    const Tree tree = parseText(*provider, "{\"tags\": [\"a\", \"b\"], \"pos\": [[1, 2], [3]]}",
                                "t.json");
    const auto& root = tree.node(tree.root());
    CHECK(root.children.empty());

    const nmxd::Property* tags = root.findProperty("tags");
    REQUIRE(tags != nullptr);
    CHECK(tags->ordered);
    REQUIRE(tags->children.size() == 2);
    CHECK(tags->children[1].value == "\"b\"");

    const nmxd::Property* pos = root.findProperty("pos");
    REQUIRE(pos != nullptr);
    REQUIRE(pos->children.size() == 2);
    CHECK(pos->children[0].ordered);
    REQUIRE(pos->children[0].children.size() == 2);
    CHECK(pos->children[0].children[1].value == "2");
}

TEST_CASE("on JSON enter sees the members and can key what is inside", "[lua][json]") {
    // The nested-children case on JSON. The wrapper is the "children" array,
    // which forwards its items, and each object is keyed by the id the object
    // above left at enter.
    const auto registry = registryWithScript(
        "provider 'j' {\n"
        "  base = 'json',\n"
        "  enter = function(el, frame)\n"
        "    if el.attr.id ~= nil then frame.data = el.attr.id end\n"
        "    if el.name == 'editor' then frame:opaque() end\n"
        "  end,\n"
        "  exit = function(el, out)\n"
        "    if el.name == 'children' then out:forward(el.items) return end\n"
        "    if el.attr.id == nil then return end\n"
        "    local owner = el.parent\n"
        "    while owner ~= nil and owner.data == nil do owner = owner.parent end\n"
        "    local key = el.attr.id\n"
        "    if owner ~= nil then key = owner.data .. '/' .. key end\n"
        "    out:node('n', el):identity(key, 'strong'):attributes(el, '#type'):adopt(el.items)\n"
        "  end,\n"
        "}\n");
    const auto* provider = registry.byName("j");
    REQUIRE(provider != nullptr);

    const char* text =
        "{\"id\": \"root\", \"children\": [{\"id\": \"a\"},"
        " {\"id\": \"b\", \"children\": [{\"id\": \"c\"}]}], \"editor\": {\"x\": 1}}";
    const auto source = SourceFile::fromMemory(text, "t.json", "t.json");
    auto parsed = provider->parse(source, {});
    REQUIRE(parsed.ok());
    const Tree& tree = parsed.value();

    REQUIRE(tree.size() == 4);
    const auto& root = tree.node(tree.root());
    REQUIRE(root.children.size() == 2);
    CHECK(root.findProperty("children") == nullptr);

    const auto& b = tree.node(root.children[1]);
    CHECK(provider->identity(tree, b.id).value == "\"root\"/\"b\"");
    CHECK(provider->identity(tree, b.children[0]).value == "\"b\"/\"c\"");

    // The editor object was kept opaque: one property, its raw text.
    const nmxd::Property* editor = root.findProperty("editor");
    REQUIRE(editor != nullptr);
    CHECK(editor->value == "\"editor\": {\"x\": 1}");
    CHECK(source.slice(editor->span) == editor->value);
}
