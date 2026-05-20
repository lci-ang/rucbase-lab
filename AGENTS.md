# AGENTS.md

Teaching DBMS (RucBase) — students implement Lab1-4 via `// Todo:` stubs in source files.

## Build

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

All test binaries land in `build/bin/`. Tests must be run from `build/`.

## Run Tests

```bash
# Lab1 — run each, in order (later tests depend on earlier ones)
make disk_manager_test && ./bin/disk_manager_test
make lru_replacer_test && ./bin/lru_replacer_test
make buffer_pool_manager_test && ./bin/buffer_pool_manager_test
make record_manager_test && ./bin/record_manager_test

# Lab2
make b_plus_tree_insert_test && ./bin/b_plus_tree_insert_test
make b_plus_tree_delete_test && ./bin/b_plus_tree_delete_test
make b_plus_tree_concurrent_test && ./bin/b_plus_tree_concurrent_test

# Lab3 / Lab4 — Python-based
python src/test/query/query_test_basic.py
python src/test/transaction/transaction_test.py
python src/test/concurrency/concurrency_test.py

# All tests
ctest --output-on-failure
```

## Labs & TODO Stubs

Functions marked `// Todo:` are student exercises. **Do not implement them unless explicitly asked.**

| Lab | Theme | Key files |
|-----|-------|-----------|
| Lab1 | Storage | `src/storage/disk_manager.cpp`, `src/replacer/lru_replacer.cpp`, `src/storage/buffer_pool_manager.cpp`, `src/record/rm_file_handle.cpp`, `src/record/rm_scan.cpp` |
| Lab2 | Index | `src/index/b_plus_tree.cpp` and related |
| Lab3 | Query execution | `src/execution/` operators |
| Lab4 | Concurrency | `src/transaction/` |

Dependency chain: Lab1 → Lab2 → Lab3 → Lab4.

## Architecture (bottom-up)

```
SQL → Parser (flex/bison) → Analyzer → Optimizer → Executor → Record/Index → BufferPoolManager → DiskManager → disk
```

Key layers:
- `storage/` — DiskManager (raw POSIX I/O), BufferPoolManager (page cache)
- `replacer/` — LRU eviction policy
- `record/` — RmFileHandle (CRUD on fixed-length records), RmScan (iterator), RmManager (file lifecycle)
- `index/` — B+ tree
- `execution/` — Volcano-model operators
- `system/` — System manager, metadata catalog
- `transaction/` — Lock/transaction manager (Lab4)
- `recovery/` — WAL log manager

## Core Types & Layouts

Defined in `src/common/config.h`:
- `frame_id_t`, `page_id_t` = `int32_t`
- `PAGE_SIZE` = 4096, `INVALID_PAGE_ID` = -1

Page: `[4B LSN][PageHdr][payload]` — `OFFSET_PAGE_HDR = 4` (see `src/storage/page.h`)

Record file: page 0 = file header (RmFileHdr), data pages start at page 1. Each data page: `[RmPageHdr][bitmap][slots]` (see `src/record/rm_defs.h`).

## Code Style

`.clang-format`: Google base, 4-space indent, 120-column limit. Run `clang-format` if unsure.

## Learning Guides

Complete implementation guides with copy-pasteable code live at:
```
docs/lciang_database_learning/labN/labN_complete_guide.md
```

Official lab specs: `docs/Rucbase-LabN[...].md`

## Key Gotchas

- `fetch_page` / `new_page` can return `nullptr` — always check.
- `pin_count` must be incremented on fetch, decremented on unpin. Pages with pin_count > 0 cannot be evicted.
- Dirty pages must be written back before eviction (see `update_page` in BPM).
- `erase` on `page_table_` invalidates the iterator — save `frame_id` before erasing.
- `close_file` must extract the path string *before* calling `close(fd)`, since the fd becomes invalid after close.
- LRU: `unpin` adds to list head (most recent); `victim` pops from list tail (oldest).
- Record bitmap: `1` = occupied, `0` = free. Use `Bitmap::first_bit(false, ...)` to find a free slot.
- RmScan starts at `{page=1, slot=-1}` because `next()` searches from `slot_no + 1`.
