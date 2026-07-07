#ifndef ENDFIELDS_TEST_COMMON_H
#define ENDFIELDS_TEST_COMMON_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "endfields.h"
#include "ef_config.h"
#include "ef_index.h"
#include "ef_sb_layout.h"
#include "ef_crc.h"
#include "ef_crc_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

extern int g_failures;

void expect_true(int cond, const char *msg);
void expect_err(enum ef_err got, enum ef_err want, const char *msg);
int main_io_read(FILE *fp, void *buf, size_t nbytes);
int main_io_write(FILE *fp, const void *buf, size_t nbytes);
double now_seconds(void);
void remove_test_file(const char *path);
int test_finish(const char *label);

#endif
