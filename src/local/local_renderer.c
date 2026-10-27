/*
 * Copyright 2026 The biscuit authors
 * SPDX-License-Identifier: MIT
 */

#include "local_renderer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#include "c11/threads.h"
#include "util/bitscan.h"
#include "util/hash_table.h"
#include "util/list.h"
#include "util/macros.h"
#include "virgl_context.h"
#include "virgl_os.h"
#include "virgl_util.h"
#include "virglrenderer.h"
#include "virtgpu_drm.h"
#include "vkr_renderer.h"

#define LOCAL_CONTEXT_TIMELINE_COUNT 64

struct local_fence {
   uint32_t seqno;
   uint64_t fence_id;
   struct list_head head;
};

struct local_timeline {
   uint32_t next_seqno;
   struct list_head fences;
};

struct local_context {
   struct virgl_context base;

   /* in local_renderer.contexts */
   struct list_head head;

   /* resources created by get_blob, which vkr already knows about */
   struct hash_table *resource_table;

   mtx_t timeline_mutex;
   struct local_timeline timelines[LOCAL_CONTEXT_TIMELINE_COUNT];
   /* which timelines have pending fences */
   uint64_t timeline_busy_mask;

   /* latest signaled seqno of each timeline, stored by vkr's sync threads */
   atomic_uint signaled_seqnos[LOCAL_CONTEXT_TIMELINE_COUNT];
};

static struct {
   uint32_t flags;
   bool initialized;

   /* serializes vkr_renderer_* calls; vkr's callbacks never take it */
   mtx_t vkr_mutex;

   /* guards contexts, which vkr's sync threads search to signal fences */
   mtx_t contexts_mutex;
   struct list_head contexts;
} local_renderer;

static void PRINTFLIKE(1, 2)
local_log(const char *fmt, ...)
{
   va_list va;

   va_start(va, fmt);
   virgl_prefixed_logv("local", VIRGL_LOG_LEVEL_INFO, fmt, va);
   va_end(va);
}

static bool
local_fence_is_signaled(const struct local_fence *fence, uint32_t signaled_seqno)
{
   /* takes wrapping into account */
   const uint32_t d = signaled_seqno - fence->seqno;
   return d < INT32_MAX;
}

/* Hands fences whose seqno vkr has reached to the VMM, oldest first. */
static void
local_context_retire_fences_internal(struct local_context *ctx)
{
   mtx_lock(&ctx->timeline_mutex);

   uint64_t busy_mask = ctx->timeline_busy_mask;
   while (busy_mask) {
      const uint32_t ring_idx = u_bit_scan64(&busy_mask);
      struct local_timeline *timeline = &ctx->timelines[ring_idx];
      const uint32_t signaled_seqno = atomic_load(&ctx->signaled_seqnos[ring_idx]);

      list_for_each_entry_safe (struct local_fence, fence, &timeline->fences, head) {
         if (!local_fence_is_signaled(fence, signaled_seqno))
            break;

         ctx->base.fence_retire(&ctx->base, ring_idx, fence->fence_id);
         list_del(&fence->head);
         free(fence);
      }

      if (list_is_empty(&timeline->fences))
         ctx->timeline_busy_mask &= ~(1ull << ring_idx);
   }

   mtx_unlock(&ctx->timeline_mutex);
}

/* vkr_renderer_callbacks::retire_fence; runs on vkr's sync threads */
static void
local_renderer_retire_fence(uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id)
{
   mtx_lock(&local_renderer.contexts_mutex);

   list_for_each_entry (struct local_context, ctx, &local_renderer.contexts, head) {
      if (ctx->base.ctx_id != ctx_id)
         continue;

      /* local_context_submit_fence passed the seqno as the fence id */
      atomic_store(&ctx->signaled_seqnos[ring_idx], (uint32_t)fence_id);
      if (local_renderer.flags & VIRGL_RENDERER_ASYNC_FENCE_CB)
         local_context_retire_fences_internal(ctx);
      break;
   }

   mtx_unlock(&local_renderer.contexts_mutex);
}

static void
local_context_destroy(struct virgl_context *base)
{
   struct local_context *ctx = (struct local_context *)base;

   /* unlist first, so that late fence callbacks no longer find the context */
   mtx_lock(&local_renderer.contexts_mutex);
   list_del(&ctx->head);
   mtx_unlock(&local_renderer.contexts_mutex);

   mtx_lock(&local_renderer.vkr_mutex);
   vkr_renderer_destroy_context(ctx->base.ctx_id);
   mtx_unlock(&local_renderer.vkr_mutex);

   for (uint32_t i = 0; i < LOCAL_CONTEXT_TIMELINE_COUNT; i++) {
      list_for_each_entry_safe (struct local_fence, fence, &ctx->timelines[i].fences, head)
         free(fence);
   }
   mtx_destroy(&ctx->timeline_mutex);
   _mesa_hash_table_destroy(ctx->resource_table, NULL);
   free(ctx);
}

