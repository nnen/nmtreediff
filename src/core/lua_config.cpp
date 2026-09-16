/// \file
/// \brief Implementation of reading a configuration script.

#include "core/lua_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

#include <sol/sol.hpp>

#include "core/lua_state.h"

namespace nmxd {

namespace {

/// \brief The name a script uses to map extensions to providers.
constexpr const char* kFormatsFunction = "formats";
/// \brief The name a script uses to set the fallback provider.
constexpr const char* kFallbackFunction = "fallback";
/// \brief The name a script uses to set the graph direction.
constexpr const char* kDirectionFunction = "graph_direction";
/// \brief The name a script uses to set the key that closes the window.
constexpr const char* kExitKeyFunction = "exit_key";
/// \brief The name a script uses to declare a format of its own.
constexpr const char* kProviderFunction = "provider";

/// \brief What a script writes to mean "the graph runs top down".
constexpr const char* kTopDownWord = "top_down";
/// \brief What a script writes to mean "the graph runs left to right".
constexpr const char* kLeftToRightWord = "left_to_right";

/// \brief Lower-cases a string.
///
/// \param text The text to fold.
///
/// \returns The same text in lower case.
///
/// \remarks Extensions and key names are matched without regard to case,
///          because Windows is careless about the first and people are careless
///          about the second.
[[nodiscard]] std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// \brief Records one problem against a script.
///
/// \param problems Where to record it.
/// \param origin The file the problem is in.
/// \param message What is wrong.
/// \param line The line it is on, or zero when unknown.
/// \param detail Everything the interpreter said, when it said more than the
///        message; empty otherwise.
void complain(std::vector<ConfigProblem>& problems, const std::filesystem::path& origin,
              std::string message, std::uint32_t line = 0, std::string detail = {}) {
    problems.push_back(ConfigProblem{line, std::move(message), origin, std::move(detail)});
}

/// \brief Decides whether an interpreter message carries more than its first
///        line.
///
/// \param message The message Lua produced.
///
/// \returns The whole message when it runs past its first line, so a
///          traceback is kept; empty when the first line was all of it.
[[nodiscard]] std::string detailOf(const std::string& message) {
    return message.find('\n') == std::string::npos ? std::string{} : message;
}

/// \brief Turns a direction word into a direction.
///
/// \param word What the script said.
///
/// \returns The direction, or nothing when the word is not one of the two.
[[nodiscard]] std::optional<GraphDirection> directionFromWord(const std::string& word) {
    if (word == kTopDownWord) {
        return GraphDirection::TopDown;
    }
    if (word == kLeftToRightWord) {
        return GraphDirection::LeftToRight;
    }
    return std::nullopt;
}

/// \brief Reads a list of strings out of a provider declaration.
///
/// \param table The table the script passed.
/// \param key Which entry to read.
/// \param fold Whether to lower-case what is read.
///
/// \returns The strings, in the order the script listed them.
///
/// \remarks A Lua array iterates in order, unlike a keyed table, which is
///          why property order can be a list and an extension map cannot.
[[nodiscard]] std::vector<std::string> readStringList(const sol::table& table, const char* key,
                                                      bool fold) {
    std::vector<std::string> values;
    const sol::optional<sol::table> list = table[key];
    if (!list) {
        return values;
    }
    for (const auto& entry : *list) {
        if (entry.second.is<std::string>()) {
            std::string value = entry.second.as<std::string>();
            values.push_back(fold ? lower(std::move(value)) : std::move(value));
        }
    }
    return values;
}

/// \brief Binds the functions a configuration script may call.
class ConfigBindings {
public:
    /// \brief Prepares bindings that write into one configuration.
    ///
    /// \param origin The script being read.
    /// \param source The text of that script, kept with any provider it
    ///        declares so a worker can reload it later.
    /// \param config Where to put what the script asks for.
    /// \param problems Where to record anything wrong.
    ConfigBindings(const std::filesystem::path& origin, std::string_view source,
                   ProviderConfig& config, std::vector<ConfigProblem>& problems)
        : origin_(origin), source_(source), config_(config), problems_(problems) {}

    /// \brief Installs the functions into an interpreter.
    ///
    /// \param lua The interpreter to install into.
    void installInto(sol::state& lua) {
        lua.set_function(kFormatsFunction, [this](sol::table table) { readFormats(table); });
        lua.set_function(kFallbackFunction,
                         [this](std::string name) { config_.fallback = std::move(name); });
        lua.set_function(kDirectionFunction, [this](std::string word) { readDirection(word); });
        lua.set_function(kExitKeyFunction,
                         [this](std::string key) { config_.exitKey = lower(std::move(key)); });

        // Declaring a provider reads as `provider "bt" { ... }`, which in Lua is
        // a call returning a function that takes the table. Two calls rather
        // than one so the name reads before the body it names.
        lua.set_function(kProviderFunction, [this](std::string name) {
            return [this, name](sol::table body) { readProvider(name, body); };
        });
    }

private:
    /// \brief Reads a table of extension to provider name.
    ///
    /// \param table What the script passed.
    void readFormats(const sol::table& table) {
        for (const auto& entry : table) {
            if (!entry.first.is<std::string>() || !entry.second.is<std::string>()) {
                complain(problems_, origin_, "formats takes a table of extension to format name");
                continue;
            }
            const std::string extension = lower(entry.first.as<std::string>());
            if (extension.size() < 2 || extension.front() != '.') {
                complain(problems_, origin_,
                         "\"" + extension + "\" is not an extension; extensions start with a dot");
                continue;
            }
            config_.extensions.emplace_back(extension, entry.second.as<std::string>());
        }
    }

