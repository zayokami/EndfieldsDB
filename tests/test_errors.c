#include "test_common.h"

static void test_null_args_open(void)
{
    struct ef_db *db = NULL;
    enum ef_err err;
    static alignas(64) uint8_t buf[64 + 64 * 64];

    err = ef_open_ex(NULL, 64, &db);
    expect_err(err, EF_ERR_NULL_ARG, "ef_open_ex NULL filepath");
    expect_true(db == NULL, "ef_open_ex NULL filepath returns null db");

    err = ef_open_ex("path", 64, NULL);
    expect_err(err, EF_ERR_NULL_ARG, "ef_open_ex NULL db_out");

    err = ef_open_ex_hash("path", 64, 16, NULL);
    expect_err(err, EF_ERR_NULL_ARG, "ef_open_ex_hash NULL db_out");

    err = ef_open_memory(NULL, sizeof(buf), 64, 1, &db);
    expect_err(err, EF_ERR_NULL_ARG, "ef_open_memory NULL buffer");
    expect_true(db == NULL, "ef_open_memory NULL buffer returns null db");
}

static void test_null_args_with_db(void)
{
    static alignas(64) uint8_t buf[64 + 64 * 64];
    struct ef_db *db = NULL;
    struct ef_cmd cmd;
    enum ef_err err;

    err = ef_open_memory(buf, sizeof(buf), 64, 1, &db);
    expect_err(err, EF_OK, "open db for null-arg tests");
    if (db == NULL) {
        return;
    }

    err = ef_write_payload(db, 1, NULL, 5);
    expect_err(err, EF_ERR_NULL_ARG, "ef_write_payload NULL data with len > 0");

    cmd.opcode = EF_OP_GET_SLOT;
    cmd.param = 0;
    cmd.field_offset = 0;
    expect_true(ef_execute(NULL, &cmd, NULL) == NULL, "ef_execute NULL db");
    expect_err(ef_last_error(NULL), EF_ERR_NULL_ARG, "ef_execute NULL db error");

    expect_true(ef_execute(db, NULL, NULL) == NULL, "ef_execute NULL cmd");
    expect_err(ef_last_error(db), EF_ERR_NULL_ARG, "ef_execute NULL cmd error");

    ef_close(db);
}

#if EF_HAS_FILE_IO
static void test_illegal_hash_capacities(void)
{
    struct ef_db *db = NULL;
    enum ef_err err;

    /* Capacity 0 is accepted by ef_open_ex_hash (equivalent to no index). */
    remove_test_file("test_hash0.endf");
    err = ef_open_ex_hash("test_hash0.endf", 64, 0, &db);
    expect_err(err, EF_OK, "ef_open_ex_hash capacity 0 succeeds (no index)");
    if (db != NULL) {
        expect_true(db->hash_capacity == 0, "hash capacity 0 disables index");
        ef_close(db);
        db = NULL;
    }
    remove_test_file("test_hash0.endf");

    /* Non-power-of-two capacity is rejected. */
    db = NULL;
    remove_test_file("test_hash15.endf");
    err = ef_open_ex_hash("test_hash15.endf", 64, 15, &db);
    expect_err(err, EF_ERR_BAD_VERSION, "ef_open_ex_hash capacity 15 rejected");
    expect_true(db == NULL, "ef_open_ex_hash capacity 15 returns null db");
    remove_test_file("test_hash15.endf");

    /* Capacity above u16 layout limit is rejected. */
    db = NULL;
    remove_test_file("test_hash_big.endf");
    err = ef_open_ex_hash("test_hash_big.endf", 64, 0x10000, &db);
    expect_err(err, EF_ERR_BAD_VERSION, "ef_open_ex_hash capacity 0x10000 rejected");
    expect_true(db == NULL, "ef_open_ex_hash capacity 0x10000 returns null db");
    remove_test_file("test_hash_big.endf");
}
#endif

#if EF_HAS_FILE_IO
static void test_readonly_write_attempts(void)
{
    struct ef_db *rw = NULL;
    struct ef_db *ro = NULL;
    enum ef_err err;
    uint64_t slot_id = 0;
    uint8_t buf[64];
    size_t out_len = 0;

    remove_test_file("test_ro_errors.endf");

    err = ef_open_ex_hash("test_ro_errors.endf", 16, 16, &rw);
    expect_err(err, EF_OK, "create rw db with index for readonly test");
    if (rw == NULL) {
        return;
    }
    err = ef_write_payload(rw, 0, "seed", 4);
    expect_err(err, EF_OK, "seed rw db");
    ef_close(rw);

    err = ef_open_readonly_ex("test_ro_errors.endf", &ro);
    expect_err(err, EF_OK, "open readonly db");
    if (ro == NULL) {
        remove_test_file("test_ro_errors.endf");
        return;
    }
    expect_true(ef_is_readonly(ro), "readonly flag set");

    err = ef_alloc(ro, &slot_id);
    expect_err(err, EF_ERR_READONLY, "readonly blocks ef_alloc");

    err = ef_write_payload(ro, 0, "nope", 4);
    expect_err(err, EF_ERR_READONLY, "readonly blocks ef_write_payload");

    err = ef_free_slot(ro, 0);
    expect_err(err, EF_ERR_READONLY, "readonly blocks ef_free_slot");

    err = ef_queue_push(ro, "msg", 3);
    expect_err(err, EF_ERR_READONLY, "readonly blocks ef_queue_push");

    err = ef_index_put(ro, "key", 0);
    expect_err(err, EF_ERR_READONLY, "readonly blocks ef_index_put");

    err = ef_grow(ro, 32);
    expect_err(err, EF_ERR_READONLY, "readonly blocks ef_grow");

    /* Readonly queue pop is also blocked at the write gate. */
    err = ef_queue_pop(ro, buf, sizeof(buf), &out_len);
    expect_err(err, EF_ERR_READONLY, "readonly blocks ef_queue_pop");

    ef_close(ro);
    remove_test_file("test_ro_errors.endf");
}
#endif

