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

    ef_close(db);

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
