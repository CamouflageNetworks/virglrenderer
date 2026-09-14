/*
 * Copyright 2026 Camouflage Networks
 * SPDX-License-Identifier: MIT
 */

#include "vkr_vmm_hooks.h"

#include <stddef.h>

#include "util/macros.h"

#ifdef __APPLE__

/* Weak no-op defaults; see vkr_vmm_hooks.h. */

__attribute__((weak)) void
egg_virgl_untrack_blob_sync_by_ptr(UNUSED void *old_data)
{
}

__attribute__((weak)) void *
egg_virgl_get_mtl_buffer_contents(UNUSED void *mtl_buffer)
{
   return NULL;
}

__attribute__((weak)) void
egg_virgl_track_metal_texture_with_memory(UNUSED void *vk_image,
                                          UNUSED void *mtl_texture,
                                          UNUSED void *memory_ptr,
                                          UNUSED uint64_t memory_offset)
{
}

__attribute__((weak)) void
egg_virgl_untrack_metal_texture(UNUSED void *vk_image)
{
}

#endif /* __APPLE__ */
