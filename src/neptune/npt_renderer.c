/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_common.h"
#include "egg_hostmem.h"

#include <stdlib.h>

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif

#include "drm-uapi/virtgpu_drm.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune_hw.h"
#include "npt_context.h"
#include "npt_library.h"
#include "npt_profile.h"
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>

struct npt_renderer_state {
   const struct npt_renderer_callbacks *cbs;
   bool initialized;
   struct npt_d3d_library library;

   struct list_head contexts;
};

static struct npt_renderer_state npt_state;

/* egg: the VMM's hostmem BAR window is shared between Neptune and Venus
 * (egg_hostmem.h); these two keep npt's names for its callers. */
static void
npt_hostmem_init(void)
{
   egg_hostmem_init();
}

bool
npt_hostmem_place(uint64_t offset, uint64_t size, void **out_ptr, int *out_fd)
{
   return egg_hostmem_place(offset, size, out_ptr, out_fd);
}

/* Whether the D3D12 backend library exists and exports D3D12CreateDevice.
 *
 * Probed with a private dlopen rather than from npt_library state,
 * because the capset is reported before any context -- and so before
 * npt_library_init -- exists.  The dlopen refcount makes the later real
 * load cheap.  A split-arch proxy runs on a different arch than the
 * render server, so its local dlopen proves nothing and the answer has
 * to be overridden instead.
 */
static bool
npt_capset_probe_d3d12(void)
{
   const long override = npt_capset_d3d12_override();
   if (override >= 0)
      return override != 0;

#ifdef HAVE_DLFCN_H
   const char *path = getenv(NPT_D3D12_LIBRARY_ENV);
   if (!path)
      path = NPT_D3D12_LIBRARY_DEFAULT;

   void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!handle) {
      npt_log("capset: no D3D12 on this host (%s: %s)", path, dlerror());
      return false;
   }

   bool has_entry = dlsym(handle, "D3D12CreateDevice") != NULL;
   if (!has_entry)
      npt_log("capset: %s loaded but D3D12CreateDevice not found", path);
   dlclose(handle);
   return has_entry;
#else
   return false;
#endif
}

/* Backend-capability bits for the backend the render server will load.
 *
 * The capset is filled in the QEMU process before any worker has the
 * backend dlopen'ed, so the backend cannot be asked -- but it is already
 * determined, since render_worker.c picks the slice from NPT_BACKEND and
 * QEMU's environment propagates down.  Mirror that parse and answer from
 * a fixed per-backend table.
 *
 * npt_capset_caps_override replaces the whole derived word, the D3D12
 * bit included, where that mirroring cannot hold.
 */
static uint32_t
npt_capset_backend_caps(void)
{
   const char *backend = getenv("NPT_BACKEND");
   if (backend && !strcmp(backend, "dxmt")) {
      /* Not EXTENDED_RESOURCE_SHARING: DXMT reports the cap but fails
       * every SHARED_NTHANDLE texture create, so advertising it would
       * only move an app's failure from the cap check to create time.
       *
       * CAP_DXIL: DXMT now consumes DXIL containers (its DXIL front end;
       * the app-local D3D12 path already runs SM 6.x DXIL), and Triton
       * forwards the container to the host verbatim through pfnCreateShader
       * (tritonPipeline12.c t12CreateShaderCommon), so the WDDM/system path
       * is a DXIL consumer too, not just the app-local pair.  The bit is
       * what makes the D3D12 runtime hand this driver SM 6.x DXIL and makes
       * Triton advertise SM 6.x + pipeline level 12_1 instead of the
       * DXBC-only SM 5.1 / FL 11_1 floor (tritonDDI12.c GetCaps: the
       * SHADER_MODELS and 3DPIPELINESUPPORT1 cases gate on this bit).
       * Setting it also makes Triton report the FL 12_0/12_1 label caps
       * TiledResourcesTier>=2 and ConservativeRasterizationTier>=1
       * (tritonDDI12.c:382-394) which DXMT does not actually back -- but
       * DXMT fails those cleanly (CreateReservedResource* -> E_NOTIMPL,
       * UpdateTileMappings/CopyTileMappings no-op, a conservative-raster
       * PSO fails at create), never a crash.
       *
       * Kill switch if DXIL regresses: boot the host with
       * NPT_CAPSET_CAPS=0x37 -- this table's value WITHOUT the DXIL bit
       * (npt_capset_caps_override replaces the whole derived word, the
       * D3D12 bit included), which reverts to DXBC-only SM 5.1 / FL 11_1. */
      return VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_TBDR |
             VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_MSAA_RTV_FORCED_SC1 |
             VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_MAP_DEFAULT_BUFFERS |
             VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_SHADER_CACHE |
             VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_DXIL;
   }
   /* D3DMetal has none of the DXMT bits, but its shader front end
    * (Metal Shader Converter) also takes DXIL natively. */
   return VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_DXIL;
}

