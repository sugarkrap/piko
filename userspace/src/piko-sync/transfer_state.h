
#ifndef PIKO_SYNC_TRANSFER_STATE_H
#define PIKO_SYNC_TRANSFER_STATE_H

#include <stdint.h>
#include <stdio.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace piko_sync {

struct TransferRecord {
    std::string final_name;
    uint64_t bytes_on_disk;
    bool complete;

    TransferRecord() : bytes_on_disk(0), complete(false) {}
};

typedef std::pair<std::string, uint64_t> TransferKey;
typedef std::map<TransferKey, TransferRecord> TransferMap;

struct OfferDecision {
    std::string final_name;
    uint64_t resume_offset;
};

inline std::string resolve_collision(const std::string &name,
                                      const TransferMap &map,
                                      const std::vector<std::string> &existing_complete_names)
{
    std::string base = name, ext;
    std::string::size_type dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        base = name.substr(0, dot);
        ext = name.substr(dot);
    }

    for (int n = 0;; n++) {
        std::string candidate = name;
        if (n > 0) {
            char suffix[32];
            snprintf(suffix, sizeof(suffix), "-%d", n);
            candidate = base + suffix + ext;
        }

        bool taken = false;
        for (TransferMap::const_iterator it = map.begin(); it != map.end(); ++it) {
            if (it->second.final_name == candidate) { taken = true; break; }
        }
        if (!taken) {
            for (size_t i = 0; i < existing_complete_names.size(); i++) {
                if (existing_complete_names[i] == candidate) { taken = true; break; }
            }
        }
        if (!taken)
            return candidate;
    }
}

inline OfferDecision decide_offer(TransferMap &map,
                                   const std::string &original_name,
                                   uint64_t total_size,
                                   const std::vector<std::string> &existing_complete_names)
{
    TransferKey key(original_name, total_size);
    TransferMap::iterator it = map.find(key);

    OfferDecision d;
    if (it != map.end()) {
        d.final_name = it->second.final_name;
        d.resume_offset = it->second.bytes_on_disk;
        return d;
    }

    TransferRecord rec;
    rec.final_name = resolve_collision(original_name, map, existing_complete_names);
    map[key] = rec;

    d.final_name = rec.final_name;
    d.resume_offset = 0;
    return d;
}

}

#endif
