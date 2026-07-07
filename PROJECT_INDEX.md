# Endfields DB Project Index

更新时间: 2026-07-07

本索引用于快速接手代码库。它描述当前工作区内容，而不只是不含未提交改动的 Git 基线。

## 项目定位

Endfields DB 是一个 C11 实现的嵌入式数据库核心。核心设计是固定 64 字节槽位、mmap/内存后端、物理偏移寻址、持久化空闲链、FIFO 队列、Robin Hood 字符串哈希索引和 CRC 校验。

当前源码版本使用 `EF_SCHEMA_VERSION 4`。v4 在 v3 的队列和哈希索引基础上，为索引增加写锁和 seqlock 读一致性保护。

## 源码入口

| 路径 | 角色 |
|------|------|
| `src/endfields.h` | 公共 API、槽位/超级块/数据库句柄结构、错误码、命令格式 |
| `src/endfields.c` | 核心实现: 打开/关闭、布局绑定、槽位访问、分配、扩容、blob、队列、CRC 提交、指令执行 |
| `src/ef_index.h` | Robin Hood 哈希索引公共 API 和容量/装载因子常量 |
| `src/ef_index.c` | 索引 put/get/remove/rehash/clear、v4 seqlock 读、写锁、槽区搬迁修正 |
| `src/ef_sb_layout.h` | 超级块 `reserved[28]` 的 v3/v4 字段布局声明 |
| `src/ef_sb_layout.c` | 超级块哈希容量、队列锁、索引写锁、index seq 和 v3->v4 布局迁移 |
| `src/ef_port.h` | 文件/内存映射 I/O 抽象接口 |
| `src/ef_port.c` | POSIX/Win32 文件映射、truncate/grow、sync、纯内存后端 |
| `src/ef_crc.h` | CRC32 公共接口 |
| `src/ef_crc.c` | portable slicing-by-4 CRC32 |
| `src/ef_crc_internal.h` | CRC 内部接口，连接 portable 与 PCLMUL 实现 |
| `src/ef_crc_pclmul.c` | x86/x64 PCLMULQDQ CRC32 快路径 |
| `src/ef_atomic_unaligned.h` | 非自然对齐 mmap 字段上的 u8/u32/u64 原子 load/store/CAS helper |
| `src/ef_config.h` | schema 版本、平台开关、prefetch、原子能力、branch hint |
| `src/main.c` | 文件+内存综合测试、并发测试、性能 bench |
| `src/main_embedded.c` | 纯 RAM/embedded-only 测试 |

## 构建与测试

主构建文件: `CMakeLists.txt`

Targets:

| Target | 类型 | 说明 |
|--------|------|------|
| `endfields` | static lib | 文件 + 内存后端 |
| `endfields_embedded` | static lib | 纯内存后端，定义 `EF_PLATFORM_EMBEDDED` |
| `endfields_test` | executable | `src/main.c`，非 embedded-only 构建时启用 |
| `endfields_embedded_test` | executable | `src/main_embedded.c` |

CMake 选项:

| 选项 | 默认 | 用途 |
|------|------|------|
| `ENDFIELDS_EMBEDDED_ONLY` | OFF | 只构建 RAM 后端，不构建文件后端测试 |
| `ENDFIELDS_ENABLE_PREFETCH` | ON | 给追逐热路径启用 `EF_ENABLE_PREFETCH` |
| `ENDFIELDS_WARNINGS_AS_ERRORS` | OFF | CI 中开启 |
| `ENDFIELDS_SANITIZE` | OFF | ASan + UBSan |
| `ENDFIELDS_TSAN` | OFF | ThreadSanitizer |
| `ENDFIELDS_COVERAGE` | OFF | gcov/lcov 覆盖率 |
| `ENDFIELDS_CI_FAST` | OFF | 缩短 bench 轮次 |

常用命令:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows MinGW:

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CI 在 `.github/workflows/ci.yml` 中覆盖 Linux GCC/Clang、macOS Clang、Windows MSVC、Windows MinGW、coverage、clang-tidy、cppcheck、embedded-only。严格 CI 传入 `ENDFIELDS_WARNINGS_AS_ERRORS=ON` 和 `ENDFIELDS_CI_FAST=ON`。

## 数据布局

