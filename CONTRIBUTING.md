# Contributing to MiniDB

## Development Setup

```bash
git clone <repo-url>
cd minidb
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
```

## Running Tests

All tests must pass before submitting changes:

```bash
# Full test suite (400+ tests)
bash tests/run_all_tests.sh ./build/minidb

# Individual suites
python3 tests/comprehensive_test.py ./build/minidb   # 191 SQL tests
python3 tests/consistency_test.py ./build/minidb      # 62 persistence tests
python3 tests/ultimate_test.py ./build/minidb         # 105 edge case tests
python3 tests/concurrent_test.py ./build/minidb       # 8 concurrency tests
python3 tests/bug_verify.py ./build/minidb            # 32 regression tests
bash tests/sql_regression.sh ./build/minidb           # ~60 syntax tests
bash tests/resource_limits.sh ./build/minidb          # Resource management

# Recovery tests
bash tests/recovery_smoke.sh ./build/minidb
bash tests/persistence_and_composite.sh ./build/minidb

# Unit tests
./build/tests/lock_manager_wait_test
./build/tests/tuple_value_edge_test
```

## Code Style

- **Language:** C++17
- **Indentation:** 4 spaces
- **Naming:** PascalCase for types, camelCase for functions/methods, snake_case for variables
- **Headers:** `#pragma once`, doxygen-style `@file` + `@brief` comments
- **Line length:** 120 characters maximum
- **Comments:** English only, focused on "why" not "what"
- **Containers:** Use custom containers (`container/vector.h`, `container/hash_map.h`) instead of STL

## Pull Request Guidelines

1. All tests must pass (`bash tests/run_all_tests.sh ./build/minidb`)
2. New features must include tests
3. Performance-sensitive changes should include benchmark results
4. User-facing changes require documentation updates (README.md, BUILD.md)
5. Keep commits focused and well-described

## Architecture Overview

### Components

```
Parser → Planner → Optimizer → Executor
                                    ↓
  Transaction Manager + Catalog + Lock Manager
                                    ↓
           Buffer Pool + B+ Tree + WAL
                                    ↓
               Disk Manager (double-write)
```

### Key Design Decisions

- **Volcano iterator model:** All executors implement `init()` + `next()` interface
- **WAL-first:** All data modifications are logged to WAL before page writes
- **MVCC snapshot isolation:** Readers never block writers, writers never block readers
- **Cost-based optimizer:** Uses NDV statistics for cardinality estimation
- **Custom containers:** No STL dependency (Vector, HashMap, String are hand-written)

### Source Layout

| Directory | Responsibility |
|-----------|---------------|
| `src/catalog/` | Table/index metadata, column statistics (NDV) |
| `src/common/` | Configuration, synchronization (Mutex/RwLock), error handling (Result/Status), resource management |
| `src/concurrency/` | Lock manager (table/row/key locks, deadlock detection) |
| `src/container/` | Custom data structures (Vector, HashMap, String, UniquePtr, LinkedList) |
| `src/database/` | Database lifecycle, flush, statistics, background maintenance |
| `src/index/` | B+ Tree (insert/search/delete, bulk load, range scan, per-tree RwLock) |
| `src/network/` | TCP server (admission control, streaming execution, cursor protocol) |
| `src/record/` | Value system (7 types), Tuple (MVCC header), Schema |
| `src/recovery/` | WAL (checkpoint, log, segments, recovery, GC) |
| `src/repl/` | Interactive SQL shell |
| `src/sql/executor/` | 14 executor types (Volcano iterator model) |
| `src/sql/optimizer/` | Cost-based optimizer (NDV selectivity, index/join path selection) |
| `src/sql/parser/` | Recursive descent parser (18 statement types) |
| `src/sql/planner/` | Query planner (14 plan node types) |
| `src/storage/` | Buffer pool, disk manager, heap file, page layout, shared memory |
| `src/transaction/` | MVCC transaction manager (snapshot isolation, version chains, undo) |

## License

MIT
