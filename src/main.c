#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "endfields.h"
#include "ef_config.h"
#include "ef_index.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

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

static int main_io_read(FILE *fp, void *buf, size_t nbytes)
{
    if (fp == NULL || buf == NULL) {
        return 0;
    }
    return fread(buf, 1, nbytes, fp) == nbytes;
}

static int main_io_write(FILE *fp, const void *buf, size_t nbytes)
{
    if (fp == NULL) {
        return 0;
    }
    if (nbytes == 0) {
        return 1;
    }
    if (buf == NULL) {
        return 0;
    }
    return fwrite(buf, 1, nbytes, fp) == nbytes;
}

static double now_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency = {0};
    LARGE_INTEGER counter;

    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void remove_test_file(const char *path)
{
    remove(path);
}

static void test_offset_roundtrip(struct ef_db *db)
{
    uint64_t slot_id = 0;
    uint64_t offset;
    enum ef_err err;

    offset = ef_slot_to_offset(db, 50);
    expect_true(offset == (uint64_t)(sizeof(struct ef_superblock) + 50 * sizeof(struct ef_slot)),
                "ef_slot_to_offset(50)");

    err = ef_offset_to_slot_id(db, offset, &slot_id);
    expect_err(err, EF_OK, "ef_offset_to_slot_id valid offset");
    expect_true(slot_id == 50, "roundtrip slot_id == 50");

    err = ef_offset_to_slot_id(db, 63, &slot_id);
    expect_err(err, EF_ERR_OFFSET, "ef_offset_to_slot_id misaligned offset");

    expect_true(ef_slot_to_offset(db, 9999) == 0, "ef_slot_to_offset out of range");
}

static void test_write_read_api(struct ef_db *db)
{
    const char *text = "Hello Endfields";
    struct ef_slot *slot;
    enum ef_err err;

    err = ef_write_payload(db, 3, text, (uint8_t)strlen(text));
    expect_err(err, EF_OK, "ef_write_payload");

    slot = ef_get_slot(db, 3);
    expect_true(slot != NULL, "ef_get_slot after write");
    expect_true(slot->status == EF_STATUS_USED, "status auto-set to USED");
    expect_true(strcmp(slot->payload, text) == 0, "payload content");

    err = ef_write_payload(db, 3, NULL, 0);
    expect_err(err, EF_OK, "ef_write_payload zero-length clears");

    slot = ef_get_slot(db, 3);
    expect_true(slot->payload[0] == '\0', "payload cleared");
}

static void test_write_execute(struct ef_db *db)
{
    struct ef_cmd cmd;
    struct ef_slot *slot;
    const char *text = "Written via opcode";
    uint64_t next = ef_slot_to_offset(db, 7);
    uint32_t status = EF_STATUS_USED;
    uint8_t byte = 0xAB;

    cmd.opcode = EF_OP_WRITE_PAYLOAD;
    cmd.param = 5;
    cmd.field_offset = (uint8_t)strlen(text);
    slot = (struct ef_slot *)ef_execute(db, &cmd, text);
    expect_true(slot != NULL, "EF_OP_WRITE_PAYLOAD result");
    expect_true(strcmp(slot->payload, text) == 0, "EF_OP_WRITE_PAYLOAD content");

    cmd.opcode = EF_OP_SET_NEXT;
    cmd.param = 5;
    cmd.field_offset = 0;
    slot = (struct ef_slot *)ef_execute(db, &cmd, &next);
    expect_true(slot != NULL, "EF_OP_SET_NEXT result");
    expect_true(slot->next_offset == next, "EF_OP_SET_NEXT value");

    cmd.opcode = EF_OP_SET_STATUS;
    cmd.param = 8;
    cmd.field_offset = 0;
    slot = (struct ef_slot *)ef_execute(db, &cmd, &status);
    expect_true(slot != NULL, "EF_OP_SET_STATUS result");
    expect_true(slot->status == EF_STATUS_USED, "EF_OP_SET_STATUS value");

    cmd.opcode = EF_OP_WRITE_FIELD;
    cmd.param = 8;
    cmd.field_offset = 8;
    expect_true(ef_execute(db, &cmd, &byte) != NULL, "EF_OP_WRITE_FIELD result");
    slot = ef_get_slot(db, 8);
    expect_true(slot != NULL && ((uint8_t *)ef_slot_payload_ptr(db, slot))[0] == 0xAB,
                "EF_OP_WRITE_FIELD byte");
}

static void test_chase_loop(struct ef_db *db)
{
    struct ef_cmd chase_cmd;
    struct ef_slot *slot0;
    struct ef_slot *slot50;
    struct ef_slot *chased;
    uint64_t slot0_offset;
    uint64_t slot50_offset;
    enum ef_err err;

    slot0 = ef_get_slot(db, 0);
    slot50 = ef_get_slot(db, 50);
    expect_true(slot0 != NULL && slot50 != NULL, "setup chase slots");

    err = ef_write_payload(db, 0, "Root Node", 9);
    expect_err(err, EF_OK, "write slot0 payload");

    slot0_offset = ef_slot_to_offset(db, 0);
    slot50_offset = ef_slot_to_offset(db, 50);

    err = ef_set_next_offset(db, 0, slot50_offset);
    expect_err(err, EF_OK, "set slot0 next");

    err = ef_write_payload(db, 50, "Chased Target Node", 18);
    expect_err(err, EF_OK, "write slot50 payload");

    chase_cmd.opcode = EF_OP_CHASE;
    chase_cmd.param = slot0_offset;
    chase_cmd.field_offset = 0;

    chased = (struct ef_slot *)ef_execute(db, &chase_cmd, NULL);
    expect_true(chased != NULL, "chase result");
    expect_true(strcmp(chased->payload, "Chased Target Node") == 0, "chase payload");

    printf("Chase test passed: payload = \"%s\"\n", chased->payload);
    printf("Slot 0 offset = %llu, Slot 50 offset = %llu\n",
           (unsigned long long)slot0_offset,
           (unsigned long long)slot50_offset);
}

static void test_chase_n(struct ef_db *db)
{
    struct ef_cmd cmd;
    struct ef_slot *slot;
    uint64_t off_a;
    uint64_t off_b;
    uint64_t off_c;
    uint32_t hops = 0;
    uint32_t hops_param = 2;
    enum ef_err err;

    err = ef_write_payload(db, 10, "A", 1);
    expect_err(err, EF_OK, "chase_n write A");
    err = ef_write_payload(db, 11, "B", 1);
    expect_err(err, EF_OK, "chase_n write B");
    err = ef_write_payload(db, 12, "C", 1);
    expect_err(err, EF_OK, "chase_n write C");

    off_a = ef_slot_to_offset(db, 10);
    off_b = ef_slot_to_offset(db, 11);
    off_c = ef_slot_to_offset(db, 12);

    err = ef_set_next_offset(db, 10, off_b);
    expect_err(err, EF_OK, "chase_n link A->B");
    err = ef_set_next_offset(db, 11, off_c);
    expect_err(err, EF_OK, "chase_n link B->C");

    slot = ef_chase_n(db, off_a, 2, &hops);
    expect_true(slot != NULL, "ef_chase_n two hops");
    expect_true(hops == 2, "ef_chase_n hops count");
    expect_true(slot->payload[0] == 'C', "ef_chase_n lands on C");

    cmd.opcode = EF_OP_CHASE_N;
    cmd.param = off_a;
    cmd.field_offset = 2;
    slot = (struct ef_slot *)ef_execute(db, &cmd, NULL);
    expect_true(slot != NULL && slot->payload[0] == 'C', "EF_OP_CHASE_N");

    cmd.field_offset = 0;
    cmd.param = off_a;
    slot = (struct ef_slot *)ef_execute(db, &cmd, &hops_param);
    expect_true(slot != NULL && slot->payload[0] == 'C', "EF_OP_CHASE_N via aux hops");

    err = ef_set_next_offset(db, 10, off_b);
    expect_err(err, EF_OK, "cycle link A->B");
    err = ef_set_next_offset(db, 11, off_c);
    expect_err(err, EF_OK, "cycle link B->C");
    err = ef_set_next_offset(db, 12, off_a);
    expect_err(err, EF_OK, "cycle link C->A");
    slot = ef_chase_n(db, off_a, 8, &hops);
    expect_err(ef_last_error(db), EF_ERR_CHASE_CYCLE, "chase_n detects cycle");
    (void)slot;
}

static void test_memory_backend(void)
{
    static uint8_t buf[64 + 16 * 64];
    struct ef_db *db = NULL;
    uint64_t sid = 0;
    enum ef_err err;

    err = ef_open_memory(buf, sizeof(buf), 16, 1, &db);
    expect_err(err, EF_OK, "memory backend open");
    if (db == NULL) {
        return;
    }

    expect_true(db->backend == EF_BACKEND_MEMORY, "backend is memory");
    expect_true(db->sb->schema_version == EF_SCHEMA_VERSION, "schema version set");
    expect_true(ef_count_free_slots(db) == 16, "memory free_count");

    err = ef_alloc_slot(db, &sid);
    expect_err(err, EF_OK, "memory alloc");
    err = ef_write_payload(db, sid, "ram", 3);
    expect_err(err, EF_OK, "memory write");

    ef_close(db);

    err = ef_open_memory(buf, sizeof(buf), 16, 0, &db);
    expect_err(err, EF_OK, "memory reopen");
    if (db != NULL) {
        expect_true(strcmp(ef_get_slot(db, sid)->payload, "ram") == 0, "memory data kept");
        ef_close(db);
    }
}

static int count_used_visit(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot, void *ctx)
{
    int *count = (int *)ctx;
    (void)db;
    (void)slot_id;
    (void)slot;
    ++*count;
    return 1;
}

static void test_slot_iterator_memory(void)
{
    static uint8_t buf[64 + 32 * 64];
    struct ef_db *db = NULL;
    struct ef_slot_iter it;
    uint64_t id = 0;
    uint64_t found_id = 0;
    struct ef_slot *found_slot = NULL;
    int visit_count = 0;
    int iter_count = 0;
    enum ef_err err;

    err = ef_open_memory(buf, sizeof(buf), 32, 1, &db);
    expect_err(err, EF_OK, "iterator memory open");
    if (db == NULL) {
        return;
    }

    err = ef_alloc_slot(db, &id);
    expect_err(err, EF_OK, "iterator alloc a");
    err = ef_write_payload(db, id, "A", 1);
    expect_err(err, EF_OK, "iterator write a");

    err = ef_alloc_slot(db, &id);
    expect_err(err, EF_OK, "iterator alloc b");
    err = ef_write_payload(db, id, "B", 1);
    expect_err(err, EF_OK, "iterator write b");

    err = ef_foreach_used(db, count_used_visit, &visit_count);
    expect_err(err, EF_OK, "ef_foreach_used");
    expect_true(visit_count == 2, "foreach sees two used slots");

    ef_slot_iter_init(db, &it);
    while (ef_slot_iter_next(&it, &found_id, &found_slot) == 1) {
        ++iter_count;
        expect_true(found_slot != NULL, "iter slot pointer");
        expect_true(found_slot->status == EF_STATUS_USED, "iter slot used");
    }
    expect_true(iter_count == 2, "slot_iter sees two used slots");

    ef_close(db);
}