公共常量在 `src/endfields.h`:

| 常量 | 值 | 含义 |
|------|----|------|
| `EF_SLOT_SIZE` | 64 | 固定槽位大小 |
| `EF_PAYLOAD_SIZE` | 48 | v2+ 槽内 payload 大小 |
| `EF_SUPERBLOCK_SIZE` | 64 | 超级块大小 |
| `EF_HASH_ENTRY_SIZE` | 16 | 哈希项大小 |

Schema v4 哈希文件布局:

```text
[superblock 64B][hash index capacity * 16B][slots max_slots * 64B]
```

无哈希区布局:

```text
[superblock 64B][slots max_slots * 64B]
```

槽位布局:

```text
status u32 | header_crc u32 | payload[48] | next_offset u64
```

v4 `superblock.reserved[28]` 由 `src/ef_sb_layout.h` 定义:

| 范围 | 字段 |
|------|------|
| `[0..3]` | superblock CRC32 |
| `[4..11]` | queue head offset |
| `[12..19]` | queue tail offset |
| `[20..21]` | hash capacity u16 |
| `[22]` | queue lock u8 |
| `[23]` | index write lock u8 |
| `[24..27]` | index seq u32 |

注意: README、THREADING.md 与本索引均已同步到 schema v4；判断行为事实时仍优先读 `src/*.h` 和实现。

## 子系统地图

### 打开、校验、迁移

主要在 `src/endfields.c`:

- `ef_open_ex_hash` / `ef_open_ex`: 文件后端创建或打开。
- `ef_open_readonly_ex`: 只读 mmap 打开。
- `ef_open_memory_hash` / `ef_open_memory`: 纯内存 buffer 后端。
- `ef_db_init_mapped`: 绑定 mmap、校验或初始化超级块、构建/迁移布局。
- `ef_validate_superblock`: magic、版本、槽大小、文件大小、CRC 等校验。
- `ef_upgrade`: v1/v2/v3 升到当前 schema。
- `ef_sb_migrate_v3_index_layout`: v3 reserved 布局迁移为 v4。

### 物理寻址与槽位访问

- `ef_slot_to_offset`: slot id -> 物理偏移。
- `ef_offset_to_slot_id`: 物理偏移 -> slot id。
- `ef_offset_to_ptr`: 物理偏移 -> mmap 指针。
- `ef_get_slot`: 边界 + 状态 + CRC 校验。
- `ef_peek_slot`: 只做边界检查，用于热路径或已持锁路径。
- `ef_chase` / `ef_chase_n`: 通过 `next_offset` 指针追逐。

### 分配、释放、扩容

- 空闲链是持久化 LIFO 链表，头指针在超级块。
- `ef_alloc_slot`: 从空闲链弹出，池空返回 `EF_ERR_SLOT_FULL`。
- `ef_alloc`: 池空时调用 `ef_grow(max_slots + 1)` 后重试。
- `ef_free_slot`: 释放前会移除指向该槽的索引项。
- `ef_return_slot_to_pool`: 内部归还槽，队列出队路径在锁内使用。
- `ef_grow`: 文件 truncate/remap 或内存容量内扩展，并追加新空闲槽。

### Blob

- `ef_write_blob` / `ef_read_blob` 在普通槽上实现大对象。
- 头槽 payload 前 8 字节是 magic + total length。
- 头槽最多内联 40 字节，后续数据使用 `EF_STATUS_OVERFLOW` 槽链。
- `ef_write_payload` 与 blob 格式是不同 API 层，不应混用解释。

### FIFO 队列

主要在 `src/endfields.c`:

- `ef_queue_push` / `ef_queue_pop`: dummy-head FIFO。
- `queue_head_offset` / `queue_tail_offset` / `queue_lock` 存在超级块 reserved。
- `ef_queue_empty`: 无锁快路径，只适合启发式判断。
- `ef_queue_drained`: 持锁检查，用于多消费者在生产结束后退出。
- 高争用可能返回 `EF_ERR_QUEUE_BUSY`，调用方应重试。

### Robin Hood 索引

主要在 `src/ef_index.c`:

