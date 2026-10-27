/*
 * Copyright 2026 The biscuit authors
 * SPDX-License-Identifier: MIT
 */

#ifndef VIRGL_OS_H
#define VIRGL_OS_H

#include <stddef.h>

/*
 * Shared memory and OS handles, the parts of resource sharing that differ
 * between POSIX and Windows.
 *
 * A handle is stored in an int everywhere (virgl_resource::fd and friends).
 * On POSIX it is a file descriptor.  On Windows it is a kernel HANDLE: only
 * the low 32 bits of kernel handles are significant, so HandleToLong and
 * LongToHandle round-trip them through an int.  -1 means no handle on both.
 */

/* Returns a handle to a new zero-filled shared memory object, or -1. */
int
virgl_os_shm_create(size_t size, const char *debug_name);

/* Maps the whole shared memory object read/write; returns NULL on failure. */
void *
virgl_os_shm_map(int handle, size_t size);

void
virgl_os_shm_unmap(void *ptr, size_t size);

/*
 * Writes every page of a fresh mapping once, keeping its contents.  On Windows
 * a shared page that this process has not written yet reaches a guest
 * read-only, so the guest's first write to each page would exit to the
 * hypervisor.  No-op elsewhere.
 */
void
virgl_os_shm_prefault_write(void *ptr, size_t size);

/* Returns a new handle to the same object, or -1. */
int
virgl_os_handle_dup(int handle);

void
virgl_os_handle_close(int handle);

size_t
virgl_os_page_size(void);

#endif /* VIRGL_OS_H */
