// a minimal stand-in for <windows.h>: enough to compile PadHook.h on 32-bit Linux and run its pure logic,
// its dispatcher, and route 1's patch on code in memory. The few calls those make are defined in the tests
// that make them (pad_wrapper.cpp); the rest are only declared.
#pragma once
#include <stdint.h>
#include <time.h>
#define __stdcall __attribute__((stdcall))
#define WINAPI __stdcall
typedef unsigned long DWORD; typedef unsigned short WORD; typedef unsigned char BYTE; typedef short SHORT;
typedef long LONG; typedef long long LONGLONG; typedef void* HMODULE; typedef void* HANDLE; typedef int BOOL;
typedef const char* LPCSTR; typedef void* LPVOID; typedef void const* LPCVOID; typedef uintptr_t SIZE_T;
typedef union { struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
struct MEMORY_BASIC_INFORMATION { void* BaseAddress; void* AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize; DWORD State, Protect, Type; };
enum { PAGE_NOACCESS=1, PAGE_READONLY=2, PAGE_READWRITE=4, PAGE_WRITECOPY=8, PAGE_EXECUTE=0x10, PAGE_EXECUTE_READ=0x20,
  PAGE_EXECUTE_READWRITE=0x40, PAGE_EXECUTE_WRITECOPY=0x80, PAGE_GUARD=0x100, MEM_COMMIT=0x1000, MEM_RESERVE=0x2000,
  MEM_RELEASE=0x8000, MEM_IMAGE=0x1000000, ERROR_SUCCESS=0, ERROR_DEVICE_NOT_CONNECTED=1167, ERROR_ALREADY_EXISTS=183,
  GET_MODULE_HANDLE_EX_FLAG_PIN=1, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT=2, GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS=4,
  FILE_MAP_ALL_ACCESS=0xF001F };
// the PE headers PadHook.h reads to find the client's sections (never read on Linux)
enum { IMAGE_DOS_SIGNATURE=0x5A4D, IMAGE_NT_SIGNATURE=0x00004550, IMAGE_SCN_MEM_EXECUTE=0x20000000, IMAGE_SCN_MEM_WRITE=0x80000000u };
struct IMAGE_DOS_HEADER { WORD e_magic; WORD unused[29]; LONG e_lfanew; };
struct IMAGE_FILE_HEADER { WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; WORD SizeOfOptionalHeader, Characteristics; };
struct IMAGE_NT_HEADERS32 { DWORD Signature; IMAGE_FILE_HEADER FileHeader; BYTE OptionalHeader[224]; };
struct IMAGE_SECTION_HEADER { BYTE Name[8]; union { DWORD PhysicalAddress, VirtualSize; } Misc; DWORD VirtualAddress, SizeOfRawData,
  PointerToRawData, PointerToRelocations, PointerToLinenumbers; WORD NumberOfRelocations, NumberOfLinenumbers; DWORD Characteristics; };
#define IMAGE_FIRST_SECTION(nt) ((IMAGE_SECTION_HEADER const*)((uintptr_t)(nt) + 24 + (nt)->FileHeader.SizeOfOptionalHeader))
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
inline LONG InterlockedIncrement(volatile LONG* p) { return __sync_add_and_fetch(p, 1); }
inline LONG InterlockedDecrement(volatile LONG* p) { return __sync_sub_and_fetch(p, 1); }
inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __sync_lock_test_and_set(p, v); }
inline LONG InterlockedCompareExchange(volatile LONG* p, LONG x, LONG c) { return __sync_val_compare_and_swap(p, c, x); }
inline LONGLONG InterlockedCompareExchange64(volatile LONGLONG* p, LONGLONG x, LONGLONG c) { return __sync_val_compare_and_swap(p, c, x); }
inline void MemoryBarrier() { __sync_synchronize(); }
inline void YieldProcessor() {}
inline DWORD GetCurrentThreadId() { return 1; }
extern DWORD g_fake_tick;
inline DWORD GetTickCount() { return g_fake_tick; }
SIZE_T VirtualQuery(LPCVOID, MEMORY_BASIC_INFORMATION*, SIZE_T); BOOL GetModuleHandleExA(DWORD, LPCSTR, HMODULE*);
HMODULE GetModuleHandleA(LPCSTR);
void* GetProcAddress(HMODULE, LPCSTR); void* VirtualAlloc(void*, SIZE_T, DWORD, DWORD); BOOL VirtualFree(void*, SIZE_T, DWORD);
BOOL VirtualProtect(void*, SIZE_T, DWORD, DWORD*); BOOL FlushInstructionCache(HANDLE, LPCVOID, SIZE_T); HANDLE GetCurrentProcess();
HANDLE CreateFileMappingA(HANDLE, void*, DWORD, DWORD, DWORD, LPCSTR); void* MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
BOOL UnmapViewOfFile(LPCVOID); BOOL CloseHandle(HANDLE); DWORD GetLastError(); DWORD GetCurrentProcessId();
BOOL QueryPerformanceCounter(LARGE_INTEGER*); void Sleep(DWORD);
