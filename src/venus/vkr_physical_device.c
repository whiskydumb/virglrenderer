/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_physical_device.h"

#include "venus-protocol/vn_protocol_renderer_device.h"

#include "vkr_context.h"
#include "vkr_device.h"
#include "vkr_instance.h"

#ifdef HAVE_LINUX_UDMABUF_H
#include <fcntl.h>

static int udmabuf_dev_fd = -1;

static void
vkr_udmabuf_init_once(void)
{
   udmabuf_dev_fd = open("/dev/udmabuf", O_RDWR);
   if (udmabuf_dev_fd < 0) {
      vkr_log("failed to open udmabuf device: %s", strerror(errno));
   }
}

static inline int
vkr_physical_device_get_udmabuf_dev_fd(void)
{
   static once_flag udmabuf_once_flag = ONCE_FLAG_INIT;
   call_once(&udmabuf_once_flag, vkr_udmabuf_init_once);
   return udmabuf_dev_fd;
}

#else /* HAVE_LINUX_UDMABUF_H */

static inline int
vkr_physical_device_get_udmabuf_dev_fd(void)
{
   return -1;
}

#endif /* HAVE_LINUX_UDMABUF_H */

#ifdef ENABLE_GBM_ALLOCATION
#include <gbm.h>
#ifdef MINIGBM
#include <minigbm/minigbm_helpers.h>
#else
#define minigbm_create_default_device(out_fd) NULL
#endif /* MINIGBM */

/* TODO remove minigbm allocation fallback after requiring exporting from raw mappable
 * device memory via a new Vulkan extension
 */
static struct gbm_device *vkr_gbm_dev;

static void
vkr_gbm_device_init_once(void)
{
   UNUSED int gbm_fd;
   vkr_gbm_dev = minigbm_create_default_device(&gbm_fd);
   if (!vkr_gbm_dev) {
      vkr_log("minigbm_create_default_device failed");
      exit(-1);
   }
}

static inline void *
vkr_physical_device_get_gbm_device(void)
{
   static once_flag gbm_once_flag = ONCE_FLAG_INIT;
   call_once(&gbm_once_flag, vkr_gbm_device_init_once);
   return (void *)vkr_gbm_dev;
}

#else

static inline void *
vkr_physical_device_get_gbm_device(void)
{
   return NULL;
}

#endif /* ENABLE_GBM_ALLOCATION */

void
vkr_physical_device_destroy(struct vkr_context *ctx,
                            struct vkr_physical_device *physical_dev)
{
   list_for_each_entry_safe (struct vkr_device, dev, &physical_dev->devices, base.track_head)
      vkr_device_destroy(ctx, dev, false);

   free(physical_dev->extensions);
   free(physical_dev->queue_family_properties);

   vkr_context_remove_object(ctx, &physical_dev->base);
}

static VkResult
vkr_instance_enumerate_physical_devices(struct vkr_instance *instance)
{
   if (instance->physical_device_count)
      return VK_SUCCESS;

   struct vn_instance_proc_table *vk = &instance->proc_table;
   uint32_t count;
   VkResult result =
      vk->EnumeratePhysicalDevices(instance->base.handle.instance, &count, NULL);
   if (result != VK_SUCCESS)
      return result;

   VkPhysicalDevice *handles = calloc(count, sizeof(*handles));
   struct vkr_physical_device **physical_devs = calloc(count, sizeof(*physical_devs));
   if (!handles || !physical_devs) {
      free(physical_devs);
      free(handles);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   result = vk->EnumeratePhysicalDevices(instance->base.handle.instance, &count, handles);
   if (result != VK_SUCCESS) {
      free(physical_devs);
      free(handles);
      return result;
   }

   instance->physical_device_count = count;
   instance->physical_device_handles = handles;
   instance->physical_devices = physical_devs;

   return VK_SUCCESS;
}

static struct vkr_physical_device *
vkr_instance_lookup_physical_device(struct vkr_instance *instance,
                                    VkPhysicalDevice handle)
{
   for (uint32_t i = 0; i < instance->physical_device_count; i++) {
      /* VkPhysicalDevice handles are fine to contain duplicates. Client side always
       * returns unique handles upon the first enumeration call (either physical deivces
       * or groups). The other enumeration call later can return duplicate handles based
       * on this lookup, which is fine since still matching the Vulkan driver.
       */
      if (instance->physical_device_handles[i] == handle)
         return instance->physical_devices[i];
   }
   return NULL;
}

static void
vkr_physical_device_init_id_properties(struct vkr_physical_device *physical_dev)
{
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   VkPhysicalDevice handle = physical_dev->base.handle.physical_device;
   physical_dev->id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
   VkPhysicalDeviceProperties2 props2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &physical_dev->id_properties
   };
   vk->GetPhysicalDeviceProperties2(handle, &props2);
}

