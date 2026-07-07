#include "test_common.h"

int g_failures = 0;

void expect_true(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_failures;
    }
}

void expect_err(enum ef_err got, enum ef_err want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %s, want %s)\n",
                msg, ef_strerror(got), ef_strerror(want));
        ++g_failures;
    }
}

int main_io_read(FILE *fp, void *buf, size_t nbytes)
{
    if (fp == NULL || buf == NULL) {
        return 0;
    }
    return fread(buf, 1, nbytes, fp) == nbytes;
}

int main_io_write(FILE *fp, const void *buf, size_t nbytes)
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

double now_seconds(void)
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

void remove_test_file(const char *path)
{
    remove(path);
}

int test_finish(const char *label)
{
    if (g_failures != 0) {
        fprintf(stderr, "%s: %d test(s) failed\n", label, g_failures);
        return 1;
    }

    printf("%s passed.\n", label);
    return 0;
}