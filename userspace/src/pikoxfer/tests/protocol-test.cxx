/*
 * Host-side tests for protocol.h, transfer_state.h and transfer_queue.h --
 * the wire format, the resume/collision decision, and the row bookkeeping
 * behind both pikoxfer apps' transfer lists.
 *
 * These run on the BUILD machine, with no FLTK, no X, no socket and no
 * device -- same reasoning as pikostore's romstate-test.cxx: this is the
 * only part of pikoxfer that can be exercised before it reaches the one
 * spare Zaurus, or before a real flaky link is involved at all.
 *
 *   g++ -O2 -Wall -Wextra -o protocol-test tests/protocol-test.cxx && ./protocol-test
 */

#include "../protocol.h"
#include "../transfer_state.h"
#include "../transfer_queue.h"

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

using namespace pikoxfer;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_str(const std::string &got, const std::string &want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s\n        got  [%s]\n        want [%s]\n",
               what, got.c_str(), want.c_str());
    }
}

static void check_u64(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s\n        got  [%llu]\n        want [%llu]\n",
               what, (unsigned long long)got, (unsigned long long)want);
    }
}

/* ---------------------------------------------------------------- */

static void test_message_roundtrips()
{
    printf("protocol: every message type round-trips through encode/decode\n");

    HelloMsg h; h.version = PROTO_VERSION;
    HelloMsg h2;
    check(decode_hello(encode(h), h2), "hello decodes");
    check(h2.version == PROTO_VERSION, "hello version");

    FileOfferMsg fo; fo.name = "photo.jpg"; fo.total_size = 123456789ull;
    FileOfferMsg fo2;
    check(decode_file_offer(encode(fo), fo2), "file_offer decodes");
    check_str(fo2.name, "photo.jpg", "file_offer name");
    check_u64(fo2.total_size, 123456789ull, "file_offer total_size");

    FileOfferAckMsg ack; ack.accepted = true; ack.final_name = "photo (1).jpg";
    ack.resume_offset = 4096;
    FileOfferAckMsg ack2;
    check(decode_file_offer_ack(encode(ack), ack2), "offer_ack (accepted) decodes");
    check(ack2.accepted, "offer_ack accepted flag");
    check_str(ack2.final_name, "photo (1).jpg", "offer_ack final_name");
    check_u64(ack2.resume_offset, 4096, "offer_ack resume_offset");

    FileOfferAckMsg rej; rej.accepted = false; rej.reason = "disk full";
    FileOfferAckMsg rej2;
    check(decode_file_offer_ack(encode(rej), rej2), "offer_ack (rejected) decodes");
    check(!rej2.accepted, "offer_ack rejected flag");
    check_str(rej2.reason, "disk full", "offer_ack reason");

    DataChunkMsg dc; dc.offset = 65536; dc.data = std::string(1000, 'x');
    DataChunkMsg dc2;
    check(decode_data_chunk(encode(dc), dc2), "data_chunk decodes");
    check_u64(dc2.offset, 65536, "data_chunk offset");
    check(dc2.data.size() == 1000, "data_chunk payload size");
    check(dc2.data == dc.data, "data_chunk payload bytes");

    ChunkAckMsg ca; ca.bytes_written = 999999999999ull;
    ChunkAckMsg ca2;
    check(decode_chunk_ack(encode(ca), ca2), "chunk_ack decodes");
    check_u64(ca2.bytes_written, 999999999999ull, "chunk_ack big value survives 64 bits");

    FileCompleteMsg fc; fc.crc32 = 0xdeadbeefu;
    FileCompleteMsg fc2;
    check(decode_file_complete(encode(fc), fc2), "file_complete decodes");
    check(fc2.crc32 == 0xdeadbeefu, "file_complete crc32");

    FileCompleteAckMsg cok; cok.ok = true;
    FileCompleteAckMsg cok2;
    check(decode_file_complete_ack(encode(cok), cok2), "complete_ack (ok) decodes");
    check(cok2.ok, "complete_ack ok flag");

    FileCompleteAckMsg cbad; cbad.ok = false; cbad.reason = "crc mismatch";
    FileCompleteAckMsg cbad2;
    check(decode_file_complete_ack(encode(cbad), cbad2), "complete_ack (bad) decodes");
    check(!cbad2.ok, "complete_ack !ok flag");
    check_str(cbad2.reason, "crc mismatch", "complete_ack reason");

    ErrorMsg em; em.message = "protocol version mismatch";
    ErrorMsg em2;
    check(decode_error(encode(em), em2), "error decodes");
    check_str(em2.message, "protocol version mismatch", "error message");
}

