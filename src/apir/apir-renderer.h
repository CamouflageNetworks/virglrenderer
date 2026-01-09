#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "virgl_resource.h"

#include "apir-impl.h"

// TRANSITION Venus -> APIR
#include <stdlib.h>
#include <stdio.h>

bool apir_renderer_init(void);
void apir_renderer_fini(void);
bool apir_renderer_create_context(uint32_t ctx_id, uint32_t ctx_flags, uint32_t nlen, const char *name);
void apir_renderer_destroy_context(uint32_t ctx_id);
bool apir_renderer_submit_fence(uint32_t ctx_id, uint32_t flags, uint64_t ring_idx, uint64_t fence_id);
bool apir_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size);
bool apir_renderer_create_resource(uint32_t ctx_id,
                                   uint32_t res_id,
                                   uint64_t blob_id,
                                   uint64_t blob_size,
                                   uint32_t blob_flags,
                                   enum virgl_resource_fd_type *out_fd_type,
                                   int *out_res_fd,
                                   uint32_t *out_map_info,
                                   struct virgl_resource_vulkan_info *out_vulkan_info);

bool apir_renderer_import_resource(uint32_t ctx_id,
                                   uint32_t res_id,
                                   enum virgl_resource_fd_type fd_type,
                                   int fd,
                                   uint64_t size);

void apir_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id);
size_t apir_renderer_get_capset(void *capset, uint32_t flags);

#define APIR_VA_PRINT(prefix, format)               \
   do {                                             \
      FILE *dest = get_log_dest();                  \
      fprintf(dest, prefix);                        \
      va_list argptr;                               \
      va_start(argptr, format);                     \
      vfprintf(dest, format, argptr);               \
      fprintf(dest, "\n");                          \
      va_end(argptr);                               \
      fflush(dest);                                 \
   } while (0)

static inline void APIR_WARNING(const char *format, ...);

// Declaration for the logging function (implemented in apir-renderer.c)
FILE *get_log_dest(void);

static inline void
APIR_INFO(const char *format, ...)
{
   APIR_VA_PRINT("INFO: ", format);
}

static inline void
APIR_WARNING(const char *format, ...)
{
   APIR_VA_PRINT("WARNING: ", format);
}

static inline void
APIR_ERROR(const char *format, ...)
{
   APIR_VA_PRINT("ERROR: ", format);
}

// TRANSITION Venus -> APIR
static inline bool use_apir_backend_instead_of_vk(void) {
   static int vk_use_apir_backend = -1;

   if (vk_use_apir_backend == -1) {
      if (getenv("VIRGL_ROUTE_VENUS_TO_APIR")) {
         // the frontend uses the Venus capset to issue APIR commands (easier
         // to test with an unmodified hypervisor), so intercept the Venus
         // entrypoints and re-route them to the APIR componment.
         vk_use_apir_backend = 1;
         APIR_INFO("Venus -> APIR backend re-routing enabled. Frontend can use the Venus capset.");
      } else {
         // the frontend uses the APIR capset, so no need for the Venus
         // workflow to redirect to APIR backend
         vk_use_apir_backend = 0;
         APIR_INFO("Venus -> APIR re-routing NOT enabled. Frontend must use the APIR capset.");
      }
   }

   return vk_use_apir_backend == 1;
}
