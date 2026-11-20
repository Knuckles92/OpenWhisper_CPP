// RealtimeGuard and, in builds without NDEBUG, the replacement global operator new and delete it
// counts through. The replacement applies to the whole test executable, because the linker takes
// this file's operator new before the C++ runtime's; outside a guard it only reads one
// thread-local counter.
#include "testkit/testkit.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <new>

namespace fv::testkit {
namespace {

// Plain integers: operator new reads them, so they must not allocate on first use.
thread_local std::size_t t_allocations = 0;
thread_local int t_guards = 0;

} // namespace

RealtimeGuard::RealtimeGuard() : RealtimeGuard(true) {}

RealtimeGuard::RealtimeGuard(bool fail_on_allocation)
    : start_(t_allocations), fail_(fail_on_allocation) {
    ++t_guards;
}

RealtimeGuard::~RealtimeGuard() {
    --t_guards;
    const std::size_t count = t_allocations - start_;
    if (fail_ && enforced() && count > 0) {
        FAIL_CHECK("RealtimeGuard: " << count
                                     << " heap allocation(s) on a path that must not allocate");
    }
}

bool RealtimeGuard::enforced() {
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

std::size_t RealtimeGuard::allocations() const {
    return t_allocations - start_;
}

} // namespace fv::testkit

#ifndef NDEBUG
namespace {

void note_allocation() {
    if (fv::testkit::t_guards > 0) {
        ++fv::testkit::t_allocations;
    }
}

void* allocate(std::size_t size) {
    note_allocation();
    if (size == 0) {
        size = 1;
    }
    for (;;) {
        if (void* block = std::malloc(size)) {
            return block;
        }
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) {
            throw std::bad_alloc();
        }
        handler();
    }
}

void* allocate_aligned(std::size_t size, std::align_val_t alignment) {
    note_allocation();
    const auto align = static_cast<std::size_t>(alignment);
    if (size == 0) {
        size = align;
    }
    for (;;) {
#ifdef _WIN32
        void* block = _aligned_malloc(size, align);
#else
        void* block = std::aligned_alloc(align, (size + align - 1) / align * align);
#endif
        if (block != nullptr) {
            return block;
        }
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) {
            throw std::bad_alloc();
        }
        handler();
    }
}

void release_aligned(void* block) noexcept {
#ifdef _WIN32
    _aligned_free(block);
#else
    std::free(block);
#endif
}

} // namespace

void* operator new(std::size_t size) {
    return allocate(size);
}
void* operator new[](std::size_t size) {
    return allocate(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return allocate_aligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return allocate_aligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* block) noexcept {
    std::free(block);
}
void operator delete[](void* block) noexcept {
    std::free(block);
}
void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}
void operator delete[](void* block, std::size_t) noexcept {
    std::free(block);
}
void operator delete(void* block, const std::nothrow_t&) noexcept {
    std::free(block);
}
void operator delete[](void* block, const std::nothrow_t&) noexcept {
    std::free(block);
}
void operator delete(void* block, std::align_val_t) noexcept {
    release_aligned(block);
}
void operator delete[](void* block, std::align_val_t) noexcept {
    release_aligned(block);
}
void operator delete(void* block, std::size_t, std::align_val_t) noexcept {
    release_aligned(block);
}
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept {
    release_aligned(block);
}
void operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    release_aligned(block);
}
void operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    release_aligned(block);
}
#endif