static void
local_context_attach_resource(struct virgl_context *base, struct virgl_resource *res)
{
   struct local_context *ctx = (struct local_context *)base;
   const uint32_t res_id = res->res_id;

   /* resources from get_blob are attached already */
   if (_mesa_hash_table_search(ctx->resource_table, (void *)(uintptr_t)res_id))
      return;

   /* vkr can only import resources that are backed by something shareable */
   if (res->fd_type != VIRGL_RESOURCE_FD_SHM && res->fd_type != VIRGL_RESOURCE_FD_DMABUF &&
       res->fd_type != VIRGL_RESOURCE_FD_OPAQUE) {
      local_log("cannot attach res %u with fd_type %d", res_id, res->fd_type);
      return;
   }

   /* vkr keeps dma-buf and opaque handles, but only maps shm ones */
   const int fd = virgl_os_handle_dup(res->fd);
   if (fd < 0) {
      local_log("failed to dup the handle of res %u", res_id);
      return;
   }

   mtx_lock(&local_renderer.vkr_mutex);
   const bool imported =
      vkr_renderer_import_resource(ctx->base.ctx_id, res_id, res->fd_type, fd, res->map_size);
   mtx_unlock(&local_renderer.vkr_mutex);

   if (!imported || res->fd_type == VIRGL_RESOURCE_FD_SHM)
      virgl_os_handle_close(fd);
   if (!imported) {
      local_log("failed to attach res %u", res_id);
      return;
   }

   _mesa_hash_table_insert(ctx->resource_table, (void *)(uintptr_t)res_id, NULL);
}

static void
local_context_detach_resource(struct virgl_context *base, struct virgl_resource *res)
{
   struct local_context *ctx = (struct local_context *)base;
   const uint32_t res_id = res->res_id;

   struct hash_entry *entry =
      _mesa_hash_table_search(ctx->resource_table, (void *)(uintptr_t)res_id);
   if (!entry)
      return;

   mtx_lock(&local_renderer.vkr_mutex);
   vkr_renderer_destroy_resource(ctx->base.ctx_id, res_id);
   mtx_unlock(&local_renderer.vkr_mutex);

   _mesa_hash_table_remove(ctx->resource_table, entry);
}

static int
local_context_transfer_3d(struct virgl_context *base,
                          struct virgl_resource *res,
                          UNUSED const struct vrend_transfer_info *info,
                          UNUSED int transfer_mode)
{
   local_log("no transfer support for ctx %u and res %u", base->ctx_id, res->res_id);
   return -1;
}

static int
local_context_get_blob(struct virgl_context *base,
                       uint32_t res_id,
                       uint64_t blob_id,
                       uint64_t blob_size,
                       uint32_t blob_flags,
                       struct virgl_context_blob *blob)
{
   struct local_context *ctx = (struct local_context *)base;

   enum virgl_resource_fd_type fd_type = VIRGL_RESOURCE_FD_INVALID;
   int fd = -1;
   uint32_t map_info = 0;
   struct virgl_resource_vulkan_info vulkan_info = { 0 };

   mtx_lock(&local_renderer.vkr_mutex);
   const bool created =
      vkr_renderer_create_resource(ctx->base.ctx_id, res_id, blob_id, blob_size, blob_flags,
                                   &fd_type, &fd, &map_info, &vulkan_info);
   mtx_unlock(&local_renderer.vkr_mutex);

   if (!created) {
      local_log("failed to get blob %" PRIu64 " for res %u", blob_id, res_id);
      return -1;
   }

   *blob = (struct virgl_context_blob){
      .type = fd_type,
      .u.fd = fd,
      .map_info = map_info,
      .vulkan_info = vulkan_info,
   };

   _mesa_hash_table_insert(ctx->resource_table, (void *)(uintptr_t)res_id, NULL);
   return 0;
}

static int
local_context_submit_cmd(struct virgl_context *base, const void *buffer, size_t size)
{
   if (!size)
      return 0;

   /* vkr decodes the stream in place; the VMM hands over its own copy */
   mtx_lock(&local_renderer.vkr_mutex);
   const bool submitted = vkr_renderer_submit_cmd(base->ctx_id, (void *)buffer, size);
   mtx_unlock(&local_renderer.vkr_mutex);

   return submitted ? 0 : -1;
}

static int
local_context_get_fencing_fd(UNUSED struct virgl_context *base)
{
   return -1;
}

static void
local_context_retire_fences(struct virgl_context *base)
{
   local_context_retire_fences_internal((struct local_context *)base);
}

