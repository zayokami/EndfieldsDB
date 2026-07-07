#include "test_common.h"

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
#error "MPMC bench requires Win32 threads or pthread (GCC/Clang)"
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

#if defined(_WIN32)
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

#endif /* EF_HAS_FILE_IO */

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
    static alignas(64) uint8_t hash_arena[64 + 1024 * 16 + 512 * 64];
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
    const int auto_iters = 512;
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

    for (r = 0; r < BENCH_ROUNDS; ++r) {
        int j;
        uint32_t cap_before;

        err = ef_open_memory_hash(hash_arena, sizeof(hash_arena), 512, 16, 1, &db);
        if (err != EF_OK || db == NULL) {
            break;
        }
        cap_before = ef_index_capacity(db);
        t0 = now_seconds();
        for (j = 0; j < auto_iters; ++j) {
            snprintf(key, sizeof(key), "au-%d-%04d", r, j);
            err = ef_alloc(db, &slot_id);
            if (err != EF_OK) {
                break;
            }
            err = ef_index_put(db, key, slot_id);
            if (err != EF_OK) {
                *sink ^= (uintptr_t)err;
                break;
            }
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) * 1e9 / (double)j;
        *sink ^= (uintptr_t)(db->hash_capacity != cap_before);
        ef_close(db);
        db = NULL;
    }
    bench_print_stats("ef_index_put + auto rehash (cap 16->grows)", auto_iters, BENCH_ROUNDS, samples);
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

    chase_cur = ef_peek_slot(db, 0);
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
        t0 = now_seconds();
        for (i = 0; i < verify_iters; ++i) {
            sink ^= (uintptr_t)ef_peek_slot(db, (uint64_t)(i % 32));
        }
        t1 = now_seconds();
        samples[r] = (t1 - t0) / (double)verify_iters * 1e9;
    }
    bench_print_stats("ef_peek_slot (no CRC)", verify_iters, BENCH_ROUNDS, samples);

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
    static alignas(64) uint8_t arena[64 + 256 * 64];
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
    run_memory_engineering();

#if EF_HAS_FILE_IO
    run_engineering_scenarios();
    remove_test_file("bench.endf");
    err = ef_open_ex("bench.endf", 100, &db);
    if (err != EF_OK || db == NULL) {
        fprintf(stderr, "bench open failed: %s\n", ef_strerror(err));
        return 1;
    }
    run_perf_suite(db);
    ef_close(db);
    remove_test_file("bench.endf");
#else
    (void)db;
    (void)err;
#endif

    return 0;
}
