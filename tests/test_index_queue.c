#include "test_common.h"

static void test_v3_alloc_queue_index(void)
{
    static alignas(64) uint8_t arena[64 + 16 * 16 + 8 * 64];
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

    expect_true(db->sb->schema_version == EF_SCHEMA_VERSION, "hash db schema v4");
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

static void test_execute_queue_and_index(void)
{
    static alignas(64) uint8_t arena[64 + 16 * 16 + 8 * 64];
    struct ef_db *db = NULL;
    struct ef_cmd cmd;
    enum ef_err err;
    uint64_t slot_id = 0;
    uint64_t looked = 0;
    char out[64];
    const char *msg = "exec-queue";
    const char *key = "exec:key";
    void *result;

    err = ef_open_memory_hash(arena, sizeof(arena), 4, 16, 1, &db);
    expect_err(err, EF_OK, "execute queue/index open");
    if (db == NULL) {
        return;
    }

    cmd.opcode = EF_OP_QUEUE_PUSH;
    cmd.param = 0;
    cmd.field_offset = (uint8_t)strlen(msg);
    result = ef_execute(db, &cmd, msg);
    expect_true(result != NULL, "EF_OP_QUEUE_PUSH result");
    expect_true(!ef_queue_empty(db), "queue non-empty after execute push");

    memset(out, 0, sizeof(out));
    cmd.opcode = EF_OP_QUEUE_POP;
    cmd.param = 0;
    cmd.field_offset = sizeof(out);
    result = ef_execute(db, &cmd, out);
    expect_true(result != NULL, "EF_OP_QUEUE_POP result");
    expect_true(cmd.field_offset == strlen(msg), "EF_OP_QUEUE_POP writes length");
    expect_true(memcmp(out, msg, strlen(msg)) == 0, "EF_OP_QUEUE_POP payload");
    expect_true(ef_queue_empty(db), "queue empty after execute pop");

    err = ef_alloc(db, &slot_id);
    expect_err(err, EF_OK, "execute index alloc");
    err = ef_write_payload(db, slot_id, "indexed", 7);
    expect_err(err, EF_OK, "execute index write");

    cmd.opcode = EF_OP_INDEX_PUT;
    cmd.param = (uint64_t)(uintptr_t)key;
    cmd.field_offset = 0;
    result = ef_execute(db, &cmd, &slot_id);
    expect_true(result != NULL, "EF_OP_INDEX_PUT result");

    cmd.opcode = EF_OP_INDEX_GET;
    cmd.param = (uint64_t)(uintptr_t)key;
    cmd.field_offset = 0;
    result = ef_execute(db, &cmd, &looked);
    expect_true(result != NULL, "EF_OP_INDEX_GET result");
    expect_true(looked == slot_id, "EF_OP_INDEX_GET slot id");

    cmd.opcode = EF_OP_INDEX_REMOVE;
    cmd.param = (uint64_t)(uintptr_t)key;
    cmd.field_offset = 0;
    result = ef_execute(db, &cmd, NULL);
    expect_true(result != NULL, "EF_OP_INDEX_REMOVE result");
    expect_err(ef_index_get(db, key, &slot_id), EF_ERR_NOT_FOUND,
               "index miss after execute remove");

    ef_close(db);
}

static void test_index_get_slot(void)
{
    static alignas(64) uint8_t arena[64 + 16 * 16 + 8 * 64];
    struct ef_db *db = NULL;
    enum ef_err err;
    uint64_t slot_id = 0;
    struct ef_slot *slot = NULL;
    char buf[64];
    size_t len = 0;
    const char *key = "get-slot:key";

    printf("\n=== v4: ef_index_get_slot atomic index+slot read ===\n");

    err = ef_open_memory_hash(arena, sizeof(arena), 4, 16, 1, &db);
    expect_err(err, EF_OK, "open for index_get_slot");
    if (db == NULL) {
        return;
    }

    err = ef_alloc(db, &slot_id);
    expect_err(err, EF_OK, "alloc for index_get_slot");
    err = ef_write_payload(db, slot_id, "indexed", 7);
    expect_err(err, EF_OK, "write payload for index_get_slot");
    err = ef_index_put(db, key, slot_id);
    expect_err(err, EF_OK, "index put for index_get_slot");

    memset(buf, 0, sizeof(buf));
    err = ef_index_get_slot(db, key, &slot, buf, sizeof(buf), &len);
    expect_err(err, EF_OK, "index_get_slot with buffer");
    expect_true(slot != NULL, "index_get_slot slot_out non-NULL");
    expect_true(len == ef_payload_capacity(db), "index_get_slot payload_len_out");
    expect_true(memcmp(buf, "indexed", 7) == 0, "index_get_slot payload matches");

    memset(buf, 0, sizeof(buf));
    len = 0;
    err = ef_index_get_slot(db, key, NULL, buf, 4, &len);
    expect_err(err, EF_ERR_PAYLOAD_LEN, "index_get_slot too-small buffer");
    expect_true(len == ef_payload_capacity(db), "index_get_slot payload_len_out on short buf");

    len = 0;
    err = ef_index_get_slot(db, key, NULL, NULL, 0, &len);
    expect_err(err, EF_OK, "index_get_slot NULL buffer");
    expect_true(len == ef_payload_capacity(db), "index_get_slot length only");

    err = ef_index_get_slot(db, "missing-key", NULL, buf, sizeof(buf), &len);
    expect_err(err, EF_ERR_NOT_FOUND, "index_get_slot missing key");

    err = ef_free_slot(db, slot_id);
    expect_err(err, EF_OK, "free indexed slot");
    err = ef_index_get_slot(db, key, NULL, buf, sizeof(buf), &len);
    expect_true(err == EF_ERR_NOT_FOUND || err == EF_ERR_SLOT_FREE,
                "index_get_slot after free returns NOT_FOUND or SLOT_FREE");

    ef_close(db);
}

static void test_index_lifecycle_and_rehash(void)
{
    static alignas(64) uint8_t arena[64 + 32 * 16 + 16 * 64];
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

static void test_index_auto_rehash(void)
{
    static alignas(64) uint8_t arena[64 + 64 * 16 + 128 * 64];
    static alignas(64) uint8_t tight_arena[64 + 16 * 16 + 32 * 64];
    struct ef_db *db = NULL;
    enum ef_err err;
    uint64_t slots[48];
    uint64_t slot_id = 0;
    uint64_t update_slot = 0;
    uint64_t looked = 0;
    uint32_t i;
    char key[24];

    printf("\n=== v4: index auto rehash on load threshold ===\n");

    memset(slots, 0, sizeof(slots));
    err = ef_open_memory_hash(arena, sizeof(arena), 128, 16, 1, &db);
    expect_err(err, EF_OK, "open tiny index for auto rehash");
    if (db == NULL) {
        return;
    }

    expect_true(ef_index_capacity(db) == 16, "initial capacity 16");

    for (i = 0; i < 12; ++i) {
        snprintf(key, sizeof(key), "auto-%03u", i);
        err = ef_alloc(db, &slots[i]);
        expect_err(err, EF_OK, "alloc before threshold");
        err = ef_write_payload(db, slots[i], key, (uint8_t)strlen(key));
        expect_err(err, EF_OK, "write before threshold");
        err = ef_index_put(db, key, slots[i]);
        expect_err(err, EF_OK, "put before threshold");
    }
    expect_true(ef_index_capacity(db) == 16, "12/16 load stays at capacity 16");
    expect_true(ef_index_count_entries(db) == 12, "12 indexed entries before threshold");

    err = ef_alloc(db, &update_slot);
    expect_err(err, EF_OK, "alloc replacement slot");
    err = ef_write_payload(db, update_slot, "updated", 7);
    expect_err(err, EF_OK, "write replacement slot");
    err = ef_index_put(db, "auto-000", update_slot);
    expect_err(err, EF_OK, "updating existing key does not rehash");
    expect_true(ef_index_capacity(db) == 16, "update at threshold keeps capacity 16");
    expect_true(ef_index_count_entries(db) == 12, "update at threshold keeps entry count");
    err = ef_index_get(db, "auto-000", &looked);
    expect_err(err, EF_OK, "lookup updated key");
    expect_true(looked == update_slot, "updated key points at replacement slot");
    slots[0] = update_slot;

    snprintf(key, sizeof(key), "auto-%03u", 12U);
    err = ef_alloc(db, &slots[12]);
    expect_err(err, EF_OK, "alloc threshold crossing entry");
    err = ef_write_payload(db, slots[12], key, (uint8_t)strlen(key));
    expect_err(err, EF_OK, "write threshold crossing entry");
    err = ef_index_put(db, key, slots[12]);
    expect_err(err, EF_OK, "13th insert auto rehashes");
    expect_true(ef_index_capacity(db) == 32, "13/16 load grows to 32");
    expect_true(ef_index_count_entries(db) == 13, "13 indexed entries after first auto rehash");

    for (i = 13; i < 48; ++i) {
        snprintf(key, sizeof(key), "auto-%03u", i);
        err = ef_alloc(db, &slots[i]);
        expect_err(err, EF_OK, "alloc after first auto rehash");
        err = ef_write_payload(db, slots[i], key, (uint8_t)strlen(key));
        expect_err(err, EF_OK, "write after first auto rehash");
        err = ef_index_put(db, key, slots[i]);
        expect_err(err, EF_OK, "put after first auto rehash");
    }

    expect_true(ef_index_capacity(db) == 64, "25th insert grows 32->64 and 48/64 stays put");
    expect_true(ef_index_count_entries(db) == 48, "48 indexed entries after auto rehash");

    for (i = 0; i < 48; ++i) {
        snprintf(key, sizeof(key), "auto-%03u", i);
        err = ef_index_get(db, key, &looked);
        expect_err(err, EF_OK, "lookup after auto rehash");
        if (looked != slots[i]) {
            char msg[128];

            snprintf(msg, sizeof(msg), "lookup %s returned slot %llu, want %llu",
                     key, (unsigned long long)looked, (unsigned long long)slots[i]);
            expect_true(0, msg);
        }
        expect_true(ef_get_slot(db, looked) != NULL, "slot reachable after auto rehash");
    }

    ef_close(db);

    db = NULL;
    err = ef_open_memory_hash(tight_arena, sizeof(tight_arena), 32, 16, 1, &db);
    expect_err(err, EF_OK, "open tight arena for failed auto rehash");
    if (db == NULL) {
        return;
    }

    for (i = 0; i < 12; ++i) {
        snprintf(key, sizeof(key), "tight-%03u", i);
        err = ef_alloc(db, &slot_id);
        expect_err(err, EF_OK, "tight alloc before threshold");
        err = ef_index_put(db, key, slot_id);
        expect_err(err, EF_OK, "tight put before threshold");
    }
    expect_true(ef_index_capacity(db) == 16, "tight arena starts at capacity 16");
    expect_true(ef_index_count_entries(db) == 12, "tight arena has threshold entries");

    err = ef_alloc(db, &slot_id);
    expect_err(err, EF_OK, "tight alloc crossing entry");
    err = ef_index_put(db, "tight-012", slot_id);
    expect_err(err, EF_ERR_FILE_SIZE, "auto rehash fails when memory map cannot grow");
    expect_true(ef_index_capacity(db) == 16, "failed auto rehash keeps old capacity");
    expect_true(ef_index_count_entries(db) == 12, "failed auto rehash keeps old entries");
    expect_err(ef_index_get(db, "tight-012", &looked), EF_ERR_NOT_FOUND,
               "failed auto rehash does not insert new key");
    for (i = 0; i < 12; ++i) {
        snprintf(key, sizeof(key), "tight-%03u", i);
        err = ef_index_get(db, key, &looked);
        expect_err(err, EF_OK, "old key survives failed auto rehash");
    }

    ef_close(db);

#if EF_HAS_FILE_IO
    remove_test_file("test_index_auto.endf");
    db = NULL;
    err = ef_open_ex_hash("test_index_auto.endf", 64, 16, &db);
    expect_err(err, EF_OK, "open file index for auto rehash");
    if (db != NULL) {
        for (i = 0; i < 25; ++i) {
            snprintf(key, sizeof(key), "file-%03u", i);
            err = ef_alloc(db, &slot_id);
            expect_err(err, EF_OK, "file auto rehash alloc");
            err = ef_index_put(db, key, slot_id);
            expect_err(err, EF_OK, "file auto rehash put");
        }
        expect_true(ef_index_capacity(db) == 64, "file auto rehash grows to 64");
        err = ef_sync(db);
        expect_err(err, EF_OK, "sync file auto rehash db");
        ef_close(db);

        db = NULL;
        err = ef_open_ex_hash("test_index_auto.endf", 64, 16, &db);
        expect_err(err, EF_OK, "reopen file auto rehash db");
        if (db != NULL) {
            expect_true(ef_index_capacity(db) == 64, "reopened file keeps auto-grown capacity");
            for (i = 0; i < 25; ++i) {
                snprintf(key, sizeof(key), "file-%03u", i);
                err = ef_index_get(db, key, &looked);
                expect_err(err, EF_OK, "file lookup after auto rehash reopen");
            }
            ef_close(db);
        }
    }
    remove_test_file("test_index_auto.endf");
#endif
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
#elif defined(__GNUC__) || defined(__clang__)
    (void)__atomic_fetch_add(value, 1L, __ATOMIC_SEQ_CST);
#else
    *value += 1L;
#endif
}

static long mpmc_atomic_load(const volatile long *value)
{
#if defined(_WIN32)
    return InterlockedCompareExchange((volatile LONG *)value, 0, 0);
#elif defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#else
    return *value;
#endif
}

static void mpmc_stop_signal(volatile long *stop)
{
#if defined(_WIN32)
    InterlockedExchange((volatile LONG *)stop, 1L);
#elif defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(stop, 1L, __ATOMIC_RELEASE);
#else
    *stop = 1L;
#endif
}

static int mpmc_stop_requested(const volatile long *stop)
{
    return mpmc_atomic_load(stop) != 0L;
}

static int mpmc_producers_finished(const volatile long *done, int producer_count)
{
    return mpmc_atomic_load(done) >= producer_count;
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

#ifdef ENDFIELDS_CI_FAST
#define EF_INDEX_MRSR_ROUNDS 1500
#else
#define EF_INDEX_MRSR_ROUNDS 8000
#endif

struct ef_index_mrsr_ctx {
    struct ef_db *db;
    int thread_id;
    volatile long *errors;
    volatile long *stop;
};

static void ef_index_mrsr_seed_keys(struct ef_db *db)
{
    uint32_t i;
    char key[24];

    for (i = 0; i < 32U; ++i) {
        uint64_t slot_id = 0;
        enum ef_err err;

        snprintf(key, sizeof(key), "key-%02u", i);
        err = ef_alloc(db, &slot_id);
        if (err != EF_OK) {
            return;
        }
        err = ef_write_payload(db, slot_id, key, (uint8_t)strlen(key));
        if (err != EF_OK) {
            return;
        }
        (void)ef_index_put(db, key, slot_id);
    }
}

#if defined(_WIN32)
static unsigned __stdcall ef_index_mrsr_reader_win(void *arg)
{
    struct ef_index_mrsr_ctx *ctx = (struct ef_index_mrsr_ctx *)arg;
    char key[24];
    int round;

    for (round = 0; round < EF_INDEX_MRSR_ROUNDS; ++round) {
        uint32_t kid = (uint32_t)((ctx->thread_id * 97 + round) % 32);
        uint64_t slot_id = 0;
        enum ef_err err;

        if (mpmc_stop_requested(ctx->stop)) {
            break;
        }

        snprintf(key, sizeof(key), "key-%02u", kid);
        err = ef_index_get(ctx->db, key, &slot_id);
        if (err == EF_ERR_INDEX_BUSY || err == EF_ERR_NOT_FOUND) {
            --round;
            Sleep(0);
            continue;
        }
        if (err != EF_OK) {
            mpmc_atomic_inc(ctx->errors);
            break;
        }
    }
    return 0;
}

static unsigned __stdcall ef_index_mrsr_writer_win(void *arg)
{
    struct ef_index_mrsr_ctx *ctx = (struct ef_index_mrsr_ctx *)arg;
    char key[24];
    int round;

    for (round = 0; round < EF_INDEX_MRSR_ROUNDS / 8; ++round) {
        uint32_t kid = (uint32_t)(round % 32U);
        uint64_t slot_id = 0;
        enum ef_err err;
        int attempt;

        if (mpmc_stop_requested(ctx->stop)) {
            break;
        }

        snprintf(key, sizeof(key), "key-%02u", kid);
        for (attempt = 0; attempt < 64; ++attempt) {
            err = ef_index_get(ctx->db, key, &slot_id);
            if (err == EF_ERR_INDEX_BUSY) {
                Sleep(0);
                continue;
            }
            break;
        }
        if (err == EF_OK) {
            for (attempt = 0; attempt < 64; ++attempt) {
                err = ef_index_remove(ctx->db, key);
                if (err == EF_ERR_INDEX_BUSY) {
                    Sleep(0);
                    continue;
                }
                break;
            }
            if (err != EF_OK && err != EF_ERR_NOT_FOUND) {
                mpmc_atomic_inc(ctx->errors);
                break;
            }
            (void)ef_free_slot(ctx->db, slot_id);
        }

        err = ef_alloc(ctx->db, &slot_id);
        if (err != EF_OK) {
            mpmc_atomic_inc(ctx->errors);
            break;
        }
        snprintf(key, sizeof(key), "key-%02u", kid);
        err = ef_write_payload(ctx->db, slot_id, key, (uint8_t)strlen(key));
        if (err != EF_OK) {
            mpmc_atomic_inc(ctx->errors);
            break;
        }
        for (attempt = 0; attempt < 64; ++attempt) {
            err = ef_index_put(ctx->db, key, slot_id);
            if (err == EF_ERR_INDEX_BUSY) {
                Sleep(0);
                continue;
            }
            break;
        }
        if (err != EF_OK) {
            mpmc_atomic_inc(ctx->errors);
            break;
        }
    }

    mpmc_stop_signal(ctx->stop);
    return 0;
}
#else
static void *ef_index_mrsr_reader_pthread(void *arg)
{
    struct ef_index_mrsr_ctx *ctx = (struct ef_index_mrsr_ctx *)arg;
    char key[24];
    int round;

    for (round = 0; round < EF_INDEX_MRSR_ROUNDS; ++round) {
        uint32_t kid = (uint32_t)((ctx->thread_id * 97 + round) % 32);
        uint64_t slot_id = 0;
        enum ef_err err;

        if (mpmc_stop_requested(ctx->stop)) {
            break;
        }

        snprintf(key, sizeof(key), "key-%02u", kid);
        err = ef_index_get(ctx->db, key, &slot_id);
        if (err == EF_ERR_INDEX_BUSY || err == EF_ERR_NOT_FOUND) {
            --round;
            sched_yield();
            continue;
        }
        if (err != EF_OK) {
            mpmc_atomic_inc(ctx->errors);
            break;
        }
    }
    return NULL;
}

static void *ef_index_mrsr_writer_pthread(void *arg)
{
    struct ef_index_mrsr_ctx *ctx = (struct ef_index_mrsr_ctx *)arg;
    char key[24];
    int round;

    for (round = 0; round < EF_INDEX_MRSR_ROUNDS / 8; ++round) {
        uint32_t kid = (uint32_t)(round % 32U);
        uint64_t slot_id = 0;
        enum ef_err err;
        int attempt;

        if (mpmc_stop_requested(ctx->stop)) {
            break;
        }

        snprintf(key, sizeof(key), "key-%02u", kid);
        for (attempt = 0; attempt < 64; ++attempt) {
            err = ef_index_get(ctx->db, key, &slot_id);
            if (err == EF_ERR_INDEX_BUSY) {
                sched_yield();
                continue;
            }
            break;
        }
        if (err == EF_OK) {
            for (attempt = 0; attempt < 64; ++attempt) {
                err = ef_index_remove(ctx->db, key);
                if (err == EF_ERR_INDEX_BUSY) {
                    sched_yield();
                    continue;
                }
                break;
            }
            if (err != EF_OK && err != EF_ERR_NOT_FOUND) {
                mpmc_atomic_inc(ctx->errors);
                break;
            }
            (void)ef_free_slot(ctx->db, slot_id);
        }

        err = ef_alloc(ctx->db, &slot_id);
        if (err != EF_OK) {
            mpmc_atomic_inc(ctx->errors);
            break;
        }
        snprintf(key, sizeof(key), "key-%02u", kid);
        err = ef_write_payload(ctx->db, slot_id, key, (uint8_t)strlen(key));
        if (err != EF_OK) {
            mpmc_atomic_inc(ctx->errors);
            break;
        }
        for (attempt = 0; attempt < 64; ++attempt) {
            err = ef_index_put(ctx->db, key, slot_id);
            if (err == EF_ERR_INDEX_BUSY) {
                sched_yield();
                continue;
            }
            break;
        }
        if (err != EF_OK) {
            mpmc_atomic_inc(ctx->errors);
            break;
        }
    }

    mpmc_stop_signal(ctx->stop);
    return NULL;
}
#endif

static void test_sb_checksum_store(struct ef_superblock *sb)
{
    uint32_t crc;
    uint32_t zero_crc = 0;

    if (!(sb->flags & EF_FLAG_SB_CRC)) {
        return;
    }

    crc = ef_crc32_update(0xFFFFFFFFU, sb, offsetof(struct ef_superblock, reserved));
    crc = ef_crc32_update(crc, &zero_crc, sizeof(zero_crc));
    crc = ef_crc32_update(crc, sb->reserved + sizeof(uint32_t),
                          sizeof(sb->reserved) - sizeof(uint32_t));
    *(uint32_t *)&sb->reserved[0] = crc ^ 0xFFFFFFFFU;
}

static void test_v3_to_v4_index_migration(void)
{
    static alignas(64) uint8_t arena[64 + 16 * 16 + 8 * 64];
    struct ef_db *db = NULL;
    enum ef_err err;
    uint64_t slot_id = 0;
    uint64_t looked = 0;
    const char *key = "migrate-me";

    printf("\n=== v3->v4: index layout migration ===\n");

    err = ef_open_memory_hash(arena, sizeof(arena), 4, 16, 1, &db);
    expect_err(err, EF_OK, "migration seed open");
    if (db == NULL) {
        return;
    }

    err = ef_alloc(db, &slot_id);
    expect_err(err, EF_OK, "migration seed alloc");
    err = ef_write_payload(db, slot_id, key, (uint8_t)strlen(key));
    expect_err(err, EF_OK, "migration seed payload");
    err = ef_index_put(db, key, slot_id);
    expect_err(err, EF_OK, "migration seed index put");
    ef_close(db);

    {
        struct ef_superblock *sb = (struct ef_superblock *)arena;
        uint32_t hash32 = 16U;
        uint32_t queue_lock32 = 0U;

        sb->schema_version = EF_SCHEMA_VERSION_V3;
        memcpy(&sb->reserved[EF_SB_OFF_HASH_CAP_V3], &hash32, sizeof(hash32));
        memcpy(&sb->reserved[EF_SB_OFF_QUEUE_LOCK_V3], &queue_lock32, sizeof(queue_lock32));
        test_sb_checksum_store(sb);
    }

    err = ef_open_memory_hash(arena, sizeof(arena), 4, 16, 0, &db);
    expect_err(err, EF_OK, "migration reopen writable");
    if (db == NULL) {
        return;
    }

    expect_true(db->sb->schema_version == EF_SCHEMA_VERSION, "migrated schema v4");
    expect_true(db->hash_capacity == 16U, "migrated hash capacity");
    expect_true(ef_sb_index_seq_load(db->sb) == 0U, "migrated index seq zero");

    err = ef_index_get(db, key, &looked);
    expect_err(err, EF_OK, "migration index lookup");
    expect_true(looked == slot_id, "migration slot id preserved");

    ef_close(db);
}

static void test_index_mrsr(void)
{
    static alignas(64) uint8_t arena[64 + 64 * 16 + 128 * 64];
    struct ef_db *db = NULL;
    enum ef_err err;
    struct ef_index_mrsr_ctx ctx[5];
    volatile long errors = 0;
    volatile long stop = 0;
    uint32_t i;
#if defined(_WIN32)
    HANDLE threads[5];
#else
    pthread_t threads[5];
#endif

    printf("\n=== v4: index MRSW (4 readers + 1 writer) ===\n");

    err = ef_open_memory_hash(arena, sizeof(arena), 128, 64, 1, &db);
    expect_err(err, EF_OK, "mrsr open");
    expect_true(db != NULL, "mrsr db");
    if (db == NULL) {
        return;
    }
    expect_true(db->sb->schema_version == EF_SCHEMA_VERSION, "mrsr schema v4");

    ef_index_mrsr_seed_keys(db);

    for (i = 0; i < 5; ++i) {
        ctx[i].db = db;
        ctx[i].thread_id = (int)i;
        ctx[i].errors = &errors;
        ctx[i].stop = &stop;
#if defined(_WIN32)
        threads[i] = (HANDLE)_beginthreadex(
            NULL, 0,
            (i < 4) ? ef_index_mrsr_reader_win : ef_index_mrsr_writer_win,
            &ctx[i], 0, NULL);
        if (threads[i] == NULL) {
            expect_true(0, "mrsr thread create");
            ef_close(db);
            return;
        }
#else
        if (pthread_create(&threads[i], NULL,
                           (i < 4) ? ef_index_mrsr_reader_pthread : ef_index_mrsr_writer_pthread,
                           &ctx[i]) != 0) {
            expect_true(0, "mrsr thread create");
            ef_close(db);
            return;
        }
#endif
    }

#if defined(_WIN32)
    if (!mpmc_join_threads_win32(threads, 5, 60000, "test_index_mrsr")) {
        expect_true(0, "mrsr threads finished");
    }
    for (i = 0; i < 5; ++i) {
        CloseHandle(threads[i]);
    }
#else
    if (!mpmc_join_threads_pthread(threads, 5, "test_index_mrsr")) {
        expect_true(0, "mrsr threads finished");
    }
#endif

    expect_true(errors == 0, "mrsr reader/writer errors");
    ef_close(db);
}

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


int main(void)
{
    printf("platform: %s\n", ef_platform_name());

    test_v3_alloc_queue_index();
    test_execute_queue_and_index();
    test_index_get_slot();
    test_index_lifecycle_and_rehash();
    test_index_auto_rehash();
#if EF_HAS_FILE_IO
    test_v3_to_v4_index_migration();
    test_index_mrsr();
    test_queue_mpmc();
#endif

    return test_finish("Index and queue tests");
}
