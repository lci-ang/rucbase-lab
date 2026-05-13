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

## Lab Implementation Status

Functions with `// Todo:` stubs in the source files are student exercises. Do not implement them unless explicitly asked. The labs are:

1. **Lab1 - Storage**: DiskManager, LRUReplacer, BufferPoolManager, RmFileHandle, RmScan
2. **Lab2 - Index**: B+ tree insert/delete/scan
3. **Lab3 - Query Execution**: Executor operators
4. **Lab4 - Concurrency**: Transaction/lock management

## Language & Toolchain

- C++17 (`-std=c++17`), compiled with `-Wall -O0 -g -ggdb3`
- Google Test for unit testing (vendored in deps/googletest)
- Flex/Bison for SQL parsing
- POSIX I/O: `read`/`write`/`lseek`/`open`/`close`/`unlink`
