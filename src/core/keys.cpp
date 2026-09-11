#include "ssstudio/keys.h"

#include <cctype>
#include <set>

#include "ssstudio/hash.h"

namespace ssstudio {
namespace {

Diagnostic error(std::string msg) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "SSSTUDIO-KEY";
    d.message = std::move(msg);
    return d;
}

// Deterministic rehash used when a hashed key collides. Documented in
// SHADERS.md so an external tool can reproduce the same value.
std::uint32_t rehash(std::uint32_t h, std::uint32_t attempt) {
    return fnv1a32(to_hex(h, 8) + "#" + std::to_string(attempt));
}

}  // namespace

std::vector<std::uint32_t> assign_keys(const std::vector<KeyRequest>& requests,
                                       KeyStrategy strategy, bool key16, Diagnostics& out_diags,
                                       std::map<std::string, std::uint32_t>* out_new_pins) {
    const std::uint32_t mask = key16 ? 0xFFFFu : 0xFFFFFFFFu;
    std::vector<std::uint32_t> keys(requests.size(), 0);
    std::set<std::uint32_t> used;

    // Pinned keys always win, whatever the strategy, so switching strategy on an
    // existing project never renumbers shaders that were already shipped.
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (!requests[i].pinned) continue;
        const std::uint32_t k = *requests[i].pinned & mask;
        if (!used.insert(k).second) {
            out_diags.push_back(error("shader '" + requests[i].id + "' pins key " +
                                      std::to_string(k) + ", which is already used"));
        }
        keys[i] = k;
    }

    std::uint32_t next = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].pinned) continue;
        std::uint32_t k = 0;
        switch (strategy) {
            case KeyStrategy::Enum: {
                while (used.count(next)) ++next;
                k = next++;
                break;
            }
            case KeyStrategy::Hash: {
                k = fnv1a32(requests[i].id) & mask;
                for (std::uint32_t attempt = 1; used.count(k) && attempt < 64; ++attempt) {
                    k = rehash(k, attempt) & mask;
                }
                if (used.count(k)) {
                    out_diags.push_back(error("could not find a free hashed key for '" +
                                              requests[i].id + "'"));
                }
                break;
            }
            case KeyStrategy::Explicit: {
                // First build assigns; the caller pins the result so it never moves.
                k = fnv1a32(requests[i].id) & mask;
                for (std::uint32_t attempt = 1; used.count(k) && attempt < 64; ++attempt) {
                    k = rehash(k, attempt) & mask;
                }
                if (out_new_pins) (*out_new_pins)[requests[i].id] = k;
                break;
            }
        }
        used.insert(k);
        keys[i] = k;
    }
    return keys;
}

std::string enum_name(const std::string& prefix, const std::string& id) {
    std::string out = prefix.empty() ? std::string() : prefix + "_";
    bool prev_underscore = out.empty();
    for (char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            prev_underscore = false;
        } else if (!prev_underscore) {
            out += '_';
            prev_underscore = true;
        }
    }
    if (!out.empty() && out.back() == '_') out.pop_back();
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
    return out;
}

}  // namespace ssstudio
