# Endfields DB 并发与跨进程语义

本文说明哪些 API 可在多线程 / 多进程下安全使用，以及调用方应遵守的约定。

## 总览

| 子系统 | 同进程多线程 | 跨进程（共享 mmap 文件） | 说明 |
|--------|-------------|--------------------------|------|
| FIFO 队列 `ef_queue_*` | 支持 MPMC | 支持 MPMC | 超级块自旋锁 + 锁内摘链/归还 |
| 空闲链 `ef_alloc*` / `ef_free_slot` | CAS 弹入/弹出（GCC/Clang） | 同左 | 与队列可并发 |
| Robin Hood 索引 `ef_index_*` | **非线程安全** | **非线程安全** | 需外部互斥 |
| 槽位读写 / 追逐 / blob | **非线程安全** | **非线程安全** | 需外部互斥 |
| `ef_sync` / `ef_close` | 调用方串行化 | 调用方串行化 | 避免与其他写者并发 |
| 超级块延迟 CRC | 与写路径共享 mmap | 与写路径共享 mmap | `ef_db_commit_meta` 非原子屏障 |

**默认假设**：除队列与空闲链外，同一 `struct ef_db` 实例在同一时刻只有一个写者，或调用方自行加锁。

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

测试实现中在 `ef_queue_drained` 连续多次为真后才退出，以降低与最后几条消息的竞态。

### 入队失败

若 `ef_queue_push` 在链接入队前失败（如 `EF_ERR_QUEUE_BUSY`），库内通过 `ef_return_slot_to_pool` 回收槽位（`ef_free_slot` 无法释放 `EF_STATUS_QUEUED` 槽）。

### 槽位池容量

队列深度受 `max_slots` 限制。高吞吐压测时，在途消息数可能接近「生产者数 × 每生产者消息数」；应保证 `max_slots` 大于峰值在途消息数（bench 使用 `消息总数 + 64`）。

## 空闲链与 `ef_alloc`

- `ef_alloc_slot`：CAS 从 LIFO 空闲链弹出；失败返回 `EF_ERR_SLOT_FULL` 或 `EF_ERR_QUEUE_BUSY`（CAS 重试耗尽）。
- `ef_alloc`：池空时 `ef_grow(+1)` 后重试。
- `ef_free_slot`：归还空闲链并 `ef_index_remove_by_slot`；**不可**用于仍在队列中的 `EF_STATUS_QUEUED` 槽。

与队列并发时：消费者 `pop` 归还槽位，生产者 `push` 分配槽位，由 CAS 与队列锁协同保证安全。

## 索引与槽位数据（单写者）

以下 API **未**做内部同步，多线程并发读写会导致数据竞争与损坏：

- `ef_index_put` / `ef_index_get` / `ef_index_remove` / `ef_index_rehash`
- `ef_write_payload` / `ef_write_blob` / `ef_set_*` / `ef_chase*`
- `ef_foreach_used` / `ef_slot_iter`

**推荐模式**：

1. **单写线程 + 多读线程**：读侧仍须与写者互斥，除非只读打开（`ef_open_readonly`）且无任何写者。
2. **多写者**：调用方对整库或按子系统（索引区 / 槽区）使用互斥锁。
3. **跨进程**：仅队列场景可多个进程无锁并发；索引与槽位写入应通过文件锁或单一「写进程」串行化。

## 持久化与崩溃窗口

- 热路径通过 `ef_db_mark_meta_dirty` 延迟超级块 CRC；`ef_sync` / `ef_close` / `ef_db_commit_meta` 才刷新校验和。
- 进程崩溃时，最后几次元数据变更可能尚未反映到超级块 CRC；槽头 CRC（若启用）仍保护已提交槽位。
- 多进程场景：一个进程 `ef_sync` 不会自动使其他进程的 `mmap` 失效；依赖 OS 页面缓存一致性。写者应在协议层协调 sync 时机。

## 平台与测试

| 平台 | 多线程测试 |
|------|------------|
| Windows | `test_queue_mpmc`（Win32 `_beginthreadex`） |
| Linux / macOS 等 | `test_queue_mpmc`（pthread） |
| 嵌入式 `EF_PLATFORM_EMBEDDED` | 跳过（无文件 I/O） |

构建非 Windows 测试时需链接 pthread（CMake `Threads::Threads`）。

## 后续计划（未实现）

- 索引读侧无锁或读写锁
- `ef_execute` 队列/索引操作码
- 更短的队列临界区（retire 链批量归还）