static void test_blob_memory(void)
{
    static uint8_t buf[64 + 64 * 64];
    struct ef_db *db = NULL;
    uint64_t id = 0;
    uint8_t blob[120];
    uint8_t out[120];
    size_t out_len = 0;
    int visit_count = 0;
    size_t i;
    enum ef_err err;

    for (i = 0; i < sizeof(blob); ++i) {
        blob[i] = (uint8_t)(i & 0xFFU);
    }

    err = ef_open_memory(buf, sizeof(buf), 64, 1, &db);
    expect_err(err, EF_OK, "blob memory open");
    if (db == NULL) {
        return;
    }

    expect_true(ef_blob_inline_capacity(db) == 40, "blob inline capacity");

    err = ef_alloc_slot(db, &id);
    expect_err(err, EF_OK, "blob alloc head");
    expect_true(ef_count_free_slots(db) == 63, "one slot allocated");

    err = ef_write_blob(db, id, blob, sizeof(blob));
    expect_err(err, EF_OK, "blob write overflow chain");
    expect_true(ef_blob_size(db, id) == sizeof(blob), "blob stored size");

    memset(out, 0, sizeof(out));
    err = ef_read_blob(db, id, out, sizeof(out), &out_len);
    expect_err(err, EF_OK, "blob read back");
    expect_true(out_len == sizeof(blob), "blob read length");
    expect_true(memcmp(blob, out, sizeof(blob)) == 0, "blob payload match");

    visit_count = 0;
    err = ef_foreach_used(db, count_used_visit, &visit_count);
    expect_err(err, EF_OK, "blob foreach");
    expect_true(visit_count == 1, "blob iterator skips overflow slots");

    err = ef_free_slot(db, id);
    expect_err(err, EF_OK, "blob free releases chain");
    expect_true(ef_count_free_slots(db) == 64, "blob free restores all slots");

    ef_close(db);
}

static void test_v1_upgrade_memory(void)
{
    static uint8_t buf[64 + 8 * 64];
    struct ef_superblock *sb;
    struct ef_slot *slots;
    struct ef_db *db = NULL;
    struct ef_slot *slot;
    uint8_t legacy[EF_PAYLOAD_SIZE_LEGACY];
    size_t i;
    enum ef_err err;

    memset(buf, 0, sizeof(buf));
    sb = (struct ef_superblock *)buf;
    sb->magic[0] = EF_MAGIC_0;
    sb->magic[1] = EF_MAGIC_1;
    sb->magic[2] = EF_MAGIC_2;
    sb->magic[3] = EF_MAGIC_3;
    sb->slot_size = EF_SLOT_SIZE;
    sb->max_slots = 8;
    sb->free_list_head = (uint64_t)sizeof(struct ef_superblock);
    sb->schema_version = 1;
    sb->flags = EF_FLAG_NONE;
    sb->free_count = 7;

    slots = (struct ef_slot *)(buf + sizeof(struct ef_superblock));
    for (i = 0; i < sizeof(legacy); ++i) {
        legacy[i] = (uint8_t)i;
    }
    slots[0].status = EF_STATUS_USED;
    memcpy(&slots[0].header_crc, legacy, sizeof(legacy));
    for (i = 1; i < 8; ++i) {
        slots[i].status = EF_STATUS_FREE;
    }

    err = ef_open_memory(buf, sizeof(buf), 8, 0, &db);
    expect_err(err, EF_OK, "open legacy v1 image");
    if (db == NULL) {
        return;
    }

    expect_true(ef_needs_upgrade(db), "legacy needs upgrade");
    err = ef_upgrade(db);
    expect_err(err, EF_OK, "ef_upgrade v1 to v2");
    expect_true(db->sb->schema_version == EF_SCHEMA_VERSION, "upgraded schema version");
    expect_true(ef_needs_upgrade(db) == 0, "no further upgrade needed");

    slot = ef_get_slot(db, 0);
    expect_true(slot != NULL, "upgraded slot passes crc");
    for (i = 0; i < EF_PAYLOAD_SIZE; ++i) {
        char msg[64];
        snprintf(msg, sizeof(msg), "legacy byte %u migrated", (unsigned)i);
        expect_true(((uint8_t *)ef_slot_payload_ptr(db, slot))[i] == (uint8_t)i, msg);
    }

    ef_close(db);

    err = ef_open_memory(buf, sizeof(buf), 8, 0, &db);
    expect_err(err, EF_OK, "reopen upgraded v2 image");
    if (db != NULL) {
        expect_true(ef_get_slot(db, 0) != NULL, "v2 reopen validates slot crc");
        ef_close(db);
    }
}

static void test_grow_memory(void)
{
    static uint8_t arena[64 + 16 * 64];
    struct ef_db *db = NULL;
    uint64_t seed_id = 0;
    uint64_t grown_id = 0;
    uint64_t i;
    enum ef_err err;

    err = ef_open_memory(arena, sizeof(arena), 4, 1, &db);
    expect_err(err, EF_OK, "grow memory open");
    if (db == NULL) {
        return;
    }

    expect_true(db->backend == EF_BACKEND_MEMORY, "grow memory backend");
    expect_true(db->sb->max_slots == 4, "grow memory initial max_slots");
    expect_true(db->file_size == sizeof(struct ef_superblock) + 4 * sizeof(struct ef_slot),
                "grow memory initial file_size");
    expect_true(db->map_capacity == sizeof(arena), "grow memory map_capacity");

    err = ef_alloc_slot(db, &seed_id);
    expect_err(err, EF_OK, "grow memory alloc seed");
    err = ef_write_payload(db, seed_id, "grow-seed", 9);
    expect_err(err, EF_OK, "grow memory write seed");
    expect_true(ef_count_free_slots(db) == 3, "grow memory one slot used");

    err = ef_grow(db, 3);
    expect_err(err, EF_ERR_GROW, "grow memory reject non-increase");
    err = ef_grow(db, 64);
    expect_err(err, EF_ERR_FILE_SIZE, "grow memory reject over map_capacity");

    err = ef_grow(db, 8);
    expect_err(err, EF_OK, "grow memory 4 to 8");
    expect_true(db->sb->max_slots == 8, "grow memory max_slots 8");
    expect_true(db->file_size == sizeof(struct ef_superblock) + 8 * sizeof(struct ef_slot),
                "grow memory file_size 8");
    expect_true(ef_count_free_slots(db) == 7, "grow memory free count after grow");
    expect_true(strcmp((char *)ef_slot_payload_ptr(db, ef_get_slot(db, seed_id)), "grow-seed") == 0,
                "grow memory seed preserved");

    err = ef_alloc_slot(db, &grown_id);
    expect_err(err, EF_OK, "grow memory alloc in new region");
    expect_true(grown_id == 7, "grow memory first new slot id (LIFO free list)");
    err = ef_write_payload(db, grown_id, "new-slot", 8);
    expect_err(err, EF_OK, "grow memory write new slot");

    err = ef_grow(db, 16);
    expect_err(err, EF_OK, "grow memory 8 to 16");
    expect_true(db->sb->max_slots == 16, "grow memory max_slots 16");
    expect_true(ef_count_free_slots(db) == 14, "grow memory free count after second grow");

    for (i = 8; i < 16; ++i) {
        expect_true(db->slots[i].status == EF_STATUS_FREE, "grow memory appended slot free");
    }

    ef_close(db);

    err = ef_open_memory(arena, sizeof(arena), 4, 0, &db);
    expect_err(err, EF_OK, "grow memory reopen detects grown size");
    if (db != NULL) {
        expect_true(db->sb->max_slots == 16, "grow memory persisted max_slots");
        expect_true(strcmp((char *)ef_slot_payload_ptr(db, ef_get_slot(db, seed_id)), "grow-seed") == 0,
                    "grow memory persisted seed");
        expect_true(strcmp((char *)ef_slot_payload_ptr(db, ef_get_slot(db, grown_id)), "new-slot") == 0,
                    "grow memory persisted grown slot");
        ef_close(db);
    }
}

static int crc_foreach_fail_on_bad(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot, void *ctx)
{
    (void)db;
    (void)slot_id;
    (void)slot;
    *(int *)ctx += 1;
    return 1;
}

static void test_slot_header_crc_memory(void)
{
    static uint8_t buf[64 + 32 * 64];
    struct ef_db *db = NULL;
    struct ef_slot *slot;
    struct ef_slot *ov;
    uint64_t id_tamper = 0;
    uint64_t id_zero = 0;
    uint64_t id_blob = 0;
    uint64_t ov_id = 0;
    uint8_t blob[100];
    uint8_t out[100];
    size_t out_len = 0;
    size_t i;
    enum ef_err err;

    err = ef_open_memory(buf, sizeof(buf), 32, 1, &db);
    expect_err(err, EF_OK, "slot crc test open");
    if (db == NULL) {
        return;
    }

    err = ef_alloc_slot(db, &id_tamper);
    expect_err(err, EF_OK, "slot crc alloc tamper");
    err = ef_write_payload(db, id_tamper, "crc-me", 6);
    expect_err(err, EF_OK, "slot crc write");

    slot = ef_get_slot(db, id_tamper);
    expect_true(slot != NULL, "slot crc read before tamper");
    if (slot != NULL) {
        slot->header_crc ^= 0xA5A5A5A5U;
        expect_true(ef_get_slot(db, id_tamper) == NULL, "reject tampered header_crc");
        expect_err(ef_last_error(db), EF_ERR_BAD_CHECKSUM, "tampered header_crc error");
    }

    err = ef_alloc_slot(db, &id_zero);
    expect_err(err, EF_OK, "slot crc alloc zero");
    err = ef_write_payload(db, id_zero, "zero", 4);
    expect_err(err, EF_OK, "slot crc write zero case");
    slot = ef_get_slot(db, id_zero);
    expect_true(slot != NULL, "slot crc before zero tamper");
    if (slot != NULL) {
        slot->header_crc = 0;
        expect_true(ef_get_slot(db, id_zero) == NULL, "reject zero header_crc");
        expect_err(ef_last_error(db), EF_ERR_BAD_CHECKSUM, "zero header_crc error");
    }

    for (i = 0; i < sizeof(blob); ++i) {
        blob[i] = (uint8_t)(0xC0U + (i & 0x1FU));
    }
    err = ef_alloc_slot(db, &id_blob);
    expect_err(err, EF_OK, "overflow crc alloc head");
    err = ef_write_blob(db, id_blob, blob, sizeof(blob));
    expect_err(err, EF_OK, "overflow crc write blob");

    expect_true(db->slots[id_blob].next_offset != 0, "blob has overflow chain");
    err = ef_offset_to_slot_id(db, db->slots[id_blob].next_offset, &ov_id);
    expect_err(err, EF_OK, "overflow slot id");
    ov = db->slots + ov_id;
    expect_true(ov->status == EF_STATUS_OVERFLOW, "overflow slot status");
    ov->header_crc ^= 0x0F0F0F0FU;
    err = ef_read_blob(db, id_blob, out, sizeof(out), &out_len);
    expect_err(err, EF_ERR_BAD_CHECKSUM, "reject tampered overflow header_crc");

    {
        uint64_t id_status = 0;
        uint64_t id_next = 0;
        uint64_t id_free = 0;
        struct ef_slot_iter it;
        int visit_count = 0;
        int iter_rc;

        err = ef_alloc_slot(db, &id_status);
        expect_err(err, EF_OK, "slot crc alloc status");
        err = ef_write_payload(db, id_status, "status", 6);
        expect_err(err, EF_OK, "slot crc write status");
        slot = ef_get_slot(db, id_status);
        if (slot != NULL) {
            slot->status = EF_STATUS_OVERFLOW;
            expect_true(ef_get_slot(db, id_status) == NULL, "reject tampered status");
            expect_err(ef_last_error(db), EF_ERR_BAD_CHECKSUM, "tampered status error");
        }

        err = ef_alloc_slot(db, &id_next);
        expect_err(err, EF_OK, "slot crc alloc next");
        err = ef_write_payload(db, id_next, "next", 4);
        expect_err(err, EF_OK, "slot crc write next");
        slot = ef_get_slot(db, id_next);
        if (slot != NULL) {
            slot->next_offset ^= 0x00FF00FF00FF00FFULL;
            expect_true(ef_get_slot(db, id_next) == NULL, "reject tampered next_offset");
            expect_err(ef_last_error(db), EF_ERR_BAD_CHECKSUM, "tampered next_offset error");
        }

        err = ef_alloc_slot(db, &id_free);
        expect_err(err, EF_OK, "slot crc alloc free probe");
        err = ef_free_slot(db, id_free);
        expect_err(err, EF_OK, "slot crc free probe");
        expect_true(ef_get_slot(db, id_free) != NULL, "free slot skips crc verify");
        expect_true(db->slots[id_free].status == EF_STATUS_FREE, "free slot status readable");

        db->slots[id_tamper].header_crc ^= 0x11111111U;
        visit_count = 0;
        err = ef_foreach_used(db, crc_foreach_fail_on_bad, &visit_count);
        expect_err(err, EF_ERR_BAD_CHECKSUM, "foreach aborts on bad slot crc");

        db->slots[id_tamper].header_crc ^= 0x11111111U;
        ef_slot_iter_init(db, &it);
        iter_rc = 0;
        while ((iter_rc = ef_slot_iter_next(&it, NULL, NULL)) == 1) {
        }
        expect_true(iter_rc == -1, "slot_iter aborts on bad slot crc");
        expect_err(ef_last_error(db), EF_ERR_BAD_CHECKSUM, "slot_iter bad crc error");
    }

    ef_close(db);
}