static void test_deploy_message_roundtrips()
{
    printf("pikodeploy: every new message type round-trips through encode/decode\n");

    PutOfferMsg po; po.path = "/boot/zImage-full"; po.total_size = 1234567;
    po.mode = 0755; po.policy = PUT_ALWAYS; po.crc32 = 0xabcd1234u; po.backup = true;
    po.staging = STAGE_SD;
    PutOfferMsg po2;
    check(decode_put_offer(encode(po), po2), "put_offer decodes");
    check_str(po2.path, "/boot/zImage-full", "put_offer path");
    check_u64(po2.total_size, 1234567, "put_offer total_size");
    check(po2.mode == 0755u, "put_offer mode");
    check(po2.policy == PUT_ALWAYS, "put_offer policy");
    check(po2.crc32 == 0xabcd1234u, "put_offer crc32");
    check(po2.backup, "put_offer backup flag");
    check(po2.staging == STAGE_SD, "put_offer staging");

    PutOfferMsg po_default;
    check(po_default.staging == STAGE_NAND, "put_offer defaults to NAND staging");

    PutOfferMsg po3; po3.policy = PUT_IF_MISSING; po3.backup = false;
    PutOfferMsg po4;
    check(decode_put_offer(encode(po3), po4), "put_offer (if_missing) decodes");
    check(po4.policy == PUT_IF_MISSING, "put_offer if_missing policy");
    check(!po4.backup, "put_offer backup false");

    PutOfferAckMsg resume; resume.outcome = PUT_RESUME; resume.resume_offset = 4096;
    PutOfferAckMsg resume2;
    check(decode_put_offer_ack(encode(resume), resume2), "put_offer_ack (resume) decodes");
    check(resume2.outcome == PUT_RESUME, "put_offer_ack resume outcome");
    check_u64(resume2.resume_offset, 4096, "put_offer_ack resume_offset");

    PutOfferAckMsg rej; rej.outcome = PUT_REJECTED; rej.reason = "no such directory";
    PutOfferAckMsg rej2;
    check(decode_put_offer_ack(encode(rej), rej2), "put_offer_ack (rejected) decodes");
    check(rej2.outcome == PUT_REJECTED, "put_offer_ack rejected outcome");
    check_str(rej2.reason, "no such directory", "put_offer_ack reason");

    PathMsg path; path.path = "/lib/modules/7.1.4/zaurus-audio";
    PathMsg path2;
    check(decode_path(encode(path), path2), "path decodes");
    check_str(path2.path, "/lib/modules/7.1.4/zaurus-audio", "path value");

    OkReasonMsg ok; ok.ok = true;
    OkReasonMsg ok2;
    check(decode_ok_reason(encode(ok), ok2), "ok_reason (ok) decodes");
    check(ok2.ok, "ok_reason ok flag");

    OkReasonMsg bad; bad.ok = false; bad.reason = "permission denied";
    OkReasonMsg bad2;
    check(decode_ok_reason(encode(bad), bad2), "ok_reason (bad) decodes");
    check(!bad2.ok, "ok_reason !ok flag");
    check_str(bad2.reason, "permission denied", "ok_reason reason");

    SymlinkMsg sl; sl.target = "ld-uClibc-1.0.54.so"; sl.linkname = "/lib/ld-uClibc.so.1";
    SymlinkMsg sl2;
    check(decode_symlink(encode(sl), sl2), "symlink decodes");
    check_str(sl2.target, "ld-uClibc-1.0.54.so", "symlink target");
    check_str(sl2.linkname, "/lib/ld-uClibc.so.1", "symlink linkname");

    RunMsg run; run.op = RUN_MOUNT_SD_CARD;
    RunMsg run2;
    check(decode_run(encode(run), run2), "run decodes");
    check(run2.op == RUN_MOUNT_SD_CARD, "run op");

    QueryExistingAckMsg qe; qe.exists = true; qe.size = 999;
    QueryExistingAckMsg qe2;
    check(decode_query_existing_ack(encode(qe), qe2), "query_existing_ack decodes");
    check(qe2.exists, "query_existing_ack exists flag");
    check_u64(qe2.size, 999, "query_existing_ack size");

    QueryExistingAckMsg qe3; qe3.exists = false;
    QueryExistingAckMsg qe4;
    check(decode_query_existing_ack(encode(qe3), qe4), "query_existing_ack (missing) decodes");
    check(!qe4.exists, "query_existing_ack !exists flag");

    FreeSpaceAckMsg fs; fs.free_bytes = 18364ull * 1024;
    FreeSpaceAckMsg fs2;
    check(decode_free_space_ack(encode(fs), fs2), "free_space_ack decodes");
    check_u64(fs2.free_bytes, 18364ull * 1024, "free_space_ack free_bytes");
}

