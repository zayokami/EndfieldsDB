#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "endfields.h"
#include "ef_config.h"

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

    fwrite(image, 1, file_size, fp);
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
    fread(&byte, 1, 1, fp);
    fseek(fp, pos, SEEK_SET);
    byte ^= 0xFFU;
    fwrite(&byte, 1, 1, fp);
    fclose(fp);

    err = ef_open_ex("test_crc.endf", 2, &db);
    expect_err(err, EF_ERR_BAD_CHECKSUM, "reject tampered superblock");
    expect_true(db == NULL, "tampered superblock null db");

    remove_test_file("test_crc.endf");
}
#endif

static void test_sync(struct ef_db *db)
{
    enum ef_err err;

    err = ef_sync(db);
    expect_err(err, EF_OK, "ef_sync");
}

static void run_benchmark(struct ef_db *db)
{
    struct ef_cmd chase_cmd;
    double start;
    double end;
    double elapsed;
    double avg_ns;
    const int iterations = 1000000;
    int i;

    chase_cmd.opcode = EF_OP_CHASE;
    chase_cmd.param = ef_slot_to_offset(db, 0);
    chase_cmd.field_offset = 0;

    start = now_seconds();
    for (i = 0; i < iterations; ++i) {
        (void)ef_execute(db, &chase_cmd, NULL);
    }
    end = now_seconds();

    elapsed = end - start;
    avg_ns = (elapsed / (double)iterations) * 1e9;

    printf("Benchmark: %d executions in %.6f seconds\n", iterations, elapsed);
    printf("Average latency: %.2f ns per ef_execute\n", avg_ns);
}

int main(void)
{
    struct ef_db *db = NULL;
    enum ef_err err;

    printf("platform: %s\n", ef_platform_name());

    test_memory_backend();

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
    test_sync(db);
    ef_close(db);

    test_reopen_existing();
    test_bad_magic();
    test_alloc_free();
    test_grow();
    test_readonly_open();
    test_superblock_checksum_reject();

    if (g_failures == 0) {
        err = ef_open_ex("test.endf", 100, &db);
        if (err == EF_OK && db != NULL) {
            run_benchmark(db);
            ef_close(db);
        }
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
