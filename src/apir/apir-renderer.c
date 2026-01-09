#include <sys/mman.h>

#include "apir-renderer.h"
#include "apir-context.h"
#include "apir-protocol.h"
#include "apir-impl.h"
#include "apir-resource.h"
#include "apir-hw.h"
#include "apir-codec.h"

#include "../src/drm/drm-uapi/virtgpu_drm.h"
#include "virglrenderer.h"
#include "virgl_util.h"

#include "util/os_misc.h"

#define UNUSED __attribute__((unused))

FILE *
get_log_dest(void)
{
   static FILE *dest = NULL;
   if (dest) {
      return dest;
   }

   const char *apir_log_to_file = getenv(VIRGL_APIR_LOG_TO_FILE_ENV);
   if (!apir_log_to_file) {
      dest = stderr;
      return dest;
   }

   // Use append mode instead of write mode to prevent truncation
   dest = fopen(apir_log_to_file, "a");

   if (!dest) {
      dest = stderr;
      // Don't use APIR_WARNING here to avoid circular dependency
      fprintf(stderr, "WARNING: Failed to open log file at '%s'\n", apir_log_to_file);
   }

   return dest;
}

bool apir_renderer_init(void) {
   apir_context_table_init();
   TRACE_FUNC();
   return true;
}

void apir_renderer_fini(void) {

}

bool apir_renderer_create_context(uint32_t ctx_id, uint32_t ctx_flags, uint32_t UNUSED nlen, const char *name)
{
   // Validate context ID
   assert(ctx_id);
   assert(!(ctx_flags & ~VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK));

   // Validate capset ID matches APIR
   if ((ctx_flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK) != VIRTGPU_DRM_CAPSET_APIR) {
      bool using_venus_capset_id = ((ctx_flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK) == VIRTGPU_DRM_CAPSET_VENUS);

      if (!using_venus_capset_id) {
         APIR_ERROR("APIR called with the wrong capset_id (ctx_flags=0x%x)", ctx_flags);
         return false;
      }
      APIR_INFO("TRANSITION: using the Venus capset_id");
   }

   // Check for duplicate context creation
   struct apir_context *existing_ctx = apir_context_lookup(ctx_id);
   if (existing_ctx) {
      APIR_ERROR("APIR context %u already exists", ctx_id);
      return false;
   }

   // Create new APIR context using your existing function
   struct apir_context *ctx = apir_context_create(ctx_id, name);
   if (!ctx) {
      APIR_ERROR("apir_context_create FAILED");
      return false;
   }

   return true;
}

void apir_renderer_destroy_context(uint32_t ctx_id) {
   assert(ctx_id);

   struct apir_context *ctx = apir_context_lookup(ctx_id);
   if (!ctx) {
      return;
   }

   apir_context_destroy(ctx);
}

static inline bool apir_renderer_dispatch_command(struct apir_context *ctx)
{
   ApirCommandType cmd_type;
   ApirCommandFlags cmd_flags;

   if (!apir_decode_command_type(&ctx->decoder, &cmd_type)) {
      APIR_ERROR("could not decode the command type ...");
      return false;
   }

   if (!apir_decode_command_flags(&ctx->decoder, &cmd_flags)) {
      APIR_ERROR("could not decode the command flags ...");
      return false;
   }

   // TRANSITION Venus -> APIR
#define VENUS_COMMAND_TYPE_LENGTH 331
   if (use_apir_backend_instead_of_vk() && cmd_type >= VENUS_COMMAND_TYPE_LENGTH) {
      cmd_type -= VENUS_COMMAND_TYPE_LENGTH;
   }

   if (cmd_type < APIR_COMMAND_TYPE_LENGTH && apir_protocol_dispatch_command(cmd_type)) {
      apir_protocol_dispatch_command(cmd_type)(ctx, cmd_flags);
   }
   else {
      APIR_ERROR("invalid command type: cmd_type=%d (apir_name=%s, apir_function=%p | apir_cmd_length=%d)",
                 cmd_type,
                 apir_command_name(cmd_type),
                 apir_protocol_dispatch_command(cmd_type),
                 APIR_COMMAND_TYPE_LENGTH);
      apir_context_set_fatal(ctx);
   }

   if (apir_context_get_fatal(ctx)) {
      APIR_ERROR("%s resulted in CS error", apir_command_name(cmd_type));
      return false;
   }

   return true;
}

bool apir_renderer_submit_fence(UNUSED uint32_t ctx_id,
                                UNUSED uint32_t flags,
                                UNUSED uint64_t ring_idx,
                                UNUSED uint64_t fence_id)
{
   APIR_ERROR("%s --> not implemented for APIR", __func__);
   return false;
}

