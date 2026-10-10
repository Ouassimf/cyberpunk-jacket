// Persistence as the engine sees it: a handful of small named blobs.
// Device code implements this on NVS; MemoryStorage is the in-memory stand-in
// used by the unit tests.
//
// Keys the engine uses (all at most 15 characters, the NVS limit):
//   "set"         settings: budget, dev mode                (4 bytes)
//   "last"        the scene shown when the jacket went off  (up to 117 bytes)
//   "p0" .. "p7"  preset slots                              (up to 133 bytes)
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jacket {

constexpr size_t kMaxStorageBlob = 160;  // no blob the engine writes is larger

class Storage {
public:
    virtual ~Storage() = default;

    // Copies the blob into `out` and returns its size. Returns 0 if the key
    // does not exist or the blob is larger than `cap`.
    virtual size_t read(const char* key, uint8_t* out, size_t cap) = 0;

    // Creates or replaces the blob. Returns false if it could not be stored.
    virtual bool write(const char* key, const uint8_t* data, size_t len) = 0;

    // Removes the blob. Removing a key that does not exist is not an error.
    virtual bool erase(const char* key) = 0;
};

// Volatile Storage for tests and for running without flash.
class MemoryStorage : public Storage {
public:
    size_t read(const char* key, uint8_t* out, size_t cap) override;
    bool write(const char* key, const uint8_t* data, size_t len) override;
    bool erase(const char* key) override;

    // When set, write() stores nothing and returns false, and erase() of an
    // existing key removes nothing and returns false.
    bool fail_writes = false;
    uint32_t write_count = 0;  // successful writes so far

private:
    static constexpr size_t kEntries = 16;
    static constexpr size_t kKeySize = 16;

    struct Entry {
        bool used = false;
        char key[kKeySize] = {};
        uint8_t len = 0;
        uint8_t data[kMaxStorageBlob] = {};
    };

    Entry* find(const char* key);

    Entry entries_[kEntries];
};

}  // namespace jacket
