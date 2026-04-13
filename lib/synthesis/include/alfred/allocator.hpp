#pragma once

#include <memory>
#include <algorithm>
#include <utility>
#include <cstddef>

// Global static stateless allocator

namespace alfred::allocator {
    template<std::size_t StorageSize, std::size_t BlockSize, std::size_t BlockAlignment, bool Throw = false>
    struct StaticAllocatorStorage {
    private:
        static consteval std::size_t calculate_block_size() {
            const auto quot = BlockSize / BlockAlignment;  // TODO __cpp_lib_constexpr_cmath is still undefined
            const auto rem = BlockSize % BlockAlignment;

            if (rem == 0) {
                return BlockSize;
            }

            return (quot + 1) * BlockAlignment;
        }
    public:
        static constexpr auto STORAGE_SIZE = StorageSize;
        static constexpr auto BLOCK_SIZE = calculate_block_size();
        static constexpr auto BLOCK_ALIGNMENT = BlockAlignment;
        static constexpr bool THROW = Throw;

        alignas(BLOCK_ALIGNMENT) unsigned char m_base[STORAGE_SIZE * BLOCK_SIZE] {};
        bool m_blocks[STORAGE_SIZE] {};
        std::size_t m_pointer {};

        static StaticAllocatorStorage& get() {
            static StaticAllocatorStorage instance;
            return instance;
        }
    };

    template<typename T, typename Storage>
    struct StaticAllocator {
        using value_type = T;
        using size_type = std::size_t;

        static_assert(sizeof(T) <= Storage::BLOCK_SIZE);
        static_assert(alignof(T) <= Storage::BLOCK_ALIGNMENT);

        value_type* allocate(size_type n) {
            auto& storage = Storage::get();

            for (size_type i = storage.m_pointer; i < storage.STORAGE_SIZE - n + 1; i++) {
                if (try_allocate(storage, i, n)) {
                    return reinterpret_cast<value_type*>(storage.m_base + Storage::BLOCK_SIZE * i);
                }
            }

            for (size_type i {}; i < storage.m_pointer - n + 1; i++) {
                if (try_allocate(storage, i, n)) {
                    return reinterpret_cast<value_type*>(storage.m_base + Storage::BLOCK_SIZE * i);
                }
            }

            if constexpr (Storage::THROW) {
                throw std::bad_alloc();
            }

            std::unreachable();
        }

        void deallocate(value_type* p, size_type n) {
            auto& storage = Storage::get();

            const size_type block_pointer = reinterpret_cast<size_type>(p) - reinterpret_cast<size_type>(storage.m_base);
            const size_type index = block_pointer / Storage::BLOCK_SIZE;

            std::for_each(storage.m_blocks + index, storage.m_blocks + index + n, [](bool& block) { block = false; });
        }
    private:
        static bool try_allocate(Storage& storage, size_type i, size_type n) {
            if (std::all_of(storage.m_blocks + i, storage.m_blocks + i + n, [](const bool& block) { return !block; })) {
                std::for_each(storage.m_blocks + i, storage.m_blocks + i + n, [](bool& block) { block = true; });
                storage.m_pointer = i + 1;

                return true;
            }

            return false;
        }
    };

    template<typename T, typename U, typename Storage>
    bool operator==(const StaticAllocator<T, Storage>&, const StaticAllocator<U, Storage>&) { return true; }

    template<typename T, typename U, typename Storage>
    bool operator!=(const StaticAllocator<T, Storage>&, const StaticAllocator<U, Storage>&) { return false; }

    template<typename T, typename Storage>
    struct StaticAllocated {
        void* operator new(std::size_t) {
            StaticAllocator<T, Storage> alloc;
            using Alloc = std::allocator_traits<decltype(alloc)>;

            return Alloc::allocate(alloc, 1);
        }

        void operator delete(void* ptr) {
            StaticAllocator<T, Storage> alloc;
            using Alloc = std::allocator_traits<decltype(alloc)>;

            Alloc::deallocate(alloc, static_cast<T*>(ptr), 1);
        }
    };
}
