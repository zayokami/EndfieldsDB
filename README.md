# Endfields DB

轻量级、零拷贝、基于 mmap 物理寻址的嵌入式数据库核心，使用 C11 实现。

## 特性

- **固定 64 字节槽位**：超级块与槽位均为 64 字节对齐，适合缓存友好访问
- **零拷贝**：通过 mmap 直接按文件偏移读写，无额外序列化层
- **物理寻址**：`ef_slot_to_offset` / `ef_offset_to_ptr` 实现 O(1) 槽位定位
- **指针追逐**：单跳 `ef_chase`、多跳 `ef_chase_n`，带环检测与可选 CPU prefetch
- **槽位池**：持久化 LIFO 空闲链表（`ef_alloc_slot` / `ef_free_slot`），复用 `next_offset`，无额外元数据开销
- **尾部自动扩容**（v3）：`ef_alloc` 在空闲链耗尽时 `ef_grow(+1)` 追加槽位
- **跨进程 MPMC 队列**（v3）：`ef_queue_push` / `ef_queue_pop`，dummy 头节点 + 共享自旋锁，多生产者/多消费者安全；出队在锁内归还槽位，空闲链 CAS 弹入/弹出支持并发分配
- **Robin Hood 哈希索引**（v3）：`ef_index_put` / `ef_index_get`，字符串键直达槽位，装载率可达 90%+
- **索引生命周期**（v3）：`ef_free_slot` 自动 `ef_index_remove_by_slot`；`ef_index_rehash` 扩容哈希区并搬迁槽区、修正所有物理偏移
- **紧凑指令集**：`ef_execute` 通过 10 字节 `ef_cmd` 派发读写、追逐、分配等操作
- **动态扩容**：`ef_grow` 扩展槽位数量（文件 truncate + 重映射，或内存后端）
- **只读打开**：`ef_open_readonly` 以只读 mmap 打开，写操作返回 `EF_ERR_READONLY`
- **槽位迭代**：`ef_foreach_used` / `ef_slot_iter` 遍历已用头槽（跳过溢出续链槽与队列槽）
- **溢出链 Blob**：`ef_write_blob` / `ef_read_blob` 支持超过 48 字节的大对象存储
- **数据校验**（Schema v2+）：超级块 CRC32 + 已用/队列/溢出槽位头 CRC32；超级块 CRC **延迟提交**（`ef_db_commit_meta` / `ef_sync` / `ef_close`）
- **在线迁移**：`ef_needs_upgrade` / `ef_upgrade` 将 Schema v1 升级为 v2/v3（内联 payload 52→48 字节，尾部 4 字节丢弃）
- **追逐热路径优化**：`ef_chase` / `ef_chase_n` 使用位移寻址，跳过逐跳 CRC（`ef_get_slot` 仍校验）
- **多后端**：文件（POSIX / Win32）与纯内存（嵌入式 RAM arena）
- **持久化**：Linux `msync`；Windows `FlushViewOfFile` + `FlushFileBuffers`

## 目录结构

```
endfields-db/
├── CMakeLists.txt
├── LICENSE
├── README.md
└── src/
    ├── endfields.h / endfields.c   # 核心 API
    ├── ef_config.h                   # 编译选项与 schema 版本
    ├── ef_index.h / ef_index.c       # Robin Hood 字符串索引
    ├── ef_port.h / ef_port.c         # 平台 I/O 抽象
    ├── ef_crc.h / ef_crc.c           # CRC32
    ├── main.c                        # 文件 + 内存综合测试
    └── main_embedded.c               # 纯嵌入式测试
```

## 构建

需要 **CMake ≥ 3.16** 与 **GCC**（推荐 MinGW on Windows）。

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

产物：

- `libendfields.a` — 文件 + 内存后端
- `libendfields_embedded.a` — 纯 RAM 后端（无文件 I/O）
- `endfields_test` / `endfields_embedded_test` — 测试可执行文件

链接示例：

```cmake
target_link_libraries(your_app PRIVATE endfields)
target_include_directories(your_app PRIVATE path/to/endfields/src)
```

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `ENDFIELDS_EMBEDDED_ONLY` | OFF | 仅 RAM 后端，禁用文件 I/O |
| `ENDFIELDS_ENABLE_PREFETCH` | ON | 追逐热路径启用 `__builtin_prefetch` |

