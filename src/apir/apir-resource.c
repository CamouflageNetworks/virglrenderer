#include "apir-resource.h"
#include "apir-context.h"
#include "apir-impl.h"  // for APIR_ERROR

#include "util/anon_file.h"
#include "virgl_context.h" // for struct virgl_context_blob
#include "virglrenderer.h"

volatile uint32_t *
apir_resource_get_shmem_ptr(struct apir_context *ctx, uint32_t res_id) {
   struct apir_resource *res = apir_resource_get(ctx, res_id);

   if (!res) {
      APIR_ERROR("%s: failed to find resource: invalid res_id %u", __func__, res_id);
      apir_context_set_fatal(ctx);
      return NULL;
   }

   if (res->fd_type != VIRGL_RESOURCE_FD_SHM) {
      APIR_ERROR("%s: res_id %u has unexpected resource type (%u, expected VIRGL_RESOURCE_FD_SHM=%d)",
                 __func__, res_id, res->fd_type, VIRGL_RESOURCE_FD_SHM);
      apir_context_set_fatal(ctx);
      return NULL;
   }

   return (volatile uint32_t *) res->u.data;
}

bool
apir_resource_create_blob(uint64_t blob_size,
                          UNUSED uint32_t blob_flags,
                          struct virgl_context_blob *out_blob)
{
   // For APIR, we primarily use shared memory for communication
   // Create anonymous shared memory file
   int fd = os_create_anonymous_file(blob_size, "apir-shmem");
   if (fd < 0) {
      APIR_ERROR("failed to create anonymous file");
      return false;
   }

   // Set up the blob output
   *out_blob = (struct virgl_context_blob) {
      .type = VIRGL_RESOURCE_FD_SHM,
      .u.fd = fd,
      .map_info = VIRGL_RENDERER_MAP_CACHE_CACHED,
      .vulkan_info = {{ 0 }},  // Not used for APIR
   };

   return true;
}

void
apir_resource_destroy_locked(struct apir_resource *res)
{
   // Clean up resource-specific data
   if (res->fd_type == VIRGL_RESOURCE_FD_SHM && res->u.data) {
      munmap(res->u.data, res->size);
   }

   // Close file descriptor
   if (res->fd >= 0) {
      close(res->fd);
   }

   // Free the resource memory
   free(res);
}

void
apir_resource_destroy(struct apir_context *ctx, uint32_t res_id)
{
   mtx_lock(&ctx->resource_mutex);
   // Look up the resource entry
   const struct hash_entry *entry = _mesa_hash_table_search(ctx->resource_table, (void*)(uintptr_t)res_id);
   if (entry) {
      struct apir_resource *res = (struct apir_resource *)entry->data;

      apir_resource_destroy_locked(res);

      // Remove from hash table
      _mesa_hash_table_remove_key(ctx->resource_table, (void*)(uintptr_t)res_id);
   }

   mtx_unlock(&ctx->resource_mutex);
}
