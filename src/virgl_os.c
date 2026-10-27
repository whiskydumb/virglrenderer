/*
 * Copyright 2026 The biscuit authors
 * SPDX-License-Identifier: MIT
 */

#include "virgl_os.h"

#include <stdint.h>

#include "util/macros.h"

#ifdef _WIN32

#include <windows.h>

int
virgl_os_shm_create(size_t size, UNUSED const char *debug_name)
{
   const uint64_t size64 = size;
   /* pagefile-backed and committed, so it is zero-filled like a memfd */
   HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                       (DWORD)(size64 >> 32), (DWORD)size64, NULL);
   if (!section)
      return -1;

   return HandleToLong(section);
}

void *
virgl_os_shm_map(int handle, size_t size)
{
   return MapViewOfFile(LongToHandle(handle), FILE_MAP_ALL_ACCESS, 0, 0, size);
}

void
virgl_os_shm_unmap(void *ptr, UNUSED size_t size)
{
   UnmapViewOfFile(ptr);
}

void
virgl_os_shm_prefault_write(void *ptr, size_t size)
{
   const size_t page_size = virgl_os_page_size();
   volatile char *bytes = ptr;

   /* a locked OR with 0 is a write that cannot race with concurrent writers */
   for (size_t offset = 0; offset < size; offset += page_size) {
#if defined(__GNUC__)
      __atomic_fetch_or(&bytes[offset], 0, __ATOMIC_RELAXED);
#else
      InterlockedOr8(&bytes[offset], 0);
#endif
   }
}

int
virgl_os_handle_dup(int handle)
{
   HANDLE process = GetCurrentProcess();
   HANDLE dup;
   if (!DuplicateHandle(process, LongToHandle(handle), process, &dup, 0, FALSE,
                        DUPLICATE_SAME_ACCESS))
      return -1;

   return HandleToLong(dup);
}

void
virgl_os_handle_close(int handle)
{
   if (handle >= 0)
      CloseHandle(LongToHandle(handle));
}

size_t
virgl_os_page_size(void)
{
   SYSTEM_INFO info;
   GetSystemInfo(&info);
   return info.dwPageSize;
}

#else /* _WIN32 */

#include <sys/mman.h>
#include <unistd.h>

#include "util/anon_file.h"
#include "util/os_file.h"

int
virgl_os_shm_create(size_t size, const char *debug_name)
{
   return os_create_anonymous_file(size, debug_name);
}

void *
virgl_os_shm_map(int handle, size_t size)
{
   void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
   return ptr == MAP_FAILED ? NULL : ptr;
}

void
virgl_os_shm_unmap(void *ptr, size_t size)
{
   munmap(ptr, size);
}

void
virgl_os_shm_prefault_write(UNUSED void *ptr, UNUSED size_t size)
{
}

int
virgl_os_handle_dup(int handle)
{
   return os_dupfd_cloexec(handle);
}

void
virgl_os_handle_close(int handle)
{
   if (handle >= 0)
      close(handle);
}

size_t
virgl_os_page_size(void)
{
   return (size_t)sysconf(_SC_PAGESIZE);
}

#endif /* _WIN32 */
