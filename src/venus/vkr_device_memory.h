/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_MEMORY_H
#define VKR_DEVICE_MEMORY_H

#include "vkr_common.h"

struct gbm_bo;

/* Shared memory imported as host memory; see vkr_shm_get_import_info. */
struct vkr_device_memory_shm {
   int handle;
   void *ptr;
   size_t size;
};

struct vkr_device_memory {
   struct vkr_object base;

   struct vkr_device *device;

   bool might_export;

   uint32_t property_flags;
   uint32_t valid_fd_types;

   /* gbm bo backing non-external mappable memory */
   struct gbm_bo *gbm_bo;

   /* udmabuf backing non-external mappable memory */
   int udmabuf_fd;

   /* shared memory backing mappable memory where nothing can be exported (Windows) */
   struct vkr_device_memory_shm shm;

   uint64_t allocation_size;
   uint32_t memory_type_index;
   bool dedicated;

   bool exported;
};
VKR_DEFINE_OBJECT_CAST(device_memory, VK_OBJECT_TYPE_DEVICE_MEMORY, VkDeviceMemory)

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx);

/* Must run after the VkDeviceMemory is freed: it unmaps imported host memory. */
void
vkr_device_memory_release(struct vkr_device_memory *mem);

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob);

#endif /* VKR_DEVICE_MEMORY_H */
