/// \file
/// \brief Implementation of provider resolution.

#include "core/registry.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "formats/bt_xml.h"
#include "formats/json_generic.h"
#include "formats/xml_generic.h"

namespace nmxd {

namespace {

/// \brief Lower-cases a string.
///
/// \param text The text to convert.
///
/// \returns A lower-case copy.
///
/// \remarks Extensions are compared in lower case, so a file called LEVEL.BT
///          resolves the same way as level.bt. Windows is case-insensitive about
///          them and a studio's exporter is not always consistent.
std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

void ProviderRegistry::add(std::unique_ptr<IFormatProvider> provider) {
    if (provider == nullptr) {
        return;
    }
    if (fallback_.empty()) {
        fallback_ = std::string(provider->name());
    }
    providers_.push_back(std::move(provider));
}

void ProviderRegistry::addFirst(std::unique_ptr<IFormatProvider> provider) {
    if (provider == nullptr) {
        return;
    }
    providers_.insert(providers_.begin(), std::move(provider));
}

const IFormatProvider* ProviderRegistry::byName(std::string_view name) const {
    const auto it = std::find_if(providers_.begin(), providers_.end(),
                                 [name](const auto& p) { return p->name() == name; });
    return it == providers_.end() ? nullptr : it->get();
}

void ProviderRegistry::setFallback(std::string_view name) { fallback_ = std::string(name); }

bool ProviderRegistry::mapExtension(std::string_view extension, std::string_view providerName) {
    if (byName(providerName) == nullptr) {
        return false;
    }

    const std::string key = lower(extension);
    // Last mapping wins, so a configuration that names an extension twice
    // behaves the way a reader of the file would expect rather than keeping
    // whichever line happened to come first.
    for (auto& entry : overrides_) {
        if (entry.first == key) {
            entry.second = std::string(providerName);
            return true;
        }
    }
    overrides_.emplace_back(key, std::string(providerName));
    return true;
}

std::vector<std::string> ProviderRegistry::apply(const ProviderConfig& config) {
    std::vector<std::string> unknown;

    for (const auto& [extension, providerName] : config.extensions) {
        if (!mapExtension(extension, providerName)) {
            unknown.push_back(providerName);
        }
    }

    if (!config.fallback.empty()) {
        if (byName(config.fallback) == nullptr) {
            unknown.push_back(config.fallback);
        } else {
            setFallback(config.fallback);
        }
    }

    return unknown;
}

const IFormatProvider* ProviderRegistry::overrideFor(std::string_view extension) const {
    if (extension.empty()) {
        return nullptr;
    }
    const std::string key = lower(extension);
    for (const auto& entry : overrides_) {
        if (entry.first == key) {
            return byName(entry.second);
        }
    }
    return nullptr;
}

const IFormatProvider* ProviderRegistry::resolve(const SourceFile& source,
                                                 std::string_view explicitName,
                                                 bool* unknownName) const {
    if (unknownName != nullptr) {
        *unknownName = false;
    }
    if (providers_.empty()) {
        return nullptr;
    }

    if (!explicitName.empty()) {
        if (const IFormatProvider* named = byName(explicitName)) {
            return named;
        }
        if (unknownName != nullptr) {
            *unknownName = true;
        }
        return nullptr;
    }

    // A configured extension is a studio's standing decision and beats a guess
    // about the file's contents.
    if (const IFormatProvider* configured = overrideFor(source.path().extension().string())) {
        return configured;
    }

    const IFormatProvider* best = nullptr;
    int bestScore = 0;
    for (const auto& provider : providers_) {
        const int score = provider->score(source);
        // Strictly greater, so the earliest registered provider wins a tie and
        // resolution stays deterministic.
        if (score > bestScore) {
            bestScore = score;
            best = provider.get();
        }
    }
    if (best != nullptr) {
        return best;
    }
    return byName(fallback_);
}

std::vector<std::string_view> ProviderRegistry::names() const {
    std::vector<std::string_view> out;
    out.reserve(providers_.size());
    for (const auto& provider : providers_) {
        out.push_back(provider->name());
    }
    return out;
}

ProviderRegistry makeDefaultRegistry() {
    ProviderRegistry registry;
    registry.add(makeGenericXmlProvider());
    registry.add(makeGenericJsonProvider());
    registry.add(makeBehaviorTreeProvider());
    registry.setFallback("xml");
    return registry;
}

}  // namespace nmxd
