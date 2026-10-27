/*
 * Copyright 2026 The biscuit authors
 * SPDX-License-Identifier: MIT
 */

#ifndef LOCAL_RENDERER_H
#define LOCAL_RENDERER_H

#include <stddef.h>
#include <stdint.h>

struct virgl_context;

/*
 * Runs venus contexts inside the VMM process, for platforms that have no
 * render server yet (Windows).  This mirrors the proxy renderer: where the
 * proxy forwards each virgl_context call over a socket to render_state in
 * the render server, this calls vkr_renderer_* directly.
 */

int
local_renderer_init(uint32_t flags);

void
local_renderer_fini(void);

void
local_renderer_reset(void);

size_t
local_get_capset(uint32_t set, void *caps);

struct virgl_context *
local_context_create(uint32_t ctx_id,
                     uint32_t ctx_flags,
                     size_t debug_len,
                     const char *debug_name);

#endif /* LOCAL_RENDERER_H */
