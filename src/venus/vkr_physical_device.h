/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_PHYSICAL_DEVICE_H
#define VKR_PHYSICAL_DEVICE_H

#include "vkr_common.h"

#include "venus-protocol/vn_protocol_renderer_util.h"

struct vkr_physical_device {
   struct vkr_object base;

   struct vn_physical_device_proc_table proc_table;

   VkPhysicalDeviceProperties properties;
   uint32_t api_version;

   VkExtensionProperties *extensions;
   uint32_t extension_count;

   bool KHR_external_memory_fd;
   bool EXT_external_memory_dma_buf;

   bool KHR_external_fence_fd;
   bool KHR_external_semaphore_fd;

   /* backs mappable memory with shared memory where it cannot be exported (Windows) */
   bool EXT_external_memory_host;
   VkDeviceSize min_imported_host_pointer_alignment;

   /* The guest only reaches sync_fd semaphores and fences through the venus
    * MESA commands, so hosts without VK_KHR_external_{semaphore,fence}_fd
    * (Windows) still advertise them and vkr_queue.c emulates the commands.
    * Common WSI in the guest needs them for VK_KHR_swapchain.
    */
   bool emulate_sync_fd;

   /* Hosts without dma-buf but with VK_KHR_external_memory_win32 (Windows)
    * advertise dma-buf to the guest and use OPAQUE_WIN32 underneath: guest
    * dma-bufs are virtio-gpu blobs, which the host only shares between its
    * own contexts (vkr and vrend), so the host handle type is free to pick.
    */
   bool KHR_external_memory_win32;
   bool emulate_dma_buf;

   VkPhysicalDeviceMemoryProperties memory_properties;
   VkPhysicalDeviceIDProperties id_properties;
   bool is_dma_buf_fd_export_supported;
   bool is_opaque_fd_export_supported;
   void *gbm_device;
   int udmabuf_dev_fd;

   VkQueueFamilyProperties *queue_family_properties;
   uint32_t queue_family_property_count;

   struct list_head devices;
};
VKR_DEFINE_OBJECT_CAST(physical_device, VK_OBJECT_TYPE_PHYSICAL_DEVICE, VkPhysicalDevice)

void
vkr_context_init_physical_device_dispatch(struct vkr_context *ctx);

void
vkr_physical_device_destroy(struct vkr_context *ctx,
                            struct vkr_physical_device *physical_dev);

/* Whether vkr advertises the device extension without host support. */
bool
vkr_physical_device_is_emulated_extension(const struct vkr_physical_device *physical_dev,
                                          const char *name);

/* Rewrites guest dma-buf (and opaque fd) handle types into the host handle
 * type when dma-buf is emulated; returns whether anything changed.
 */
bool
vkr_physical_device_translate_handle_types(const struct vkr_physical_device *physical_dev,
                                           VkExternalMemoryHandleTypeFlags *handle_types);

/* The reverse, for properties reported back to the guest. */
void
vkr_physical_device_untranslate_memory_properties(
   const struct vkr_physical_device *physical_dev, VkExternalMemoryProperties *props);

#endif /* VKR_PHYSICAL_DEVICE_H */
