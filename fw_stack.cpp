#include <atomic>
#include <vector>
#include <utility>
#include <thread>
#include <iostream>
#include <unistd.h>

template <typename T, uint32_t Size>
class FlyweightPool {
    union Slot {
        T data;
        uint32_t next_free;
        Slot() : next_free(0) {}
        ~Slot() {}
    };

    std::vector<Slot> storage;
    std::atomic<uint64_t> free_head; // [32-bit Version | 32-bit Index]

public:
    FlyweightPool() : storage(Size + 1) {
        for (uint32_t i = 1; i < Size; ++i) {
            storage[i].next_free = i + 1;
        }
        storage[Size].next_free = 0;
        free_head.store(1ULL); // Version 0, Index 1
    }

    uint32_t acquire(T&& value) {
        uint64_t old_head = free_head.load(std::memory_order_acquire);
        while (true) {
            uint32_t idx = static_cast<uint32_t>(old_head);
            if (idx == 0) throw std::bad_alloc();

            uint32_t next = storage[idx].next_free;
            uint64_t new_head = ((old_head >> 32) + 1) << 32 | next;

            if (free_head.compare_exchange_weak(old_head, new_head)) {
                new (&storage[idx].data) T(std::move(value));
                return idx;
            }
        }
    }

    void release(uint32_t idx) {
        storage[idx].data.~T(); // Destroy the object
        uint64_t old_head = free_head.load(std::memory_order_relaxed);
        while (true) {
            storage[idx].next_free = static_cast<uint32_t>(old_head);
            uint64_t new_head = ((old_head >> 32) + 1) << 32 | idx;
            if (free_head.compare_exchange_weak(old_head, new_head)) break;
        }
    }

    T& operator[](uint32_t idx) { return storage[idx].data; }
};
template <typename T, uint32_t Size>
class LockFreeStack {
    struct NodeMetadata {
        std::atomic<uint64_t> next_versioned_idx{0};
    };

    FlyweightPool<T, Size> pool;
    std::vector<NodeMetadata> metadata;
    std::atomic<uint64_t> head{0}; // [32-bit Version | 32-bit Index]

public:
    LockFreeStack() : pool{},metadata(Size + 1) {}

    void push(T value) {
        // 1. Get a stable slot from the pool
        uint32_t idx = pool.acquire(std::move(value));
        uint64_t old_head = head.load(std::memory_order_relaxed);

        while (true) {
            // 2. Point this node to the current head
            metadata[idx].next_versioned_idx.store(old_head, std::memory_order_relaxed);

            // 3. Increment the HEAD version (The ABA Killer)
            uint64_t new_head = ((old_head >> 32) + 1) << 32 | idx;

            if (head.compare_exchange_weak(old_head, new_head)) break;
        }
    }

    bool pop(T& out_value) {
        uint64_t old_head = head.load(std::memory_order_acquire);
        while (true) {
            uint32_t idx = static_cast<uint32_t>(old_head);
            if (idx == 0) return false;

            // 4. Safe read! The slot is stable in the pool's vector.
            uint64_t next_val = metadata[idx].next_versioned_idx.load(std::memory_order_relaxed);

            if (head.compare_exchange_weak(old_head, next_val)) {
                // SUCCESS: Move the data out and return index to pool
                out_value = std::move(pool[idx]);
                pool.release(idx);
                return true;
            }
        }
    }
};
void producer(LockFreeStack<std::string, 1024>& stack) {
    stack.push("Data");
}
void process(const std::string & r)
{
    std::cout<<r<<"\n";
}
int main() {
    // 1. Declare the stable "Flatland"
    LockFreeStack<std::string, 1024> stack;

    // 2. Pass by reference (The "Borrow")
    std::thread t1(producer, std::ref(stack));

    // If successful, the object is moved from the Pool into 'result'.
    // The slot index is then automatically returned to the Pool's free-list.
    std::string result;
    while (!stack.pop(result)) {
        std::this_thread::yield(); // Give the producer a slice of CPU
    }
    process(result);
    t1.join();
}