#if EF_HAS_FILE_IO
static void test_blob_file(void)
{
    struct ef_db *db = NULL;
    uint64_t id = 0;
    char text[80];
    char out[80];
    size_t out_len = 0;
    size_t i;
    enum ef_err err;

    remove_test_file("test_blob.endf");

    for (i = 0; i < sizeof(text); ++i) {
        text[i] = (char)('A' + (i % 26));
    }

    err = ef_open_ex("test_blob.endf", 16, &db);
    expect_err(err, EF_OK, "blob file open");
    if (db == NULL) {
        return;
    }

    err = ef_alloc_slot(db, &id);
    expect_err(err, EF_OK, "blob file alloc");
    err = ef_write_blob(db, id, text, sizeof(text));
    expect_err(err, EF_OK, "blob file write");
    ef_close(db);

    err = ef_open_ex("test_blob.endf", 16, &db);
    expect_err(err, EF_OK, "blob file reopen");
    if (db == NULL) {
        return;
    }

    err = ef_read_blob(db, id, out, sizeof(out), &out_len);
    expect_err(err, EF_OK, "blob file read");
    expect_true(out_len == sizeof(text), "blob file length");
    expect_true(memcmp(text, out, sizeof(text)) == 0, "blob file persisted");

    ef_close(db);
    remove_test_file("test_blob.endf");
}
#endif

static void test_reopen_existing(void)
{
    struct ef_db *db = NULL;
    struct ef_slot *slot;
    enum ef_err err;

    err = ef_open_ex("test_rw.endf", 16, &db);
    expect_err(err, EF_OK, "create db for reopen test");
    if (db == NULL) {
        return;
    }

    err = ef_write_payload(db, 2, "persist me", 10);
    expect_err(err, EF_OK, "persist payload");
    ef_close(db);

    err = ef_open_ex("test_rw.endf", 16, &db);
    expect_err(err, EF_OK, "reopen existing db");
    if (db == NULL) {
        return;
    }

    expect_true(db->sb->max_slots == 16, "reopen preserves max_slots");

    slot = ef_get_slot(db, 2);
    expect_true(slot != NULL, "reopen get slot");
    expect_true(strcmp(slot->payload, "persist me") == 0, "reopen payload persisted");
    expect_true(slot->status == EF_STATUS_USED, "reopen status persisted");

    ef_close(db);
}

static void test_bad_magic(void)
{
    struct ef_db *db = NULL;
    enum ef_err err;
    FILE *fp;
    size_t file_size = (size_t)(sizeof(struct ef_superblock) + sizeof(struct ef_slot));
    uint8_t *image;

    image = (uint8_t *)calloc(1, file_size);
    if (image == NULL) {
        fprintf(stderr, "FAIL: cannot allocate bad magic image\n");
        ++g_failures;
        return;
    }

    fp = fopen("bad_magic.endf", "wb");
    if (fp == NULL) {
        fprintf(stderr, "FAIL: cannot create bad_magic.endf\n");
        ++g_failures;
        free(image);
        return;
    }

    if (!main_io_write(fp, image, file_size)) {
        fprintf(stderr, "FAIL: write bad_magic.endf\n");
        ++g_failures;
    }
    fclose(fp);
    free(image);

    err = ef_open_ex("bad_magic.endf", 1, &db);
    expect_err(err, EF_ERR_BAD_MAGIC, "reject bad magic");
    expect_true(db == NULL, "bad magic returns null db");

    remove_test_file("bad_magic.endf");
}

static void test_error_paths(struct ef_db *db)
{
    struct ef_cmd cmd;
    uint64_t slot_id;
    enum ef_err err;

    expect_true(ef_get_slot(db, db->sb->max_slots) == NULL, "get_slot oob");
    expect_err(ef_last_error(db), EF_ERR_SLOT_ID, "get_slot oob error");

    err = ef_write_payload(db, 0, "x", 53);
    expect_err(err, EF_ERR_PAYLOAD_LEN, "payload too long");

    err = ef_set_next_offset(db, 0, 65);
    expect_err(err, EF_ERR_OFFSET, "invalid next_offset");

    cmd.opcode = 0xFF;
    cmd.param = 0;
    cmd.field_offset = 0;
    expect_true(ef_execute(db, &cmd, NULL) == NULL, "invalid opcode");
    expect_err(ef_last_error(db), EF_ERR_OPCODE, "invalid opcode error");

    err = ef_offset_to_slot_id(db, ef_slot_to_offset(db, 1) + 1, &slot_id);
    expect_err(err, EF_ERR_OFFSET, "misaligned offset to slot_id");
}

static void test_alloc_free(void)
{
    struct ef_db *db = NULL;
    struct ef_cmd cmd;
    struct ef_slot *slot;
    uint64_t id_a = 0;
    uint64_t id_b = 0;
    uint64_t id_c = 0;
    uint64_t i;
    enum ef_err err;

    remove_test_file("test_alloc.endf");

    err = ef_open_ex("test_alloc.endf", 8, &db);
    expect_err(err, EF_OK, "open alloc test db");
    if (db == NULL) {
        return;
    }

    expect_true(ef_count_free_slots(db) == 8, "all slots start free");
    expect_true(db->sb->free_list_head == ef_slot_to_offset(db, 0), "free list head at slot 0");

    err = ef_alloc_slot(db, &id_a);
    expect_err(err, EF_OK, "alloc first slot");
    expect_true(id_a == 0, "first alloc returns slot 0");
    expect_true(ef_count_free_slots(db) == 7, "one slot allocated");

    err = ef_write_payload(db, id_a, "node-a", 6);
    expect_err(err, EF_OK, "write allocated slot");

    err = ef_alloc_slot(db, &id_b);
    expect_err(err, EF_OK, "alloc second slot");
    expect_true(id_b == 1, "second alloc returns slot 1");

    slot = ef_alloc_slot_ptr(db, &id_c);
    expect_true(slot != NULL, "alloc_slot_ptr");
    expect_true(id_c == 2, "third alloc returns slot 2");

    err = ef_free_slot(db, id_b);
    expect_err(err, EF_OK, "free slot 1");
    expect_true(ef_count_free_slots(db) == 6, "free restores pool count");

    err = ef_alloc_slot(db, &id_b);
    expect_err(err, EF_OK, "realloc after free");
    expect_true(id_b == 1, "freed slot 1 reused first");

    err = ef_free_slot(db, id_b);
    expect_err(err, EF_OK, "free slot 1 again");
    err = ef_free_slot(db, id_b);
    expect_err(err, EF_ERR_SLOT_FREE, "double free rejected");

    for (i = 0; i < 6; ++i) {
        err = ef_alloc_slot(db, &id_b);
        expect_err(err, EF_OK, "alloc until nearly full");
    }

    err = ef_alloc_slot(db, &id_b);
    expect_err(err, EF_ERR_SLOT_FULL, "pool exhausted");

    cmd.opcode = EF_OP_FREE;
    cmd.param = id_a;
    cmd.field_offset = 0;
    expect_true(ef_execute(db, &cmd, NULL) == NULL, "EF_OP_FREE");
    expect_err(ef_last_error(db), EF_OK, "EF_OP_FREE error code");

    cmd.opcode = EF_OP_ALLOC;
    cmd.param = 0;
    cmd.field_offset = 0;
    id_a = 999;
    slot = (struct ef_slot *)ef_execute(db, &cmd, &id_a);
    expect_true(slot != NULL, "EF_OP_ALLOC");
    expect_true(id_a == 0, "EF_OP_ALLOC returns slot 0");

    ef_close(db);

    err = ef_open_ex("test_alloc.endf", 8, &db);
    expect_err(err, EF_OK, "reopen alloc db rebuilds free list");
    if (db != NULL) {
        expect_true(ef_count_free_slots(db) < 8, "reopen reflects used slots");
        ef_close(db);
    }

    remove_test_file("test_alloc.endf");
}

