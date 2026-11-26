// windows.h - a minimal stand-in for test/hd_linux: what src/assets.cpp, src/client.hpp and src/device.hpp name,
// so Assets::load builds natively on Linux. Only the functions assets.cpp calls are defined; the rest is declared
// so the headers parse.
#pragma once
#include <cstddef>
#include <cstdint>

typedef unsigned long DWORD;
typedef long LONG;
typedef std::size_t SIZE_T;

struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress;
    void* AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
};
#define MEM_COMMIT 0x1000
SIZE_T VirtualQuery(void const* address, MEMORY_BASIC_INFORMATION* info, SIZE_T size);

inline LONG InterlockedCompareExchange(volatile LONG* p, LONG x, LONG c) { return __sync_val_compare_and_swap(p, c, x); }
inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __sync_lock_test_and_set(p, v); }
inline LONG InterlockedIncrement(volatile LONG* p) { return __sync_add_and_fetch(p, 1); }
inline void Sleep(DWORD) {}