    /// \brief Reads the graph direction.
    ///
    /// \param word What the script passed.
    void readDirection(const std::string& word) {
        const std::optional<GraphDirection> direction = directionFromWord(lower(word));
        if (!direction) {
            complain(problems_, origin_,
                     "graph_direction takes \"top_down\" or \"left_to_right\", not \"" + word +
                         "\"");
            return;
        }
        config_.graphDirection = *direction;
    }

    /// \brief Reads one provider declaration.
    ///
    /// \param name The name the script gave it.
    /// \param body The table describing it.
    void readProvider(const std::string& name, const sol::table& body) {
        if (name.empty()) {
            complain(problems_, origin_, "a provider needs a name");
            return;
        }

        ScriptedProviderSpec spec;
        spec.name = name;
        spec.origin = origin_;
        spec.source = std::string(source_);
        spec.displayName = body.get_or("display_name", name);
        spec.base = lower(body.get_or("base", std::string("xml")));
        spec.extensions = readStringList(body, "extensions", true);
        spec.propertyOrder = readStringList(body, "property_order", false);

        const sol::optional<std::string> direction = body["graph_direction"];
        if (direction) {
            const std::optional<GraphDirection> resolved = directionFromWord(lower(*direction));
            if (!resolved) {
                complain(problems_, origin_,
                         "provider \"" + name + "\" asked for an unknown graph direction");
            } else {
                spec.direction = *resolved;
            }
        }

        const sol::optional<std::string> pin = body["stack_entry_pin"];
        if (pin) {
            const std::string word = lower(*pin);
            if (word == "top") {
                spec.entryPin = StackEntryPin::Top;
            } else if (word == "bottom") {
                spec.entryPin = StackEntryPin::Bottom;
            } else {
                complain(problems_, origin_,
                         "provider \"" + name + "\" asked for an unknown stack entry pin; " +
                             "stack_entry_pin takes \"top\" or \"bottom\", not \"" + *pin + "\"");
            }
        }

        config_.providers.push_back(std::move(spec));
    }

    const std::filesystem::path& origin_;
    std::string_view source_;
    ProviderConfig& config_;
    std::vector<ConfigProblem>& problems_;
};

}  // namespace

bool runConfigScript(std::string_view text, const std::filesystem::path& origin,
                     ProviderConfig& config, std::vector<ConfigProblem>& problems) {
    const std::size_t before = problems.size();

    LuaState state;
    ConfigBindings bindings(origin, text, config, problems);
    bindings.installInto(state.get());

    // The chunk is named after the file, so an error and its traceback say
    // "config.lua:12" rather than quoting the script's first line. The name
    // alone, not the path: a Windows path carries a colon, and Lua's message
    // format puts the line after the first one.
    const sol::protected_function_result result = state.get().safe_script(
        std::string(text), sol::script_pass_on_error, "@" + origin.filename().string());
    if (!result.valid()) {
        const sol::error error = result;
        const std::string message = error.what();
        complain(problems, origin, trimLuaError(message), luaErrorLine(message),
                 detailOf(message));
        return false;
    }

    // A script that ran to the end but asked for something impossible has still
    // failed. Reporting only the first kind would let the second through.
    return problems.size() == before;
}

Result<std::monostate, ConfigError> loadConfigScript(const std::filesystem::path& path,
                                                     ProviderConfig& config,
                                                     std::vector<ConfigProblem>& problems) {
    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        return fail(ConfigError::NotFound);
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(ConfigError::NotReadable);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    if (!runConfigScript(buffer.str(), path, config, problems)) {
        return fail(ConfigError::Malformed);
    }
    return std::monostate{};
}

Result<std::monostate, ConfigError> loadConfiguration(const std::filesystem::path& explicitPath,
                                                      ProviderConfig& config,
                                                      std::vector<ConfigProblem>& problems) {
    // The home files are read where they exist and passed over where they do
    // not, because a home directory with no configuration in it is the ordinary
    // case rather than a mistake.
    for (const std::filesystem::path& path : configSearchPaths()) {
        std::error_code code;
        if (!std::filesystem::exists(path, code)) {
            continue;
        }
        if (const auto loaded = loadConfigScript(path, config, problems); !loaded.ok()) {
            return fail(loaded.error());
        }
    }

    if (explicitPath.empty()) {
        return std::monostate{};
    }
    return loadConfigScript(explicitPath, config, problems);
}

}  // namespace nmxd