#if EF_HAS_FILE_IO
static void test_grow(void)
{
    struct ef_db *db = NULL;
    uint64_t id = 0;
    enum ef_err err;

    remove_test_file("test_grow.endf");

    err = ef_open_ex("test_grow.endf", 4, &db);
    expect_err(err, EF_OK, "open grow db");
    if (db == NULL) {
        return;
    }

    expect_true(db->sb->schema_version == EF_SCHEMA_VERSION, "grow db schema v2");
    expect_true((db->sb->flags & EF_FLAG_SB_CRC) != 0, "superblock crc enabled");

    err = ef_alloc_slot(db, &id);
    expect_err(err, EF_OK, "alloc before grow");
    err = ef_write_payload(db, id, "seed", 4);
    expect_err(err, EF_OK, "write before grow");

    err = ef_grow(db, 8);
    expect_err(err, EF_OK, "ef_grow to 8 slots");
    expect_true(db->sb->max_slots == 8, "max_slots after grow");
    expect_true(ef_count_free_slots(db) == 7, "free count after grow");

    err = ef_alloc_slot(db, &id);
    expect_err(err, EF_OK, "alloc after grow");

    ef_close(db);

    err = ef_open_ex("test_grow.endf", 4, &db);
    expect_err(err, EF_OK, "reopen grown db");
    if (db != NULL) {
        expect_true(db->sb->max_slots == 8, "persisted max_slots");
        ef_close(db);
    }

    remove_test_file("test_grow.endf");
}

static void test_readonly_open(void)
{
    struct ef_db *db = NULL;
    struct ef_db *ro = NULL;
    enum ef_err err;

    remove_test_file("test_ro.endf");

    err = ef_open_ex("test_ro.endf", 4, &db);
    expect_err(err, EF_OK, "create rw db");
    if (db == NULL) {
        return;
    }

    err = ef_write_payload(db, 0, "frozen", 6);
    expect_err(err, EF_OK, "seed rw db");
    ef_close(db);

    ro = ef_open_readonly("test_ro.endf");
    expect_true(ro != NULL, "open readonly");
    if (ro != NULL) {
        expect_true(ef_is_readonly(ro), "readonly flag");
        expect_true(strcmp((char *)ef_slot_payload_ptr(ro, ef_get_slot(ro, 0)), "frozen") == 0,
                    "readonly read payload");
        err = ef_write_payload(ro, 1, "nope", 4);
        expect_err(err, EF_ERR_READONLY, "readonly blocks write");
        ef_close(ro);
    }

    remove_test_file("test_ro.endf");
}

static void test_superblock_checksum_reject(void)
{
    struct ef_db *db = NULL;
    enum ef_err err;
    FILE *fp;
    long pos;
    uint8_t byte;

    remove_test_file("test_crc.endf");
    err = ef_open_ex("test_crc.endf", 2, &db);
    expect_err(err, EF_OK, "create crc db");
    if (db != NULL) {
        ef_close(db);
    }

    fp = fopen("test_crc.endf", "r+b");
    if (fp == NULL) {
        fprintf(stderr, "FAIL: open test_crc.endf for tamper\n");
        ++g_failures;
        return;
    }

    pos = (long)offsetof(struct ef_superblock, reserved);
    fseek(fp, pos, SEEK_SET);
    if (!main_io_read(fp, &byte, 1)) {
        fprintf(stderr, "FAIL: read superblock tamper byte\n");
        fclose(fp);
        ++g_failures;
        return;
    }
    fseek(fp, pos, SEEK_SET);
    byte ^= 0xFFU;
    if (!main_io_write(fp, &byte, 1)) {
        fprintf(stderr, "FAIL: write superblock tamper byte\n");
        fclose(fp);
        ++g_failures;
        return;
    }
    fclose(fp);

    err = ef_open_ex("test_crc.endf", 2, &db);
    expect_err(err, EF_ERR_BAD_CHECKSUM, "reject tampered superblock");
    expect_true(db == NULL, "tampered superblock null db");

    remove_test_file("test_crc.endf");
}

static void test_v1_upgrade_file(void)
{
    static uint8_t image[64 + 4 * 64];
    struct ef_superblock *sb;
    struct ef_slot *slots;
    struct ef_db *db = NULL;
    FILE *fp;
    size_t i;
    enum ef_err err;

    memset(image, 0, sizeof(image));
    sb = (struct ef_superblock *)image;
    sb->magic[0] = EF_MAGIC_0;
    sb->magic[1] = EF_MAGIC_1;
    sb->magic[2] = EF_MAGIC_2;
    sb->magic[3] = EF_MAGIC_3;
    sb->slot_size = EF_SLOT_SIZE;
    sb->max_slots = 4;
    sb->schema_version = 1;
    sb->flags = EF_FLAG_NONE;
    sb->free_count = 3;

    slots = (struct ef_slot *)(image + sizeof(struct ef_superblock));
    slots[0].status = EF_STATUS_USED;
    memcpy(&slots[0].header_crc, "legacy-v1-payload-data-ok!!", 28);
    for (i = 1; i < 4; ++i) {
        slots[i].status = EF_STATUS_FREE;
    }

    remove_test_file("test_v1.endf");
    fp = fopen("test_v1.endf", "wb");
    if (fp == NULL) {
        fprintf(stderr, "FAIL: cannot create test_v1.endf\n");
        ++g_failures;
        return;
    }
    if (!main_io_write(fp, image, sizeof(image))) {
        fprintf(stderr, "FAIL: write test_v1.endf\n");
        ++g_failures;
    }
    fclose(fp);

    err = ef_open_ex("test_v1.endf", 4, &db);
    expect_err(err, EF_OK, "open v1 file");
    if (db == NULL) {
        return;
    }

    expect_true(ef_needs_upgrade(db), "v1 file needs upgrade");
    err = ef_upgrade(db);
    expect_err(err, EF_OK, "upgrade v1 file");
    expect_true(strncmp((char *)ef_slot_payload_ptr(db, ef_get_slot(db, 0)),
                        "legacy-v1-payload-data-ok!!", 28) == 0,
                "v1 payload preserved after upgrade");
    err = ef_sync(db);
    expect_err(err, EF_OK, "sync upgraded file");
    ef_close(db);

    err = ef_open_ex("test_v1.endf", 4, &db);
    expect_err(err, EF_OK, "reopen upgraded v1 file");
    if (db != NULL) {
        expect_true(db->sb->schema_version == EF_SCHEMA_VERSION, "persisted v2 schema");
        expect_true(ef_get_slot(db, 0) != NULL, "persisted slot crc");
        ef_close(db);
    }

    remove_test_file("test_v1.endf");
}

static void test_slot_header_crc_file(struct ef_db *db)
{
    struct ef_slot *slot;
    enum ef_err err;

    err = ef_write_payload(db, 20, "slot-crc", 8);
    expect_err(err, EF_OK, "file slot crc write");
    slot = ef_get_slot(db, 20);
    expect_true(slot != NULL, "file slot crc get");
    if (slot != NULL) {
        slot->payload[0] = 'X';
        expect_true(ef_get_slot(db, 20) == NULL, "file payload tamper fails crc");
        expect_err(ef_last_error(db), EF_ERR_BAD_CHECKSUM, "file payload tamper error");
    }
}

static void test_slot_header_crc_persist_file(void)
{
    struct ef_db *db = NULL;
    struct ef_slot *slot;
    FILE *fp;
    long pos;
    uint32_t crc;
    enum ef_err err;

    remove_test_file("test_slot_crc.endf");

    err = ef_open_ex("test_slot_crc.endf", 8, &db);
    expect_err(err, EF_OK, "slot crc persist create");
    if (db == NULL) {
        return;
    }

    err = ef_write_payload(db, 2, "persist-crc", 11);
    expect_err(err, EF_OK, "slot crc persist write");
    ef_close(db);

    fp = fopen("test_slot_crc.endf", "r+b");
    if (fp == NULL) {
        fprintf(stderr, "FAIL: open test_slot_crc.endf for tamper\n");
        ++g_failures;
        return;
    }

    pos = (long)(sizeof(struct ef_superblock) + 2 * sizeof(struct ef_slot) +
                 offsetof(struct ef_slot, header_crc));
    fseek(fp, pos, SEEK_SET);
    if (!main_io_read(fp, &crc, sizeof(crc))) {
        fprintf(stderr, "FAIL: read slot header crc tamper\n");
        fclose(fp);
        ++g_failures;
        return;
    }
    fseek(fp, pos, SEEK_SET);
    crc ^= 0xDEADBEEFU;
    if (!main_io_write(fp, &crc, sizeof(crc))) {
        fprintf(stderr, "FAIL: write slot header crc tamper\n");
        fclose(fp);
        ++g_failures;
        return;
    }
    fclose(fp);

    err = ef_open_readonly_ex("test_slot_crc.endf", &db);
    expect_err(err, EF_OK, "slot crc persist readonly reopen");
    if (db != NULL) {
        slot = ef_get_slot(db, 2);
        expect_true(slot == NULL, "persisted slot crc tamper rejected");
        expect_err(ef_last_error(db), EF_ERR_BAD_CHECKSUM, "persisted slot crc error");
        ef_close(db);
    }

    remove_test_file("test_slot_crc.endf");
}

static void test_slot_header_crc_overflow_persist_file(void)
{
    struct ef_db *db = NULL;
    uint64_t head_id = 0;
    uint64_t ov_id = 0;
    FILE *fp;
    long pos;
    uint32_t crc;
    uint8_t blob[96];
    uint8_t out[96];
    size_t out_len = 0;
    size_t i;
    enum ef_err err;

    remove_test_file("test_slot_crc_ov.endf");

    for (i = 0; i < sizeof(blob); ++i) {
        blob[i] = (uint8_t)(0xD0U + (i & 0x0FU));
    }

    err = ef_open_ex("test_slot_crc_ov.endf", 16, &db);
    expect_err(err, EF_OK, "overflow crc persist create");
    if (db == NULL) {
        return;
    }

    err = ef_alloc_slot(db, &head_id);
    expect_err(err, EF_OK, "overflow crc persist alloc");
    err = ef_write_blob(db, head_id, blob, sizeof(blob));
    expect_err(err, EF_OK, "overflow crc persist write");
    expect_true(db->slots[head_id].next_offset != 0, "overflow crc persist chain");
    err = ef_offset_to_slot_id(db, db->slots[head_id].next_offset, &ov_id);
    expect_err(err, EF_OK, "overflow crc persist ov id");
    ef_close(db);

    fp = fopen("test_slot_crc_ov.endf", "r+b");
    if (fp == NULL) {
        fprintf(stderr, "FAIL: open test_slot_crc_ov.endf for tamper\n");
        ++g_failures;
        return;
    }

    pos = (long)(sizeof(struct ef_superblock) + ov_id * sizeof(struct ef_slot) +
                 offsetof(struct ef_slot, header_crc));
    fseek(fp, pos, SEEK_SET);
    if (!main_io_read(fp, &crc, sizeof(crc))) {
        fprintf(stderr, "FAIL: read overflow slot crc tamper\n");
        fclose(fp);
        ++g_failures;
        return;
    }
    fseek(fp, pos, SEEK_SET);
    crc ^= 0xBEEFU;
    if (!main_io_write(fp, &crc, sizeof(crc))) {
        fprintf(stderr, "FAIL: write overflow slot crc tamper\n");
        fclose(fp);
        ++g_failures;
        return;
    }
    fclose(fp);

    err = ef_open_readonly_ex("test_slot_crc_ov.endf", &db);
    expect_err(err, EF_OK, "overflow crc persist readonly reopen");
    if (db != NULL) {
        err = ef_read_blob(db, head_id, out, sizeof(out), &out_len);
        expect_err(err, EF_ERR_BAD_CHECKSUM, "persisted overflow crc tamper rejected");
        ef_close(db);
    }

    remove_test_file("test_slot_crc_ov.endf");
}
#endif