static void test_decode_rejects_truncated()
{
    printf("protocol: truncated payloads are rejected, not read out of bounds\n");

    HelloMsg h;
    check(!decode_hello(std::string(), h), "empty hello rejected");
    check(!decode_hello(std::string("\x00\x00", 2), h), "2-byte hello rejected");

    FileOfferMsg fo;
    std::string full = encode(FileOfferMsg());
    for (size_t i = 0; i < full.size(); i++) {
        std::string cut = full.substr(0, i);
        check(!decode_file_offer(cut, fo), "file_offer truncated at various points rejected");
    }
}

/* ---------------------------------------------------------------- */

static void test_frame_reader_whole()
{
    printf("frame reader: a single feed containing one whole frame\n");
    FrameReader r;
    std::string f = encode_frame(MSG_HELLO, encode(HelloMsg()));
    r.feed(f.data(), f.size());

    uint32_t type = 0;
    std::string payload;
    check(r.next(type, payload) == FrameReader::GOT_FRAME, "got the frame");
    check(type == MSG_HELLO, "frame type");
    check(r.next(type, payload) == FrameReader::NEED_MORE, "nothing left after one frame");
}

static void test_frame_reader_split_bytes()
{
    printf("frame reader: the same frame fed one byte at a time\n");
    FrameReader r;
    std::string f = encode_frame(MSG_DATA_CHUNK, std::string(500, 'z'));

    uint32_t type = 0;
    std::string payload;
    FrameReader::Result res = FrameReader::NEED_MORE;
    for (size_t i = 0; i < f.size(); i++) {
        r.feed(f.data() + i, 1);
        res = r.next(type, payload);
        if (res != FrameReader::NEED_MORE)
            break;
    }
    check(res == FrameReader::GOT_FRAME, "frame eventually completes byte by byte");
    check(payload.size() == 500, "payload size survives a byte-at-a-time feed");
}

static void test_frame_reader_multiple_in_one_feed()
{
    printf("frame reader: two frames arriving in a single feed() call\n");
    FrameReader r;
    std::string f1 = encode_frame(MSG_HELLO, std::string());
    std::string f2 = encode_frame(MSG_FILE_COMPLETE, encode(FileCompleteMsg()));
    std::string both = f1 + f2;
    r.feed(both.data(), both.size());

    uint32_t type = 0;
    std::string payload;
    check(r.next(type, payload) == FrameReader::GOT_FRAME, "first frame");
    check(type == MSG_HELLO, "first frame type");
    check(r.next(type, payload) == FrameReader::GOT_FRAME, "second frame");
    check(type == MSG_FILE_COMPLETE, "second frame type");
    check(r.next(type, payload) == FrameReader::NEED_MORE, "nothing left");
}

