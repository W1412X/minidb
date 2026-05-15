# storage — 存储引擎层

> MiniADB 存储引擎的详细设计文档，对齐 PostgreSQL 核心存储概念。

---

## 目录

1. [PostgreSQL 存储模型对齐](#1-postgresql-存储模型对齐)
2. [页结构 (Page)](#2-页结构-page)
3. [磁盘管理 (DiskManager)](#3-磁盘管理-diskmanager)
4. [缓冲池 (BufferPool)](#4-缓冲池-bufferpool)
5. [堆文件 (HeapFile)](#5-堆文件-heapfile)
6. [元组操作流程](#6-元组操作流程)
7. [文件清单](#7-文件清单)

---

## 1. PostgreSQL 存储模型对齐

MiniADB 的存储引擎核心概念对齐 PostgreSQL：

| 概念 | PostgreSQL | MiniADB | 说明 |
|------|-----------|---------|------|
| 页大小 | 8KB | 8KB | 与 PG 完全一致 |
| 页头 | `PageHeaderData` 24B | `PageHeader` 24B | 字段名对齐 |
| 行指针 | `ItemIdData` (block, offset, flags) | `LinePointer` (offset, length) | 简化版，去掉 block |
| 元组头 | `HeapTupleHeaderData` 23B | 16B (xmin+xmax+num_cols) | 精简，保留 MVCC 核心 |
| 缓冲池替换 | Clock-Sweep | LRU + 防污染 | 简化但保留核心思想 |
| 堆存储 | Heap 关系 | HeapFile | 单文件 = 单表 |
| WAL | XLog | WAL (简化 ARIES) | 日志序列号对齐 |
| VACUUM | 全量 VACUUM + autovacuum | 碎片标记 (后续扩展) | 保留碎片概念 |

### 1.1 页头字段对齐

```
PostgreSQL PageHeaderData (24 bytes):
  pd_lsn        (8B)  — 最后修改该页的 WAL LSN
  pd_checksum   (2B)  — 页校验和 (PG12+)
  pd_flags      (2B)  — 页标志位
  pd_lower      (2B)  — 空闲空间起始偏移 (紧跟 line pointer 尾部)
  pd_upper      (2B)  — 空闲空间结束偏移 (tuple data 起始)
  pd_special    (2B)  — 特殊空间起始偏移 (索引用)
  pd_pagesize_version (2B) — 页大小 + 版本号
  pd_prune_xid  (4B)  — 用于 VACUUM pruning 的事务ID

MiniADB PageHeader (24 bytes):
  page_id         (8B)  — 页全局唯一标识 (file_id << 32 | page_num)
  lsn             (8B)  — 最后修改该页的 WAL LSN (对齐 PG)
  page_type       (2B)  — 页类型 (替代 PG 的 flags 部分功能)
  free_space_offset (2B) — 空闲空间起始偏移 (对齐 PG pd_lower)
  num_tuples      (2B)  — 元组计数 (简化: PG 无此字段，靠 pd_lower 计算)
  reserved        (2B)  — 保留字段
```

### 1.2 行指针对齐

```
PostgreSQL ItemIdData (4 bytes):
  lp_off   (15 bits) — 元组在页内的偏移
  lp_flags (2 bits)  — LP_NORMAL / LP_REDIRECT / LP_DEAD / LP_UNUSED
  lp_len   (14 bits) — 元组长度 (含头部)

MiniADB LinePointer (4 bytes):
  offset (u16) — 元组在页内的偏移
  length (u16) — 元组数据长度

简化点:
  - 去掉 flags 字段 (LP_DEAD 等由上层 MVCC 判断)
  - offset 用完整 u16 (65535 > 8192，足够)
  - 不需要 lp_redirect (简化版不做 HOT)
```

### 1.3 缓冲池替换对齐

```
PostgreSQL: Clock-Sweep 算法
  - 每个 buffer 有一个 usage_count (0-5)
  - 每次访问: usage_count = min(usage_count+1, 5)
  - 驱逐: 从 clock hand 开始扫描, usage_count==0 的帧被驱逐
  - 扫到 usage_count>0 的帧: usage_count--, 移动 clock hand

MiniADB: LRU + 防污染 (简化版)
  - 正常访问: 页移到 LRU 头部 (高优先级)
  - 顺序扫描: 新页放 LRU 尾部 (低优先级，防止污染热数据)
  - 驱逐: 从 LRU 尾部开始，找 pin_count==0 的帧

为什么不用 Clock-Sweep:
  - Clock-Sweep 实现更复杂，需要 usage_count 维护
  - LRU 在学习项目中更直观，易于理解和调试
  - 防污染机制保留了 PG 的核心思想
```

---

## 2. 页结构 (Page)

### 2.1 完整内存布局

```
MiniADB 8KB 页布局 (8192 bytes):

偏移 0x0000:
┌──────────────────────────────────────────────────────────┐
│ PageHeader (24 bytes)                                     │
│                                                          │
│  [0x0000-0x0007]  page_id          (u64)  8 bytes        │
│  [0x0008-0x000F]  lsn              (u64)  8 bytes        │
│  [0x0010-0x0011]  page_type        (u16)  2 bytes        │
│  [0x0012-0x0013]  free_space_offset (u16) 2 bytes        │
│  [0x0014-0x0015]  num_tuples       (u16)  2 bytes        │
│  [0x0016-0x0017]  reserved         (u16)  2 bytes        │
│                                                          │
├──────────────────────────────────────────────────────────┤
│ Line Pointer Array (从 0x0018 开始)                        │
│                                                          │
│  每个 LinePointer 4 bytes:                                │
│  ┌────────────────────────────────────────────────┐      │
│  │ [0-1] offset (u16)  元组在页内的偏移             │      │
│  │ [2-3] length (u16)  元组数据长度                 │      │
│  └────────────────────────────────────────────────┘      │
│                                                          │
│  Line Pointer 0: 偏移 0x0018, 长度 4                     │
│  Line Pointer 1: 偏移 0x001C, 长度 4                     │
│  ...                                                     │
│  Line Pointer N: 偏移 0x0018 + N*4                        │
│                                                          │
├──────────────────────────────────────────────────────────┤
│ Free Space (空闲空间)                                     │
│                                                          │
│  起始: free_space_offset (紧跟 Line Pointer 数组尾部)      │
│  结束: 元组数据区的起始                                    │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │                可用空间                           │   │
│  │   (Line Pointer 向后增长 ← → 元组数据向前增长)     │   │
│  └──────────────────────────────────────────────────┘   │
│                                                          │
├──────────────────────────────────────────────────────────┤
│ Tuple Data (元组数据区)                                   │
│                                                          │
│  从页尾部 (0x1FFF) 向前增长:                              │
│                                                          │
│  ┌──────────┬──────────┬──────────┐                     │
│  │ Tuple N  │ Tuple N-1│ ...      │                     │
│  │ (最新)   │          │ Tuple 1  │                     │
│  └──────────┴──────────┴──────────┘                     │
│  ↑ 最新插入的元组在最前面                                  │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### 2.2 PageId 编码

```
PageId (u64) = file_id (高32位) | page_num (低32位)

  file_id: 标识文件 (每个 .heap/.btree/.cat 文件有唯一 file_id)
  page_num: 页在文件内的序号 (0-based)

  示例:
    file_id=10, page_num=3 → PageId = (10 << 32) | 3 = 4294967303
    PageId=0 保留为无效页

  磁盘寻址:
    文件路径 = 根据 file_id 映射到具体文件
    文件偏移 = page_num * 8192
```

### 2.3 空间计算

```
可用空间 = kPageSize - kPageHeaderSize - (num_tuples + 1) * kLinePointerSize - 已用元组空间

  kPageSize = 8192
  kPageHeaderSize = 24
  kLinePointerSize = 4

  空页可用空间: 8192 - 24 - 0 = 8168 bytes
  插入1个元组后: 8192 - 24 - 4 - tuple_size = 8164 - tuple_size

  判断是否能插入:
    需要空间 = kLinePointerSize + tuple_size
    可用空间 = (kPageSize - free_space_offset) - (kPageSize - data_area_start)
    简化判断: lp_end + tuple_size <= kPageSize
      其中 lp_end = kPageHeaderSize + (num_tuples + 1) * kLinePointerSize
```

### 2.4 页类型

```
PageType 枚举:
  kHeapData    = 1   堆表数据页
  kHeapMeta    = 2   堆表元数据页 (首页)
  kIndexData   = 3   B+树索引数据页
  kIndexMeta   = 4   B+树索引元数据页 (首页)
  kFreeList    = 5   空闲页链表
  kWalPage     = 6   WAL 日志页
  kCatalogData = 7   系统目录数据页

每种页类型复用同一个 Page 类 (8KB buffer)，
通过 page_type 字段区分如何解析内容。
```

---

## 3. 磁盘管理 (DiskManager)

### 3.1 职责

```
DiskManager 是存储引擎的最底层，只做两件事:
  1. read_page(page_id, buffer)  — 从磁盘读一个 8KB 页
  2. write_page(page_id, buffer) — 写一个 8KB 页到磁盘

不管理缓冲池，不管理事务，不做页解析。
它是 BufferPool 和物理磁盘之间的桥梁。
```

### 3.2 文件组织

```
data/                          # 数据库根目录
├── db.minidb                  # 数据库描述文件
│   magic_number  (4B)  = 0x4D494E49 ("MINI")
│   version       (4B)  = 1
│   next_file_id  (4B)
│   num_tables    (4B)
│   reserved      (其余)
│
├── catalog/                   # 系统目录
│   └── 1.cat                  # file_id=1 的系统表数据
│
├── tables/                    # 堆表数据
│   ├── 10.heap                # file_id=10 的表数据
│   ├── 11.heap                # file_id=11 的表数据
│   └── ...
│
├── indexes/                   # B+树索引
│   ├── 100.btree              # file_id=100 的索引
│   └── ...
│
└── wal/                       # WAL 日志
    ├── wal_00000001.log       # WAL 段文件 (固定大小)
    └── ...
```

### 3.3 PageId 到文件的映射

```
page_to_path(page_id):
  file_id = page_id >> 32
  根据 file_id 查找对应的文件路径:
    - file_id == 1: catalog/1.cat
    - file_id < 1000: tables/<file_id>.heap
    - file_id >= 1000: indexes/<file_id>.btree

page_to_offset(page_id):
  page_num = page_id & 0xFFFFFFFF
  return page_num * 8192

关键: 文件内所有页是连续的 8KB 块，
      偏移 = page_num * 8192
      读写 = lseek(fd, offset) + read/write(fd, 8KB)
```

### 3.4 文件描述符管理

```
避免频繁 open/close:
  HashMap<String, int> fd_cache_;  // 文件路径 → fd

get_fd(path):
  if path in fd_cache_:
      return fd_cache_[path]
  else:
      fd = open(path, O_RDWR | O_CREAT, 0644)
      fd_cache_[path] = fd
      return fd

注意: 简化版不做 fd 池化和 LRU 淘汰。
      保持打开所有已访问的文件。
```

---

## 4. 缓冲池 (BufferPool)

### 4.1 职责

```
BufferPool 是存储引擎的核心组件:
  1. 缓存磁盘页到内存
  2. 管理帧的 pin/unpin 引用计数
  3. 脏页管理 (dirty flag)
  4. 页面替换策略 (LRU + 防污染)
  5. 并发保护 (读写锁)
```

### 4.2 内部数据结构

```
BufferPool 内部布局:

┌──────────────────────────────────────────────────────┐
│ Frame 数组 (固定大小, 预分配)                          │
│                                                      │
│  Frame 0:  ┌─────────────────────────────────────┐  │
│            │ Page page        (8KB)               │  │
│            │ PageId page_id   (当前缓存的页ID)      │  │
│            │ u32  pin_count   (引用计数, >0不可驱逐)│  │
│            │ bool is_dirty    (是否被修改)          │  │
│            │ FrameIdx lru_idx (在LRU链表中的位置)   │  │
│            └─────────────────────────────────────┘  │
│  Frame 1:  ┌─────────────────────────────────────┐  │
│            │ ...                                  │  │
│            └─────────────────────────────────────┘  │
│  ...                                                │
│  Frame N:  ┌─────────────────────────────────────┐  │
│            │ ...                                  │  │
│            └─────────────────────────────────────┘  │
│                                                      │
├──────────────────────────────────────────────────────┤
│ Page Table (HashMap<PageId, FrameIdx>)                │
│                                                      │
│  快速查找: PageId → 哪个 Frame 缓存了这个页           │
│                                                      │
│  示例: {5→0, 12→1, 3→2, 8→3}                        │
│  含义: PageId=5 缓存在 Frame 0, PageId=12 在 Frame 1 │
│                                                      │
├──────────────────────────────────────────────────────┤
│ LRU List (LinkedList<FrameIdx>)                      │
│                                                      │
│  头部(MRU) ←→ [F2] ←→ [F0] ←→ [F3] ←→ 尾部(LRU)    │
│                                                      │
│  头部 = 最近访问 (最不可能被驱逐)                       │
│  尾部 = 最久未访问 (最可能被驱逐)                       │
│                                                      │
│  访问顺序: F2(最近) → F0 → F3 → F1(最久)              │
│                                                      │
└──────────────────────────────────────────────────────┘
```

### 4.3 fetch_page 完整流程

```
fetch_page(page_id, is_sequential = false):
  返回: Result<Page*>

  加写锁: WriteGuard guard(latch_);

  ┌─── Step 1: 哈希表查找 ───────────────────────────┐
  │                                                     │
  │  auto* frame_idx = page_table_.find(page_id);      │
  │                                                     │
  │  if (frame_idx 存在):                               │
  │      Frame& f = frames_[*frame_idx];               │
  │      f.pin_count++;                                │
  │      move_to_lru_head(*frame_idx);                 │
  │      return &f.page;          // 命中! 直接返回     │
  │                                                     │
  └─────────────────────────────────────────────────────┘

  ┌─── Step 2: 未命中, 找 victim 帧 ─────────────────┐
  │                                                     │
  │  FrameIdx victim = find_victim_frame();            │
  │                                                     │
  │  find_victim 逻辑:                                 │
  │    a. 先遍历找 pin_count==0 的空帧                  │
  │    b. 没有空帧 → 从 LRU 尾部开始扫描               │
  │       while (尾部帧.pin_count > 0):                │
  │         move_to_lru_head(尾部帧);  // 跳过被 pin 的 │
  │       如果扫描一圈都找不到 → 返回 kBufferFull 错误   │
  │                                                     │
  └─────────────────────────────────────────────────────┘

  ┌─── Step 3: 驱逐旧页 ─────────────────────────────┐
  │                                                     │
  │  Frame& victim_frame = frames_[victim];            │
  │                                                     │
  │  if (victim_frame.is_dirty):                       │
  │      disk_mgr_->write_page(                        │
  │          victim_frame.page_id,                     │
  │          victim_frame.page.data());                │
  │                                                     │
  │  page_table_.erase(victim_frame.page_id);          │
  │                                                     │
  └─────────────────────────────────────────────────────┘

  ┌─── Step 4: 加载新页 ─────────────────────────────┐
  │                                                     │
  │  disk_mgr_->read_page(page_id,                     │
  │                        victim_frame.page.data());  │
  │                                                     │
  │  victim_frame.page_id = page_id;                   │
  │  victim_frame.pin_count = 1;                       │
  │  victim_frame.is_dirty = false;                    │
  │                                                     │
  │  page_table_[page_id] = victim;                    │
  │                                                     │
  │  if (is_sequential):                               │
  │      lru_list_.push_back(victim);   // 放尾部       │
  │  else:                                              │
  │      lru_list_.push_front(victim);  // 放头部       │
  │                                                     │
  │  return &victim_frame.page;                        │
  │                                                     │
  └─────────────────────────────────────────────────────┘
```

### 4.4 unpin_page 流程

```
unpin_page(page_id):
  WriteGuard guard(latch_);

  auto* frame_idx = page_table_.find(page_id);
  if (frame_idx == nullptr) return;  // 页不在缓冲池

  Frame& f = frames_[*frame_idx];
  if (f.pin_count > 0) f.pin_count--;

  // pin_count 降到 0 时, 帧可以被驱逐
  // 不立即移动 LRU 位置, 由驱逐算法决定
```

### 4.5 mark_dirty 流程

```
mark_dirty(page_id):
  WriteGuard guard(latch_);

  auto* frame_idx = page_table_.find(page_id);
  if (frame_idx == nullptr) return;

  frames_[*frame_idx].is_dirty = true;
```

### 4.6 flush_page / flush_all

```
flush_page(page_id):
  WriteGuard guard(latch_);

  auto* frame_idx = page_table_.find(page_id);
  if (frame_idx == nullptr) return;

  Frame& f = frames_[*frame_idx];
  if (f.is_dirty):
      disk_mgr_->write_page(f.page_id, f.page.data());
      f.is_dirty = false;

flush_all():
  WriteGuard guard(latch_);

  for (i = 0; i < pool_size_; i++):
      if (frames_[i].is_dirty):
          disk_mgr_->write_page(frames_[i].page_id,
                                frames_[i].page.data());
          frames_[i].is_dirty = false;
```

### 4.7 new_page 流程

```
new_page(page_id):
  返回: Result<Page*>

  // 与 fetch_page 类似, 但不从磁盘读取
  // 直接分配一个空帧并初始化

  WriteGuard guard(latch_);

  FrameIdx victim = find_victim_frame();
  // 驱逐旧帧...

  Frame& f = frames_[victim];
  f.page.init(page_id, type);  // 初始化为空白页
  f.page_id = page_id;
  f.pin_count = 1;
  f.is_dirty = true;           // 新页标记为脏

  page_table_[page_id] = victim;
  lru_list_.push_front(victim);

  return &f.page;
```

### 4.8 LRU 防污染策略

```
问题:
  SELECT * FROM big_table;  -- 全表扫描, 加载所有页

  普通 LRU: 每次加载新页都移到头部
  结果: 热数据被全部挤出缓冲池

PostgreSQL 方案: Ring Buffer
  使用一个 16 帧的小缓冲区专门给顺序扫描
  顺序扫描的页只在 Ring 内循环, 不影响主缓冲池

MiniADB 方案: LRU 位置控制
  正常访问 (点查、索引扫描):
    新加载的页 → LRU 头部 (高优先级)
    已有页被访问 → 移到头部

  顺序扫描 (SeqScan):
    新加载的页 → LRU 尾部 (低优先级)
    不会挤掉头部的热数据

  实现:
    fetch_page(page_id, is_sequential):
      // ... 加载到 victim 帧 ...
      if (is_sequential):
          lru_list_.push_back(victim);     // 尾部
      else:
          lru_list_.push_front(victim);    // 头部

  调用方:
    SeqScanExecutor::next() → fetch_page(pid, true)
    IndexScanExecutor::next() → fetch_page(pid, false)
    HeapFile::insert_tuple() → fetch_page(pid, false)
```

---

## 5. 堆文件 (HeapFile)

### 5.1 职责

```
HeapFile 管理一个逻辑表的所有数据:
  - 创建新表 (创建 .heap 文件)
  - 插入元组 (找到有空间的页)
  - 删除元组 (标记删除)
  - 全表扫描 (遍历所有数据页)
```

### 5.2 文件布局

```
<table_id>.heap 文件结构:

Page 0: Meta Page (元数据页)
┌──────────────────────────────────────────────────────┐
│ PageHeader (page_type = kHeapMeta)                   │
├──────────────────────────────────────────────────────┤
│ HeapMeta 结构:                                        │
│                                                      │
│  table_id           (u32)   表的内部 ID              │
│  first_data_page_id (u64)   第一个数据页的 PageId     │
│  last_data_page_id  (u64)   最后一个数据页的 PageId   │
│  num_data_pages     (u32)   数据页总数               │
│  num_tuples         (u64)   有效元组总数              │
│  num_deleted_tuples (u64)   已删除元组数              │
│                                                      │
│  (Schema 序列化数据紧跟其后, 由 Catalog 管理)         │
│                                                      │
└──────────────────────────────────────────────────────┘

Page 1+: Data Pages (数据页)
┌──────────────────────────────────────────────────────┐
│ PageHeader (page_type = kHeapData)                   │
├──────────────────────────────────────────────────────┤
│ Line Pointers + Free Space + Tuple Data              │
├──────────────────────────────────────────────────────┤
│ 页尾最后 8 字节: next_page_id (u64)                  │
│   0 = 没有下一页 (链表末尾)                           │
│   >0 = 下一页的 PageId                               │
└──────────────────────────────────────────────────────┘
```

### 5.3 页链表

```
数据页通过 next_page_id 串联成单链表:

  Meta Page (Page 0)
    │
    ├─ first_data_page_id ──→ Page 1 ──→ Page 2 ──→ Page 3 ──→ (null)
    │                          ↑
    └─ last_data_page_id ───────┘

  遍历: 从 first_data_page_id 开始, 逐个读取 next_page_id
  插入: 总是在 last_data_page_id 对应的页插入
  全表扫描: 从头遍历链表
```

### 5.4 插入流程

```
insert_tuple(tuple_data, tuple_size):
  返回: Result<Pair<PageId, SlotIdx>>

  Step 1: 读取 Meta Page
  ┌──────────────────────────────────────────────┐
  │ meta_page = pool_->fetch_page(meta_page_id); │
  │ last_page_id = meta.last_data_page_id;       │
  └──────────────────────────────────────────────┘

  Step 2: 如果没有数据页, 创建第一个
  ┌──────────────────────────────────────────────┐
  │ if (last_page_id == kNullPageId):            │
  │     new_page_id = allocate_new_page();       │
  │     meta.first_data_page_id = new_page_id;   │
  │     meta.last_data_page_id = new_page_id;    │
  │     last_page_id = new_page_id;              │
  └──────────────────────────────────────────────┘

  Step 3: 尝试在最后一页插入
  ┌──────────────────────────────────────────────┐
  │ last_page = pool_->fetch_page(last_page_id); │
  │                                              │
  │ lp_end = kPageHeaderSize +                   │
  │          (last_page.num_tuples + 1) * 4;     │
  │ if (lp_end + tuple_size <= kPageSize):       │
  │     // 空间足够, 直接插入                     │
  │     slot = last_page.insert_tuple(data, len);│
  │     pool_->mark_dirty(last_page_id);         │
  │     return {last_page_id, slot};             │
  └──────────────────────────────────────────────┘

  Step 4: 最后一页满了, 分配新页
  ┌──────────────────────────────────────────────┐
  │ new_page_id = allocate_new_page();           │
  │ new_page = pool_->fetch_page(new_page_id);   │
  │                                              │
  │ // 设置新页的 next_page_id 为 null            │
  │ *reinterpret_cast<u64*>(                     │
  │     new_page->data() + kPageSize - 8) = 0;  │
  │                                              │
  │ // 更新旧最后一页的 next_page_id              │
  │ *reinterpret_cast<u64*>(                     │
  │     last_page->data() + kPageSize - 8) =     │
  │     new_page_id;                             │
  │                                              │
  │ slot = new_page.insert_tuple(data, len);     │
  │                                              │
  │ meta.last_data_page_id = new_page_id;        │
  │ meta.num_data_pages++;                       │
  │ pool_->mark_dirty(meta_page_id);             │
  │                                              │
  │ return {new_page_id, slot};                  │
  └──────────────────────────────────────────────┘
```

### 5.5 删除流程

```
delete_tuple(page_id, slot_idx):
  page = pool_->fetch_page(page_id);
  line_ptr = page->line_pointer(slot_idx);

  // 标记 Line Pointer 为空 (offset=0, length=0)
  line_ptr->offset = 0;
  line_ptr->length = 0;

  meta.num_tuples--;
  meta.num_deleted_tuples++;
  pool_->mark_dirty(page_id);
  pool_->mark_dirty(meta_page_id);

  // 碎片产生:
  // Line Pointer 被清空, 但元组数据仍在页内
  // 空间无法被新元组复用 (除非新元组恰好从尾部增长)
  // 需要 VACUUM 回收 (后续扩展)

  注意: 在 MVCC 模式下, 删除操作实际上是:
    1. 设置旧元组的 xmax = current_txn_id
    2. 插入一个新版本的元组 (如果有 UPDATE)
    真正的空间回收由 VACUUM 负责
```

### 5.6 全表扫描

```
scan_all(callback):
  current_page_id = meta.first_data_page_id;

  while (current_page_id != kNullPageId):
      page = pool_->fetch_page(current_page_id, true);  // is_sequential=true

      for (slot = 0; slot < page->header().num_tuples; slot++):
          lp = page->get_line_pointer(slot);
          if (lp.is_valid()):
              tuple = Tuple::deserialize_from_page(
                  page->data() + lp.offset, schema);
              callback(tuple);  // 处理元组

      // 读取 next_page_id (页尾最后 8 字节)
      next = *reinterpret_cast<const u64*>(
          page->data() + kPageSize - sizeof(u64));

      pool_->unpin_page(current_page_id);
      current_page_id = next;
```

---

## 6. 元组操作流程

### 6.1 元组在页内的生命周期

```
INSERT:
  1. Tuple::serialize_to_page() → 序列化为字节流
  2. Page::insert_tuple(data, size)
     a. 计算 lp_offset = kPageHeaderSize + num_tuples * 4
     b. 计算 tuple_offset = kPageSize - size (从尾部)
     c. 检查 lp_offset + 4 <= tuple_offset (空间够)
     d. 写入 LinePointer: {offset=tuple_offset, length=size}
     e. 写入 Tuple Data: memcpy(page + tuple_offset, data, size)
     f. num_tuples++

DELETE:
  1. MVCC 模式: 设置 tuple.xmax = current_txn_id
  2. 物理删除 (VACUUM): 清空 LinePointer
  3. Page 碎片产生, 等待 VACUUM 回收

UPDATE (MVCC):
  1. 旧元组: 设置 xmax = current_txn_id
  2. 新元组: insert_tuple(新数据)
  3. 产生两个版本, 由 MVCC 可见性控制谁可见

SELECT (MVCC):
  1. 读取元组: page->get_tuple(slot_idx)
  2. 反序列化: Tuple::deserialize_from_page(data, schema)
  3. 可见性检查: MVCC::is_visible(tuple, txn)
     - xmin 必须已提交
     - xmin <= txn.read_ts
     - xmax == 0 或未提交
  4. 可见 → 返回元组, 不可见 → 跳过
```

### 6.2 LinePointer 状态机

```
  ┌─────────────────────────────────────────────────┐
  │                                                 │
  │  LP_UNUSED (初始状态)                            │
  │    offset=0, length=0                           │
  │    │                                            │
  │    │ insert_tuple()                             │
  │    ▼                                            │
  │  LP_NORMAL (正常)                               │
  │    offset>0, length>0                           │
  │    │                                            │
  │    │ delete_tuple() / MVCC xmax 设置            │
  │    ▼                                            │
  │  LP_DEAD (已删除, 等待 VACUUM)                  │
  │    offset=0, length=0 (物理删除后)              │
  │    │                                            │
  │    │ VACUUM 回收空间                            │
  │    ▼                                            │
  │  LP_UNUSED (回收, 可复用)                       │
  │                                                 │
  └─────────────────────────────────────────────────┘

  MiniADB 简化: 去掉 LP_DEAD 状态
    - 删除直接标记 offset=0, length=0
    - 不区分 "逻辑删除" 和 "物理删除"
    - 碎片直接产生, 后续 VACUUM 扩展时处理
```

---

## 7. 文件清单

```
src/storage/
├── README.md            # 本文档 — 存储引擎详细设计
├── page.h               # Page 类声明
├── page.cpp             # Page 类实现
├── disk_manager.h       # DiskManager 类声明
├── disk_manager.cpp     # DiskManager 类实现
├── buffer_pool.h        # BufferPool 类声明
├── buffer_pool.cpp      # BufferPool 类实现
├── heap_file.h          # HeapFile 类声明
└── heap_file.cpp        # HeapFile 类实现
```

### 各文件职责

| 文件 | 核心类 | 职责 |
|------|--------|------|
| `page.h/.cpp` | `Page` | 8KB 页的内存表示，提供元组插入/删除/读取接口 |
| `disk_manager.h/.cpp` | `DiskManager` | PageId 到物理文件的映射，底层文件 I/O |
| `buffer_pool.h/.cpp` | `BufferPool` | 磁盘页缓存，LRU 替换，pin/unpin，脏页管理 |
| `heap_file.h/.cpp` | `HeapFile` | 堆表管理，元组插入/删除/全表扫描 |

### 依赖关系

```
HeapFile
  └── BufferPool
        └── DiskManager
              └── OS (open/read/write/close)

上层调用链:
  Executor → HeapFile → BufferPool → DiskManager → Disk
```