static void test_v3_alloc_queue_index(void)
{
    static uint8_t arena[64 + 16 * 16 + 8 * 64];
    struct ef_db *db = NULL;
    enum ef_err err;
    uint64_t slot_id = 0;
    uint64_t looked = 0;
    char out[64];
    size_t out_len = 0;
    const char *msg1 = "queue-a";
    const char *msg2 = "queue-b";

    printf("\n=== v3: tail-grow alloc, FIFO queue, Robin Hood index ===\n");

    err = ef_open_memory_hash(arena, sizeof(arena), 4, 16, 1, &db);
    expect_err(err, EF_OK, "ef_open_memory_hash");
    expect_true(db != NULL, "hash db opened");
    if (db == NULL) {
        return;
    }

    expect_true(db->sb->schema_version == EF_SCHEMA_VERSION, "v3 schema");
    expect_true(db->hash_capacity == 16, "hash capacity bound");
    expect_true(db->slots_base == 64 + 16 * 16, "slots after hash region");

    {
        uint64_t i;

        for (i = 0; i < db->sb->max_slots; ++i) {
            err = ef_alloc_slot(db, &slot_id);
            expect_err(err, EF_OK, "alloc_slot drain pool");
        }
    }
    expect_err(ef_alloc_slot(db, &slot_id), EF_ERR_SLOT_FULL, "alloc_slot empty pool");

    {
        uint64_t headroom[3];
        uint64_t j;

        for (j = 0; j < 3; ++j) {
            err = ef_alloc(db, &headroom[j]);
            expect_err(err, EF_OK, "queue headroom alloc");
        }
        for (j = 0; j < 3; ++j) {
            err = ef_free_slot(db, headroom[j]);
            expect_err(err, EF_OK, "queue headroom free");
        }
    }

    expect_true(ef_count_free_slots(db) >= 3, "queue test free headroom");
    expect_true(ef_queue_empty(db), "queue starts empty");
    err = ef_queue_push(db, msg1, (uint8_t)strlen(msg1));
    expect_err(err, EF_OK, "queue push 1");
    err = ef_queue_push(db, msg2, (uint8_t)strlen(msg2));
    expect_err(err, EF_OK, "queue push 2");
    expect_true(!ef_queue_empty(db), "queue non-empty");

    err = ef_queue_pop(db, out, sizeof(out), &out_len);
    expect_err(err, EF_OK, "queue pop 1");
    expect_true(out_len == strlen(msg1), "pop len msg1");
    expect_true(memcmp(out, msg1, out_len) == 0, "pop payload msg1");

    err = ef_queue_pop(db, out, sizeof(out), &out_len);
    expect_err(err, EF_OK, "queue pop 2");
    expect_true(out_len == strlen(msg2), "pop len msg2");
    expect_true(memcmp(out, msg2, out_len) == 0, "pop payload msg2");
    expect_true(ef_queue_empty(db), "queue drained");

    err = ef_alloc(db, &slot_id);
    expect_err(err, EF_OK, "alloc for index");
    err = ef_write_payload(db, slot_id, "indexed-value", 13);
    expect_err(err, EF_OK, "write indexed payload");
    err = ef_index_put(db, "user:42", slot_id);
    expect_err(err, EF_OK, "index put");
    err = ef_index_get(db, "user:42", &looked);
    expect_err(err, EF_OK, "index get");
    expect_true(looked == slot_id, "index roundtrip slot id");
    err = ef_index_remove(db, "user:42");
    expect_err(err, EF_OK, "index remove");
    expect_err(ef_index_get(db, "user:42", &looked), EF_ERR_NOT_FOUND, "index miss after remove");

    ef_close(db);

#if EF_HAS_FILE_IO
    {
        struct ef_db *file_db = NULL;

        remove_test_file("test_v3.endf");
        err = ef_open_ex_hash("test_v3.endf", 4, 16, &file_db);
        expect_err(err, EF_OK, "ef_open_ex_hash file");
        if (file_db != NULL) {
            err = ef_queue_push(file_db, "persist", 7);
            expect_err(err, EF_OK, "file queue push");
            ef_close(file_db);

            err = ef_open_ex_hash("test_v3.endf", 4, 16, &file_db);
            expect_err(err, EF_OK, "reopen v3 file");
            if (file_db != NULL) {
                expect_true(!ef_queue_empty(file_db), "queue persisted");
                err = ef_queue_pop(file_db, out, sizeof(out), &out_len);
                expect_err(err, EF_OK, "file queue pop");
                expect_true(out_len == 7 && memcmp(out, "persist", 7) == 0, "persisted payload");
                ef_close(file_db);
            }
        }
        remove_test_file("test_v3.endf");
    }
#endif
}

static void test_index_lifecycle_and_rehash(void)
{
    static uint8_t arena[64 + 32 * 16 + 16 * 64];
    struct ef_db *db = NULL;
    enum ef_err err;
    uint64_t slot_id = 0;
    uint64_t looked = 0;
    uint32_t i;
    char key[24];

    printf("\n=== v3: index lifecycle + rehash ===\n");

    err = ef_open_memory_hash(arena, sizeof(arena), 16, 16, 1, &db);
    expect_err(err, EF_OK, "open for rehash test");
    if (db == NULL) {
        return;
    }

    for (i = 0; i < 12; ++i) {
        snprintf(key, sizeof(key), "key-%02u", i);
        err = ef_alloc(db, &slot_id);
        expect_err(err, EF_OK, "alloc for index fill");
        err = ef_write_payload(db, slot_id, key, (uint8_t)strlen(key));
        expect_err(err, EF_OK, "write indexed payload");
        err = ef_index_put(db, key, slot_id);
        expect_err(err, EF_OK, "index put");
    }

    err = ef_index_rehash(db, 32);
    expect_err(err, EF_OK, "index rehash 16->32");
    expect_true(db->hash_capacity == 32, "hash capacity after rehash");
    expect_true(db->slots_base == 64 + 32 * 16, "slots base after rehash");

    for (i = 0; i < 12; ++i) {
        snprintf(key, sizeof(key), "key-%02u", i);
        err = ef_index_get(db, key, &looked);
        expect_err(err, EF_OK, "index get after rehash");
        expect_true(ef_get_slot(db, looked) != NULL, "slot reachable after rehash");
    }

    snprintf(key, sizeof(key), "key-%02u", 0U);
    err = ef_index_get(db, key, &looked);
    expect_err(err, EF_OK, "lookup key-00");
    err = ef_free_slot(db, looked);
    expect_err(err, EF_OK, "free indexed slot");
    expect_err(ef_index_get(db, key, &looked), EF_ERR_NOT_FOUND, "index cleared on free");

    ef_close(db);
}

#ifdef ENDFIELDS_CI_FAST
#define BENCH_ROUNDS 3
#define BENCH_MPMC_ROUNDS 2
#define BENCH_MPMC_PER_PRODUCER 500
#else
#define BENCH_ROUNDS 15
#define BENCH_MPMC_ROUNDS 5
#define BENCH_MPMC_PER_PRODUCER 2000
#endif
#define BENCH_MAX_ROUNDS 32
#define EF_RUN_BENCH_MPMC 1

#if EF_HAS_FILE_IO

#if defined(_WIN32)
#include <process.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <pthread.h>
#include <sched.h>
#else
#error "MPMC tests require Win32 threads or pthread (GCC/Clang)"
#endif

static void mpmc_thread_yield(void)
{
#if defined(_WIN32)
    Sleep(0);
#else
    (void)sched_yield();
#endif
}

static void mpmc_atomic_inc(volatile long *value)
{
#if defined(_WIN32)
    InterlockedIncrement(value);
#else
    (void)__atomic_fetch_add(value, 1, __ATOMIC_SEQ_CST);
#endif
}

static int mpmc_producers_finished(const volatile long *done, int producer_count)
{
    return *done >= producer_count;
}

static int mpmc_consumer_should_exit(struct ef_db *db, const volatile long *done, int producer_count)
{
    int pass;

    if (!mpmc_producers_finished(done, producer_count)) {
        return 0;
    }
    for (pass = 0; pass < 8; ++pass) {
        if (!ef_queue_drained(db)) {
            return 0;
        }
        mpmc_thread_yield();
    }
    return ef_queue_drained(db);
}

struct ef_mpmc_ctx {
    struct ef_db *db;
    int thread_id;
    int count;
    volatile long *received;
    volatile long *producers_done;
};

static void ef_mpmc_producer_body(struct ef_mpmc_ctx *ctx)
{
    int i;
    uint8_t payload[5];

    for (i = 0; i < ctx->count; ++i) {
        uint32_t id = (uint32_t)(ctx->thread_id * 100000 + (uint32_t)i);
        enum ef_err err;
        memcpy(payload, &id, sizeof(id));
        payload[4] = 0;
        do {
            err = ef_queue_push(ctx->db, payload, 4);
        } while (err != EF_OK);
    }
    mpmc_atomic_inc(ctx->producers_done);
}

static void ef_mpmc_consumer_body(struct ef_mpmc_ctx *ctx)
{
    uint8_t buf[8];
    size_t len = 0;
    enum ef_err err;

    for (;;) {
        err = ef_queue_pop(ctx->db, buf, sizeof(buf), &len);
        if (err == EF_ERR_QUEUE_EMPTY || err == EF_ERR_NOT_FOUND) {
            if (mpmc_consumer_should_exit(ctx->db, ctx->producers_done, 2)) {
                break;
            }
            continue;
        }
        if (err == EF_ERR_QUEUE_BUSY) {
            continue;
        }
        if (err == EF_OK && len == 4) {
            uint32_t id = 0;
            memcpy(&id, buf, sizeof(id));
            if (id < 400000U) {
                mpmc_atomic_inc(&ctx->received[id]);
            }
        }
    }
}

#if defined(_WIN32)
static unsigned __stdcall ef_mpmc_producer_win(void *arg)
{
    ef_mpmc_producer_body((struct ef_mpmc_ctx *)arg);
    return 0;
}

static unsigned __stdcall ef_mpmc_consumer_win(void *arg)
{
    ef_mpmc_consumer_body((struct ef_mpmc_ctx *)arg);
    return 0;
}

static int mpmc_join_threads_win32(HANDLE *threads, DWORD count, DWORD timeout_ms, const char *label)
{
    DWORD wr;

    wr = WaitForMultipleObjects(count, threads, TRUE, timeout_ms);
    if (wr != WAIT_FAILED && wr != WAIT_TIMEOUT && (wr - WAIT_OBJECT_0) < count) {
        return 1;
    }
    fprintf(stderr, "%s: thread join failed (wr=%lu)\n", label, (unsigned long)wr);
    return 0;
}
#else
static void *ef_mpmc_producer_pthread(void *arg)
{
    ef_mpmc_producer_body((struct ef_mpmc_ctx *)arg);
    return NULL;
}

