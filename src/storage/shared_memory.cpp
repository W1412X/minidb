/**
 * @file shared_memory.cpp
 * @brief Shared memory allocator implementation
 *
 * Implement inter-process shared memory using mmap:
 *   - Create: shm_open + mmap
 *   - Attach: shm_open + mmap (MAP_SHARED)
 *   - Release: munmap + shm_unlink
 *
 * Memory management: simple first-fit free linked list
 */
#include "storage/shared_memory.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace minidb {

SharedMemory::SharedMemory()
    : fd_(-1), mmap_ptr_(nullptr), mmap_size_(0), header_(nullptr), data_(nullptr) {}

SharedMemory::~SharedMemory() {
    if (mmap_ptr_ && mmap_ptr_ != MAP_FAILED) {
        munmap(mmap_ptr_, mmap_size_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

SharedMemory* SharedMemory::create(const String& name, u32 size) {
    // Ensure size is enough for header
    u32 total_size = sizeof(ShmRegionHeader) + size;
    if (total_size < 4096) total_size = 4096;  // minimum 4KB

    // CreateShared memory对象
    String shm_name = String("/") + name;
    int fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) return nullptr;

    // Settings大小
    if (ftruncate(fd, total_size) < 0) {
        close(fd);
        shm_unlink(shm_name.c_str());
        return nullptr;
    }

    // mmap
    void* ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        shm_unlink(shm_name.c_str());
        return nullptr;
    }

    // Initialize
    std::memset(ptr, 0, total_size);

    auto* shm = new SharedMemory();
    shm->name_ = shm_name;
    shm->fd_ = fd;
    shm->mmap_ptr_ = ptr;
    shm->mmap_size_ = total_size;
    shm->header_ = static_cast<ShmRegionHeader*>(ptr);
    shm->data_ = static_cast<byte*>(ptr) + sizeof(ShmRegionHeader);

    // Initialize header
    shm->header_->magic = kShmMagic;
    shm->header_->total_size = size;
    shm->header_->used_size = 0;
    shm->header_->free_list_head = 0;
    shm->header_->num_allocs = 0;
    shm->header_->num_frees = 0;

    // Initialize第一个空闲块
    auto* first_block = reinterpret_cast<ShmBlockHeader*>(shm->data_);
    first_block->size = size;
    first_block->next = 0;
    first_block->is_free = true;

    return shm;
}

SharedMemory* SharedMemory::attach(const String& name) {
    String shm_name = String("/") + name;
    int fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
    if (fd < 0) return nullptr;

    // Get大小
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return nullptr;
    }

    u32 total_size = static_cast<u32>(st.st_size);

    // mmap
    void* ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    auto* shm = new SharedMemory();
    shm->name_ = shm_name;
    shm->fd_ = fd;
    shm->mmap_ptr_ = ptr;
    shm->mmap_size_ = total_size;
    shm->header_ = static_cast<ShmRegionHeader*>(ptr);
    shm->data_ = static_cast<byte*>(ptr) + sizeof(ShmRegionHeader);

    // Validate magic
    if (shm->header_->magic != kShmMagic) {
        delete shm;
        return nullptr;
    }

    return shm;
}

void SharedMemory::destroy(const String& name) {
    String shm_name = String("/") + name;
    shm_unlink(shm_name.c_str());
}

u32 SharedMemory::allocate(u32 size) {
    WriteGuard guard(latch_);

    u32 total_needed = size + kShmBlockSize;

    // First-fit 搜索空闲Linked List
    u32 offset = header_->free_list_head;
    u32 prev_offset = 0;

    while (offset > 0 && offset < header_->total_size) {
        auto* block = reinterpret_cast<ShmBlockHeader*>(data_ + offset);
        if (block->is_free && block->size >= total_needed) {
            // 找到合适的块
            if (block->size > total_needed + kShmBlockSize + 16) {
                // Split块
                u32 remaining_offset = offset + total_needed;
                auto* remaining = reinterpret_cast<ShmBlockHeader*>(data_ + remaining_offset);
                remaining->size = block->size - total_needed;
                remaining->next = block->next;
                remaining->is_free = true;

                block->size = total_needed;
                block->next = remaining_offset;
            }

            block->is_free = false;
            header_->used_size += block->size;
            header_->num_allocs++;

            // 从空闲Linked List移除
            if (prev_offset == 0) {
                header_->free_list_head = block->next;
            } else {
                auto* prev = reinterpret_cast<ShmBlockHeader*>(data_ + prev_offset);
                prev->next = block->next;
            }

            return offset + kShmBlockSize;
        }

        prev_offset = offset;
        offset = block->next;
    }

    return 0;  // 分配失败
}

void SharedMemory::deallocate(u32 offset) {
    if (offset <= kShmBlockSize) return;

    WriteGuard guard(latch_);

    u32 block_offset = offset - kShmBlockSize;
    auto* block = reinterpret_cast<ShmBlockHeader*>(data_ + block_offset);

    if (block->is_free) return;  // 已经是空闲的

    block->is_free = true;
    header_->used_size -= block->size;
    header_->num_frees++;

    // 添加到空闲Linked List头部
    block->next = header_->free_list_head;
    header_->free_list_head = block_offset;

    // Merge相邻空闲块 (简化版: 只合并前向)
    u32 prev_offset = 0;
    u32 scan_offset = header_->free_list_head;
    while (scan_offset > 0 && scan_offset < header_->total_size) {
        auto* scan_block = reinterpret_cast<ShmBlockHeader*>(data_ + scan_offset);
        if (scan_offset + scan_block->size == block_offset) {
            // Merge: scan_block 紧邻 block 前面, 将 block 的空间合入 scan_block
            scan_block->size += block->size;
            scan_block->next = block->next;
            // 从Linked List移除 block (刚被释放的小块), 保留合并后的 scan_block
            if (prev_offset == 0) {
                header_->free_list_head = scan_offset;
            } else {
                auto* prev = reinterpret_cast<ShmBlockHeader*>(data_ + prev_offset);
                prev->next = scan_offset;
            }
            break;
        }
        prev_offset = scan_offset;
        scan_offset = scan_block->next;
    }
}

void* SharedMemory::get_ptr(u32 offset) {
    if (offset == 0 || offset >= header_->total_size) return nullptr;
    return data_ + offset;
}

} // namespace minidb
