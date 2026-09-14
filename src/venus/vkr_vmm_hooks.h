/*
 * Copyright 2026 Camouflage Networks
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_VMM_HOOKS_H
#define VKR_VMM_HOOKS_H

#include <stdint.h>

/* VMM-side hooks for the in-process macOS Venus path.
 *
 * The egg runner links libvirglrenderer statically and defines these
 * (bridge/egg_virgl.m) to track MoltenVK device memory and Metal textures
 * for its SHM-BAR blob sync.  They are called from vkr_image.c and
 * vkr_device_memory.c under __APPLE__.
 *
 * The defaults in vkr_vmm_hooks.c are weak no-ops so that the standalone
 * virgl_render_server (where Neptune/Venus workers run and no VMM bridge
 * exists) links; a non-weak definition in the embedding process overrides
 * them.  The symbol names are part of the runner's link ABI: do not rename.
 */

void egg_virgl_untrack_blob_sync_by_ptr(void *old_data);
void *egg_virgl_get_mtl_buffer_contents(void *mtl_buffer);
void egg_virgl_track_metal_texture_with_memory(void *vk_image,
                                               void *mtl_texture,
                                               void *memory_ptr,
                                               uint64_t memory_offset);
void egg_virgl_untrack_metal_texture(void *vk_image);

#endif /* VKR_VMM_HOOKS_H */