static void *ef_mpmc_consumer_pthread(void *arg)
{
    ef_mpmc_consumer_body((struct ef_mpmc_ctx *)arg);
    return NULL;
}

static int mpmc_join_threads_pthread(pthread_t *threads, int count, const char *label)
{
    int i;

    for (i = 0; i < count; ++i) {
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "%s: pthread_join[%d] failed (%d)\n", label, i, rc);
            return 0;
        }
    }
    return 1;
}
#endif

static void test_queue_mpmc(void)
{
    struct ef_db *db = NULL;
    enum ef_err err;
    struct ef_mpmc_ctx ctx[4];
    static volatile long received[400000];
    volatile long producers_done = 0;
    uint32_t i;
    long total = 0;
    long dupes = 0;
#if defined(_WIN32)
    HANDLE threads[4];
#else
    pthread_t threads[4];
#endif

#if defined(_WIN32)
    printf("\n=== v3: MPMC queue (4 threads, Win32) ===\n");
#else
    printf("\n=== v3: MPMC queue (4 threads, pthread) ===\n");
#endif

    memset((void *)received, 0, sizeof(received));
    remove_test_file("test_mpmc.endf");
    err = ef_open_ex("test_mpmc.endf", 512, &db);
    expect_err(err, EF_OK, "open mpmc db");
    if (db == NULL) {
        return;
    }

    for (i = 0; i < 4; ++i) {
        ctx[i].db = db;
        ctx[i].thread_id = (int)i;
        ctx[i].count = (i < 2) ? 200 : 0;
        ctx[i].received = received;
        ctx[i].producers_done = &producers_done;
#if defined(_WIN32)
        threads[i] = (HANDLE)_beginthreadex(NULL, 0,
                                            (i < 2) ? ef_mpmc_producer_win : ef_mpmc_consumer_win,
                                            &ctx[i], 0, NULL);
        if (threads[i] == NULL) {
            expect_true(0, "mpmc thread create");
            ef_close(db);
            remove_test_file("test_mpmc.endf");
            return;
        }
#else
        if (pthread_create(&threads[i], NULL,
                           (i < 2) ? ef_mpmc_producer_pthread : ef_mpmc_consumer_pthread,
                           &ctx[i]) != 0) {
            expect_true(0, "mpmc thread create");
            ef_close(db);
            remove_test_file("test_mpmc.endf");
            return;
        }
#endif
    }

#if defined(_WIN32)
    if (!mpmc_join_threads_win32(threads, 4, 60000, "test_queue_mpmc")) {
        expect_true(0, "mpmc threads finished");
        for (i = 0; i < 4; ++i) {
            if (threads[i] != NULL) {
                CloseHandle(threads[i]);
            }
        }
        ef_close(db);
        remove_test_file("test_mpmc.endf");
        return;
    }
    for (i = 0; i < 4; ++i) {
        CloseHandle(threads[i]);
    }
#else
    if (!mpmc_join_threads_pthread(threads, 4, "test_queue_mpmc")) {
        expect_true(0, "mpmc threads finished");
        ef_close(db);
        remove_test_file("test_mpmc.endf");
        return;
    }
#endif

    for (i = 0; i < 400; ++i) {
        uint32_t id = (i < 200) ? i : (100000U + (i - 200));
        if (received[id] == 1) {
            ++total;
        } else if (received[id] > 1) {
            ++dupes;
        }
    }

    expect_true(total == 400, "mpmc all messages received once");
    expect_true(dupes == 0, "mpmc no duplicate delivery");
    ef_close(db);
    remove_test_file("test_mpmc.endf");
}

#if EF_RUN_BENCH_MPMC

struct bench_mpmc_ctx {
    struct ef_db *db;
    int tid;
    int count;
    volatile long *received;
    volatile long *done;
};

static void bench_mpmc_producer_body(struct bench_mpmc_ctx *ctx)
{
    uint8_t payload[5];
    int i;

    for (i = 0; i < ctx->count; ++i) {
        uint32_t id = (uint32_t)(ctx->tid * 100000 + (uint32_t)i);
        enum ef_err err;
        memcpy(payload, &id, sizeof(id));
        do {
            err = ef_queue_push(ctx->db, payload, 4);
        } while (err != EF_OK);
    }
    mpmc_atomic_inc(ctx->done);
}

static void bench_mpmc_consumer_body(struct bench_mpmc_ctx *ctx)
{
    uint8_t buf[8];
    size_t len;

    for (;;) {
        enum ef_err err = ef_queue_pop(ctx->db, buf, sizeof(buf), &len);
        if (err == EF_ERR_QUEUE_EMPTY || err == EF_ERR_NOT_FOUND) {
            if (mpmc_consumer_should_exit(ctx->db, ctx->done, 2)) {
                break;
            }
            continue;
        }
        if (err == EF_ERR_QUEUE_BUSY) {
            continue;
        }
        (void)len;
    }
}

#if defined(_WIN32)
static unsigned __stdcall bench_mpmc_producer_win(void *arg)
{
    bench_mpmc_producer_body((struct bench_mpmc_ctx *)arg);
    return 0;
}

static unsigned __stdcall bench_mpmc_consumer_win(void *arg)
{
    bench_mpmc_consumer_body((struct bench_mpmc_ctx *)arg);
    return 0;
}
#else
static void *bench_mpmc_producer_pthread(void *arg)
{
    bench_mpmc_producer_body((struct bench_mpmc_ctx *)arg);
    return NULL;
}

static void *bench_mpmc_consumer_pthread(void *arg)
{
    bench_mpmc_consumer_body((struct bench_mpmc_ctx *)arg);
    return NULL;
}
#endif

static void bench_print_throughput(const char *label, uint64_t total_ops, int rounds,
                                   const double *sec_per_round);

static void bench_mpmc_throughput(volatile uintptr_t *sink)
{
    struct ef_db *db = NULL;
    enum ef_err err;
    double sec[BENCH_MAX_ROUNDS];
    struct bench_mpmc_ctx ctx[4];
    static volatile long producers_done;
    const int per_producer = BENCH_MPMC_PER_PRODUCER;
    const int mpmc_rounds = BENCH_MPMC_ROUNDS;
    const uint64_t total_ops = (uint64_t)(per_producer * 2);
    const uint64_t bench_slots = total_ops + 64U;
    int r;
    uint32_t i;
#if defined(_WIN32)
    HANDLE threads[4];
#else
    pthread_t threads[4];
#endif

    printf("\n=== MPMC throughput bench (%d rounds, 4 threads, %llu msgs) ===\n",
           mpmc_rounds, (unsigned long long)total_ops);

    for (r = 0; r < mpmc_rounds; ++r) {
        double t0;
        double t1;

        remove_test_file("bench_mpmc.endf");
        err = ef_open_ex("bench_mpmc.endf", bench_slots, &db);
        if (err != EF_OK || db == NULL) {
            fprintf(stderr, "mpmc bench: open failed\n");
            return;
        }

        producers_done = 0;
        t0 = now_seconds();
        for (i = 0; i < 4; ++i) {
            ctx[i].db = db;
            ctx[i].tid = (int)i;
            ctx[i].count = (i < 2) ? per_producer : 0;
            ctx[i].received = NULL;
            ctx[i].done = &producers_done;
#if defined(_WIN32)
            threads[i] = (HANDLE)_beginthreadex(NULL, 0,
                                                  (i < 2) ? bench_mpmc_producer_win : bench_mpmc_consumer_win,
                                                  &ctx[i], 0, NULL);
            if (threads[i] == NULL) {
                fprintf(stderr, "mpmc bench: thread create failed\n");
                ef_close(db);
                db = NULL;
                remove_test_file("bench_mpmc.endf");
                return;
            }
#else
            if (pthread_create(&threads[i], NULL,
                               (i < 2) ? bench_mpmc_producer_pthread : bench_mpmc_consumer_pthread,
                               &ctx[i]) != 0) {
                fprintf(stderr, "mpmc bench: thread create failed\n");
                ef_close(db);
                db = NULL;
                remove_test_file("bench_mpmc.endf");
                return;
            }
#endif
        }
#if defined(_WIN32)
        if (!mpmc_join_threads_win32(threads, 4, 120000, "bench_mpmc_throughput")) {
            for (i = 0; i < 4; ++i) {
                if (threads[i] != NULL) {
                    CloseHandle(threads[i]);
                }
            }
            ef_close(db);
            db = NULL;
            remove_test_file("bench_mpmc.endf");
            return;
        }
        for (i = 0; i < 4; ++i) {
            if (threads[i] != NULL) {
                CloseHandle(threads[i]);
                threads[i] = NULL;
            }
        }
#else
        if (!mpmc_join_threads_pthread(threads, 4, "bench_mpmc_throughput")) {
            ef_close(db);
            db = NULL;
            remove_test_file("bench_mpmc.endf");
            return;
        }
#endif
        t1 = now_seconds();
        sec[r] = t1 - t0;
        if (sec[r] <= 0.0) {
            sec[r] = 1e-9;
        }
        *sink ^= (uintptr_t)ef_queue_empty(db);
        (void)ef_db_commit_meta(db);
        ef_close(db);
        db = NULL;
        remove_test_file("bench_mpmc.endf");
    }

    bench_print_throughput("queue MPMC push+pop total", total_ops, mpmc_rounds, sec);
}

#endif /* EF_RUN_BENCH_MPMC */

#else /* !EF_HAS_FILE_IO */

static void test_queue_mpmc(void)
{
    printf("\n=== v3: MPMC queue (skipped, no file I/O) ===\n");
}

#endif /* EF_HAS_FILE_IO */

static void test_sync(struct ef_db *db)
{
    enum ef_err err;

    err = ef_sync(db);
    expect_err(err, EF_OK, "ef_sync");
}

static void bench_sort_doubles(double *v, int n)
{
    int i;
    int j;

    for (i = 1; i < n; ++i) {
        double key = v[i];
        j = i;
        while (j > 0 && v[j - 1] > key) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = key;
    }
}

static double bench_pick_percentile(const double *sorted, int n, double pct)
{
    int idx;

    if (n <= 0) {
        return 0.0;
    }
    if (n == 1) {
        return sorted[0];
    }
    idx = (int)((double)(n - 1) * pct + 0.5);
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= n) {
        idx = n - 1;
    }
    return sorted[idx];
}

static void bench_print_stats(const char *label, int iterations, int rounds, const double *ns_per_op)
{
    double sorted[BENCH_MAX_ROUNDS];
    double min_v;
    double max_v;
    double sum;
    double p50;
    double p99;
    int r;
    int use_rounds;

    use_rounds = rounds;
    if (use_rounds > BENCH_MAX_ROUNDS) {
        use_rounds = BENCH_MAX_ROUNDS;
    }
    if (use_rounds <= 0) {
        return;
    }

    min_v = max_v = ns_per_op[0];
    sum = 0.0;
    for (r = 0; r < use_rounds; ++r) {
        sorted[r] = ns_per_op[r];
        if (ns_per_op[r] < min_v) {
            min_v = ns_per_op[r];
        }
        if (ns_per_op[r] > max_v) {
            max_v = ns_per_op[r];
        }
        sum += ns_per_op[r];
    }
    bench_sort_doubles(sorted, use_rounds);
    p50 = bench_pick_percentile(sorted, use_rounds, 0.50);
    p99 = bench_pick_percentile(sorted, use_rounds, 0.99);

    printf("  %-40s %9d ops  avg %8.1f ns  p50 %8.1f  p99 %8.1f  min %8.1f  max %8.1f\n",
           label, iterations, sum / (double)use_rounds, p50, p99, min_v, max_v);
}

