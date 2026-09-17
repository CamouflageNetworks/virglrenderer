/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_common.h"

#include "virtgpu_drm.h"

#include "proxy_client.h"
#include "proxy_renderer.h"
#include "proxy_server.h"

#ifdef ENABLE_VENUS
#include "vkr_renderer.h"
#endif

#ifdef ENABLE_NEPTUNE
#include "npt_renderer.h"
#endif

int
proxy_renderer_init(const struct proxy_renderer_cbs *cbs, uint32_t flags)
{
   assert(flags & VIRGL_RENDERER_NO_VIRGL);

   proxy_renderer.cbs = cbs;
   proxy_renderer.flags = flags;

   proxy_renderer.server = proxy_server_create();
   if (!proxy_renderer.server)
      goto fail;

   proxy_renderer.client =
      proxy_client_create(proxy_renderer.server, proxy_renderer.flags);
   if (!proxy_renderer.client)
      goto fail;

   return 0;

fail:
   proxy_renderer_fini();
   return -1;
}

void
proxy_renderer_fini(void)
{
   if (proxy_renderer.client)
      proxy_client_destroy(proxy_renderer.client);

   if (proxy_renderer.server)
      proxy_server_destroy(proxy_renderer.server);

   memset(&proxy_renderer, 0, sizeof(struct proxy_renderer));
}

void
proxy_renderer_reset(void)
{
   proxy_client_reset(proxy_renderer.client);
}

size_t
proxy_get_capset(uint32_t set, void *caps)
{
   /* Only advertise what the render server was initialised with: a guest
    * that sees a Venus capset on a Neptune-only server creates a Venus
    * context, which the server could not back (vkr never initialised) --
    * seen 2026-09-17 as a SIGSEGV in vkr_renderer_create_context that took
    * every context on the device down with it. */
   switch (set) {
#ifdef ENABLE_VENUS
   case VIRTGPU_DRM_CAPSET_VENUS:
      if (!(proxy_renderer.flags & VIRGL_RENDERER_VENUS))
         return 0;
      return vkr_get_capset(caps, proxy_renderer.flags);
#endif
#ifdef ENABLE_NEPTUNE
   case VIRTGPU_DRM_CAPSET_NEPTUNE:
      if (!(proxy_renderer.flags & VIRGL_RENDERER_NEPTUNE))
         return 0;
      return npt_get_capset(caps, proxy_renderer.flags);
#endif
   default:
      break;
   }

   return 0;
}
