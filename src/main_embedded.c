#include "endfields.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void expect_true(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_failures;
    }
}

static void expect_err(enum ef_err got, enum ef_err want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %s, want %s)\n",
                msg, ef_strerror(got), ef_strerror(want));
        ++g_failures;
    }
}

int main(void)
{
    static uint8_t arena[64 + 32 * 64];
    struct ef_db *db = NULL;
    struct ef_slot *end;
    uint64_t id0 = 0;
    uint64_t id1 = 0;
    uint32_t hops = 0;
    enum ef_err err;

    printf("embedded platform: %s\n", ef_platform_name());

    err = ef_open_memory(arena, sizeof(arena), 32, 1, &db);
    expect_err(err, EF_OK, "ef_open_memory init");
    if (db == NULL) {
        return 1;
    }

    expect_true(db->backend == EF_BACKEND_MEMORY, "memory backend");
    expect_true(ef_count_free_slots(db) == 32, "initial free_count");

    err = ef_alloc_slot(db, &id0);
    expect_err(err, EF_OK, "alloc slot 0");
    err = ef_write_payload(db, id0, "head", 4);
    expect_err(err, EF_OK, "write head");

    err = ef_alloc_slot(db, &id1);
    expect_err(err, EF_OK, "alloc slot 1");
    err = ef_write_payload(db, id1, "tail", 4);
    expect_err(err, EF_OK, "write tail");

    err = ef_set_next_offset(db, id0, ef_slot_to_offset(db, id1));
    expect_err(err, EF_OK, "link head->tail");

    end = ef_chase_n(db, ef_slot_to_offset(db, id0), 1, &hops);
    expect_true(end != NULL, "chase_n one hop");
    expect_true(hops == 1, "chase_n hops done");
    expect_true(strcmp((char *)ef_slot_payload_ptr(db, end), "tail") == 0, "chase_n payload");

    expect_true(ef_get_slot(db, id0) != NULL && ef_get_slot(db, id1) != NULL, "get allocated slots");
    err = ef_set_next_offset(db, id1, ef_slot_to_offset(db, id0));
    expect_err(err, EF_OK, "embedded cycle link");
    end = ef_chase_n(db, ef_slot_to_offset(db, id0), 4, &hops);
    expect_err(ef_last_error(db), EF_ERR_CHASE_CYCLE, "cycle detected");

    err = ef_sync_ex(db, EF_SYNC_ASYNC);
    expect_err(err, EF_OK, "embedded sync noop");

    {
        uint64_t blob_id = 0;
        uint8_t blob[72];
        uint8_t out[72];
        size_t out_len = 0;
        size_t i;

        for (i = 0; i < sizeof(blob); ++i) {
            blob[i] = (uint8_t)(0xA0U + (i & 0x0FU));
        }

        err = ef_alloc_slot(db, &blob_id);
        expect_err(err, EF_OK, "embedded blob alloc");
        err = ef_write_blob(db, blob_id, blob, sizeof(blob));
        expect_err(err, EF_OK, "embedded blob write");
        err = ef_read_blob(db, blob_id, out, sizeof(out), &out_len);
        expect_err(err, EF_OK, "embedded blob read");
        expect_true(out_len == sizeof(blob), "embedded blob size");
        expect_true(memcmp(blob, out, sizeof(blob)) == 0, "embedded blob bytes");
    }

    ef_close(db);
    db = NULL;

    {
        static uint8_t grow_arena[64 + 16 * 64];
        uint64_t seed_id = 0;
        uint64_t grown_id = 0;

        err = ef_open_memory(grow_arena, sizeof(grow_arena), 4, 1, &db);
        expect_err(err, EF_OK, "embedded grow open");
        if (db != NULL) {
            err = ef_alloc_slot(db, &seed_id);
            expect_err(err, EF_OK, "embedded grow alloc");
            err = ef_write_payload(db, seed_id, "grown", 5);
            expect_err(err, EF_OK, "embedded grow write");
            err = ef_grow(db, 12);
            expect_err(err, EF_OK, "embedded grow expand");
            expect_true(db->sb->max_slots == 12, "embedded grow max_slots");
            err = ef_alloc_slot(db, &grown_id);
            expect_err(err, EF_OK, "embedded grow alloc new");
            expect_true(grown_id == 11, "embedded grow new slot id");
            ef_close(db);

            err = ef_open_memory(grow_arena, sizeof(grow_arena), 4, 0, &db);
            expect_err(err, EF_OK, "embedded grow reopen");
            if (db != NULL) {
                expect_true(db->sb->max_slots == 12, "embedded grow persisted");
                expect_true(strcmp((char *)ef_slot_payload_ptr(db, ef_get_slot(db, seed_id)), "grown") == 0,
                            "embedded grow persisted seed");
                ef_close(db);
            }
        }
    }

    err = ef_open_memory(arena, sizeof(arena), 32, 0, &db);
    expect_err(err, EF_OK, "ef_open_memory reopen");
    if (db != NULL) {
        expect_true(strcmp((char *)ef_slot_payload_ptr(db, ef_get_slot(db, id0)), "head") == 0,
                    "persisted in RAM");
        ef_close(db);
    }

    if (g_failures != 0) {
        fprintf(stderr, "%d embedded test(s) failed\n", g_failures);
        return 1;
    }

    printf("Embedded tests passed.\n");
    return 0;
}