#if EF_RUN_BENCH_MPMC

static void bench_print_throughput(const char *label, uint64_t total_ops, int rounds, const double *sec_per_round)
{
    double sorted[BENCH_MAX_ROUNDS];
    double min_ops;
    double max_ops;
    double sum_ops;
    double p50;
    double p99;
    int r;
    int use_rounds;

    use_rounds = rounds;
    if (use_rounds > BENCH_MAX_ROUNDS) {
        use_rounds = BENCH_MAX_ROUNDS;
    }
    if (use_rounds <= 0 || total_ops == 0) {
        return;
    }

    min_ops = max_ops = (double)total_ops / sec_per_round[0];
    sum_ops = 0.0;
    for (r = 0; r < use_rounds; ++r) {
        double ops = (double)total_ops / sec_per_round[r];
        sorted[r] = ops;
        if (ops < min_ops) {
            min_ops = ops;
        }
        if (ops > max_ops) {
            max_ops = ops;
        }
        sum_ops += ops;
    }
    bench_sort_doubles(sorted, use_rounds);
    p50 = bench_pick_percentile(sorted, use_rounds, 0.50);
    p99 = bench_pick_percentile(sorted, use_rounds, 0.99);

    printf("  %-40s %9llu ops  avg %8.0f/s  p50 %8.0f/s  p99 %8.0f/s  min %8.0f/s  max %8.0f/s\n",
           label, (unsigned long long)total_ops, sum_ops / (double)use_rounds, p50, p99, min_ops,
           max_ops);
}

#endif

static void bench_touch_cold_cache(void)
{
    static volatile uint8_t scratch[256 * 1024];
    size_t i;

    for (i = 0; i < sizeof(scratch); i += 64) {
        scratch[i] = (uint8_t)(scratch[i] + 1U);
    }
}

static void bench_prepare_chain(struct ef_db *db, uint64_t chain_len)
{
    uint64_t i;
    char payload[32];
    enum ef_err err;

    for (i = 0; i < chain_len; ++i) {
        snprintf(payload, sizeof(payload), "node-%04llu", (unsigned long long)i);
        err = ef_write_payload(db, i, payload, (uint8_t)strlen(payload));
        if (err != EF_OK) {
            fprintf(stderr, "bench chain write failed at %llu\n", (unsigned long long)i);
            return;
        }
        if (i + 1 < chain_len) {
            err = ef_set_next_offset(db, i, ef_slot_to_offset(db, i + 1));
            if (err != EF_OK) {
                fprintf(stderr, "bench chain link failed at %llu\n", (unsigned long long)i);
                return;
            }
        }
    }
}

static void run_hash_perf_suite(volatile uintptr_t *sink)
{
    static uint8_t hash_arena[64 + 256 * 16 + 128 * 64];
    struct ef_db *db = NULL;
    double samples[BENCH_MAX_ROUNDS];
    double t0;
    double t1;
    int r;
    int i;
    enum ef_err err;
    const int put_iters = 50000;
    const int get_iters = 200000;
    const int remove_iters = 20000;
    char key[32];
    uint64_t slot_id;
    uint64_t looked;

    printf("\n=== Hash index bench (%d rounds, cap=256, slots=128) ===\n", BENCH_ROUNDS);

    err = ef_open_memory_hash(hash_arena, sizeof(hash_arena), 128, 256, 1, &db);
    if (err != EF_OK || db == NULL) {
        fprintf(stderr, "hash bench: open failed: %s\n", ef_strerror(err));
        return;
    }

    for (i = 0; i < 96; ++i) {
        snprintf(key, sizeof(key), "pre-%03d", i);
        err = ef_alloc(db, &slot_id);
        if (err != EF_OK) {
            fprintf(stderr, "hash bench: preload alloc failed\n");
            ef_close(db);
            return;
        }
        err = ef_index_put(db, key, slot_id);
        if (err != EF_OK) {
            fprintf(stderr, "hash bench: preload put failed\n");
            ef_close(db);
            return;
        }
    }

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        t0 = now_seconds();
        for (i = 0; i < put_iters; ++i) {
            snprintf(key, sizeof(key), "put-%d-%d", r, i);
            err = ef_alloc(db, &slot_id);
            if (err == EF_OK) {
                *sink ^= (uintptr_t)ef_index_put(db, key, slot_id);
            }
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)put_iters * 1e9;
    }
    bench_print_stats("ef_index_put + alloc", put_iters, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        t0 = now_seconds();
        for (i = 0; i < get_iters; ++i) {
            snprintf(key, sizeof(key), "pre-%03d", i % 96);
            err = ef_index_get(db, key, &looked);
            *sink ^= (uintptr_t)err ^ (uintptr_t)looked;
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)get_iters * 1e9;
    }
    bench_print_stats("ef_index_get (96 keys, ~38% load)", get_iters, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        t0 = now_seconds();
        for (i = 0; i < remove_iters; ++i) {
            snprintf(key, sizeof(key), "put-%d-%d", r, i);
            err = ef_index_get(db, key, &looked);
            if (err == EF_OK) {
                (void)ef_index_remove(db, key);
                *sink ^= (uintptr_t)ef_free_slot(db, looked);
            }
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)remove_iters * 1e9;
    }
    bench_print_stats("index remove + free (put keys)", remove_iters, BENCH_ROUNDS, samples);

    ef_close(db);
    db = NULL;

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        int j;

        err = ef_open_memory_hash(hash_arena, sizeof(hash_arena), 128, 256, 1, &db);
        if (err != EF_OK || db == NULL) {
            break;
        }
        for (j = 0; j < 96; ++j) {
            snprintf(key, sizeof(key), "rh-%03d", j);
            err = ef_alloc(db, &slot_id);
            if (err != EF_OK) {
                break;
            }
            err = ef_index_put(db, key, slot_id);
            if (err != EF_OK) {
                break;
            }
        }
        t0 = now_seconds();
        err = ef_index_rehash(db, 512);
        t1 = now_seconds();
        samples[r] = (t1 - t0) * 1e9;
        *sink ^= (uintptr_t)err;
        ef_close(db);
        db = NULL;
    }
    bench_print_stats("ef_index_rehash 256->512 (fresh db)", 1, BENCH_ROUNDS, samples);
}

static void run_perf_suite(struct ef_db *db)
{
    struct ef_cmd chase_cmd;
    struct ef_slot *chase_cur;
    volatile uintptr_t sink = 0;
    double samples[BENCH_MAX_ROUNDS];
    double t0;
    double t1;
    int r;
    int i;
    const int fast_iters = 500000;
    const int verify_iters = 200000;
    const int write_iters = 50000;
    const int sync_iters = 500;
    const int hop_iters = 100000;
    const int pool_iters = 20000;
    const int queue_iters = 20000;
    uint64_t slot_id = 7;
    enum ef_err err;
    char qbuf[48];
    size_t qlen;

    printf("\n=== Performance suite (%d rounds each, platform=%s) ===\n",
           BENCH_ROUNDS, ef_platform_name());

    bench_prepare_chain(db, 32);

    chase_cur = ef_get_slot(db, 0);
    for (r = 0; r < BENCH_ROUNDS; ++r) {
        t0 = now_seconds();
        for (i = 0; i < fast_iters; ++i) {
            sink ^= (uintptr_t)ef_chase(db, chase_cur);
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)fast_iters * 1e9;
    }
    bench_print_stats("ef_chase direct 1-hop", fast_iters, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        chase_cmd.opcode = EF_OP_CHASE;
        chase_cmd.param = ef_slot_to_offset(db, 0);
        chase_cmd.field_offset = 0;
        t0 = now_seconds();
        for (i = 0; i < fast_iters; ++i) {
            sink ^= (uintptr_t)ef_execute(db, &chase_cmd, NULL);
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)fast_iters * 1e9;
    }
    bench_print_stats("ef_execute CHASE 1-hop", fast_iters, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        t0 = now_seconds();
        for (i = 0; i < verify_iters; ++i) {
            sink ^= (uintptr_t)ef_get_slot(db, (uint64_t)(i % 32));
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)verify_iters * 1e9;
    }
    bench_print_stats("ef_get_slot + CRC verify", verify_iters, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        bench_touch_cold_cache();
        t0 = now_seconds();
        for (i = 0; i < verify_iters / 10; ++i) {
            bench_touch_cold_cache();
            sink ^= (uintptr_t)ef_get_slot(db, (uint64_t)(i % 32));
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)(verify_iters / 10) * 1e9;
    }
    bench_print_stats("ef_get_slot cold-ish (cache flush)", verify_iters / 10, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        uint32_t hops = 0;
        t0 = now_seconds();
        for (i = 0; i < hop_iters; ++i) {
            sink ^= (uintptr_t)ef_chase_n(db, ef_slot_to_offset(db, 0), 16, &hops);
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)hop_iters * 1e9;
    }
    bench_print_stats("chase_n 16 hops (no per-hop CRC)", hop_iters, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        char buf[24];
        t0 = now_seconds();
        for (i = 0; i < write_iters; ++i) {
            snprintf(buf, sizeof(buf), "w-%d-%d", r, i);
            sink ^= (uintptr_t)ef_write_payload(db, slot_id, buf, (uint8_t)strlen(buf));
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)write_iters * 1e9;
    }
    bench_print_stats("ef_write_payload (slot CRC)", write_iters, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        t0 = now_seconds();
        for (i = 0; i < pool_iters; ++i) {
            err = ef_alloc_slot(db, &slot_id);
            if (err == EF_OK) {
                sink ^= (uintptr_t)ef_free_slot(db, slot_id);
            }
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)pool_iters * 1e9;
    }
    bench_print_stats("ef_alloc_slot + ef_free_slot", pool_iters, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        t0 = now_seconds();
        for (i = 0; i < queue_iters; ++i) {
            snprintf(qbuf, sizeof(qbuf), "q-%d", i);
            err = ef_queue_push(db, qbuf, (uint8_t)strlen(qbuf));
            if (err == EF_OK) {
                err = ef_queue_pop(db, qbuf, sizeof(qbuf), &qlen);
            }
            sink ^= (uintptr_t)err;
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)queue_iters * 1e9;
    }
    bench_print_stats("ef_queue_push + pop roundtrip", queue_iters, BENCH_ROUNDS, samples);

#if EF_HAS_FILE_IO
    if (db->backend == EF_BACKEND_FILE) {
        for (r = 0; r < BENCH_ROUNDS; ++r) {
            char buf[24];
            t0 = now_seconds();
            for (i = 0; i < sync_iters; ++i) {
                snprintf(buf, sizeof(buf), "s-%d-%d", r, i);
                sink ^= (uintptr_t)ef_write_payload(db, slot_id, buf, (uint8_t)strlen(buf));
                sink ^= (uintptr_t)ef_sync(db);
            }
            t1 = now_seconds();
            samples[r] = (t1 - t0) / (double)sync_iters * 1e9;
        }
        bench_print_stats("write_payload + ef_sync (full)", sync_iters, BENCH_ROUNDS, samples);
    }
#endif

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        t0 = now_seconds();
        for (i = 0; i < pool_iters; ++i) {
            err = ef_alloc_slot(db, &slot_id);
            if (err == EF_OK) {
                (void)ef_db_commit_meta(db);
                sink ^= (uintptr_t)ef_free_slot(db, slot_id);
            }
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)pool_iters * 1e9;
    }
    bench_print_stats("alloc+free + commit_meta each op", pool_iters, BENCH_ROUNDS, samples);

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        int j;
        for (j = 0; j < pool_iters; ++j) {
            err = ef_alloc_slot(db, &slot_id);
            if (err == EF_OK) {
                sink ^= (uintptr_t)ef_free_slot(db, slot_id);
            }
        }
        t0 = now_seconds();
        sink ^= (uintptr_t)ef_db_commit_meta(db);
        t1 = now_seconds();
        samples[r] = (t1 - t0) * 1e9;
    }
    bench_print_stats("ef_db_commit_meta (batch after 20k allocs)", 1, BENCH_ROUNDS, samples);

    run_hash_perf_suite(&sink);