size_t
npt_get_capset(void *capset, UNUSED uint32_t flags)
{
   struct virgl_renderer_capset_neptune *c = capset;
   if (c) {
      /* Probe once; the answer can't change while the process lives. */
      static int d3d12_state; /* 0 = unprobed, 1 = no, 2 = yes */
      if (!d3d12_state)
         d3d12_state = npt_capset_probe_d3d12() ? 2 : 1;

      memset(c, 0, sizeof(*c));
      c->wire_format_version = NPT_PROTOCOL_WIRE_VERSION;
      if (d3d12_state == 2)
         c->caps_flags |= VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_D3D12;
      c->caps_flags |= npt_capset_backend_caps();

      const long override = npt_capset_caps_override();
      if (override >= 0)
         c->caps_flags = (uint32_t)override;

      /* Advertise the mmap alignment the SHM-heap importer requires so the
       * guest can size its blobs to carve a host-page-aligned sub-window. */
      const long page = sysconf(_SC_PAGESIZE);
      c->host_map_page_size = page > 0 ? (uint32_t)page : 0;

      npt_log("capset: wire=%u caps=0x%08x host_map_page=%u",
              c->wire_format_version, c->caps_flags, c->host_map_page_size);
   }

   return sizeof(struct virgl_renderer_capset_neptune);
}

bool
npt_renderer_init(UNUSED uint32_t flags, const struct npt_renderer_callbacks *cbs)
{
   if (npt_state.initialized)
      return true;

   npt_debug_init();
   npt_profile_init();

   /* Route log output through the host's log handler instead of
    * stderr directly. */
   if (cbs && cbs->debug_logger)
      virgl_log_set_handler(cbs->debug_logger, NULL, NULL);

   npt_library_init(&npt_state.library);
   npt_hostmem_init();

   list_inithead(&npt_state.contexts);

   npt_state.cbs = cbs;
   npt_state.initialized = true;

   npt_log("Neptune renderer initialized");

   return true;
}

void
npt_renderer_fini(void)
{
   if (!npt_state.initialized)
      return;

   list_for_each_entry_safe (struct npt_context, ctx, &npt_state.contexts, head) {
      list_del(&ctx->head);
      npt_context_destroy(ctx);
   }

   npt_library_fini(&npt_state.library);

   npt_state.cbs = NULL;
   npt_state.initialized = false;
}

struct npt_d3d_library *
npt_renderer_get_library(void)
{
   if (!npt_state.initialized)
      return NULL;
   return &npt_state.library;
}

static struct npt_context *
npt_renderer_lookup_context(uint32_t ctx_id)
{
   list_for_each_entry (struct npt_context, ctx, &npt_state.contexts, head) {
      if (ctx->ctx_id == ctx_id)
         return ctx;
   }
   return NULL;
}

bool
npt_renderer_create_context(uint32_t ctx_id,
                            uint32_t ctx_flags,
                            uint32_t nlen,
                            const char *name)
{
   assert(ctx_id);
   assert(!(ctx_flags & ~VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK));

   /* Defensive: the render-server dispatcher already routes by
    * capset_id, but the renderer API is public so re-check. */
   if ((ctx_flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK) !=
       VIRTGPU_DRM_CAPSET_NEPTUNE)
      return false;

   if (npt_renderer_lookup_context(ctx_id))
      return false;

   npt_renderer_retire_fence_callback_type retire_cb =
      npt_state.cbs ? npt_state.cbs->retire_fence : NULL;

   struct npt_context *ctx =
      npt_context_create(ctx_id, retire_cb, nlen, name);
   if (!ctx)
      return false;

   list_addtail(&ctx->head, &npt_state.contexts);
   return true;
}

void
npt_renderer_destroy_context(uint32_t ctx_id)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return;

   list_del(&ctx->head);
   npt_context_destroy(ctx);
}

bool
npt_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   return npt_context_submit_cmd(ctx, cmd, size);
}

bool
npt_renderer_submit_fence(uint32_t ctx_id,
                          UNUSED uint32_t flags,
                          uint64_t ring_idx,
                          uint64_t fence_id)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   if (!ctx->retire_fence) {
      npt_log("submit_fence: no retire_fence callback installed");
      return false;
   }

   /* virgl_context passes ring_idx as uint64; sync_queues[] is
    * indexed by uint32_t. */
   if (ring_idx > UINT32_MAX) {
      npt_log("submit_fence: ring_idx %" PRIu64 " out of range", ring_idx);
      return false;
   }

   return npt_context_submit_fence(ctx, (uint32_t)ring_idx, fence_id);
}

bool
npt_renderer_create_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             uint64_t blob_id,
                             uint64_t blob_size,
                             uint32_t blob_flags,
                             uint64_t hostmem_offset,
                             enum virgl_resource_fd_type *out_fd_type,
                             int *out_res_fd,
                             uint32_t *out_map_info,
                             uint32_t *out_export_format)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   struct virgl_context_blob blob;
   if (!npt_context_create_resource(ctx, res_id, blob_id, blob_size, blob_flags,
                                    hostmem_offset, &blob))
      return false;

   *out_fd_type = blob.type;
   *out_res_fd = blob.u.fd;
   *out_map_info = blob.map_info;
   *out_export_format = blob.export_format;

   return true;
}

bool
npt_renderer_import_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             enum virgl_resource_fd_type fd_type,
                             int fd,
                             uint64_t size)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   return npt_context_import_resource(ctx, res_id, fd_type, fd, size);
}

void
npt_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return;

   npt_context_destroy_resource(ctx, res_id);
}