static void test_frame_reader_desync()
{
    printf("frame reader: garbage instead of the magic is DESYNC, not a hang\n");
    FrameReader r;
    std::string garbage = "not a pikoxfer frame at all, just noise....";
    r.feed(garbage.data(), garbage.size());

    uint32_t type = 0;
    std::string payload;
    check(r.next(type, payload) == FrameReader::DESYNC, "bad magic detected");
}

static void test_frame_reader_oversize_length_is_desync()
{
    printf("frame reader: a header claiming an absurd length is DESYNC\n");
    FrameReader r;
    std::string bad;
    put_u32(bad, FRAME_MAGIC);
    put_u32(bad, MSG_DATA_CHUNK);
    put_u32(bad, static_cast<uint32_t>(MAX_FRAME) + 1);
    r.feed(bad.data(), bad.size());

    uint32_t type = 0;
    std::string payload;
    check(r.next(type, payload) == FrameReader::DESYNC,
          "oversize length rejected instead of trying to buffer it");
}

/* ---------------------------------------------------------------- */

static void test_crc32_known_vectors()
{
    printf("crc32: matches well-known reference values\n");
    Crc32 c1;
    c1.update("", 0);
    check(c1.final_value() == 0x00000000u, "crc32 of empty input is 0");

    Crc32 c2;
    c2.update("123456789", 9);
    check(c2.final_value() == 0xCBF43926u, "crc32(\"123456789\") is the standard check value");
}

static void test_crc32_incremental_matches_whole()
{
    printf("crc32: feeding data in pieces matches feeding it all at once\n");
    std::string data = "the quick brown fox jumps over the lazy dog, repeatedly, "
                        "to make the buffer long enough to span several chunks";

    Crc32 whole;
    whole.update(data.data(), data.size());

    Crc32 pieces;
    size_t i = 0;
    while (i < data.size()) {
        size_t n = (data.size() - i > 7) ? 7 : data.size() - i; /* awkward stride on purpose */
        pieces.update(data.data() + i, n);
        i += n;
    }
    check(whole.final_value() == pieces.final_value(), "incremental crc32 matches");
}

/* ---------------------------------------------------------------- */

static void test_offer_new_file_no_collision()
{
    printf("transfer_state: a brand new name gets itself back, resume 0\n");
    TransferMap map;
    std::vector<std::string> existing;
    OfferDecision d = decide_offer(map, "photo.jpg", 1000, existing);
    check_str(d.final_name, "photo.jpg", "no collision -> same name");
    check_u64(d.resume_offset, 0, "fresh offer resumes at 0");
}

static void test_offer_collides_with_complete_file()
{
    printf("transfer_state: a name already fully on disk gets \"(1)\" appended\n");
    TransferMap map;
    std::vector<std::string> existing;
    existing.push_back("photo.jpg");
    OfferDecision d = decide_offer(map, "photo.jpg", 1000, existing);
    check_str(d.final_name, "photo (1).jpg", "extension kept after the collision suffix");
}

static void test_offer_collides_with_in_progress_transfer()
{
    printf("transfer_state: two DIFFERENT files sharing a name both get tracked\n");
    TransferMap map;
    std::vector<std::string> existing;

    OfferDecision a = decide_offer(map, "clip.mov", 5000, existing);
    OfferDecision b = decide_offer(map, "clip.mov", 9000, existing); /* different size = different file */

    check_str(a.final_name, "clip.mov", "first claimant keeps the plain name");
    check_str(b.final_name, "clip (1).mov", "second, different-sized file gets renamed");
}