static void
vkr_physical_device_init_memory_properties(struct vkr_physical_device *physical_dev)
{
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   VkPhysicalDevice handle = physical_dev->base.handle.physical_device;
   vk->GetPhysicalDeviceMemoryProperties(handle, &physical_dev->memory_properties);

   /* XXX When a VkMemoryType has VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, we
    * assume any VkDeviceMemory with the memory type can be made external and
    * be exportable.  That is incorrect but is what we have to live with with
    * the existing external memory extensions.
    *
    * The main reason is that the external memory extensions require us to use
    * vkGetPhysicalDeviceExternalBufferProperties or
    * vkGetPhysicalDeviceImageFormatProperties2 to determine if we can
    * allocate an exportable external VkDeviceMemory.  But we normally do not
    * have the info to make the queries during vkAllocateMemory.
    *
    * We only have VkMemoryAllocateInfo during vkAllocateMemory.  The only
    * useful info in the struct is the memory type.  What we need is thus an
    * extension that tells us that, given a memory type, if all VkDeviceMemory
    * with the memory type is exportable.  If we had the extension, we could
    * filter out VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT here if a memory type is
    * not always exportable.
    */

   /* XXX is_dma_buf_fd_export_supported and is_opaque_fd_export_supported
    * needs to be filled with a new extension which supports query fd export
    * against the raw memory types. Currently, we workaround by checking
    * external buffer properties before force-enabling either dma_buf or opaque
    * fd path of device memory allocation.
    */
   physical_dev->is_dma_buf_fd_export_supported = false;
   physical_dev->is_opaque_fd_export_supported = false;

   VkPhysicalDeviceExternalBufferInfo info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
   };
   VkExternalBufferProperties props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
   };

   if (physical_dev->EXT_external_memory_dma_buf) {
      info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      vk->GetPhysicalDeviceExternalBufferProperties(handle, &info, &props);
      physical_dev->is_dma_buf_fd_export_supported =
         (props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
         (props.externalMemoryProperties.exportFromImportedHandleTypes &
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   }

   if (physical_dev->KHR_external_memory_fd) {
      info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
      vk->GetPhysicalDeviceExternalBufferProperties(handle, &info, &props);
      physical_dev->is_opaque_fd_export_supported =
         (props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
         (props.externalMemoryProperties.exportFromImportedHandleTypes &
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
   }

   /* fallback to gbm allocation with dma-buf import */
   if (!physical_dev->is_dma_buf_fd_export_supported &&
       !physical_dev->is_opaque_fd_export_supported &&
       physical_dev->EXT_external_memory_dma_buf)
      physical_dev->gbm_device = vkr_physical_device_get_gbm_device();

   physical_dev->udmabuf_dev_fd = -1;
   if (VKR_DEBUG(UDMABUF)) {
      if (physical_dev->EXT_external_memory_dma_buf)
         physical_dev->udmabuf_dev_fd = vkr_physical_device_get_udmabuf_dev_fd();
      else
         vkr_log("missing VK_EXT_external_memory_dma_buf for udmabuf import!");
   }
}

/* advertised without host support when emulate_sync_fd is set */
static const char *const vkr_emulated_sync_fd_extensions[] = {
   "VK_KHR_external_semaphore_fd",
   "VK_KHR_external_fence_fd",
};

/* advertised without host support when emulate_dma_buf is set */
static const char *const vkr_emulated_dma_buf_extensions[] = {
   "VK_KHR_external_memory_fd",
   "VK_EXT_external_memory_dma_buf",
};

/* advertised without host support when emulate_drm_format_modifier is set */
static const char *const vkr_emulated_drm_format_modifier_extensions[] = {
   "VK_EXT_image_drm_format_modifier",
};

static uint32_t
vkr_append_emulated_extensions(VkExtensionProperties *props,
                               const char *const *names,
                               uint32_t name_count)
{
   for (uint32_t i = 0; i < name_count; i++) {
      snprintf(props[i].extensionName, sizeof(props[i].extensionName), "%s", names[i]);
      props[i].specVersion = vkr_extension_get_spec_version(names[i]);
   }
   return name_count;
}

static bool
vkr_name_in_list(const char *name, const char *const *names, uint32_t name_count)
{
   for (uint32_t i = 0; i < name_count; i++) {
      if (!strcmp(name, names[i]))
         return true;
   }
   return false;
}

static void
vkr_physical_device_init_extensions(struct vkr_physical_device *physical_dev)
{
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   VkPhysicalDevice handle = physical_dev->base.handle.physical_device;

   VkExtensionProperties *exts;
   uint32_t count;
   VkResult result = vk->EnumerateDeviceExtensionProperties(handle, NULL, &count, NULL);
   if (result != VK_SUCCESS)
      return;

   exts = malloc(sizeof(*exts) * count);
   if (!exts)
      return;

   result = vk->EnumerateDeviceExtensionProperties(handle, NULL, &count, exts);
   if (result != VK_SUCCESS) {
      free(exts);
      return;
   }

   uint32_t advertised_count = 0;
   for (uint32_t i = 0; i < count; i++) {
      VkExtensionProperties *props = &exts[i];

      if (!strcmp(props->extensionName, "VK_KHR_external_memory_fd"))
         physical_dev->KHR_external_memory_fd = true;
      else if (!strcmp(props->extensionName, "VK_EXT_external_memory_dma_buf"))
         physical_dev->EXT_external_memory_dma_buf = true;
      else if (!strcmp(props->extensionName, "VK_KHR_external_fence_fd"))
         physical_dev->KHR_external_fence_fd = true;
      else if (!strcmp(props->extensionName, "VK_KHR_external_semaphore_fd"))
         physical_dev->KHR_external_semaphore_fd = true;
      else if (!strcmp(props->extensionName, "VK_KHR_external_memory_win32"))
         physical_dev->KHR_external_memory_win32 = true;
      else if (!strcmp(props->extensionName, "VK_EXT_external_memory_host"))
         physical_dev->EXT_external_memory_host = true;
      else if (!strcmp(props->extensionName, "VK_EXT_image_drm_format_modifier"))
         physical_dev->EXT_image_drm_format_modifier = true;

      const uint32_t spec_ver = vkr_extension_get_spec_version(props->extensionName);
      if (spec_ver) {
         if (props->specVersion > spec_ver)
            props->specVersion = spec_ver;
         exts[advertised_count++] = exts[i];
      }
   }

   if (physical_dev->EXT_external_memory_host) {
      VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
      };
      VkPhysicalDeviceProperties2 props2 = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = &host_props,
      };
      vk->GetPhysicalDeviceProperties2(handle, &props2);
      physical_dev->min_imported_host_pointer_alignment =
         host_props.minImportedHostPointerAlignment;
   }

   if (physical_dev->KHR_external_fence_fd) {
      const VkPhysicalDeviceExternalFenceInfo fence_info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      VkExternalFenceProperties fence_props = {
         .sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES,
      };
      vk->GetPhysicalDeviceExternalFenceProperties(handle, &fence_info, &fence_props);

      if (!(fence_props.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT))
         physical_dev->KHR_external_fence_fd = false;
   }

#ifdef _WIN32
   physical_dev->emulate_sync_fd =
      !physical_dev->KHR_external_semaphore_fd && !physical_dev->KHR_external_fence_fd;
   physical_dev->emulate_dma_buf = physical_dev->KHR_external_memory_win32 &&
                                   !physical_dev->EXT_external_memory_dma_buf;
   physical_dev->emulate_drm_format_modifier =
      physical_dev->emulate_dma_buf && !physical_dev->EXT_image_drm_format_modifier;
#endif
   const uint32_t sync_fd_count =
      physical_dev->emulate_sync_fd ? ARRAY_SIZE(vkr_emulated_sync_fd_extensions) : 0;
   const uint32_t dma_buf_count =
      physical_dev->emulate_dma_buf ? ARRAY_SIZE(vkr_emulated_dma_buf_extensions) : 0;
   const uint32_t drm_format_modifier_count =
      physical_dev->emulate_drm_format_modifier
         ? ARRAY_SIZE(vkr_emulated_drm_format_modifier_extensions)
         : 0;

   VkExtensionProperties *advertised =
      realloc(exts, sizeof(*exts) * (advertised_count + sync_fd_count + dma_buf_count +
                                     drm_format_modifier_count));
   if (!advertised) {
      free(exts);
      return;
   }

   advertised_count += vkr_append_emulated_extensions(
      &advertised[advertised_count], vkr_emulated_sync_fd_extensions, sync_fd_count);
   advertised_count += vkr_append_emulated_extensions(
      &advertised[advertised_count], vkr_emulated_dma_buf_extensions, dma_buf_count);
   advertised_count += vkr_append_emulated_extensions(
      &advertised[advertised_count], vkr_emulated_drm_format_modifier_extensions,
      drm_format_modifier_count);

   physical_dev->extensions = advertised;
   physical_dev->extension_count = advertised_count;
}

bool
vkr_physical_device_is_emulated_extension(const struct vkr_physical_device *physical_dev,
                                          const char *name)
{
   return (physical_dev->emulate_sync_fd &&
           vkr_name_in_list(name, vkr_emulated_sync_fd_extensions,
                            ARRAY_SIZE(vkr_emulated_sync_fd_extensions))) ||
          (physical_dev->emulate_dma_buf &&
           vkr_name_in_list(name, vkr_emulated_dma_buf_extensions,
                            ARRAY_SIZE(vkr_emulated_dma_buf_extensions))) ||
          (physical_dev->emulate_drm_format_modifier &&
           vkr_name_in_list(name, vkr_emulated_drm_format_modifier_extensions,
                            ARRAY_SIZE(vkr_emulated_drm_format_modifier_extensions)));
}

static const VkExternalMemoryHandleTypeFlags VKR_GUEST_FD_HANDLE_TYPES =
   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
   VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

bool
vkr_physical_device_translate_handle_types(const struct vkr_physical_device *physical_dev,
                                           VkExternalMemoryHandleTypeFlags *handle_types)
{
   if (!physical_dev->emulate_dma_buf || !(*handle_types & VKR_GUEST_FD_HANDLE_TYPES))
      return false;

   *handle_types = (*handle_types & ~VKR_GUEST_FD_HANDLE_TYPES) |
                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
   return true;
}

void
vkr_physical_device_untranslate_memory_properties(
   const struct vkr_physical_device *physical_dev, VkExternalMemoryProperties *props)
{
   if (!physical_dev->emulate_dma_buf)
      return;

   const VkExternalMemoryHandleTypeFlags host =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
   const VkExternalMemoryHandleTypeFlags guest =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   props->compatibleHandleTypes = (props->compatibleHandleTypes & host) ? guest : 0;
   props->exportFromImportedHandleTypes =
      (props->exportFromImportedHandleTypes & host) ? guest : 0;
}

static void
vkr_physical_device_init_properties(struct vkr_physical_device *physical_dev)
{
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   VkPhysicalDevice handle = physical_dev->base.handle.physical_device;
   vk->GetPhysicalDeviceProperties(handle, &physical_dev->properties);

   VkPhysicalDeviceProperties *props = &physical_dev->properties;
   props->apiVersion = vkr_api_version_cap_minor(props->apiVersion, VKR_MAX_API_VERSION);
}

static inline void
vkr_physical_device_init_proc_table(struct vkr_physical_device *physical_dev,
                                    struct vkr_instance *instance)
{
   vn_util_init_physical_device_proc_table(
      instance->base.handle.instance, instance->get_proc_addr, &physical_dev->proc_table);
}

static void
vkr_physical_device_init_queue_family_properties(struct vkr_physical_device *physical_dev)
{
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   VkPhysicalDevice handle = physical_dev->base.handle.physical_device;

   VkQueueFamilyProperties *props;
   uint32_t count;
   vk->GetPhysicalDeviceQueueFamilyProperties(handle, &count, NULL);

   props = malloc(sizeof(*props) * count);
   if (!props)
      return;

   vk->GetPhysicalDeviceQueueFamilyProperties(handle, &count, props);

   physical_dev->queue_family_property_count = count;
   physical_dev->queue_family_properties = props;
}

static void
vkr_dispatch_vkEnumeratePhysicalDevices(struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkEnumeratePhysicalDevices *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_instance *instance = vkr_instance_from_handle(args->instance);
   if (instance != ctx->instance) {
      vkr_context_set_fatal(ctx);
      return;
   }

   args->ret = vkr_instance_enumerate_physical_devices(instance);
   if (args->ret != VK_SUCCESS)
      return;

   uint32_t count = instance->physical_device_count;
   if (!args->pPhysicalDevices) {
      *args->pPhysicalDeviceCount = count;
      args->ret = VK_SUCCESS;
      return;
   }

   if (count > *args->pPhysicalDeviceCount) {
      count = *args->pPhysicalDeviceCount;
      args->ret = VK_INCOMPLETE;
   } else {
      *args->pPhysicalDeviceCount = count;
      args->ret = VK_SUCCESS;
   }

   uint32_t i;
   for (i = 0; i < count; i++) {
      struct vkr_physical_device *physical_dev = instance->physical_devices[i];
      const vkr_object_id id = vkr_cs_handle_load_id(
         (const void **)&args->pPhysicalDevices[i], VK_OBJECT_TYPE_PHYSICAL_DEVICE);

      if (physical_dev) {
         if (physical_dev->base.id != id) {
            vkr_context_set_fatal(ctx);
            break;
         }
         continue;
      }

      if (!vkr_context_validate_object_id(ctx, id))
         break;

      physical_dev =
         vkr_object_alloc(sizeof(*physical_dev), VK_OBJECT_TYPE_PHYSICAL_DEVICE, id);
      if (!physical_dev) {
         args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
         break;
      }

      physical_dev->base.handle.physical_device = instance->physical_device_handles[i];

      vkr_physical_device_init_proc_table(physical_dev, instance);
      vkr_physical_device_init_properties(physical_dev);
      physical_dev->api_version =
         MIN2(physical_dev->properties.apiVersion, instance->api_version);
      vkr_physical_device_init_extensions(physical_dev);
      vkr_physical_device_init_memory_properties(physical_dev);
      vkr_physical_device_init_id_properties(physical_dev);
      vkr_physical_device_init_queue_family_properties(physical_dev);

      list_inithead(&physical_dev->devices);

      instance->physical_devices[i] = physical_dev;

      vkr_context_add_object(ctx, &physical_dev->base);
   }
   /* remove all physical devices on errors */
   if (i < count) {
      for (i = 0; i < instance->physical_device_count; i++) {
         struct vkr_physical_device *physical_dev = instance->physical_devices[i];
         if (!physical_dev)
            break;
         free(physical_dev->extensions);
         free(physical_dev->queue_family_properties);
         vkr_context_remove_object(ctx, &physical_dev->base);
         instance->physical_devices[i] = NULL;
      }
   }
}

static void
vkr_dispatch_vkEnumeratePhysicalDeviceGroups(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkEnumeratePhysicalDeviceGroups *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_instance *instance = vkr_instance_from_handle(args->instance);
   if (instance != ctx->instance) {
      vkr_context_set_fatal(ctx);
      return;
   }

   args->ret = vkr_instance_enumerate_physical_devices(instance);
   if (args->ret != VK_SUCCESS)
      return;

   VkPhysicalDeviceGroupProperties *orig_props = args->pPhysicalDeviceGroupProperties;
   if (orig_props) {
      args->pPhysicalDeviceGroupProperties =
         calloc(*args->pPhysicalDeviceGroupCount, sizeof(*orig_props));
      if (!args->pPhysicalDeviceGroupProperties) {
         args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
         return;
      }

      for (uint32_t i = 0; i < *args->pPhysicalDeviceGroupCount; i++) {
         args->pPhysicalDeviceGroupProperties[i].sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
      }
   }

   struct vn_instance_proc_table *vk = &instance->proc_table;
   vn_replace_vkEnumeratePhysicalDeviceGroups_args_handle(args);
   args->ret =
      vk->EnumeratePhysicalDeviceGroups(args->instance, args->pPhysicalDeviceGroupCount,
                                        args->pPhysicalDeviceGroupProperties);
   if (args->ret != VK_SUCCESS)
      return;

   if (!orig_props)
      return;

   /* TODO avoid requiring venus driver to call vkEnumeratePhysicalDevices first */
   /* replace VkPhysicalDevice handles by object ids */
   for (uint32_t i = 0; i < *args->pPhysicalDeviceGroupCount; i++) {
      const VkPhysicalDeviceGroupProperties *props =
         &args->pPhysicalDeviceGroupProperties[i];
      VkPhysicalDeviceGroupProperties *out = &orig_props[i];

      out->physicalDeviceCount = props->physicalDeviceCount;
      out->subsetAllocation = props->subsetAllocation;
      for (uint32_t j = 0; j < props->physicalDeviceCount; j++) {
         const struct vkr_physical_device *physical_dev =
            vkr_instance_lookup_physical_device(instance, props->physicalDevices[j]);
         if (!physical_dev) {
            vkr_log("venus driver is required to call vkEnumeratePhysicalDevices first");
            args->ret = VK_ERROR_INITIALIZATION_FAILED;
            if (orig_props)
               free(args->pPhysicalDeviceGroupProperties);
            return;
         }

         vkr_cs_handle_store_id((void **)&out->physicalDevices[j], physical_dev->base.id,
                                VK_OBJECT_TYPE_PHYSICAL_DEVICE);
      }
   }

   free(args->pPhysicalDeviceGroupProperties);
   args->pPhysicalDeviceGroupProperties = orig_props;
}

static void
vkr_dispatch_vkEnumerateDeviceExtensionProperties(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkEnumerateDeviceExtensionProperties *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   if (args->pLayerName) {
      vkr_context_set_fatal(ctx);
      return;
   }

   if (!args->pProperties) {
      *args->pPropertyCount = physical_dev->extension_count;
      args->ret = VK_SUCCESS;
      return;
   }

   uint32_t count = physical_dev->extension_count;
   if (count > *args->pPropertyCount) {
      count = *args->pPropertyCount;
      args->ret = VK_INCOMPLETE;
   } else {
      *args->pPropertyCount = count;
      args->ret = VK_SUCCESS;
   }

   memcpy(args->pProperties, physical_dev->extensions,
          sizeof(*args->pProperties) * count);
}

static void
vkr_dispatch_vkGetPhysicalDeviceFeatures(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceFeatures *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceFeatures_args_handle(args);
   vk->GetPhysicalDeviceFeatures(args->physicalDevice, args->pFeatures);
}

static void
vkr_dispatch_vkGetPhysicalDeviceProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceProperties *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);

   *args->pProperties = physical_dev->properties;
}