内存后端 `ef_grow` 在 `map_capacity` 范围内扩展 `file_size`；`ef_open_memory` 重开时会按超级块中的 `max_slots` 自动识别已扩容大小。

## 测试覆盖

- `test_grow_memory` — 拒绝非法扩容、4→8→16 扩容、数据保留、重开识别
- `test_slot_header_crc_*` — 头 CRC / 溢出槽 CRC 篡改、`ef_foreach_used` / `ef_slot_iter` 中止、磁盘篡改后只读打开拒绝
- `test_v3_alloc_queue_index` — 尾部 grow 分配、FIFO 队列往返、Robin Hood 索引 put/get/remove、队列持久化重开
- `test_index_lifecycle_and_rehash` — 索引 put → rehash 16→32 → free 后 get 应 NOT_FOUND
- `test_queue_mpmc` — Windows 4 线程（2 生产者 × 200，2 消费者），400 条消息各送达一次；消费者用 `ef_queue_drained` 在锁下确认排空后退出

性能套件（`main.c`）另含 **MPMC 吞吐 bench**（5 轮 × 4 线程 × 4000 消息），槽位池按 `消息数 + 64` 预分配以避免高并发下槽位耗尽。

## 快速示例

```c
#include "endfields.h"
#include "ef_index.h"

/* 标准打开（无哈希区，兼容 v2 文件布局） */
struct ef_db *db = NULL;
ef_open_ex("data.endf", 64, &db);

/* 带 Robin Hood 索引区（capacity 须为 2 的幂） */
ef_open_ex_hash("data.endf", 64, 256, &db);

if (db) {
    uint64_t id;
    ef_alloc(db, &id);                    /* 弹空闲链，否则尾部 +1 grow */
    ef_write_payload(db, id, "hello", 5);
    ef_index_put(db, "greeting", id);

    uint64_t found;
    ef_index_get(db, "greeting", &found);

    ef_queue_push(db, "msg", 3);
    char buf[64];
    size_t n;
    ef_queue_pop(db, buf, sizeof(buf), &n);

    /* 多线程消费者：生产者全部结束后，在锁下确认队列已排空 */
    if (ef_queue_drained(db)) { /* safe to shut down workers */ }

    /* 哈希表扩容（new_capacity 须为 2 的幂且 > 当前容量） */
    ef_index_rehash(db, 512);
    ef_db_refresh_slot_crcs(db);   /* rehash 后如启用槽 CRC 可统一刷新 */

    ef_sync(db);
    ef_close(db);
}

/* 只读 */
struct ef_db *ro = ef_open_readonly("data.endf");
if (ro) {
    /* 读操作可用，写操作返回 EF_ERR_READONLY */
    ef_close(ro);
}

/* Schema v1 → v2/v3 迁移 */
if (ef_needs_upgrade(db)) {
    ef_upgrade(db);
    ef_sync(db);
}
```

## 文件布局

### Schema v3（`hash_capacity > 0`）

```
[超级块 64B][哈希索引区 capacity × 16B][数据槽区 max_slots × 64B]
```

### Schema v2 / v3（`hash_capacity = 0`，默认 `ef_open_ex`）

```
[超级块 64B][数据槽区 max_slots × 64B]
```

`ef_open_ex` / `ef_open_memory` 新建库时 `hash_capacity = 0`，与旧版文件大小一致。需要字符串索引时使用 `ef_open_ex_hash` / `ef_open_memory_hash`。

## Schema v2/v3 槽位布局

```
status (4) | header_crc (4) | payload[48] | next_offset (8)  →  64 字节
```

| status | 值 | 说明 |
|--------|-----|------|
| `EF_STATUS_FREE` | 0 | 空闲链节点 |
| `EF_STATUS_USED` | 1 | 普通头槽或 blob 头槽 |
| `EF_STATUS_OVERFLOW` | 2 | blob 溢出续链槽（全 48 字节承载数据） |
| `EF_STATUS_QUEUED` | 3 | FIFO 队列数据节点 |
| `EF_STATUS_QUEUE_DUMMY` | 4 | 队列哨兵头（惰性分配，常驻） |
| `EF_STATUS_QUEUE_LINK` | 5 | 入队链接中（内部） |
| `EF_STATUS_QUEUE_DEQ` | 6 | 出队摘链中（内部） |

