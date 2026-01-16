#include "apir-resource.h"
#include "apir-context.h"
#include "apir-renderer.h"

#include "util/anon_file.h"
#include "virgl_context.h" // for struct virgl_context_blob
#include "virglrenderer.h"
#include "virgl_resource.h"

#include <sys/mman.h>

volatile uint32_t *
apir_resource_get_shmem_ptr(struct apir_context *ctx, uint32_t res_id) {
   if (!ctx) {
      APIR_ERROR("%s: no context received", __func__);
      return NULL;
   }

   struct apir_resource *res = apir_resource_get(ctx, res_id);

   if (!res) {
      APIR_ERROR("%s: failed to find resource: invalid res_id %u", __func__, res_id);
      apir_context_set_fatal(ctx);
      return NULL;
   }

   // Accept both SHM and OPAQUE_HANDLE (Apple) resource types
   if (res->fd_type != VIRGL_RESOURCE_FD_SHM
#ifdef __APPLE__
       && res->fd_type != VIRGL_RESOURCE_OPAQUE_HANDLE
#endif
       ) {
      APIR_ERROR("%s: res_id %u has unexpected resource type (%u, expected VIRGL_RESOURCE_FD_SHM=%d"
#ifdef __APPLE__
                 " or VIRGL_RESOURCE_OPAQUE_HANDLE=%d"
#endif
                 ")",
                 __func__, res_id, res->fd_type, VIRGL_RESOURCE_FD_SHM
#ifdef __APPLE__
                 , VIRGL_RESOURCE_OPAQUE_HANDLE
#endif
                 );
      apir_context_set_fatal(ctx);
      return NULL;
   }

   return (volatile uint32_t *) res->u.data;
}

bool
apir_resource_create_blob(uint64_t blob_id,
                          uint64_t blob_size,
                          uint32_t blob_flags,
                          struct virgl_context_blob *out_blob)
{
   /* blob_id == 0 indicates HOST_3D_CACHED blob - following Venus pattern */
   if (!blob_id && blob_flags == VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE) {
#ifdef __APPLE__
      /* On Apple, create OPAQUE_HANDLE for host-side memory management */
      void *mmap_ptr = mmap(NULL, blob_size, PROT_WRITE | PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
      if (mmap_ptr == MAP_FAILED) {
         APIR_ERROR("failed to mmap anonymous memory");
         return false;
      }

      *out_blob = (struct virgl_context_blob) {
         .type = VIRGL_RESOURCE_OPAQUE_HANDLE,
         .u.fd = -1,  // No file descriptor for Apple handles
         .map_info = VIRGL_RENDERER_MAP_CACHE_CACHED,
         .map_ptr = (uint64_t)(uintptr_t)mmap_ptr,
         .vulkan_info = {{ 0 }},
      };
#else
      /* Non-Apple systems use shared memory for HOST_3D_CACHED */
      int fd = os_create_anonymous_file(blob_size, "apir-shmem");
      if (fd < 0) {
         APIR_ERROR("failed to create anonymous file");
         return false;
      }

      *out_blob = (struct virgl_context_blob) {
         .type = VIRGL_RESOURCE_FD_SHM,
         .u.fd = fd,
         .map_info = VIRGL_RENDERER_MAP_CACHE_CACHED,
         .vulkan_info = {{ 0 }},
      };
#endif
   } else {
      /* For other blob types, use shared memory */
      int fd = os_create_anonymous_file(blob_size, "apir-shmem");
      if (fd < 0) {
         APIR_ERROR("failed to create anonymous file");
         return false;
      }

      *out_blob = (struct virgl_context_blob) {
         .type = VIRGL_RESOURCE_FD_SHM,
         .u.fd = fd,
         .map_info = VIRGL_RENDERER_MAP_CACHE_CACHED,
         .vulkan_info = {{ 0 }},
      };
   }

   return true;
}

void
apir_resource_destroy_locked(struct apir_resource *res)
{
   // Clean up resource-specific data
   if (res->fd_type == VIRGL_RESOURCE_FD_SHM && res->u.data) {
      munmap(res->u.data, res->size);
   }
#ifdef __APPLE__
   else if (res->fd_type == VIRGL_RESOURCE_OPAQUE_HANDLE && res->u.data) {
      munmap(res->u.data, res->size);
   }
#endif

   // Close file descriptor (only for SHM blobs, OPAQUE_HANDLE has fd == -1)
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