static void test_offer_same_key_resumes_same_final_name()
{
    printf("transfer_state: re-offering the identical (name, size) resumes, no re-rename\n");
    TransferMap map;
    std::vector<std::string> existing;

    OfferDecision first = decide_offer(map, "video.mp4", 2000000, existing);
    check_u64(first.resume_offset, 0, "first offer starts at 0");

    /* Simulate the connection making progress, then dropping. */
    TransferKey key("video.mp4", 2000000);
    map[key].bytes_on_disk = 750000;

    OfferDecision reconnect = decide_offer(map, "video.mp4", 2000000, existing);
    check_str(reconnect.final_name, first.final_name, "reconnect keeps the same final name");
    check_u64(reconnect.resume_offset, 750000, "reconnect resumes from the tracked byte count");
}

static void test_offer_dotfile_not_treated_as_pure_extension()
{
    printf("transfer_state: a dotfile's leading dot is not an \"extension\"\n");
    TransferMap map;
    std::vector<std::string> existing;
    existing.push_back(".bashrc");
    OfferDecision d = decide_offer(map, ".bashrc", 200, existing);
    check_str(d.final_name, ".bashrc (1)", "suffix appended after the whole dotfile name");
}

/* ---------------------------------------------------------------- */

static void test_queue_basic_progress()
{
    printf("transfer_queue: per-row progress and status tracking\n");
    TransferQueue q;
    int i = q.add("a.bin", 1000);
    check(q.count() == 1, "one row after add");
    check_u64(q.row(i).bytes_done, 0, "starts at 0 bytes done");

    q.set_status(i, XFER_TRANSFERRING);
    check(q.row(i).status == XFER_TRANSFERRING, "status updates");

    q.set_progress(i, 400);
    check_u64(q.row(i).bytes_done, 400, "progress updates");

    q.set_progress(i, 5000); /* more than the total */
    check_u64(q.row(i).bytes_done, 1000, "progress is clamped to the row's total");
}

static void test_queue_aggregate_percent()
{
    printf("transfer_queue: aggregate percent across multiple rows\n");
    TransferQueue q;
    int a = q.add("a.bin", 1000);
    int b = q.add("b.bin", 3000);

    check(q.aggregate_percent() == 0.0, "nothing done yet");

    q.set_progress(a, 1000);   /* fully done */
    q.set_progress(b, 1000);   /* a third done */
    /* (1000 + 1000) / (1000 + 3000) = 50% */
    double pct = q.aggregate_percent();
    check(pct > 49.9 && pct < 50.1, "aggregate reflects both rows' weight by size");
}

static void test_queue_unsized_row_excluded_from_aggregate()
{
    printf("transfer_queue: a row with an unknown total does not skew the aggregate\n");
    TransferQueue q;
    int a = q.add("known.bin", 1000);
    q.add("still-sizing.bin", 0); /* size not known yet */
    q.set_progress(a, 500);

    double pct = q.aggregate_percent();
    check(pct > 49.9 && pct < 50.1, "unsized row excluded from both sides of the ratio");
}

static void test_queue_empty_aggregate_is_zero_not_nan()
{
    printf("transfer_queue: an empty queue reports 0%%, not NaN\n");
    TransferQueue q;
    check(q.aggregate_percent() == 0.0, "empty queue");
}

/* ---------------------------------------------------------------- */

int main()
{
    test_message_roundtrips();
    test_deploy_message_roundtrips();
    test_decode_rejects_truncated();

    test_frame_reader_whole();
    test_frame_reader_split_bytes();
    test_frame_reader_multiple_in_one_feed();
    test_frame_reader_desync();
    test_frame_reader_oversize_length_is_desync();

    test_crc32_known_vectors();
    test_crc32_incremental_matches_whole();

    test_offer_new_file_no_collision();
    test_offer_collides_with_complete_file();
    test_offer_collides_with_in_progress_transfer();
    test_offer_same_key_resumes_same_final_name();
    test_offer_dotfile_not_treated_as_pure_extension();

    test_queue_basic_progress();
    test_queue_aggregate_percent();
    test_queue_unsized_row_excluded_from_aggregate();
    test_queue_empty_aggregate_is_zero_not_nan();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("PROTOCOL-TEST: FAIL\n");
        return 1;
    }
    printf("PROTOCOL-TEST: PASS\n");
    return 0;
}
