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
| Windows | `test_queue_mpmc`、`test_index_mrsr`（Win32 线程） |
| Linux / macOS 等 | `test_queue_mpmc`、`test_index_mrsr`（pthread） |
| 嵌入式 `EF_PLATFORM_EMBEDDED` | 跳过（无文件 I/O） |

构建非 Windows 测试时需链接 pthread（CMake `Threads::Threads`）。

## 后续计划（未实现）

- 槽位读侧与索引读的原子组合 API
- `ef_execute` 队列/索引操作码