static void test_corrupted_superblock(void)
{
    static alignas(64) uint8_t buf[64 + 16 * 64];
    struct ef_superblock *sb;
    struct ef_db *db = NULL;
    enum ef_err err;

    /* Create a valid image in memory. */
    err = ef_open_memory(buf, sizeof(buf), 16, 1, &db);
    expect_err(err, EF_OK, "create valid memory db for corruption");
    if (db != NULL) {
        ef_close(db);
    }

    sb = (struct ef_superblock *)buf;

    /* Corrupt magic bytes. */
    sb->magic[0] = 'X';
    err = ef_open_memory(buf, sizeof(buf), 16, 0, &db);
    expect_err(err, EF_ERR_BAD_MAGIC, "corrupted magic rejected");
    expect_true(db == NULL, "corrupted magic returns null db");
    sb->magic[0] = EF_MAGIC_0;

    /* Corrupt schema version to an unsupported value. */
    sb->schema_version = 99;
    err = ef_open_memory(buf, sizeof(buf), 16, 0, &db);
    expect_err(err, EF_ERR_BAD_VERSION, "corrupted schema_version rejected");
    expect_true(db == NULL, "corrupted schema_version returns null db");

    /* Restore and corrupt slot_size. */
    sb->schema_version = EF_SCHEMA_VERSION;
    sb->slot_size = 32;
    err = ef_open_memory(buf, sizeof(buf), 16, 0, &db);
    expect_err(err, EF_ERR_BAD_SLOT_SIZE, "corrupted slot_size rejected");
    expect_true(db == NULL, "corrupted slot_size returns null db");
}

static void test_invalid_slot_ids_and_offsets(void)
{
    static alignas(64) uint8_t buf[64 + 16 * 64];
    struct ef_db *db = NULL;
    uint64_t slot_id = 0;
    enum ef_err err;

    err = ef_open_memory(buf, sizeof(buf), 16, 1, &db);
    expect_err(err, EF_OK, "open db for slot-id tests");
    if (db == NULL) {
        return;
    }

    /* Slot 0 is valid. */
    expect_true(ef_get_slot(db, 0) != NULL, "ef_get_slot(0) succeeds");

    /* Out-of-range slot IDs are rejected. */
    expect_true(ef_get_slot(db, db->sb->max_slots) == NULL, "ef_get_slot(max_slots) fails");
    expect_err(ef_last_error(db), EF_ERR_SLOT_ID, "ef_get_slot(max_slots) error");

    expect_true(ef_get_slot(db, db->sb->max_slots + 1) == NULL,
                "ef_get_slot(max_slots + 1) fails");
    expect_err(ef_last_error(db), EF_ERR_SLOT_ID, "ef_get_slot(max_slots + 1) error");

    /* Offset below slots_base is rejected. */
    err = ef_offset_to_slot_id(db, 0, &slot_id);
    expect_err(err, EF_ERR_OFFSET, "ef_offset_to_slot_id(0) error");

    /* Misaligned offset is rejected. */
    err = ef_offset_to_slot_id(db, db->slots_base + 1, &slot_id);
    expect_err(err, EF_ERR_OFFSET, "ef_offset_to_slot_id(misaligned) error");

    ef_close(db);
}

static void test_payload_length_errors(void)
{
    static alignas(64) uint8_t buf[64 + 16 * 64];
    struct ef_db *db = NULL;
    uint8_t data[EF_PAYLOAD_SIZE + 1];
    enum ef_err err;

    err = ef_open_memory(buf, sizeof(buf), 16, 1, &db);
    expect_err(err, EF_OK, "open db for payload-len tests");
    if (db == NULL) {
        return;
    }

    err = ef_write_payload(db, 0, data, (uint8_t)(EF_PAYLOAD_SIZE + 1));
    expect_err(err, EF_ERR_PAYLOAD_LEN, "payload length EF_PAYLOAD_SIZE + 1 rejected");

    ef_close(db);
}