- `ef_key_hash`: FNV-1a 字符串哈希。
- `ef_index_put`: key -> slot，当前工作区新增了自动 rehash 尝试。
- `ef_index_get`: v4 下用 seqlock 做无全局读锁一致性读。
- `ef_index_remove` / `ef_index_remove_by_slot`: 删除键或删除指向槽的项。
- `ef_index_rehash`: 扩容哈希区、搬迁槽区、修正 free list/queue/next_offset，再重插索引项。
- `ef_index_clear`: 清空哈希区。
- `ef_index_capacity` / `ef_index_count_entries`: 当前工作区新增的索引观测 API。

装载因子常量在 `src/ef_index.h`:

- `EF_INDEX_REHASH_LOAD_FACTOR_NUM = 3`
- `EF_INDEX_REHASH_LOAD_FACTOR_DEN = 4`
- `EF_INDEX_MAX_CAPACITY = 0x8000`

### CRC 与持久化

- 超级块 CRC 延迟提交: `ef_db_mark_meta_dirty` 标脏，`ef_db_commit_meta` / `ef_sync` / `ef_close` 刷新。
- 槽头 CRC 保护 `USED`、`OVERFLOW`、队列相关状态。
- 队列节点部分状态使用 link CRC，只覆盖 status + next_offset，以降低队列开销。
- CRC32 portable 实现在 `src/ef_crc.c`，x86/x64 可用 `src/ef_crc_pclmul.c` 快路径。

### 指令执行

`ef_execute` 接收 10 字节 `struct ef_cmd`，当前 opcode 覆盖 get slot、chase、field read/write、payload write、set next/status、alloc/free、chase_n。

## 并发语义

详细说明见 `THREADING.md`。当前关键约束:

- `ef_queue_push` / `ef_queue_pop` 支持 MPMC，同进程多线程和跨进程 mmap 均按设计支持。
- 空闲链在支持原子操作的平台上使用 CAS。
- v4 索引读是多读者 seqlock；索引写通过 `index_write_lock` 串行化。
- 槽位 payload 读写、blob、指针追逐、迭代仍需调用方外部同步。
- `ef_sync` / `ef_close` 需要调用方串行化。
- `ef_index_rehash` 会搬迁槽区；调用方不得与槽位写并发执行。

## 测试索引

`src/main.c` 的主要测试覆盖:

- 基础 offset roundtrip、payload 写读、`ef_execute`、chase/chase_n。
- 内存后端 reopen、slot iterator、blob、v1 upgrade、grow。
- 文件后端 blob、reopen、bad magic、readonly、superblock/slot CRC。
- v3/v4 队列和索引生命周期、rehash、v3->v4 index migration。
- 当前工作区新增 `test_index_auto_rehash`。
- `test_queue_mpmc`: 多生产者/多消费者队列。
- `test_index_mrsr`: 多读者 + 单写者索引并发。
- 性能套件包含 chase、queue roundtrip、MPMC throughput、hash put/get/remove/rehash、当前工作区新增 auto rehash bench。

`src/main_embedded.c` 覆盖 embedded-only 内存打开、读写、grow、reopen。

## 当前开发重点

索引自动 rehash 已接入 `ef_index_put`:

- 新键插入后若会超过 `3/4` 装载率阈值，先扩容到满足阈值的下一个 2 的幂容量。
- 更新已有 key 不触发扩容，也不增加 entry count。
- rehash 会搬迁槽区；插入前必须基于当前 `slots_base` 重新计算 slot offset。
- 内存后端容量不足时返回 `EF_ERR_FILE_SIZE`，旧索引保持可用，新 key 不插入。
- `bench-out.txt`、`build*/`、根目录 `*.endf` 和 `*.exe` 是本地运行或构建产物，不应作为源码索引依据。

## 接手建议

1. 判断行为事实时优先读 `src/*.h` 和实现，其次读 `THREADING.md`、README 与本索引。
2. 修改索引或队列前先确认 `reserved[28]` 布局，不要直接硬编码偏移，优先用 `ef_sb_layout.*` helper。
3. 修改 rehash 时必须同时考虑槽区搬迁、free list、queue head/tail、槽内 `next_offset` 和哈希项 slot offset。
4. 修改队列出队或释放路径时注意索引删除和归还空闲链的顺序。
5. 并发改动至少跑普通测试；涉及队列/索引时额外跑 TSAN 或相关 CI 配置。
