# Endfields DB

轻量级、零拷贝、基于 mmap 物理寻址的嵌入式数据库核心，使用 C11 实现。

## 特性

- **固定 64 字节槽位**：超级块与槽位均为 64 字节对齐，适合缓存友好访问
- **零拷贝**：通过 mmap 直接按文件偏移读写，无额外序列化层
- **物理寻址**：`ef_slot_to_offset` / `ef_offset_to_ptr` 实现 O(1) 槽位定位
- **指针追逐**：单跳 `ef_chase`、多跳 `ef_chase_n`，带环检测与可选 CPU prefetch
- **槽位池**：空闲链表分配与回收（`ef_alloc_slot` / `ef_free_slot`）
- **紧凑指令集**：`ef_execute` 通过 10 字节 `ef_cmd` 派发读写、追逐、分配等操作
- **动态扩容**：`ef_grow` 扩展槽位数量（文件 truncate + 重映射，或内存后端）
- **只读打开**：`ef_open_readonly` 以只读 mmap 打开，写操作返回 `EF_ERR_READONLY`
- **槽位迭代**：`ef_foreach_used` / `ef_slot_iter` 遍历已用头槽（跳过溢出续链槽）
- **溢出链 Blob**：`ef_write_blob` / `ef_read_blob` 支持超过 48 字节的大对象存储
- **数据校验**（Schema v2）：超级块 CRC32 + 已用槽位头 CRC32
- **在线迁移**：`ef_needs_upgrade` / `ef_upgrade` 将 Schema v1 升级为 v2（内联 payload 52→48 字节，尾部 4 字节丢弃）
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

## 快速示例

```c
#include "endfields.h"

struct ef_db *db = ef_open("data.endf", 64);
if (db) {
    ef_write_payload(db, 0, "hello", 5);
    struct ef_slot *slot = ef_get_slot(db, 0);
    ef_sync(db);
    ef_close(db);
}

/* 只读 */
struct ef_db *ro = ef_open_readonly("data.endf");
if (ro) {
  /* 读操作可用，写操作返回 EF_ERR_READONLY */
  ef_close(ro);
}

/* Schema v1 → v2 迁移 */
if (ef_needs_upgrade(db)) {
    ef_upgrade(db);
    ef_sync(db);
}
```

## Schema v2 槽位布局

```
status (4) | header_crc (4) | payload[48] | next_offset (8)  →  64 字节
```

- `EF_STATUS_USED (1)` — 普通头槽或 blob 头槽
- `EF_STATUS_OVERFLOW (2)` — blob 溢出续链槽（全 48 字节承载数据）

Blob 头槽 `payload[0..3]` 为 magic `BLOB`，`payload[4..7]` 为 `uint32_t` 总长度，内联数据从第 8 字节起（最多 40 字节）；超出部分经 `next_offset` 串联 `OVERFLOW` 槽。`ef_write_payload` 与 blob 格式互不干扰。

超级块校验和存放在 `reserved[0..3]`，由 `EF_FLAG_SB_CRC` 启用。

## 许可证

[MIT License](LICENSE) — Copyright (c) 2026 zayoka
