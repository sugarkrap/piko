/*
 * transfer_state.h -- the server's decision logic for "what happens when
 * this filename+size gets offered", kept apart from piko-sync-server.cxx
 * so it can be exercised without a socket, a listener or a filesystem --
 * see tests/protocol-test.cxx. Same split as pikostore's romstate.h.
 *
 * DESIGN NOTE: why an in-memory map and not something read off disk.
 *
 * Resume only needs to survive a client reconnecting to a SERVER PROCESS
 * THAT IS STILL RUNNING -- surviving a server restart is out of scope
 * (matches the SFTP/scp status quo this replaces, which offers no resume
 * at all). Within one run, the map below is the only place "this
 * .piko-sync-part on disk was promised to be N bytes total" is recorded,
 * since a partial file's own size on disk tells you how far it got, not
 * how far it is supposed to go. Keying by (original_name, total_size)
 * rather than original_name alone is what lets two unrelated files that
 * happen to share a name -- one mid-transfer, a different one offered
 * fresh -- resolve as a genuine collision (new "(1)" name) instead of the
 * second clobbering the first's resume state.
 */

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

typedef std::pair<std::string, uint64_t> TransferKey; /* (original_name, total_size) */
typedef std::map<TransferKey, TransferRecord> TransferMap;

struct OfferDecision {
    std::string final_name;
    uint64_t resume_offset;
};

/* Picks a name that does not collide with any final_name already claimed
 * in `map`, nor with anything in `existing_complete_names` (the
 * directory listing of already-finished files). "name", then
 * "name (1)", "name (2)", ... -- same scheme browsers use for downloads,
 * chosen because it is what a user dropping files into a shared
 * Transfers folder already expects. */
inline std::string resolve_collision(const std::string &name,
                                      const TransferMap &map,
                                      const std::vector<std::string> &existing_complete_names)
{
    std::string base = name, ext;
    std::string::size_type dot = name.rfind('.');
    /* Split "photo.jpg" -> base "photo", ext ".jpg" so the suffix lands
     * before the extension ("photo (1).jpg"), not after it. A dot at
     * position 0 (a dotfile) is not treated as an extension. */
    if (dot != std::string::npos && dot > 0) {
        base = name.substr(0, dot);
        ext = name.substr(dot);
    }

    for (int n = 0;; n++) {
        std::string candidate = name;
        if (n > 0) {
            char suffix[32];
            snprintf(suffix, sizeof(suffix), " (%d)", n);
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

/* The one entry point piko-sync-server.cxx calls on FILE_OFFER. Inserts a
 * fresh record into `map` for a never-seen (name, total_size), or
 * returns the existing record's resume point unchanged. Mutates `map` in
 * place rather than returning a copy, since the record needs to keep
 * living there for the rest of the transfer (chunk acks update
 * bytes_on_disk through the same entry). */
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

} /* namespace piko_sync */

#endif /* PIKO_SYNC_TRANSFER_STATE_H */
