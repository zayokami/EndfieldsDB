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
- **数据校验**（Schema v2）：超级块 CRC32 + 已用槽位头 CRC32
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

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `ENDFIELDS_EMBEDDED_ONLY` | OFF | 仅 RAM 后端，禁用文件 I/O |
| `ENDFIELDS_ENABLE_PREFETCH` | ON | 追逐热路径启用 `__builtin_prefetch` |

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
```

## Schema v2 槽位布局

```
status (4) | header_crc (4) | payload[48] | next_offset (8)  →  64 字节
```

超级块校验和存放在 `reserved[0..3]`，由 `EF_FLAG_SB_CRC` 启用。

## 许可证

[MIT License](LICENSE) — Copyright (c) 2026 zayoka