bool apir_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size)
{
   struct apir_context *ctx = apir_context_lookup(ctx_id);
   if (!ctx) {
      APIR_ERROR("apir_renderer_submit_cmd: context %u not found", ctx_id);
      return false;
   }

   apir_decoder_init(&ctx->decoder, cmd, size);

   return apir_renderer_dispatch_command(ctx);
}

bool apir_renderer_create_resource(uint32_t ctx_id,
                                   uint32_t res_id,
                                   UNUSED uint64_t blob_id,
                                   uint64_t blob_size,
                                   uint32_t blob_flags,
                                   enum virgl_resource_fd_type *out_fd_type,
                                   int *out_res_fd,
                                   uint32_t *out_map_info,
                                   struct virgl_resource_vulkan_info *out_vulkan_info)
{
   assert(res_id);
   assert(blob_size);

   struct apir_context *ctx = apir_context_lookup(ctx_id);
   if (!ctx) {
      APIR_ERROR("%s: context %u not found", __func__, ctx_id);
      return false;
   }

   struct virgl_context_blob blob;
   if (!apir_resource_create_blob(blob_size, blob_flags, &blob)) {
      APIR_ERROR("apir_resource_create_blob failed");
      return false;
   }

   void *mmap_ptr = NULL;
   if (blob.type == VIRGL_RESOURCE_FD_SHM) {
      mmap_ptr = mmap(NULL, blob_size, PROT_WRITE | PROT_READ, MAP_SHARED, blob.u.fd, 0);
      if (mmap_ptr == MAP_FAILED) {
         close(blob.u.fd);
         return false;
      }
   }

   struct apir_resource *res = malloc(sizeof(*res));
   if (!res) {
      close(blob.u.fd);
      if (mmap_ptr) {
         munmap(mmap_ptr, blob_size);
      }
      return false;
   }

   // Set up APIR resource
   res->res_id = res_id;
   res->fd_type = blob.type;
   res->size = blob_size;

   if (blob.type == VIRGL_RESOURCE_FD_SHM) {
      res->fd = blob.u.fd;
      res->u.data = (uint8_t *)mmap_ptr;
   } else {
      res->fd = -1;
   }

   // Store in APIR hash table
   mtx_lock(&ctx->resource_mutex);
   _mesa_hash_table_insert(ctx->resource_table, (void*)(uintptr_t)res->res_id, res);
   mtx_unlock(&ctx->resource_mutex);

   // Set output parameters
   *out_fd_type = blob.type;
   *out_res_fd = blob.u.fd;
   *out_map_info = blob.map_info;

   if (blob.type == VIRGL_RESOURCE_FD_OPAQUE) {
      assert(out_vulkan_info);
      *out_vulkan_info = blob.vulkan_info;
   }

   return true;
}


bool apir_renderer_import_resource(uint32_t ctx_id,
                                   uint32_t res_id,
                                   enum virgl_resource_fd_type fd_type,
                                   int fd,
                                   uint64_t size)
{
   struct apir_context *ctx = apir_context_lookup(ctx_id);
   if (!ctx) {
      APIR_ERROR("%s: context %u not found", __func__, ctx_id);
      return false;
   }

   // Create APIR resource (similar to create_resource)
   struct apir_resource *res = malloc(sizeof(*res));
   if (!res) {
      return false;
   }

   res->res_id = res_id;
   res->fd_type = fd_type;
   res->size = size;
   res->fd = fd;

   // For SHM: mmap the imported fd (same as create_resource)
   if (fd_type == VIRGL_RESOURCE_FD_SHM) {
      res->u.data = mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
      if (res->u.data == MAP_FAILED) {
         free(res);
         return false;
      }
   } else {
      res->u.data = NULL;
   }

   // Add to hash table (same as create_resource)
   mtx_lock(&ctx->resource_mutex);
   _mesa_hash_table_insert(ctx->resource_table, (void*)(uintptr_t)res_id, res);
   mtx_unlock(&ctx->resource_mutex);

   return true;
}

void apir_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id) {
   struct apir_context *ctx = apir_context_lookup(ctx_id);
   if (!ctx) {
      APIR_ERROR("apir_renderer_destroy_resource: context %u not found", ctx_id);
      return;
   }

   apir_resource_destroy(ctx, res_id);
}


size_t apir_renderer_get_capset(void *capset, UNUSED uint32_t flags)
{
   struct virgl_renderer_capset_apir *c = capset;

   if (c) {
      memset(c, 0, sizeof(*c));
      c->apir_version = 1;
      c->supports_blob_resources = 1;
   }

   return sizeof(struct virgl_renderer_capset_apir);
}