static int
local_context_submit_fence(struct virgl_context *base,
                           UNUSED uint32_t flags,
                           uint32_t ring_idx,
                           uint64_t fence_id)
{
   struct local_context *ctx = (struct local_context *)base;
   if (ring_idx >= LOCAL_CONTEXT_TIMELINE_COUNT)
      return -EINVAL;

   struct local_fence *fence = malloc(sizeof(*fence));
   if (!fence)
      return -ENOMEM;

   struct local_timeline *timeline = &ctx->timelines[ring_idx];
   mtx_lock(&ctx->timeline_mutex);
   fence->seqno = timeline->next_seqno++;
   fence->fence_id = fence_id;
   list_addtail(&fence->head, &timeline->fences);
   const uint64_t old_busy_mask = ctx->timeline_busy_mask;
   ctx->timeline_busy_mask |= 1ull << ring_idx;
   mtx_unlock(&ctx->timeline_mutex);

   /* like the render server: vkr tracks seqnos and always merges fences */
   mtx_lock(&local_renderer.vkr_mutex);
   const bool submitted = vkr_renderer_submit_fence(
      ctx->base.ctx_id, VIRGL_RENDERER_FENCE_FLAG_MERGEABLE, ring_idx, fence->seqno);
   mtx_unlock(&local_renderer.vkr_mutex);

   if (!submitted) {
      mtx_lock(&ctx->timeline_mutex);
      list_del(&fence->head);
      ctx->timeline_busy_mask = old_busy_mask;
      mtx_unlock(&ctx->timeline_mutex);
      free(fence);
      local_log("failed to submit fence");
      return -1;
   }

   return 0;
}

struct virgl_context *
local_context_create(uint32_t ctx_id,
                     uint32_t ctx_flags,
                     size_t debug_len,
                     const char *debug_name)
{
   struct local_context *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return NULL;

   ctx->resource_table = _mesa_hash_table_create_u32_keys(NULL);
   if (!ctx->resource_table) {
      free(ctx);
      return NULL;
   }

   /* virglrenderer sets ctx_id again after creation, but vkr needs it now */
   ctx->base.ctx_id = ctx_id;
   ctx->base.destroy = local_context_destroy;
   ctx->base.attach_resource = local_context_attach_resource;
   ctx->base.detach_resource = local_context_detach_resource;
   ctx->base.transfer_3d = local_context_transfer_3d;
   ctx->base.get_blob = local_context_get_blob;
   ctx->base.submit_cmd = local_context_submit_cmd;
   ctx->base.get_fencing_fd = local_context_get_fencing_fd;
   ctx->base.retire_fences = local_context_retire_fences;
   ctx->base.submit_fence = local_context_submit_fence;

   mtx_init(&ctx->timeline_mutex, mtx_plain);
   for (uint32_t i = 0; i < LOCAL_CONTEXT_TIMELINE_COUNT; i++) {
      ctx->timelines[i].next_seqno = 1;
      list_inithead(&ctx->timelines[i].fences);
      atomic_init(&ctx->signaled_seqnos[i], 0);
   }

   mtx_lock(&local_renderer.vkr_mutex);
   const bool created =
      vkr_renderer_create_context(ctx_id, ctx_flags, (uint32_t)debug_len, debug_name);
   mtx_unlock(&local_renderer.vkr_mutex);

   if (!created) {
      local_log("failed to create ctx %u", ctx_id);
      mtx_destroy(&ctx->timeline_mutex);
      _mesa_hash_table_destroy(ctx->resource_table, NULL);
      free(ctx);
      return NULL;
   }

   mtx_lock(&local_renderer.contexts_mutex);
   list_addtail(&ctx->head, &local_renderer.contexts);
   mtx_unlock(&local_renderer.contexts_mutex);

   return &ctx->base;
}

int
local_renderer_init(uint32_t flags)
{
   assert(flags & VIRGL_RENDERER_NO_VIRGL);
   if (local_renderer.initialized)
      return 0;

   /* no debug_logger: vkr then logs through the handler the VMM installed */
   static const struct vkr_renderer_callbacks cbs = {
      .retire_fence = local_renderer_retire_fence,
   };
   if (!vkr_renderer_init(VKR_RENDERER_THREAD_SYNC | VKR_RENDERER_ASYNC_FENCE_CB, &cbs))
      return -1;

   local_renderer.flags = flags;
   mtx_init(&local_renderer.vkr_mutex, mtx_plain);
   mtx_init(&local_renderer.contexts_mutex, mtx_plain);
   list_inithead(&local_renderer.contexts);
   local_renderer.initialized = true;

   return 0;
}

void
local_renderer_fini(void)
{
   if (!local_renderer.initialized)
      return;

   /* virglrenderer destroys the remaining contexts before this */
   vkr_renderer_fini();
   mtx_destroy(&local_renderer.vkr_mutex);
   mtx_destroy(&local_renderer.contexts_mutex);
   local_renderer.initialized = false;
}

void
local_renderer_reset(void)
{
   /* contexts are destroyed through virgl_context_table_reset; nothing global to reset */
}

size_t
local_get_capset(uint32_t set, void *caps)
{
   if (set != VIRTGPU_DRM_CAPSET_VENUS)
      return 0;

   return vkr_get_capset(caps, local_renderer.flags);
}