static void
vkr_dispatch_vkGetPhysicalDeviceQueueFamilyProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceQueueFamilyProperties *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceQueueFamilyProperties_args_handle(args);
   vk->GetPhysicalDeviceQueueFamilyProperties(args->physicalDevice,
                                              args->pQueueFamilyPropertyCount,
                                              args->pQueueFamilyProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceMemoryProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceMemoryProperties *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   *args->pMemoryProperties = physical_dev->memory_properties;
}

static void
vkr_dispatch_vkGetPhysicalDeviceFormatProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceFormatProperties *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceFormatProperties_args_handle(args);
   vk->GetPhysicalDeviceFormatProperties(args->physicalDevice, args->format,
                                         args->pFormatProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceImageFormatProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceImageFormatProperties *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceImageFormatProperties_args_handle(args);
   args->ret = vk->GetPhysicalDeviceImageFormatProperties(
      args->physicalDevice, args->format, args->type, args->tiling, args->usage,
      args->flags, args->pImageFormatProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceSparseImageFormatProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceSparseImageFormatProperties *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceSparseImageFormatProperties_args_handle(args);
   vk->GetPhysicalDeviceSparseImageFormatProperties(
      args->physicalDevice, args->format, args->type, args->samples, args->usage,
      args->tiling, args->pPropertyCount, args->pProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceFeatures2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceFeatures2 *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceFeatures2_args_handle(args);
   vk->GetPhysicalDeviceFeatures2(args->physicalDevice, args->pFeatures);
}

static void
vkr_dispatch_vkGetPhysicalDeviceProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceProperties2 *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceProperties2_args_handle(args);
   vk->GetPhysicalDeviceProperties2(args->physicalDevice, args->pProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceQueueFamilyProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceQueueFamilyProperties2 *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceQueueFamilyProperties2_args_handle(args);
   vk->GetPhysicalDeviceQueueFamilyProperties2(args->physicalDevice,
                                               args->pQueueFamilyPropertyCount,
                                               args->pQueueFamilyProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceMemoryProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceMemoryProperties2 *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   if (args->pMemoryProperties->pNext == NULL) {
      /* The client is querying only VkPhysicalDeviceMemoryProperties, which is
       * invariant. Return the cached properties.
       */
      args->pMemoryProperties->memoryProperties = physical_dev->memory_properties;
   } else {
      vn_replace_vkGetPhysicalDeviceMemoryProperties2_args_handle(args);
      vk->GetPhysicalDeviceMemoryProperties2(args->physicalDevice,
                                             args->pMemoryProperties);
   }
}

/* Emulated DRM_FORMAT_MOD_LINEAR covers single-plane formats only: multi-planar
 * linear images would need per-plane layouts, which guest WSI never uses.
 */
static bool
vkr_format_takes_linear_modifier(VkFormat format)
{
   const bool ycbcr = (format >= VK_FORMAT_G8B8G8R8_422_UNORM &&
                       format <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM) ||
                      (format >= VK_FORMAT_G8_B8R8_2PLANE_444_UNORM &&
                       format <= VK_FORMAT_G16_B16R16_2PLANE_444_UNORM);
   return !ycbcr;
}

/* The entry count of an emulated modifier list: LINEAR alone, if it has
 * features.  query is set when the guest only asks for the count.
 */
static uint32_t
vkr_linear_modifier_count(uint32_t capacity, bool query, bool has_features)
{
   const uint32_t count = has_features ? 1 : 0;
   return query ? count : MIN2(count, capacity);
}

/* An output struct hidden from the driver for one call.  The guest decodes
 * replies in chain order, so it goes back where it was; unhide in reverse.
 */
struct vkr_hidden_struct {
   VkBaseOutStructure *prev;
   VkBaseOutStructure *hidden;
};

static void *
vkr_hide_struct(struct vkr_hidden_struct *state, void *chain, VkStructureType type)
{
   state->prev = vkr_find_prev_struct(chain, type);
   state->hidden = state->prev ? state->prev->pNext : NULL;
   if (state->hidden)
      state->prev->pNext = state->hidden->pNext;
   return state->hidden;
}

static void
vkr_unhide_struct(const struct vkr_hidden_struct *state)
{
   if (state->hidden)
      state->prev->pNext = state->hidden;
}

static void
vkr_dispatch_vkGetPhysicalDeviceFormatProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceFormatProperties2 *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;
   VkFormatProperties2 *props = args->pFormatProperties;

   /* the driver knows no modifier lists; they are filled from linear tiling */
   struct vkr_hidden_struct hidden_list = { 0 };
   struct vkr_hidden_struct hidden_list2 = { 0 };
   VkDrmFormatModifierPropertiesListEXT *list = NULL;
   VkDrmFormatModifierPropertiesList2EXT *list2 = NULL;
   if (physical_dev->emulate_drm_format_modifier) {
      list = vkr_hide_struct(&hidden_list, props,
                             VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT);
      list2 =
         vkr_hide_struct(&hidden_list2, props,
                         VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT);
   }

   /* list2 takes its features from VkFormatProperties3 */
   VkFormatProperties3 local_props3 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
   VkFormatProperties3 *props3 =
      vkr_find_struct(props->pNext, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3);
   if (list2 && !props3) {
      local_props3.pNext = props->pNext;
      props->pNext = &local_props3;
      props3 = &local_props3;
   }

   vn_replace_vkGetPhysicalDeviceFormatProperties2_args_handle(args);
   vk->GetPhysicalDeviceFormatProperties2(args->physicalDevice, args->format, props);

   if (props3 == &local_props3)
      props->pNext = local_props3.pNext;

   const bool linear = vkr_format_takes_linear_modifier(args->format);
   if (list) {
      const VkFormatFeatureFlags features =
         linear ? props->formatProperties.linearTilingFeatures : 0;
      list->drmFormatModifierCount = vkr_linear_modifier_count(
         list->drmFormatModifierCount, !list->pDrmFormatModifierProperties, features);
      if (list->pDrmFormatModifierProperties && list->drmFormatModifierCount) {
         list->pDrmFormatModifierProperties[0] = (VkDrmFormatModifierPropertiesEXT){
            .drmFormatModifier = VKR_DRM_FORMAT_MOD_LINEAR,
            .drmFormatModifierPlaneCount = 1,
            .drmFormatModifierTilingFeatures = features,
         };
      }
   }
   if (list2) {
      const VkFormatFeatureFlags2 features = linear ? props3->linearTilingFeatures : 0;
      list2->drmFormatModifierCount = vkr_linear_modifier_count(
         list2->drmFormatModifierCount, !list2->pDrmFormatModifierProperties, features);
      if (list2->pDrmFormatModifierProperties && list2->drmFormatModifierCount) {
         list2->pDrmFormatModifierProperties[0] = (VkDrmFormatModifierProperties2EXT){
            .drmFormatModifier = VKR_DRM_FORMAT_MOD_LINEAR,
            .drmFormatModifierPlaneCount = 1,
            .drmFormatModifierTilingFeatures = features,
         };
      }
   }

   vkr_unhide_struct(&hidden_list2);
   vkr_unhide_struct(&hidden_list);
}

static void
vkr_dispatch_vkGetPhysicalDeviceImageFormatProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceImageFormatProperties2 *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   VkPhysicalDeviceImageFormatInfo2 *format_info =
      (VkPhysicalDeviceImageFormatInfo2 *)args->pImageFormatInfo;
   if (physical_dev->emulate_drm_format_modifier &&
       format_info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
      const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *modifier_info =
         vkr_unlink_struct(
            format_info,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);
      if (!modifier_info ||
          modifier_info->drmFormatModifier != VKR_DRM_FORMAT_MOD_LINEAR ||
          !vkr_format_takes_linear_modifier(format_info->format)) {
         args->ret = VK_ERROR_FORMAT_NOT_SUPPORTED;
         return;
      }
      format_info->tiling = VK_IMAGE_TILING_LINEAR;
   }

   VkPhysicalDeviceExternalImageFormatInfo *external_info =
      vkr_find_struct(args->pImageFormatInfo->pNext,
                      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   VkExternalMemoryHandleTypeFlags handle_type =
      external_info ? external_info->handleType : 0;
   const bool translated =
      vkr_physical_device_translate_handle_types(physical_dev, &handle_type);
   if (translated)
      external_info->handleType = (VkExternalMemoryHandleTypeFlagBits)handle_type;

   vn_replace_vkGetPhysicalDeviceImageFormatProperties2_args_handle(args);
   args->ret = vk->GetPhysicalDeviceImageFormatProperties2(
      args->physicalDevice, args->pImageFormatInfo, args->pImageFormatProperties);

   VkExternalImageFormatProperties *external_props =
      vkr_find_struct(args->pImageFormatProperties->pNext,
                      VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
   if (translated && args->ret == VK_SUCCESS && external_props) {
      vkr_physical_device_untranslate_memory_properties(
         physical_dev, &external_props->externalMemoryProperties);
   }
}

static void
vkr_dispatch_vkGetPhysicalDeviceSparseImageFormatProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceSparseImageFormatProperties2 *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceSparseImageFormatProperties2_args_handle(args);
   vk->GetPhysicalDeviceSparseImageFormatProperties2(
      args->physicalDevice, args->pFormatInfo, args->pPropertyCount, args->pProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceExternalBufferProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceExternalBufferProperties *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   VkPhysicalDeviceExternalBufferInfo *info =
      (VkPhysicalDeviceExternalBufferInfo *)args->pExternalBufferInfo;
   VkExternalMemoryHandleTypeFlags handle_type = info->handleType;
   const bool translated =
      vkr_physical_device_translate_handle_types(physical_dev, &handle_type);
   if (translated)
      info->handleType = (VkExternalMemoryHandleTypeFlagBits)handle_type;

   vn_replace_vkGetPhysicalDeviceExternalBufferProperties_args_handle(args);
   vk->GetPhysicalDeviceExternalBufferProperties(
      args->physicalDevice, args->pExternalBufferInfo, args->pExternalBufferProperties);

   if (translated) {
      vkr_physical_device_untranslate_memory_properties(
         physical_dev, &args->pExternalBufferProperties->externalMemoryProperties);
   }
}

static void
vkr_dispatch_vkGetPhysicalDeviceExternalSemaphoreProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceExternalSemaphoreProperties *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   const VkPhysicalDeviceExternalSemaphoreInfo *info = args->pExternalSemaphoreInfo;
   if (physical_dev->emulate_sync_fd &&
       info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
      /* sync_fd payloads only exist for binary semaphores */
      const VkSemaphoreTypeCreateInfo *type_info =
         vkr_find_struct(info->pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
      const bool binary = !type_info || type_info->semaphoreType == VK_SEMAPHORE_TYPE_BINARY;
      VkExternalSemaphoreProperties *props = args->pExternalSemaphoreProperties;
      props->exportFromImportedHandleTypes = binary ? info->handleType : 0;
      props->compatibleHandleTypes = binary ? info->handleType : 0;
      props->externalSemaphoreFeatures =
         binary ? VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
                     VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT
                : 0;
      return;
   }

   vn_replace_vkGetPhysicalDeviceExternalSemaphoreProperties_args_handle(args);
   vk->GetPhysicalDeviceExternalSemaphoreProperties(args->physicalDevice,
                                                    args->pExternalSemaphoreInfo,
                                                    args->pExternalSemaphoreProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceExternalFenceProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceExternalFenceProperties *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   if (physical_dev->emulate_sync_fd &&
       args->pExternalFenceInfo->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT) {
      VkExternalFenceProperties *props = args->pExternalFenceProperties;
      props->exportFromImportedHandleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
      props->compatibleHandleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
      props->externalFenceFeatures =
         VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT;
      return;
   }

   vn_replace_vkGetPhysicalDeviceExternalFenceProperties_args_handle(args);
   vk->GetPhysicalDeviceExternalFenceProperties(
      args->physicalDevice, args->pExternalFenceInfo, args->pExternalFenceProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR_args_handle(args);
   args->ret = vk->GetPhysicalDeviceCalibrateableTimeDomainsKHR(
      args->physicalDevice, args->pTimeDomainCount, args->pTimeDomains);
}

static void
vkr_dispatch_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR_args_handle(args);
   args->ret = vk->GetPhysicalDeviceCooperativeMatrixPropertiesKHR(
      args->physicalDevice, args->pPropertyCount, args->pProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceFragmentShadingRatesKHR(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetPhysicalDeviceFragmentShadingRatesKHR *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceFragmentShadingRatesKHR_args_handle(args);
   args->ret = vk->GetPhysicalDeviceFragmentShadingRatesKHR(
      args->physicalDevice, args->pFragmentShadingRateCount, args->pFragmentShadingRates);
}

static void
vkr_dispatch_vkGetPhysicalDeviceMultisamplePropertiesEXT(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetPhysicalDeviceMultisamplePropertiesEXT *args)
{
   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);
   struct vn_physical_device_proc_table *vk = &physical_dev->proc_table;

   vn_replace_vkGetPhysicalDeviceMultisamplePropertiesEXT_args_handle(args);
   vk->GetPhysicalDeviceMultisamplePropertiesEXT(args->physicalDevice, args->samples,
                                                 args->pMultisampleProperties);
}

void
vkr_context_init_physical_device_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkEnumeratePhysicalDevices =
      vkr_dispatch_vkEnumeratePhysicalDevices;
   dispatch->dispatch_vkEnumeratePhysicalDeviceGroups =
      vkr_dispatch_vkEnumeratePhysicalDeviceGroups;
   dispatch->dispatch_vkEnumerateDeviceExtensionProperties =
      vkr_dispatch_vkEnumerateDeviceExtensionProperties;
   dispatch->dispatch_vkEnumerateDeviceLayerProperties = NULL;

   dispatch->dispatch_vkGetPhysicalDeviceFeatures =
      vkr_dispatch_vkGetPhysicalDeviceFeatures;
   dispatch->dispatch_vkGetPhysicalDeviceProperties =
      vkr_dispatch_vkGetPhysicalDeviceProperties;
   dispatch->dispatch_vkGetPhysicalDeviceQueueFamilyProperties =
      vkr_dispatch_vkGetPhysicalDeviceQueueFamilyProperties;
   dispatch->dispatch_vkGetPhysicalDeviceMemoryProperties =
      vkr_dispatch_vkGetPhysicalDeviceMemoryProperties;
   dispatch->dispatch_vkGetPhysicalDeviceFormatProperties =
      vkr_dispatch_vkGetPhysicalDeviceFormatProperties;
   dispatch->dispatch_vkGetPhysicalDeviceImageFormatProperties =
      vkr_dispatch_vkGetPhysicalDeviceImageFormatProperties;
   dispatch->dispatch_vkGetPhysicalDeviceSparseImageFormatProperties =
      vkr_dispatch_vkGetPhysicalDeviceSparseImageFormatProperties;
   dispatch->dispatch_vkGetPhysicalDeviceFeatures2 =
      vkr_dispatch_vkGetPhysicalDeviceFeatures2;
   dispatch->dispatch_vkGetPhysicalDeviceProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceQueueFamilyProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceQueueFamilyProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceMemoryProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceMemoryProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceFormatProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceFormatProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceImageFormatProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceImageFormatProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceSparseImageFormatProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceSparseImageFormatProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceExternalBufferProperties =
      vkr_dispatch_vkGetPhysicalDeviceExternalBufferProperties;
   dispatch->dispatch_vkGetMemoryFdKHR = NULL;
   dispatch->dispatch_vkGetMemoryFdPropertiesKHR = NULL;
   dispatch->dispatch_vkGetPhysicalDeviceExternalSemaphoreProperties =
      vkr_dispatch_vkGetPhysicalDeviceExternalSemaphoreProperties;
   dispatch->dispatch_vkGetPhysicalDeviceExternalFenceProperties =
      vkr_dispatch_vkGetPhysicalDeviceExternalFenceProperties;
   dispatch->dispatch_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR =
      vkr_dispatch_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR;

   /* VK_KHR_cooperative_matrix */
   dispatch->dispatch_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR =
      vkr_dispatch_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR;

   /* VK_KHR_fragment_shading_rate */
   dispatch->dispatch_vkGetPhysicalDeviceFragmentShadingRatesKHR =
      vkr_dispatch_vkGetPhysicalDeviceFragmentShadingRatesKHR;

   /* VK_EXT_sample_locations */
   dispatch->dispatch_vkGetPhysicalDeviceMultisamplePropertiesEXT =
      vkr_dispatch_vkGetPhysicalDeviceMultisamplePropertiesEXT;
}
