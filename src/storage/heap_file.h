/**
 * @file heap_file.h
 * @brief 堆File management — 管理一个逻辑表的所有数据页
 *
 * Layout: Page 0 (Meta Page) + Page 1+ (Data Pages)
 * Data pages are linked into a single linked list via next_page_id.
 */
#pragma once

#include "common/defs.h"
#include "common/config.h"
#include "common/mutex.h"
#include "common/status.h"
#include "container/utility.h"
#include "storage/page.h"
#include "storage/buffer_pool.h"

namespace minidb {

// Meta page data (stored in page 0 data area)
#pragma pack(push, 1)
struct HeapMeta {
    u32  table_id;
    u32  reserved;
    u64  first_data_page_id;    // First data page
    u64  last_data_page_id;     // 最后一个数据页
    u32  num_data_pages;        // 数据页总数
    u64  num_tuples;            // 有效元组总数
    u64  num_deleted_tuples;    // 已删除元组数
};
#pragma pack(pop)

class HeapFile : NonCopyable {
public:
    HeapFile(BufferPool* pool, u32 table_id);
    ~HeapFile() = default;

    // Initialize新堆文件 (创建 Page 0 meta page)
    void create();

    // Insert元组, 返回 (page_id, slot_idx)
    Result<Pair<PageId, SlotIdx>> insert_tuple(const byte* data, u16 length, u64 lsn = 0);

    // WAL-first: 预定插入位置 (不Modify页面)
    struct InsertPlan {
        PageId page_id;
        bool is_new_page;
        SlotIdx predicted_slot;
    };
    Result<InsertPlan> prepare_insert(u16 length);

    // WAL-first: 提交插入 (写入数据 + Settings LSN, 在 unpin 前完成所有操作)
    Result<Pair<PageId, SlotIdx>> commit_insert(PageId page_id, bool is_new_page,
                                                 SlotIdx predicted_slot,
                                                 const byte* data, u16 length, u64 lsn);

    // WAL-first: 提交 UPDATE 的旧页Modify (set_next_version + mark_deleted + set_lsn, 原子化)
    bool commit_old_tuple(PageId page_id, SlotIdx slot_idx,
                          PageId next_page, SlotIdx next_slot,
                          u64 xmax, u64 lsn);

    // WAL-first: Settings页 LSN (兜底方法, 仅在 commit_insert 内部使用)
    void set_page_lsn(PageId page_id, u64 lsn);

    // HOT: 在指定页插入元组 (同页Version chain), 失败返回 kNullSlot
    Result<Pair<PageId, SlotIdx>> insert_tuple_in_page(PageId page_id,
                                                       const byte* data, u16 length, u64 lsn = 0);

    // WAL-first HOT: 预定同页插入 (不Modify)
    Result<SlotIdx> prepare_insert_in_page(PageId page_id, u16 length);

    // WAL-first HOT: 提交同页插入 (写入数据 + Settings LSN)
    Result<Pair<PageId, SlotIdx>> commit_insert_in_page(PageId page_id, SlotIdx slot_idx,
                                                        const byte* data, u16 length, u64 lsn);

    // 回滚插入 (标记为 UNUSED)
    bool rollback_insert(PageId page_id, SlotIdx slot_idx, u64 lsn = 0);

    // 回滚删除/旧版本失效: 清除 xmax 和 next_version, 恢复原Version chain尾
    bool rollback_delete(PageId page_id, SlotIdx slot_idx, u64 lsn = 0);

    // MVCC: 标记删除 (只设 xmax, 不物理删除)
    bool mark_deleted(PageId page_id, SlotIdx slot_idx, u64 xmax, u64 lsn = 0);

    // MVCC: Settings xmin (INSERT 时)
    bool set_xmin(PageId page_id, SlotIdx slot_idx, u64 xmin, u64 lsn = 0);

    // MVCC: SettingsVersion chain指针
    bool set_next_version(PageId page_id, SlotIdx slot_idx, PageId next_page, SlotIdx next_slot,
                          u64 lsn = 0);

    // GC: 标记 LinePointer 为 DEAD
    bool mark_dead(PageId page_id, SlotIdx slot_idx, u64 lsn = 0);
    bool prune_obsolete_version(PageId page_id, SlotIdx slot_idx, u64 oldest_active_txn,
                                u64 committed_xmax, u64 lsn = 0);
    /// @param out_new_physical_tuple 若非空，在确实新写入槽位时为 true（用于判断是否需要重建索引）
    bool recover_insert_at(PageId page_id, SlotIdx slot_idx, const byte* data, u16 length, u64 lsn,
                           bool* out_new_physical_tuple = nullptr);
    bool recover_update(PageId old_page_id, SlotIdx old_slot_idx,
                        PageId new_page_id, SlotIdx new_slot_idx,
                        u64 xmax, const byte* data, u16 length, u64 lsn);

    // GetFirst data page ID (全表扫描起点)
    PageId first_data_page_id() const;

    // Get元数据
    const HeapMeta& meta() const { return meta_; }
    u32 table_id() const { return table_id_; }
    void flush_meta();

private:
    void ensure_meta_loaded() const;
    PageId meta_page_id() const;
    PageId allocate_new_page_id();
    void load_meta();
    void save_meta();
    void note_meta_changed();
    // 预测页面上的下一个可用 slot (不Modify页面, 镜像 Page::insert_tuple 逻辑)
    SlotIdx predict_slot(Page* page, u16 length) const;

    BufferPool* pool_;
    u32 table_id_;
    mutable HeapMeta meta_;
    mutable bool meta_loaded_;
    bool meta_dirty_;
    u32 meta_mutations_since_save_;
    mutable Mutex latch_;
};

} // namespace minidb
