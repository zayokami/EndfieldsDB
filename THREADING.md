# Endfields DB 并发与跨进程语义

本文说明哪些 API 可在多线程 / 多进程下安全使用，以及调用方应遵守的约定。

## 总览

| 子系统 | 同进程多线程 | 跨进程（共享 mmap 文件） | 说明 |
|--------|-------------|--------------------------|------|
| FIFO 队列 `ef_queue_*` | 支持 MPMC | 支持 MPMC | 超级块自旋锁 + 锁内摘链/归还 |
| 空闲链 `ef_alloc*` / `ef_free_slot` | CAS 弹入/弹出（GCC/Clang） | 同左 | 与队列可并发 |
| Robin Hood 索引 `ef_index_get` | **多读**（schema v4+） | **多读** | mmap seqlock + 写自旋锁 |
| 索引写 `ef_index_put/remove/rehash/clear` | **单写者**（互斥） | **单写者** | 同一时刻仅一个写临界区 |
| 槽位读写 / 追逐 / blob | **非线程安全** | **非线程安全** | 需外部互斥 |
| `ef_sync` / `ef_close` | 调用方串行化 | 调用方串行化 | 避免与其他写者并发 |
| 超级块延迟 CRC | 与写路径共享 mmap | 与写路径共享 mmap | `ef_db_commit_meta` 非原子屏障 |

**默认假设**：槽位数据与索引写操作外，队列与索引读可并发；索引写与 `ef_index_rehash` 同一时刻只能有一个执行者（可多线程排队抢写锁，但互斥执行）。

## Schema v4 与索引 MRSW

v4 在超级块 `reserved[]` 中增加（并压缩 v3 字段布局）：

| 偏移 | 字段 | 用途 |
|------|------|------|
| `[20..21]` | `hash_capacity` u16 | 最大 65535 桶 |
| `[22]` | `queue_lock` u8 | 队列自旋锁 |
| `[23]` | `index_write_lock` u8 | 索引写自旋锁 |
| `[24..27]` | `index_seq` u32 | seqlock：偶数=稳定，奇数=写者活跃 |

**可写打开** v3 库时自动迁移到 v4（哈希容量 ≤ 65535）。**只读打开** v3 库不会改写 mmap，索引并发保护**不生效**——请先可写打开一次完成迁移，或接受单线程索引访问。

### 读路径（`ef_index_get`）

1. 读 `index_seq`；若为奇数则自旋重试。
2. 探测 Robin Hood 表（不持锁）。
3. 再读 `index_seq`；若与步骤 1 不同或仍为奇数则重试。
4. 重试超过 `EF_SB_INDEX_SEQ_READ_MAX` 返回 `EF_ERR_INDEX_BUSY`（调用方应重试）。

读侧**无**全局读锁，多个读者可并行。

### 写路径（`put` / `remove` / `remove_by_slot` / `rehash` / `clear`）

1. CAS 获取 `index_write_lock`（失败返回 `EF_ERR_INDEX_BUSY`）。
2. `index_seq++`（变奇数）+ fence。
3. 修改哈希表 / 搬迁槽区（`rehash` 全程持锁）。
4. fence + `index_seq++`（变偶数）。
5. 释放 `index_write_lock`。

`ef_index_remove_by_slot`（由 `ef_free_slot`、队列出队等触发）同样走写锁，因此**多个线程可同时调用写 API，但会串行化**——语义是 single-writer-at-a-time，不是“只能一个线程 ID”。

### 与队列 / 空闲链的交互

- 读者可与 `ef_queue_push/pop`、空闲链 CAS **并发**。
- 写者可能与队列并发；`remove_by_slot` 与消费者在出队归还槽位时争用写锁——设计预期，正确但可能 `EF_ERR_INDEX_BUSY`，需重试。
- `ef_index_rehash` 会 `memmove` 槽区并修正 `free_list` / 队列头尾 / `next_offset`；持写锁期间读者重试，**调用方不得在 rehash 时并发写槽位**。

## FIFO 队列（MPMC）

### 并发保证

- `ef_queue_push` / `ef_queue_pop` 可由任意数量的生产者与消费者并发调用（同进程或跨进程映射同一文件）。
- 出队在持有 `queue_lock` 时完成摘链，并在**同一临界区**内调用 `ef_return_slot_to_pool`，避免槽位被空闲链复用后仍被队列逻辑引用。
- 空闲链在 GCC/Clang 上使用 CAS 栈；与队列入/出队可并行（分配新节点 vs 归还已出队节点）。
- 高争用时 `ef_queue_lock_acquire` 可能返回 `EF_ERR_QUEUE_BUSY`；调用方应重试（测试与 bench 均如此处理）。

