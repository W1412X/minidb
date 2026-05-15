#include "storage/shm.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace minidb {

static constexpr u32 kShmMagic = 0x4D444243;  // "MDBC"
static constexpr u32 kShmVersion = 1;

SharedMemory::SharedMemory()
    : fd_(-1), mmap_ptr_(nullptr), size_(0), header_(nullptr), data_(nullptr) {}

SharedMemory::~SharedMemory() {
    if (mmap_ptr_ && mmap_ptr_ != MAP_FAILED) {
        munmap(mmap_ptr_, size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

SharedMemory* SharedMemory::create(const String& name, u32 total_size) {
    String path = String("/minidb_shm_") + name;
    int fd = shm_open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) return nullptr;

    if (ftruncate(fd, total_size) < 0) {
        close(fd);
        shm_unlink(path.c_str());
        return nullptr;
    }

    void* ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        shm_unlink(path.c_str());
        return nullptr;
    }

    std::memset(ptr, 0, total_size);

    auto* shm = new SharedMemory();
    shm->fd_ = fd;
    shm->mmap_ptr_ = ptr;
    shm->size_ = total_size;
    shm->header_ = static_cast<SharedMemoryHeader*>(ptr);
    shm->data_ = static_cast<byte*>(ptr) + sizeof(SharedMemoryHeader);
    shm->name_ = path;

    // Initialize header
    shm->header_->magic = kShmMagic;
    shm->header_->version = kShmVersion;
    shm->header_->next_txn_id = 1;
    shm->header_->table_count = 0;
    std::memset(shm->header_->txn_slots, 0, sizeof(shm->header_->txn_slots));
    std::memset(shm->header_->tables, 0, sizeof(shm->header_->tables));

    return shm;
}

SharedMemory* SharedMemory::attach(const String& name) {
    String path = String("/minidb_shm_") + name;
    int fd = shm_open(path.c_str(), O_RDWR, 0666);
    if (fd < 0) return nullptr;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return nullptr;
    }

    u32 total_size = static_cast<u32>(st.st_size);
    void* ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    auto* shm = new SharedMemory();
    shm->fd_ = fd;
    shm->mmap_ptr_ = ptr;
    shm->size_ = total_size;
    shm->header_ = static_cast<SharedMemoryHeader*>(ptr);
    shm->data_ = static_cast<byte*>(ptr) + sizeof(SharedMemoryHeader);
    shm->name_ = path;

    // Validate
    if (shm->header_->magic != kShmMagic) {
        delete shm;
        return nullptr;
    }

    return shm;
}

void SharedMemory::destroy(const String& name) {
    String path = String("/minidb_shm_") + name;
    shm_unlink(path.c_str());
}

} // namespace minidb
