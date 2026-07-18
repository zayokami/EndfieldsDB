#include "test_common.h"

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

static void test_execute_get_slot_and_field(struct ef_db *db)
{
    struct ef_cmd cmd;
    struct ef_slot *slot_via_get;
    struct ef_slot *slot_via_execute;
    struct ef_slot *slot;
    uint64_t off_a;
    uint64_t off_b;
    uint64_t off_c;
    uint8_t byte = 0xCD;
    enum ef_err err;

    err = ef_write_payload(db, 20, "execute", 7);
    expect_err(err, EF_OK, "execute setup payload");

    slot_via_get = ef_get_slot(db, 20);
    cmd.opcode = EF_OP_GET_SLOT;
    cmd.param = 20;
    cmd.field_offset = 0;
    slot_via_execute = (struct ef_slot *)ef_execute(db, &cmd, NULL);
    expect_true(slot_via_get != NULL && slot_via_execute == slot_via_get,
                "EF_OP_GET_SLOT matches ef_get_slot");

    cmd.opcode = EF_OP_WRITE_FIELD;
    cmd.param = 20;
    cmd.field_offset = 12;
    slot = (struct ef_slot *)ef_execute(db, &cmd, &byte);
    expect_true(slot != NULL, "EF_OP_WRITE_FIELD result");
    cmd.opcode = EF_OP_GET_FIELD;
    cmd.param = 20;
    cmd.field_offset = 12;
    expect_true(ef_execute(db, &cmd, NULL) != NULL, "EF_OP_GET_FIELD result");
    expect_true(*(const uint8_t *)ef_execute(db, &cmd, NULL) == 0xCD,
                "EF_OP_GET_FIELD reads back value");

    err = ef_write_payload(db, 21, "A", 1);
    expect_err(err, EF_OK, "chase_n execute write A");
    err = ef_write_payload(db, 22, "B", 1);
    expect_err(err, EF_OK, "chase_n execute write B");
    err = ef_write_payload(db, 23, "C", 1);
    expect_err(err, EF_OK, "chase_n execute write C");
    off_a = ef_slot_to_offset(db, 21);
    off_b = ef_slot_to_offset(db, 22);
    off_c = ef_slot_to_offset(db, 23);
    err = ef_set_next_offset(db, 21, off_b);
    expect_err(err, EF_OK, "chase_n execute link A->B");
    err = ef_set_next_offset(db, 22, off_c);
    expect_err(err, EF_OK, "chase_n execute link B->C");

    cmd.opcode = EF_OP_CHASE_N;
    cmd.param = off_a;
    cmd.field_offset = 2;
    slot = (struct ef_slot *)ef_execute(db, &cmd, NULL);
    expect_true(slot != NULL && slot->payload[0] == 'C', "EF_OP_CHASE_N result");
    expect_true(cmd.field_offset == 2, "EF_OP_CHASE_N writes hops done");

    cmd.field_offset = 10;
    slot = (struct ef_slot *)ef_execute(db, &cmd, NULL);
    expect_true(slot != NULL && slot->payload[0] == 'C', "EF_OP_CHASE_N short chain");
    expect_true(cmd.field_offset == 3, "EF_OP_CHASE_N writes actual hops on short chain");
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
    static alignas(64) uint8_t buf[64 + 16 * 64];
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
        struct ef_slot *slot = ef_get_slot(db, sid);

        expect_true(slot != NULL && strcmp((char *)ef_slot_payload_ptr(db, slot), "ram") == 0,
                    "memory data kept");
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
    static alignas(64) uint8_t buf[64 + 32 * 64];
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
    static alignas(64) uint8_t buf[64 + 64 * 64];
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
    static alignas(64) uint8_t buf[64 + 8 * 64];
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
    static alignas(64) uint8_t arena[64 + 16 * 64];
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
    static alignas(64) uint8_t buf[64 + 32 * 64];
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
        slot->header_crc ^= 0xA5A5A5A5U;
        expect_true(ef_get_slot(db, id_tamper) != NULL, "restore tampered header_crc");
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

        slot = ef_peek_slot(db, id_tamper);
        expect_true(slot != NULL, "peek_slot returns pointer without crc");
        expect_true(slot == ef_get_slot(db, id_tamper), "peek matches get when crc ok");
        slot->header_crc ^= 0x22222222U;
        expect_true(ef_peek_slot(db, id_tamper) != NULL, "peek ignores tampered crc");
        expect_true(ef_get_slot(db, id_tamper) == NULL, "get_slot still rejects tampered crc");
        slot->header_crc ^= 0x22222222U;
        expect_true(ef_get_slot(db, id_tamper) != NULL, "peek tamper restored");

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
    expect_true(ef_execute(db, &cmd, NULL) != NULL, "EF_OP_FREE");
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
    static alignas(64) uint8_t image[64 + 4 * 64];
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

static void test_sync(struct ef_db *db)
{
    enum ef_err err;

    err = ef_sync(db);
    expect_err(err, EF_OK, "ef_sync");
}

static void test_crc32_pclmul_consistency(void)
{
    static uint8_t buf[512];
    size_t i;
    size_t n;
    uint32_t vec;

    vec = ef_crc32("123456789", 9);
    expect_true(vec == 0xCBF43926U, "crc32 IEEE test vector");

    for (i = 0; i < sizeof(buf); ++i) {
        buf[i] = (uint8_t)(i * 131U + 17U);
    }

#if defined(__x86_64__) || defined(_M_X64)
    if (!ef_crc32_pclmul_available()) {
        printf("  (crc pclmul unavailable on this CPU, slice4 only)\n");
        return;
    }

    for (n = 1; n <= sizeof(buf); ++n) {
        uint32_t sw = ef_crc32_update_slice4(0xFFFFFFFFU, buf, n);
        uint32_t hw = ef_crc32_update_pclmul(0xFFFFFFFFU, buf, n);

        if (sw != hw) {
            fprintf(stderr, "FAIL: pclmul mismatch len=%zu sw=%08x hw=%08x\n",
                    n, sw, hw);
            ++g_failures;
            return;
        }
    }
    printf("  crc32 pclmul matches slice4 for 1..%zu bytes\n", sizeof(buf));
#else
    (void)n;
    (void)i;
    (void)buf;
#endif
}


int main(void)
{
    struct ef_db *db = NULL;
    enum ef_err err;

    printf("platform: %s\n", ef_platform_name());

    test_crc32_pclmul_consistency();
    test_memory_backend();
    test_slot_iterator_memory();
    test_blob_memory();
    test_grow_memory();
    test_v1_upgrade_memory();
    test_slot_header_crc_memory();

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
    test_execute_get_slot_and_field(db);
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
#else
    (void)db;
    (void)err;
#endif

    return test_finish("Core tests");
}