### 辅助 API

| API | 用途 |
|-----|------|
| `ef_queue_empty` | 无锁快路径：dummy 未创建或 `dummy.next == 0`。高并发下可能短暂误判，**不**应用于消费者退出。 |
| `ef_queue_drained` | 持有 `queue_lock` 检查无待处理消息。生产者全部结束后，消费者应循环 pop + 用此 API 确认排空再退出。 |

### 消费者退出模式（推荐）

```c
for (;;) {
    err = ef_queue_pop(db, buf, cap, &len);
    if (err == EF_ERR_QUEUE_EMPTY || err == EF_ERR_NOT_FOUND) {
        if (all_producers_finished && ef_queue_drained(db))
            break;
        continue;
    }
    if (err == EF_ERR_QUEUE_BUSY)
        continue;
    /* 处理 err == EF_OK */
}
```

### 入队失败

若 `ef_queue_push` 在链接入队前失败（如 `EF_ERR_QUEUE_BUSY`），库内通过 `ef_return_slot_to_pool` 回收槽位（`ef_free_slot` 无法释放 `EF_STATUS_QUEUED` 槽）。

## 空闲链与 `ef_alloc`

- `ef_alloc_slot`：CAS 从 LIFO 空闲链弹出；失败返回 `EF_ERR_SLOT_FULL` 或 `EF_ERR_QUEUE_BUSY`（CAS 重试耗尽）。
- `ef_alloc`：池空时 `ef_grow(+1)` 后重试。`ef_alloc_ex(..., 0)` 跳过 payload 清零，供 `ef_queue_push` 热路径使用。
- `ef_free_slot`：归还空闲链并 `ef_index_remove_by_slot`（索引写锁）；**不可**用于仍在队列中的 `EF_STATUS_QUEUED` 槽。
- `ef_db_mark_meta_dirty`：已脏时不再重复原子写；`ef_db_commit_meta` / `ef_sync` 刷新超级块 CRC（x86-64 上 ≥64 字节块用 PCLMUL IEEE CRC-32，否则 slicing-by-4）。

## 槽位数据（仍须外部同步）

以下 API **未**做槽位级同步：

- `ef_write_payload` / `ef_write_blob` / `ef_set_*` / `ef_chase*`
- `ef_foreach_used` / `ef_slot_iter`

### `ef_get_slot` vs `ef_peek_slot`

| API | CRC 校验 | 适用场景 |
|-----|----------|----------|
| `ef_get_slot` | 是（`EF_STATUS_USED` 等） | 持久化读、重开文件、不信任 mmap 时 |
| `ef_peek_slot` | 否（仅边界检查） | 追逐热路径、已持队列锁、刚写完的槽 |

队列内部节点（`QUEUED` / `QUEUE_DUMMY` 等）的槽头 CRC 仅覆盖 **status + next_offset**（link CRC），不哈希 payload，以降低入队/出队开销；`ef_get_slot` 仍接受旧版全字段 CRC（向后兼容）。

索引 MRSW **不**保护槽内 payload。典型模式：

1. 写者：`ef_index_put` 后单线程写槽（或外部槽锁）。
2. 读者：`ef_index_get` 得到 `slot_id` 后，在**无写者改该槽**的前提下读 `ef_get_slot` / payload；或只读 mmap 且无写进程。

## 持久化与崩溃窗口

- 热路径通过 `ef_db_mark_meta_dirty` 延迟超级块 CRC；`ef_sync` / `ef_close` / `ef_db_commit_meta` 才刷新校验和。
- 进程崩溃时，最后几次元数据变更可能尚未反映到超级块 CRC；槽头 CRC（若启用）仍保护已提交槽位。
- 多进程场景：一个进程 `ef_sync` 不会自动使其他进程的 `mmap` 失效；依赖 OS 页面缓存一致性。写者应在协议层协调 sync 时机。

## 平台与测试

| 平台 | 多线程测试 |
|------|------------|
| Windows | `test_queue_mpmc`、`test_index_mrsr`、`test_index_multi_writer`、`test_index_multi_writer_rehash`（Win32 线程） |
| Linux / macOS 等 | `test_queue_mpmc`、`test_index_mrsr`、`test_index_multi_writer`、`test_index_multi_writer_rehash`（pthread） |
| 嵌入式 `EF_PLATFORM_EMBEDDED` | 跳过（无文件 I/O） |

构建非 Windows 测试时需链接 pthread（CMake `Threads::Threads`）。

