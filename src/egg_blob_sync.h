/*
 * egg: Venus device-memory blobs, shared with the guest by copy.
 *
 * A host-visible VkDeviceMemory lives in Metal-backed memory the guest cannot
 * be given directly: on macOS the guest's view of it is a range of the VMM's
 * hostmem BAR window, and the two are different pages.  The in-process Linux
 * path (egg-runner bridge, egg_virgl_track_blob_sync) solved this years ago by
 * pointing the renderer's resource at the guest's pages and copying between
 * them on a timer.  This is that path, inside the render server, where Venus
 * for Windows eggs runs.
 *
 * It replaces mapping Metal pages into the guest with hv_vm_map, which worked
 * but forced a 4 KiB stage-2 granule (macOS 26) because the Windows KMD hands
 * out 4 KiB window offsets.  Copying needs no granule at all.
 */
#ifndef EGG_BLOB_SYNC_H
#define EGG_BLOB_SYNC_H

#include <stdbool.h>
#include <stdint.h>

/* Start syncing a device-memory blob.
 *  gpu_ptr    the CPU mapping of the VkDeviceMemory (what the GPU reads/writes)
 *  guest_ptr  the same bytes inside the shared hostmem window (what the guest
 *             reads/writes), already seeded by the caller
 * Idempotent per res_id; starts the sync thread on first use. */
void
egg_blob_sync_track(uint32_t res_id,
                    uint64_t blob_id,
                    void *gpu_ptr,
                    void *guest_ptr,
                    uint64_t size);

/* Stop syncing.  Both return only once any in-flight copy of that entry has
 * finished, so the caller may free the memory immediately afterwards. */
void
egg_blob_sync_untrack(uint32_t res_id);
void
egg_blob_sync_untrack_by_gpu_ptr(void *gpu_ptr);

/* One copy pass.  small_only skips blobs above the swapchain threshold, for
 * the post-fence call where a half-copied frame would be visible. */
void
egg_blob_sync_run(bool small_only);

/* Push the guest's writes to the GPU's copy.  Call it immediately before
 * handing work to the GPU: a periodic pass cannot stand in for this, because a
 * guest that writes a buffer and submits in the next instruction always wins
 * the race against it.  See the definition. */
void
egg_blob_sync_push_before_gpu(void);

#endif /* EGG_BLOB_SYNC_H */