#if EF_RUN_BENCH_MPMC && EF_HAS_FILE_IO
    bench_mpmc_throughput(&sink);
#endif

    (void)sink;
    printf("  (sink=%llu — anti-optimize guard)\n", (unsigned long long)sink);
}

#if EF_HAS_FILE_IO
static void run_engineering_scenarios(void)
{
    struct ef_db *db = NULL;
    enum ef_err err;
    double t0;
    double t1;
    int round;
    const int rounds = 3;
    uint64_t i;
    uint64_t id;

    printf("\n=== Engineering scenarios (%d rounds, file backend) ===\n", rounds);

    for (round = 0; round < rounds; ++round) {
        uint64_t verified = 0;
        uint64_t chase_ok = 0;
        char expect[32];
        size_t out_len = 0;
        uint8_t blob[96];
        uint8_t blob_out[96];

        remove_test_file("test_engineering.endf");

        t0 = now_seconds();
        err = ef_open_ex("test_engineering.endf", 128, &db);
        if (err != EF_OK || db == NULL) {
            fprintf(stderr, "engineering open failed round %d\n", round);
            continue;
        }

        for (i = 0; i < 64; ++i) {
            snprintf(expect, sizeof(expect), "save-%02llu", (unsigned long long)i);
            if (i == 0) {
                err = ef_write_payload(db, i, expect, (uint8_t)strlen(expect));
            } else {
                err = ef_alloc_slot(db, &id);
                if (err == EF_OK) {
                    err = ef_write_payload(db, id, expect, (uint8_t)strlen(expect));
                }
            }
            if (err != EF_OK) {
                break;
            }
        }
        err = ef_sync(db);
        ef_close(db);
        t1 = now_seconds();
        printf("  Round %d  create+64 writes+sync:     %.3f ms\n", round, (t1 - t0) * 1000.0);

        t0 = now_seconds();
        err = ef_open_ex("test_engineering.endf", 128, &db);
        if (err != EF_OK || db == NULL) {
            continue;
        }
        for (i = 0; i < 64; ++i) {
            struct ef_slot *slot = ef_get_slot(db, i);
            snprintf(expect, sizeof(expect), "save-%02llu", (unsigned long long)i);
            if (slot != NULL &&
                strcmp((char *)ef_slot_payload_ptr(db, slot), expect) == 0) {
                ++verified;
            }
        }
        t1 = now_seconds();
        printf("  Round %d  reopen+64 CRC reads:        %.3f ms  (ok %llu/64)\n",
               round, (t1 - t0) * 1000.0, (unsigned long long)verified);

        for (i = 0; i < 32; ++i) {
            snprintf(expect, sizeof(expect), "link-%02llu", (unsigned long long)i);
            err = ef_write_payload(db, i, expect, (uint8_t)strlen(expect));
            if (err == EF_OK && i + 1 < 32) {
                err = ef_set_next_offset(db, i, ef_slot_to_offset(db, i + 1));
            }
        }

        t0 = now_seconds();
        for (i = 0; i < 10000; ++i) {
            struct ef_slot *end;
            uint32_t hops = 0;
            end = ef_chase_n(db, ef_slot_to_offset(db, 0), 31, &hops);
            if (end != NULL) {
                ++chase_ok;
            }
            (void)hops;
        }
        t1 = now_seconds();
        printf("  Round %d  10k x chase_n(31 hops):      %.3f ms  (%.1f us/chase, ok %llu)\n",
               round, (t1 - t0) * 1000.0, (t1 - t0) / 10000.0 * 1e6,
               (unsigned long long)chase_ok);

        for (i = 0; i < sizeof(blob); ++i) {
            blob[i] = (uint8_t)(round + i);
        }
        t0 = now_seconds();
        for (i = 32; i < 48; ++i) {
            err = ef_write_blob(db, i, blob, sizeof(blob));
            if (err != EF_OK) {
                break;
            }
        }
        t1 = now_seconds();
        printf("  Round %d  16 blob writes (96B):      %.3f ms\n", round, (t1 - t0) * 1000.0);

        t0 = now_seconds();
        verified = 0;
        for (i = 32; i < 48; ++i) {
            err = ef_read_blob(db, i, blob_out, sizeof(blob_out), &out_len);
            if (err == EF_OK && out_len == sizeof(blob) &&
                memcmp(blob, blob_out, sizeof(blob)) == 0) {
                ++verified;
            }
        }
        t1 = now_seconds();
        printf("  Round %d  16 blob CRC reads:          %.3f ms  (ok %llu/16)\n",
               round, (t1 - t0) * 1000.0, (unsigned long long)verified);

        t0 = now_seconds();
        for (i = 0; i < 2000; ++i) {
            err = ef_alloc_slot(db, &id);
            if (err == EF_OK) {
                (void)ef_free_slot(db, id);
            }
        }
        t1 = now_seconds();
        printf("  Round %d  2000 alloc/free pairs:      %.3f ms  (%.1f us/pair)\n",
               round, (t1 - t0) * 1000.0, (t1 - t0) / 2000.0 * 1e6);

        err = ef_grow(db, 192);
        printf("  Round %d  ef_grow 128->192:           %s  (free=%llu)\n",
               round, err == EF_OK ? "ok" : ef_strerror(err),
               (unsigned long long)ef_count_free_slots(db));

        err = ef_sync(db);
        ef_close(db);
        remove_test_file("test_engineering.endf");
    }
}
#endif

static void run_memory_engineering(void)
{
    static uint8_t arena[64 + 256 * 64];
    struct ef_db *db = NULL;
    enum ef_err err;
    double t0;
    double t1;
    int round;
    uint64_t i;
    uint64_t id;

    printf("\n=== Engineering scenarios (3 rounds, memory backend) ===\n");

    for (round = 0; round < 3; ++round) {
        uint64_t ok = 0;
        char expect[24];

        t0 = now_seconds();
        err = ef_open_memory(arena, sizeof(arena), 64, 1, &db);
        if (err != EF_OK || db == NULL) {
            continue;
        }

        for (i = 0; i < 48; ++i) {
            snprintf(expect, sizeof(expect), "ram-%02llu", (unsigned long long)i);
            if (i == 0) {
                err = ef_write_payload(db, 0, expect, (uint8_t)strlen(expect));
            } else {
                err = ef_alloc_slot(db, &id);
                if (err == EF_OK) {
                    err = ef_write_payload(db, id, expect, (uint8_t)strlen(expect));
                }
            }
        }

        for (i = 0; i < 48; ++i) {
            struct ef_slot *slot = ef_get_slot(db, i);
            snprintf(expect, sizeof(expect), "ram-%02llu", (unsigned long long)i);
            if (slot != NULL &&
                strcmp((char *)ef_slot_payload_ptr(db, slot), expect) == 0) {
                ++ok;
            }
        }

        err = ef_grow(db, 128);
        t1 = now_seconds();
        printf("  Round %d  48 rw + grow 64->128:       %.3f ms  (read ok %llu/48, grow %s)\n",
               round, (t1 - t0) * 1000.0, (unsigned long long)ok,
               err == EF_OK ? "ok" : ef_strerror(err));

        ef_close(db);

        t0 = now_seconds();
        err = ef_open_memory(arena, sizeof(arena), 64, 0, &db);
        if (db != NULL) {
            snprintf(expect, sizeof(expect), "ram-%02llu", 0ULL);
            ok = (ef_get_slot(db, 0) != NULL &&
                  strcmp((char *)ef_slot_payload_ptr(db, ef_get_slot(db, 0)), expect) == 0) ? 1 : 0;
            ef_close(db);
        }
        t1 = now_seconds();
        printf("  Round %d  reopen grown arena:         %.3f ms  (slot0 ok=%llu)\n",
               round, (t1 - t0) * 1000.0, (unsigned long long)ok);
    }
}

int main(void)
{
    struct ef_db *db = NULL;
    enum ef_err err;

    printf("platform: %s\n", ef_platform_name());

    test_memory_backend();
    test_slot_iterator_memory();
    test_blob_memory();
    test_grow_memory();
    test_v1_upgrade_memory();
    test_slot_header_crc_memory();
    test_v3_alloc_queue_index();
    test_index_lifecycle_and_rehash();
#if EF_HAS_FILE_IO
    test_queue_mpmc();
#endif

#if EF_HAS_FILE_IO
    remove_test_file("test.endf");
    remove_test_file("test_rw.endf");

    err = ef_open_ex("test.endf", 100, &db);
    if (err != EF_OK || db == NULL) {
        fprintf(stderr, "ef_open_ex failed: %s\n", ef_strerror(err));
        return 1;
    }

    test_offset_roundtrip(db);
    test_write_read_api(db);
    test_write_execute(db);
    test_chase_loop(db);
    test_chase_n(db);
    test_error_paths(db);
    test_slot_header_crc_file(db);
    test_sync(db);
    ef_close(db);

    test_reopen_existing();
    test_bad_magic();
    test_alloc_free();
    test_grow();
    test_readonly_open();
    test_superblock_checksum_reject();
    test_v1_upgrade_file();
    test_slot_header_crc_persist_file();
    test_slot_header_crc_overflow_persist_file();
    test_blob_file();

    if (g_failures == 0) {
        run_memory_engineering();
#if EF_HAS_FILE_IO
        run_engineering_scenarios();
        err = ef_open_ex("test.endf", 100, &db);
        if (err == EF_OK && db != NULL) {
            run_perf_suite(db);
            ef_close(db);
        }
#endif
    }
#else
    (void)db;
    (void)err;
#endif

    if (g_failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }

    printf("All platform, durability, grow, and checksum tests passed.\n");
    return 0;
}