Blob 头槽 `payload[0..3]` 为 magic `BLOB`，`payload[4..7]` 为 `uint32_t` 总长度，内联数据从第 8 字节起（最多 40 字节）；超出部分经 `next_offset` 串联 `OVERFLOW` 槽。`ef_write_payload` 与 blob 格式互不干扰。

### 超级块 `reserved[28]`（v3）

| 偏移 | 字段 | 说明 |
|------|------|------|
| `[0..3]` | CRC32 | 超级块校验和（`EF_FLAG_SB_CRC`） |
| `[4..11]` | `queue_head_offset` | 指向 dummy 哨兵槽（物理偏移） |
| `[12..19]` | `queue_tail_offset` | 队列尾（物理偏移） |
| `[20..23]` | `hash_capacity` | Robin Hood 表容量（2 的幂，0 表示无索引区） |
| `[24..27]` | `queue_lock` | 跨进程自旋锁（`uint32_t`，0=空闲） |

`free_count` 为 `uint32_t`（与 `reserved` 扩展共同保持超级块 64 字节）。

### 哈希索引项（16 字节）

```
key_hash (8) | slot_offset (8)
```

- 键经 FNV-1a 哈希为 `key_hash`（原始字符串不存入文件）
- Robin Hood 线性探测，支持高装载率下的缓存友好查找

### FIFO 队列 payload 编码

队列槽 `payload` 布局：`[1 字节长度][数据…]`，最大数据 **47 字节**（`len` 参数为数据长度，不含长度前缀）。

首次 `ef_queue_push` 惰性分配 dummy 哨兵槽；逻辑空队列为 `dummy.next_offset == 0`（`ef_queue_empty`，无锁快路径）。

`ef_queue_drained` 在持有 `queue_lock` 时检查是否无待处理消息，适合多线程消费者在「所有生产者已结束」后安全退出（`ef_queue_empty` 仅作启发式判断，高并发下可能误判）。

并发语义：

- 入队/出队临界区由超级块 `queue_lock` 自旋锁保护（GCC/Clang `__atomic` CAS）
- 出队摘链后在**同一临界区内**调用 `ef_return_slot_to_pool`，避免槽位被并发复用时尚未脱离队列生命周期
- 入队失败（如 `EF_ERR_QUEUE_BUSY`）时通过 `ef_return_slot_to_pool` 回收已分配槽（`ef_free_slot` 无法释放 `QUEUED` 状态）
- 空闲链在支持原子操作的平台使用 CAS 栈式弹入/弹出，与队列并发安全
- 高争用返回 `EF_ERR_QUEUE_BUSY`，调用方应重试

## 空闲链表与分配

删除槽位时：`status → FREE`，`next_offset ← free_list_head`，`free_list_head ← 本槽物理偏移`（LIFO，O(1)）。若槽曾被索引，先 `ef_index_remove_by_slot`。

| API | 行为 |
|-----|------|
| `ef_alloc_slot` | 从空闲链弹出；池空返回 `EF_ERR_SLOT_FULL` |
| `ef_alloc` | 同上；池空时 `ef_grow(max_slots + 1)` 后重试 |
| `ef_free_slot` | 归还空闲链并清除索引项（队列槽须先 `ef_queue_pop`） |
| `ef_index_rehash` | 扩容哈希区、搬迁槽区、修正 free_list / queue / next_offset 后重插 |
| `ef_index_remove_by_slot` | 按槽位物理偏移扫描并删除索引项 |
| `ef_queue_empty` | 无锁快路径：dummy 未分配或 `dummy.next == 0` |
| `ef_queue_drained` | 持锁检查队列无待处理消息（多线程消费者退出用） |
| `ef_db_refresh_slot_crcs` | 刷新所有需 CRC 的槽头（rehash 后可选） |
| `ef_db_commit_meta` | 立即刷新延迟的超级块 CRC（`ef_sync` / `ef_close` 亦会调用） |

## 许可证

[MIT License](LICENSE) — Copyright (c) 2026 zayoka
