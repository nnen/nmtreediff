#include "core/registry.h"

#include <algorithm>
#include <utility>

#include "formats/xml_generic.h"

namespace nmxd {

void ProviderRegistry::add(std::unique_ptr<IFormatProvider> provider) {
    if (provider == nullptr) {
        return;
    }
    if (fallback_.empty()) {
        fallback_ = std::string(provider->name());
    }
    providers_.push_back(std::move(provider));
}

const IFormatProvider* ProviderRegistry::byName(std::string_view name) const {
    const auto it = std::find_if(providers_.begin(), providers_.end(),
                                 [name](const auto& p) { return p->name() == name; });
    return it == providers_.end() ? nullptr : it->get();
}

void ProviderRegistry::setFallback(std::string_view name) { fallback_ = std::string(name); }

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
    registry.setFallback("xml");
    return registry;
}

}  // namespace nmxd
