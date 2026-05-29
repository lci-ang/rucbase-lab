# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

RucBase is a teaching database management system developed by Renmin University of China. It is structured as a series of labs (Lab1-4) where students implement core DBMS components: storage management, index management, query execution, and concurrency control.

## Build & Test Commands

```bash
# Build the entire project
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# Run individual lab tests (from build/ directory)
make disk_manager_test && ./bin/disk_manager_test
make lru_replacer_test && ./bin/lru_replacer_test
make buffer_pool_manager_test && ./bin/buffer_pool_manager_test
make record_manager_test && ./bin/record_manager_test

# Lab2: Index tests
make b_plus_tree_insert_test && ./bin/b_plus_tree_insert_test
make b_plus_tree_delete_test && ./bin/b_plus_tree_delete_test
make b_plus_tree_concurrent_test && ./bin/b_plus_tree_concurrent_test

# Lab3: Query execution tests (from repo root)
cd src/test/query
python query_test_basic.py

# Lab3: Single test
python query_unit_test.py basic_query_test1.sql

# Lab4: Transaction tests
cd src/test/transaction
python transaction_test.py

# Lab4: Concurrency tests
cd src/test/concurrency
python concurrency_test.py

# Lab4: Single concurrency test
cd src/test/concurrency
python concurrency_unit_test.py dirty_write_test

# Run all tests via ctest
ctest --output-on-failure
```

## Architecture

The codebase is layered bottom-up:

```
src/
  storage/      - DiskManager (raw file I/O), BufferPoolManager (page cache)
  replacer/     - LRU replacement policy for buffer pool
  record/       - Record manager: RmFileHandle (CRUD on records), RmScan (iteration), RmManager (file lifecycle)
  index/        - B+ tree index implementation
  execution/    - Query executor operators (seq_scan, insert, delete, update, join, etc.)
  parser/       - SQL lexer/parser (flex/bison)
  optimizer/    - Query planner
  analyze/      - Semantic analysis
  system/       - System manager (SM), metadata catalog
  transaction/  - Transaction and lock management
  recovery/     - WAL log manager and recovery
  common/       - Shared types: config.h (type aliases, constants), context.h, common.h (Value, Condition)
  defs.h        - Global types: Rid, ColType, RecScan abstract class
  errors.h      - All exception classes (RMDBError hierarchy)
```

### Key Data Flow

SQL string -> Parser -> Analyzer -> Optimizer -> Executor -> Record/Index/Storage layers -> Disk

### Core Types (src/common/config.h)

- `frame_id_t`, `page_id_t` = int32_t
- `PAGE_SIZE` = 4096 bytes
- `INVALID_PAGE_ID` = -1, `INVALID_FRAME_ID` = -1

### Page Layout (src/storage/page.h)

Each `Page` has: `id_` (PageId = {fd, page_no}), `data_[PAGE_SIZE]`, `is_dirty_`, `pin_count_`.
Page data layout: [4B LSN][PageHdr][payload...]. `OFFSET_PAGE_HDR = 4`.

### Record Page Layout (src/record/rm_defs.h, rm_file_handle.h)

Record files use page 0 as the file header (RmFileHdr). Data pages start at page 1.
Each data page: [RmPageHdr][bitmap][slots]. `RmPageHandle` provides typed access via pointer arithmetic.

### Build Libraries

- `storage` (static): disk_manager.cpp, buffer_pool_manager.cpp, lru_replacer.cpp
- `lru_replacer` (static): lru_replacer.cpp (also compiled into storage)
- `record` (static): rm_file_handle.cpp, rm_scan.cpp; links to system, transaction, storage
- `index` (static): ix_index_handle.cpp, ix_scan.cpp

## Experiment Documents

- `docs/Rucbase-Lab1[存储管理实验文档].md` - Lab1 requirements
- `docs/Rucbase-Lab2[索引管理实验文档].md` - Lab2 requirements
- `docs/Rucbase-Lab3[查询执行实验文档].md` - Lab3 requirements
- `docs/Rucbase-Lab3[查询执行实验指导].md` - Lab3 guidance
- `docs/Rucbase-Lab4[并发控制实验文档].md` - Lab4 requirements
- `docs/Rucbase使用文档.md` - Usage instructions
- `docs/Rucbase环境配置文档.md` - Environment setup
- `docs/Rucbase项目结构.pdf` - Project structure
- `docs/框架图.pdf` - Framework diagram

## Lab Implementation Status

Functions with `// Todo:` stubs in the source files are student exercises. Do not implement them unless explicitly asked. The labs are:

1. **Lab1 - Storage**: DiskManager, LRUReplacer, BufferPoolManager, RmFileHandle, RmScan (27 TODOs, currently STUBS)
2. **Lab2 - Index**: B+ tree insert/delete/scan (22 TODOs, COMPLETED — src/system/sm_manager.cpp:3, src/index/ix_index_handle.cpp:20)
3. **Lab3 - Query Execution**: Executor operators
4. **Lab4 - Concurrency**: Transaction/lock management

## Code Modification Rules

- **Only modify TODO sections**: When implementing labs, only replace the `// Todo:` stub code. Do not modify any other code, existing logic, or non-TODO comments.
- **Preserve all existing source comments**: The original Chinese comments, docstrings (`@description`, `@param`, `@return`), and copyright headers must remain unchanged.
- **Do not add or remove includes**: Only add an include if the TODO implementation requires a header that is not already included.

## Language & Toolchain

