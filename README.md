# Endfields DB

[![CI](https://github.com/zayokami/EndfieldsDB/actions/workflows/ci.yml/badge.svg)](https://github.com/zayokami/EndfieldsDB/actions/workflows/ci.yml)

一个面向 C11 的嵌入式数据库核心：以固定 64 字节槽位 + 文件/内存物理偏移寻址为基础，
提供持久化 LIFO 空闲链、跨进程 MPMC 队列、Robin Hood 字符串哈希索引、可中止事务和按需 Schema 迁移。

代码量小、依赖少（仅 C 标准库与平台 mmap API），适合塞进服务端、嵌入式或工具里当一个轻量持久层。

---

## 它解决什么问题

很多 C 项目需要一个小而可控的本地数据存储：写几条记录、读回来、可能要按字符串 key 取、要能跨进程排队消费、最好能 ACID 事务和崩溃恢复。

Endfields DB 把这些做成一个**库**，而不是一个独立服务：

- **不分层** — 没有独立的 server / 协议 / 客户端，直接 `#include "endfields.h"` 调用 `ef_alloc` / `ef_write_payload` / `ef_index_get` / `ef_txn_begin`。
- **物理偏移寻址** — 数据按 64 字节槽位对齐，`ef_offset_to_ptr(slot_id)` 是 O(1) 的指针查表，方便 mmap / `ef_chase` 之类的指针追逐热路径。
- **零拷贝** — 槽位直接映射到调用方地址空间，payload 字段就是你文件的对应字节；不需要序列化层。
- **一个文件，多种后端** — 同一份库既可以用 `ef_open_ex` 落到磁盘 mmap，也可以用 `ef_open_memory` 跑在嵌入式 RAM arena 上；schema、API、CRC、并发模型全部一致。
- **可选能力按需启用** — 没有索引？`hash_capacity=0`，文件布局与传统 v3 一样；要 ACID？包一层 `ef_txn_begin` / `commit`。

---

## 能力一览

| 类别 | 提供 |
|------|------|
| 持久化 | 文件 mmap（POSIX `mmap` / Win32 `MapViewOfFile`）或纯 RAM arena；`ef_sync` 显式落盘 |
| 数据模型 | 固定 64 字节槽位、48 字节内联 payload；超大对象走 OVERFLOW 链式 blob |
| 寻址 | slot id ↔ 物理偏移 ↔ mmap 指针；`ef_chase` / `ef_chase_n` 走 `next_offset` 链 |
| 分配器 | 持久化 LIFO 空闲链（`ef_alloc_slot` / `ef_free_slot`）；池空时 `ef_alloc` 自动 grow |
| 索引 | Robin Hood 字符串哈希（`ef_index_put/get/remove/iterate/clear`），自动 rehash + 手动 shrink |
| 队列 | 跨进程 MPMC dummy-head FIFO（`ef_queue_push/pop`），超级块自旋锁 |
| 事务 (v5) | `ef_txn_begin` / `commit` / `abort`；strict serializable isolation；abort 反向 replay undo log；进程崩溃自动 recover |
| 校验 | 槽位头 CRC32 + 超级块延迟 CRC 提交；x86 PCLMULQDQ 快路径 |
| 迁移 | v1 → v2/v3 → v4 → v5 在线升级，可写打开自动追加 undo log 段；只读打开保持旧布局 |

当前 schema 版本：**v5**（`EF_SCHEMA_VERSION 5`，在 `src/ef_config.h`）。

---

## 30 秒上手

```c
#include "endfields.h"
#include "ef_index.h"

struct ef_db *db = NULL;

/* 文件 + Robin Hood 索引（hash_capacity 必须为 2 的幂，≤ 65535） */
if (ef_open_ex_hash("data.endf", 64, 256, &db) != EF_OK || db == NULL) {
    return 1;
}

/* 写一条记录并索引 */
uint64_t id = 0;
ef_alloc(db, &id);
ef_write_payload(db, id, "hello", 5);
ef_index_put(db, "greeting", id);

/* 按 key 取出来 */
uint64_t found = 0;
if (ef_index_get(db, "greeting", &found) == EF_OK) {
    /* ... */
}

/* ACID 事务：失败自动回滚 */
ef_txn_begin(db);
ef_alloc_slot(db, &id);
ef_write_payload(db, id, "draft", 5);
ef_index_put(db, "k", id);
if (/* 业务条件不满足 */ 0) {
    ef_txn_abort(db);   /* 所有写入全部撤销 */
} else {
    ef_txn_commit(db);  /* 原子生效 */
}

ef_sync(db);
ef_close(db);
```

完整 API 列表见 [`src/endfields.h`](src/endfields.h)、[`src/ef_index.h`](src/ef_index.h)、[`src/ef_txn.h`](src/ef_txn.h)。

---

## 关键概念

### 槽位 (slot)

所有数据都装在 **64 字节**对齐的槽里，超级块自身也是一个槽：

```
status u32 | header_crc u32 | payload[48] | next_offset u64   →  共 64 字节
```

`status` 取值见 [`src/endfields.h`](src/endfields.h)（`EF_STATUS_USED` / `_FREE` / `_OVERFLOW` / `_QUEUED` / `_QUEUE_DUMMY` 等）。`next_offset` 既给空闲链当链表指针，也给 blob 链和队列当续接指针；`ef_chase` / `ef_chase_n` 就是顺着它走的。

### 物理寻址

数据地址用 **文件/buffer 内的字节偏移** 表示，不是内存指针：

| 转换 | 用途 |
|------|------|
| `ef_slot_to_offset(slot_id)` | id → 物理偏移 |
| `ef_offset_to_slot_id(offset)` | 偏移 → id（用于反查） |
| `ef_offset_to_ptr(db, offset)` | 偏移 → 当前 mmap/buffer 内的可读写指针 |
| `ef_chase(db, start, fn, ctx)` | 沿 `next_offset` 单跳或多跳遍历 |

这意味着槽位可以被 mmap 到不同地址、文件可以被重映射、rehash 可以挪动整个 slot 区域——只要偏移对得上就行。

### 文件布局

```
v5（带索引 + 事务）:
[超级块 64B][哈希索引 capacity × 16B][数据槽区 max_slots × 64B][Undo Log 段]

v3/v4（带索引，无事务）:
[超级块 64B][哈希索引 capacity × 16B][数据槽区 max_slots × 64B]

v3/v4（无索引，向后兼容）:
[超级块 64B][数据槽区 max_slots × 64B]
```

新建数据库时 `hash_capacity=0` 就是无索引的紧凑布局；任何 schema 都会在可写打开时按需自动迁移到 v5，并补齐 undo log 段，**不会触碰已有数据**。

### FIFO 队列

dummy-head 链表，head / tail / lock 都放在超级块 `reserved[]` 里。push 和 pop 在同一把自旋锁下完成；空队列 lazy 分配 dummy 哨兵槽。

- `ef_queue_empty` 是**无锁**快路径，仅作启发式判断。
- 多消费者在所有生产者结束后应当用 `ef_queue_drained`（持锁检查）确认排空再退出。
- 高争用返回 `EF_ERR_QUEUE_BUSY`，调用方应重试。

### 索引

字符串键经 FNV-1a 哈希为 `uint64_t`，Robin Hood 线性探测，**只存哈希不存原 key**。

- 装载率超 `3/4` 时 `ef_index_put` 自动 rehash 到下一 2 的幂容量（上限 `EF_INDEX_MAX_CAPACITY = 65535`）。
- `ef_index_get` 用 seqlock 实现**多读者无锁**读；遇到写者时返回 `EF_ERR_INDEX_BUSY`，调用方应重试。
- 写者（put / remove / rehash / clear）共用 `index_write_lock` 串行化。
- `ef_index_rehash` **会搬迁 slot 区**，调用方不得与槽位写并发。

### 事务 (v5)

```c
ef_txn_begin(db);
// 所有 mutating API（alloc/free/index_put/index_remove/queue_push/queue_pop/write_payload/write_blob...）
// 都会向 undo log 追加反向记录
ef_txn_commit(db);   // 提交：清空 undo log，释放 txn_lock
ef_txn_abort(db);    // 回滚：反向 replay undo log 还原，再释放锁
```

要点：

- **strict serializable isolation**：通过独立 `txn_lock` 与 `index_write_lock` / `queue_lock` 解耦，无死锁。
- 事务期间 `ef_grow` 返回 `EF_ERR_GROW`；`ef_index_put` 触发 rehash 时返回 `EF_ERR_INDEX_BUSY`（必须先在外面扩好容量）。
- **跨进程崩溃恢复**：open 时若检测到 `txn_state == ACTIVE` 且当前进程 pid / epoch 不匹配，自动 replay undo log 后释放锁，数据库回到一致状态。
- v4 → v5 迁移对已有数据透明：可写打开时由 `ef_sb_migrate_v4_txn_layout` 追加 undo log 段并刷新超级块 CRC。

---

## 并发模型速览

详细说明见 [`THREADING.md`](THREADING.md)。快速记忆：

| 资源 | 并发安全 |
|------|----------|
| `ef_queue_push` / `ef_queue_pop` | ✅ MPMC（跨进程 + 跨线程） |
| `ef_index_get` | ✅ 多读者无锁（seqlock） |
| `ef_index_put` / `remove` / `rehash` | 🔒 单写者（`index_write_lock`），多线程排队 |
| `ef_txn_begin` / `commit` / `abort` | 🔒 单事务（`txn_lock` CAS），与上面两把锁无死锁 |
| 空闲链 | ✅ GCC/Clang 下 CAS 弹入/弹出 |
| 槽位 payload / blob 数据 / chase | ⚠️ 调用方外部同步 |
| `ef_sync` / `ef_close` | ⚠️ 调用方串行化所有写者 |

事务期间禁止 grow 和 auto-rehash，调用方在 begin 之前应当**预分配好容量**。

---

## 构建

需要 **CMake ≥ 3.16** + 一个支持 C11 的编译器（GCC / Clang / MSVC / MinGW）。

### 最简构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows MinGW：

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### 严格 CI（本地复现）

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DENDFIELDS_WARNINGS_AS_ERRORS=ON -DENDFIELDS_CI_FAST=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 900 --no-tests=error
```

CI 矩阵见 [`.github/workflows/ci.yml`](.github/workflows/ci.yml)：Linux GCC / Clang（Release、Debug、ASan+UBSan、TSan、coverage）、macOS Clang、Windows MSVC、Windows MinGW、static analysis（clang-tidy + cppcheck）、embedded-only。

### CMake 选项

| 选项 | 默认 | 作用 |
|------|------|------|
| `ENDFIELDS_EMBEDDED_ONLY` | OFF | 只构建纯 RAM 后端 |
| `ENDFIELDS_ENABLE_PREFETCH` | ON | 追逐热路径启用 `__builtin_prefetch` |
| `ENDFIELDS_WARNINGS_AS_ERRORS` | OFF | `-Werror` |
| `ENDFIELDS_SANITIZE` | OFF | ASan + UBSan |
| `ENDFIELDS_TSAN` | OFF | ThreadSanitizer |
| `ENDFIELDS_COVERAGE` | OFF | gcov / lcov |
| `ENDFIELDS_CI_FAST` | OFF | 缩短 bench 轮次 |

### 链接

```cmake
target_link_libraries(your_app PRIVATE endfields)
target_include_directories(your_app PRIVATE path/to/endfields-db/src)
```

产物：`libendfields.a`（文件 + 内存后端）、`libendfields_embedded.a`（纯 RAM）、
四个测试可执行文件 `endfields_core_test` / `endfields_index_queue_test` / `endfields_embedded_test` / `endfields_bench`。

---

## 测试覆盖

测试入口拆分（见 [`CMakeLists.txt`](CMakeLists.txt)）：

- `tests/test_core.c` — 基础读写、CRC、blob、grow、reopen、事务 commit/abort/persist/grow-forbidden
- `tests/test_index_queue.c` — 索引 put/get/rehash/shrink/iterate、队列 MPMC、v3→v4 / v4→v5 迁移、事务下队列/索引行为
- `src/main_embedded.c` — 纯 RAM 后端
- `bench/endfields_bench.c` — chase / queue / MPMC / hash / `bench_txn_roundtrip`

跑单个二进制：`./build/endfields_core_test`；按 ctest 名过滤：`ctest --test-dir build -R txn --output-on-failure`。

---

## 项目结构

```
endfields-db/
├── CMakeLists.txt
├── README.md                      # 你正在读的
├── THREADING.md                    # 并发与跨进程语义
├── PROJECT_INDEX.md                # 代码库接手索引
├── CLAUDE.md                       # 给 AI 助手的项目导览
├── src/
│   ├── endfields.h / .c            # 公共 API 与核心实现
│   ├── ef_index.h / .c             # Robin Hood 索引（v4 seqlock + 自动 rehash + shrink）
│   ├── ef_sb_layout.h / .c         # 超级块 reserved[] 布局与 v3→v4→v5 迁移
│   ├── ef_txn.h / .c               # 事务 API 与状态机 (v5)
│   ├── ef_undo.h / .c              # undo log 段、记录格式、replay/reset (v5)
│   ├── ef_blob.c                   # 大对象链
│   ├── ef_grow.c                   # 自动扩容
│   ├── ef_port.h / .c              # 文件/内存 I/O 抽象
│   ├── ef_atomic_unaligned.h       # mmap 字段原子 helper
│   ├── ef_crc.h / .c / _pclmul.c   # CRC32 portable + x86 PCLMULQDQ
│   ├── ef_config.h                 # schema 版本、平台开关
│   ├── main.c / main_embedded.c    # 历史测试入口（已拆分为 tests/）
├── tests/
│   ├── test_common.h / .c
│   ├── test_core.c
│   └── test_index_queue.c
├── bench/
│   └── endfields_bench.c
└── .github/workflows/ci.yml
```

---

## 接手顺序建议

1. **想用 API**：本 README 的「30 秒上手」+ 头文件 [`src/endfields.h`](src/endfields.h)。
2. **关心并发**：[`THREADING.md`](THREADING.md)。
3. **想理解代码组织**：[`PROJECT_INDEX.md`](PROJECT_INDEX.md)。
4. **修改 `superblock.reserved[]` 布局**：永远只动 [`src/ef_sb_layout.h`](src/ef_sb_layout.h) / `.c`，并在三种迁移路径（v3→v4、v4→v5、嵌入式初始化）里都加测试。
5. **改并发路径**：本地同时跑 ASan + UBSan 与 TSan 配置；新增功能测试时尽量加一个并发变种。

---

## 许可证

[MIT License](LICENSE) — Copyright (c) 2026 zayoka