static void test_queue_empty_pop(void)
{
    static alignas(64) uint8_t buf[64 + 16 * 64];
    struct ef_db *db = NULL;
    uint8_t out[64];
    size_t out_len = 0;
    enum ef_err err;

    err = ef_open_memory(buf, sizeof(buf), 16, 1, &db);
    expect_err(err, EF_OK, "open db for queue-empty test");
    if (db == NULL) {
        return;
    }

    expect_true(ef_queue_empty(db), "new queue is empty");
    err = ef_queue_pop(db, out, sizeof(out), &out_len);
    expect_err(err, EF_ERR_QUEUE_EMPTY, "ef_queue_pop on empty queue");

    ef_close(db);
}

static void test_index_without_hash(void)
{
    static alignas(64) uint8_t buf[64 + 16 * 64];
    struct ef_db *db = NULL;
    uint64_t looked = 0;
    enum ef_err err;

    err = ef_open_memory(buf, sizeof(buf), 16, 1, &db);
    expect_err(err, EF_OK, "open db without hash index");
    if (db == NULL) {
        return;
    }

    expect_true(db->hash_capacity == 0, "db has no hash index");

    err = ef_index_get(db, "key", &looked);
    expect_err(err, EF_ERR_NULL_ARG, "ef_index_get without index");

    err = ef_index_remove(db, "key");
    expect_err(err, EF_ERR_NULL_ARG, "ef_index_remove without index");

    err = ef_index_put(db, "key", 0);
    expect_err(err, EF_ERR_NULL_ARG, "ef_index_put without index");

    ef_close(db);
}

static void test_chase_depth(void)
{
    static alignas(64) uint8_t buf[64 + (EF_CHASE_MAX_DEPTH + 2) * 64];
    struct ef_db *db = NULL;
    uint64_t i;
    enum ef_err err;
    uint64_t start;
    uint32_t hops = 0;
    struct ef_slot *result;

    err = ef_open_memory(buf, sizeof(buf), EF_CHASE_MAX_DEPTH + 2, 1, &db);
    expect_err(err, EF_OK, "open db for chase-depth test");
    if (db == NULL) {
        return;
    }

    /* Link slots 0 -> 1 -> 2 -> ... -> EF_CHASE_MAX_DEPTH + 1. */
    for (i = 0; i <= EF_CHASE_MAX_DEPTH; ++i) {
        err = ef_write_payload(db, i, "x", 1);
        expect_err(err, EF_OK, "write chain slot");
        err = ef_set_next_offset(db, i, ef_slot_to_offset(db, i + 1));
        expect_err(err, EF_OK, "link chain slot");
    }
    err = ef_write_payload(db, EF_CHASE_MAX_DEPTH + 1, "y", 1);
    expect_err(err, EF_OK, "write chain tail");

    start = ef_slot_to_offset(db, 0);
    result = ef_chase_n(db, start, EF_CHASE_MAX_DEPTH + 1, &hops);
    expect_true(result == NULL, "chase_n beyond max depth returns null");
    expect_err(ef_last_error(db), EF_ERR_CHASE_DEPTH, "chase_n depth exceeded error");

    ef_close(db);
}

static void test_chase_cycle(void)
{
    static alignas(64) uint8_t buf[64 + 4 * 64];
    struct ef_db *db = NULL;
    enum ef_err err;
    uint64_t off_a;
    uint64_t off_b;
    uint32_t hops = 0;
    struct ef_slot *result;

    err = ef_open_memory(buf, sizeof(buf), 4, 1, &db);
    expect_err(err, EF_OK, "open db for chase-cycle test");
    if (db == NULL) {
        return;
    }

    err = ef_write_payload(db, 0, "A", 1);
    expect_err(err, EF_OK, "write cycle slot A");
    err = ef_write_payload(db, 1, "B", 1);
    expect_err(err, EF_OK, "write cycle slot B");

    off_a = ef_slot_to_offset(db, 0);
    off_b = ef_slot_to_offset(db, 1);

    err = ef_set_next_offset(db, 0, off_b);
    expect_err(err, EF_OK, "link A -> B");
    err = ef_set_next_offset(db, 1, off_a);
    expect_err(err, EF_OK, "link B -> A (cycle)");

    result = ef_chase_n(db, off_a, 8, &hops);
    expect_true(result == NULL, "chase_n detects cycle");
    expect_err(ef_last_error(db), EF_ERR_CHASE_CYCLE, "chase_n cycle error");

    ef_close(db);
}

int main(void)
{
    printf("platform: %s\n", ef_platform_name());

    test_null_args_open();
    test_null_args_with_db();
    test_corrupted_superblock();
    test_invalid_slot_ids_and_offsets();
    test_payload_length_errors();
    test_queue_empty_pop();
    test_index_without_hash();
    test_chase_depth();
    test_chase_cycle();

#if EF_HAS_FILE_IO
    test_illegal_hash_capacities();
    test_readonly_write_attempts();
#endif

    return test_finish("Error-path tests");
}
