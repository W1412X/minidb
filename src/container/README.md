# container

In-house container types. MiniDB deliberately avoids most of the STL
in performance-sensitive paths to keep the dependency surface small and
to make memory ownership explicit; the executor and storage layers use
these instead of `<vector>`, `<string>`, `<memory>`, etc.

## Key types

| Type | File | Role |
| --- | --- | --- |
| `String` | `string.h`, `string.cpp` | Owning char buffer; supports `compare`, append, formatting |
| `Vector<T>` | `vector.h`, `vector.cpp` | Dynamic array; provides `push_back` / `resize` / `clear` / iterators |
| `Pair<A, B>` | `pair.h` | 2-tuple with copy/move semantics |
| `HashMap<K, V>` | `hash_map.h`, `hash_map.cpp` | Open-addressed hash table used across executors and catalog |
| `HashSet<K>` | `hash_set.h`, `hash_set.cpp` | Hash set on top of `HashMap` |
| `LinkedList<T>` | `linked_list.h`, `linked_list.cpp` | Doubly-linked list; used by `BufferPool` LRU |
| `Optional<T>` | `optional.h` | Tagged optional value |
| `UniquePtr<T>` | `unique_ptr.h` | Move-only owning pointer (`std::unique_ptr` workalike) |
| `make_unique<T>(...)` | `unique_ptr.h` | Helper for constructing `UniquePtr<T>` |

## Notes

- Types are header-only when small and template-heavy (`UniquePtr`,
  `Pair`, `Optional`), `.cpp` exists when there is non-template code
  (`String`, `HashMap`).
- Container APIs intentionally lean toward iterator-by-pointer rather
  than C++20 ranges to keep compile times down.
- Move semantics use explicit `static_cast<T&&>(x)` rather than
  `std::move`; the project style avoids `<utility>` for hot headers.
