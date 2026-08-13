// 基准专用分配器覆盖：全局 operator new/delete 全量转发到 mimalloc。
// 热路径每 tick 产生数十万小对象分配（投影深拷贝、偏移暂存、字符串），
// glibc malloc 在 16 线程并行段竞争明显；mimalloc 的线程本地堆消除该竞争。
// 只在 GW_BENCH_MIMALLOC 定义且 mimalloc 静态链接进本可执行文件时生效，
// 作用域严格限于 geoworld-m4-benchmark 进程；库代码不感知该覆盖。

#include <cstddef>
#include <new>

#ifdef GW_BENCH_MIMALLOC

#include <mimalloc.h>

namespace {

[[nodiscard]] void* mi_alloc_or_throw(std::size_t size) {
    void* ptr = mi_malloc(size == 0 ? 1 : size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

[[nodiscard]] void* mi_alloc_aligned_or_throw(std::size_t size, std::align_val_t alignment) {
    void* ptr = mi_malloc_aligned(size == 0 ? 1 : size,
                                  static_cast<std::size_t>(alignment));
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

} // namespace

void* operator new(std::size_t size) {
    return mi_alloc_or_throw(size);
}

void* operator new[](std::size_t size) {
    return mi_alloc_or_throw(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return mi_alloc_aligned_or_throw(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return mi_alloc_aligned_or_throw(size, alignment);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return mi_malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return mi_malloc(size == 0 ? 1 : size);
}

void operator delete(void* ptr) noexcept {
    mi_free(ptr);
}

void operator delete[](void* ptr) noexcept {
    mi_free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    mi_free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    mi_free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
    mi_free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
    mi_free(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    mi_free(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    mi_free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    mi_free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    mi_free(ptr);
}

#endif // GW_BENCH_MIMALLOC
