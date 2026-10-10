#include "jacket/storage.h"

#include <string.h>

namespace jacket {

MemoryStorage::Entry* MemoryStorage::find(const char* key) {
    if (key == nullptr) return nullptr;
    for (Entry& e : entries_) {
        if (e.used && strncmp(e.key, key, kKeySize) == 0) return &e;
    }
    return nullptr;
}

size_t MemoryStorage::read(const char* key, uint8_t* out, size_t cap) {
    const Entry* e = find(key);
    if (e == nullptr || out == nullptr || e->len > cap) return 0;
    memcpy(out, e->data, e->len);
    return e->len;
}

bool MemoryStorage::write(const char* key, const uint8_t* data, size_t len) {
    if (fail_writes || key == nullptr || data == nullptr || len == 0 || len > kMaxStorageBlob) return false;
    if (strlen(key) >= kKeySize) return false;
    Entry* e = find(key);
    if (e == nullptr) {
        for (Entry& candidate : entries_) {
            if (!candidate.used) {
                e = &candidate;
                break;
            }
        }
    }
    if (e == nullptr) return false;
    e->used = true;
    strncpy(e->key, key, kKeySize - 1);
    e->key[kKeySize - 1] = '\0';
    e->len = static_cast<uint8_t>(len);
    memcpy(e->data, data, len);
    ++write_count;
    return true;
}

bool MemoryStorage::erase(const char* key) {
    Entry* e = find(key);
    if (e == nullptr) return true;
    if (fail_writes) return false;
    *e = Entry();
    return true;
}

}  // namespace jacket