- C++17 (`-std=c++17`), compiled with `-Wall -O0 -g -ggdb3`
- GCC 7.1+, CMake 3.16+
- Dependencies: flex, bison, readline
- Google Test for unit testing (vendored in deps/googletest)
- Flex/Bison for SQL parsing
- POSIX I/O: `read`/`write`/`lseek`/`open`/`close`/`unlink`
- Inspired by CMU 15-445 [BusTub](https://github.com/cmu-db/bustub) and Stanford CS346 [Redbase](https://web.stanford.edu/class/cs346/2015/redbase.html)

## Known Issues

### General
- `src/common/config.h` uses `std::string` but is missing `#include <string>`. If you get `'string' in namespace 'std' does not name a type` errors, add `#include <string>` to that file.
- `DiskManager::open_file` must check `is_file(path)` before calling `open()` and throw `FileNotFoundError` (not `UnixError`) if the file doesn't exist. The test expects this specific exception type.

### Lab2 B+ Tree Pitfalls (5 bugs found during implementation)

These are NOT in the learning guide — they were discovered during debugging:

1. **`create_node()` parent is 0, not -1**: `BufferPoolManager::new_page` → `update_page` → `reset_memory` zeros the page. `IxPageHdr.parent` becomes 0, but `INVALID_PAGE_ID = -1`. `is_root_page()` checks `parent == INVALID_PAGE_ID` (0 == -1 → false). Fix: `new_root->set_parent_page_no(IX_NO_PAGE)` in `insert_into_parent` when creating a new root.

2. **`coalesce_or_redistribute` leaks pin when root doesn't need adjustment**: `adjust_root` returns false without unpinning the root node. Each delete leaks one pin → buffer pool exhaustion → deadlock. Fix: unpin manually when `adjust_root` returns false.

3. **`internal_lookup` out-of-bounds**: `upper_bound` can return 0 when target < all keys, causing `value_at(-1)`. Fix: check `if (idx == 0) return value_at(0)`.

4. **Missing `maintain_parent` after modify-first-key**: `insert_entry` and `delete_entry` must call `maintain_parent(leaf)` after modifying the leaf's first key (insert at pos 0 or remove pos 0), otherwise parent's separator key becomes stale → `check_tree` assertion fails.

5. **Concurrent control required**: `b_plus_tree_concurrent_test` fails without locking. Use `std::scoped_lock lock{root_latch_}` in `get_value`, `insert_entry`, `delete_entry`. `root_latch_` is an existing `std::mutex` member of `IxIndexHandle`.

### Lab2 Order of Operations
- Implement `SmManager::create_index` before running any index tests
- Implement in dependency order: IxNodeHandle (8 TODOs) → IxIndexHandle lookup (2) → insert (3) → delete (5) → range scan (2) → concurrency (3 locks)
- After each group, rebuild and test the corresponding test binary

## Learning Documents

The `docs/lciang_database_learning/` directory contains complete implementation guides for each lab with directly copyable code:
- `lab1/lab1_complete_guide.md` — Storage management (DiskManager, LRUReplacer, BPM, RmFileHandle, RmScan)
- `lab2/lab2_complete_guide.md` — B+ tree index (22 TODOs, includes 5 bug fixes)
- `lab2/lab2作业.docx` — Lab2 experiment report (module intro, background, implementation, testing, bugs, summary)
- `项目说明.md` — Project overview
- `rucbase_learning_guide.md` — General learning guide

## Skill: Generate Lab Completion Guide

When a user asks to generate a learning/completion guide for a lab (e.g. "帮我做 lab1 的指南", "生成 lab2 学习文档"), follow this pattern:

### Process

1. **Read all source files** with TODO stubs for that lab, plus their headers and dependencies
2. **Read test files** to understand what each TODO is tested for
3. **Read the official lab document** (`docs/Rucbase-LabX[...].md`) for requirements
4. **Output to** `docs/lciang_database_learning/labN/labN_complete_guide.md` (create directory if needed)

### Document Structure

The guide must follow this exact structure:

```
# LabN <title>：完整学习与实现指南

> 目标说明

## 目录

## 1. 你需要先搞懂的基础知识
   - Explain ALL prerequisite concepts (fd, lseek, page, frame, bitmap, etc.)
   - Include small standalone code examples for each concept
   - Cover data structures the student will encounter

## 2-N. One section per module (in dependency order)
   Each TODO gets:
   ### TODO X.Y：<function_name>
   **位置**：`file.cpp` 第 N-M 行
   **做什么**：one sentence
   **你需要理解的**：why this function exists, what it connects to
   **直接复制到源文件的代码**：
   ```cpp
   // Full function with Chinese comments on EVERY logical block
   ```
   **为什么这么写**：explain non-obvious design decisions

## N+1. 完整测试流程
   - Ordered test commands, each step depends on previous

## N+2. 调试技巧
   - Common compile errors
   - Common runtime errors (segfault, deadlock, assertion)
   - Common logic errors (dirty page, pin_count, bitmap)
   - gdb/valgrind commands

## 附录：各函数速查表
   | module | function | file:line | core operation |
```

### Code Style Rules

- Every code block must be **directly copyable** into the source file (complete function, correct signatures)
- Every logical block gets a **Chinese comment** explaining WHY, not WHAT
- Include a "**为什么这么写**" section for non-obvious patterns (e.g. "erase 后迭代器失效", "先取 path 再 close")
- Fix known pitfalls in the code (e.g. save frame_id before erase)
- Never use pseudocode or "// your code here" placeholders

### Ordering Rules

- Modules must be ordered by **dependency**: bottom layer first (DiskManager → Replacer → BPM → Record → Scan)
- Within each module, helper functions before public functions
- Each section ends with a verification command

### What NOT to do

- Don't modify source files
- Don't give answers without explanations
- Don't skip "obvious" TODOs — students may not know POSIX I/O
- Don't use English comments in the guide (Chinese for teaching, English only for code identifiers)