CI 矩阵对每种组合运行 Linux GCC Release / Debug / no-prefetch / ASan+UBSan / TSan、Linux Clang Release、macOS Clang Release、Windows MSVC Release、Windows MinGW Release+Debug、Linux embedded-only、Linux clang-tidy+cppcheck 静态分析；所有 job 启用 `-Werror`（`ENDFIELDS_WARNINGS_AS_ERRORS=ON`）。

## Schema v5：事务 / Undo Log

### 新增字段（超级块 `reserved[]` 重排）

| 偏移 | 字段 | 用途 |
|------|------|------|
| `[0..3]` | `sb_checksum`（不变） | 超级块 CRC32 |
| `[4..11]` | `queue_head` u64（不变） | 队列头指针 |
| `[12..19]` | `queue_tail` u64（不变） | 队列尾指针 |
| `[20..21]` | `hash_capacity` u16（不变） | Robin Hood 容量 |
| `[22]` | `queue_lock` u8（不变） | 队列自旋锁 |
| `[23]` | `index_write_lock` u8（不变） | 索引写自旋锁 |
| `[24]` | `txn_lock` u8（新增） | 事务自旋锁（与 `index_write_lock` / `queue_lock` 错位以避免原子读冲突） |
| `[25]` | `txn_state` u8（新增） | 0=NONE / 1=ACTIVE / 2=ABORTING |
| `[26..27]` | `padding` | 保留 |
| 已删除 | `index_seq` u32（v4） | 搬迁到 undo log header 区域内（`ef_undo.h`） |

### 文件布局 (v5)

```
[ superblock 64B ][ hash index capacity * 16B ][ slots max_slots * 64B ][ undo log section ]
```

- undo log 段：`EF_UNDO_LOG_HEADER_BYTES + slots * sizeof(struct ef_undo_record)`，默认 4096 slots ≈ 128 KiB。
- undo log 段在槽区之后追加，迁移 v4 文件时按需追加（不破坏现有数据）。
- undo log 段独立 mmap 区，自有 header CRC32。

### 公开事务 API

| API | 行为 |
|-----|------|
| `ef_txn_begin(db)` | CAS 抢 `txn_lock`；失败返回 `EF_ERR_TXN_BUSY`（调用方重试或回退到只读） |
| `ef_txn_commit(db)` | 清空 undo log / 释放锁 / 状态 NONE |
| `ef_txn_abort(db)` | 反向 replay undo log → 清空 → 释放锁 → NONE |
| `ef_txn_active(db)` | 查询（无副作用） |

### 写路径自动 undo 记录

事务 active 时以下 mutating API 自动写 undo 记录：

- `ef_set_status` / `ef_write_payload` / `ef_set_next_offset` / `ef_write_field`
- `ef_alloc_slot` / `ef_free_slot`
- `ef_index_put` / `ef_index_remove` / `ef_index_remove_by_slot` / `ef_index_clear`
- `ef_queue_push` / `ef_queue_pop`（区分 push：节点+链接；pop：节点+链接还原）
- `ef_write_blob`（链槽创建、payload 前镜像）

### 事务限制

| 场景 | 行为 |
|------|------|
| `ef_alloc_slot` 槽满 → `ef_grow` | 事务内禁止隐式 grow，返回 `EF_ERR_GROW` |
| `ef_index_put` 触发 auto-rehash | 事务内禁止 rehash，返回 `EF_ERR_INDEX_BUSY` |
| undo log 段满 | 返回 `EF_ERR_TXN_LOG_FULL`；调用方 commit 后开新事务 |
| 同一进程重复 `ef_txn_begin` | 第二个返回 `EF_ERR_TXN_BUSY`（应等待第一个事务结束） |

### 崩溃恢复

- `txn_state == ACTIVE` 且 `txn_writer_pid/epoch` 与当前进程不匹配 → 视为 stale，自动 replay 后清空 undo log 释放锁。
- 正常打开路径不变。

### 测试覆盖

`test_txn_*`、`test_v4_to_v5_migration_open`：见 `tests/test_core.c` 与 `tests/test_index_queue.c`。

## 后续计划（未实现）

- 槽位读侧与索引读的原子组合 API：仍由调用方负责在 index read 返回 `slot_id` 后到访问槽位 payload 期间不发生写者修改该槽，或使用外部互斥。
- `ef_execute` 队列/索引操作码：见 `ef_execute` 当前只覆盖 get slot、chase、field read/write、payload write、set next/status、alloc/free、chase_n；索引和队列的高层操作尚未派发。
