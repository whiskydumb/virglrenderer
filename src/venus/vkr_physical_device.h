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

#endif /* VKR_PHYSICAL_DEVICE_H */
