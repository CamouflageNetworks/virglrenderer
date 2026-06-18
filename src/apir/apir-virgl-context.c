#include "c11/threads.h"
#include "util/hash_table.h"

#include "virtgpu_drm.h"

#include "virgl_context.h"
#include "virgl_resource.h"
#include "apir-virgl-context.h"
#include "apir-context.h"
#include "apir-renderer.h"
#include "apir-resource.h"

struct list_head contexts;

static void apir_virgl_context_init_base(struct apir_virgl_context *ctx);

static inline void
apir_virgl_context_resource_add(struct apir_virgl_context *ctx, uint32_t res_id)
{
   assert(!_mesa_hash_table_search(ctx->resource_table, (void *)(uintptr_t)res_id));
   _mesa_hash_table_insert(ctx->resource_table, (void *)(uintptr_t)res_id, NULL);
}

static inline bool
apir_virgl_context_resource_find(struct apir_virgl_context *ctx, uint32_t res_id)
{
   return _mesa_hash_table_search(ctx->resource_table, (void *)(uintptr_t)res_id);
}

static inline void
apir_virgl_context_resource_remove(struct apir_virgl_context *ctx, uint32_t res_id)
{
   _mesa_hash_table_remove_key(ctx->resource_table, (void *)(uintptr_t)res_id);
}

static inline bool
apir_virgl_context_resource_table_init(struct apir_virgl_context *ctx)
{
   ctx->resource_table = _mesa_hash_table_create_u32_keys(NULL);
   return ctx->resource_table;
}

static inline void
apir_virgl_context_resource_table_fini(struct apir_virgl_context *ctx)
{
   _mesa_hash_table_destroy(ctx->resource_table, NULL);
}

static void
apir_virgl_add_context(struct apir_virgl_context *ctx)
{
   list_addtail(&ctx->head, &contexts);
}

static void
apir_virgl_remove_context(struct apir_virgl_context *ctx)
{
   list_del(&ctx->head);
}

static void
apir_virgl_context_destroy(struct virgl_context *base)
{
   struct apir_virgl_context *ctx = (struct apir_virgl_context *)base;

   apir_context_destroy(apir_context_lookup(base->ctx_id));

   apir_virgl_context_resource_table_fini(ctx);

   apir_virgl_remove_context(ctx);

   free(ctx);
}

struct virgl_context *
apir_virgl_context_create(uint32_t ctx_id,
                          const char *debug_name)
{
   struct apir_virgl_context *ctx;

   if (!apir_context_create(ctx_id, debug_name)) {
      return NULL;
   }

   ctx = calloc(1, sizeof(*ctx));
   if (!ctx) {
      return NULL;
   }

   apir_virgl_context_init_base(ctx);
   apir_virgl_context_resource_table_init(ctx);
   apir_virgl_add_context(ctx);

   return &ctx->base;
}

size_t
apir_get_capset(uint32_t set, void *caps)
{
   switch (set) {
   case VIRTGPU_DRM_CAPSET_APIR:
      return apir_renderer_get_capset(caps, 0);
   default:
      break;
   }

   return 0;
}

static void
apir_virgl_context_attach_resource(struct virgl_context *base, struct virgl_resource *res)
{
   struct apir_virgl_context *ctx = (struct apir_virgl_context *)base;
   const uint32_t res_id = res->res_id;

   /* avoid importing resources created from RENDER_CONTEXT_OP_CREATE_RESOURCE */
   if (apir_virgl_context_resource_find(ctx, res_id))
      return;

   /* The current render protocol only supports importing dma-buf or pipe resource that
    * can be exported to dma-buf. A protocol change is needed when there exists use case
    * for importing external Vulkan opaque resource. For shm, we only create with blob_id
    * 0 via CREATE_RESOURCE above.
    */
   assert(res->fd_type == VIRGL_RESOURCE_FD_INVALID ||
          res->fd_type == VIRGL_RESOURCE_FD_DMABUF);

   enum virgl_resource_fd_type res_fd_type = res->fd_type;
   uint64_t res_size = res->map_size;

   if (!apir_renderer_import_resource(base->ctx_id, res_id, res_fd_type, -1, res_size)) {
      APIR_ERROR("failed to import res %d", res_id);
      return;
   }

   apir_virgl_context_resource_add(ctx, res_id);
}

static void
apir_virgl_context_detach_resource(struct virgl_context *base, struct virgl_resource *res)
{
   struct apir_virgl_context *ctx = (struct apir_virgl_context *)base;
   const uint32_t res_id = res->res_id;

   /* avoid detaching resource not belonging to this context */
   if (!apir_virgl_context_resource_find(ctx, res_id))
      return;

   apir_renderer_destroy_resource(base->ctx_id, res_id);

   apir_virgl_context_resource_remove(ctx, res_id);
}


static int
apir_virgl_context_get_blob(struct virgl_context *base,
                       uint32_t res_id,
                       uint64_t blob_id,
                       uint64_t blob_size,
                       uint32_t blob_flags,
                       struct virgl_context_blob *blob)
{
   /* RENDER_CONTEXT_OP_CREATE_RESOURCE implies resource attach, thus proxy tracks
    * resources created here to avoid double attaching the same resource when proxy is on
    * attach_resource callback.
    */
   struct apir_virgl_context *ctx = (struct apir_virgl_context *)base;
   int res_fd;
   enum virgl_resource_fd_type fd_type;
   uint32_t map_info;
   uint64_t map_ptr;
   struct virgl_resource_vulkan_info vulkan_info;

   if (!apir_renderer_create_resource(base->ctx_id, res_id, blob_id, blob_size, blob_flags,
                                      &fd_type, &res_fd, &map_info, &map_ptr, &vulkan_info)) {
      return -1;
   }

   blob->type = fd_type;
   blob->u.fd = res_fd;
   blob->map_info = map_info;
   blob->map_ptr = map_ptr;
   blob->vulkan_info = vulkan_info;

   apir_virgl_context_resource_add(ctx, res_id);

   return 0;
}

static int
apir_virgl_context_submit_cmd(struct virgl_context *base, const void *buffer, size_t size)
{
   void *cmd = (void *)buffer;

   if (!size)
      return 0;

   if (!apir_renderer_submit_cmd(base->ctx_id, cmd, size)) {
      return -1;
   }

   return 0;
}

/* APIR Apple blobs are OPAQUE_HANDLE with no fd. virgl_renderer_resource_map
 * reaches this for OPAQUE_HANDLE resources; we return the host pointer the
 * apir_context recorded at create time (res->u.data), so egg can then map those
 * same pages into the guest GPA. Without this, the OPAQUE_HANDLE path would try
 * to export a dmabuf (unavailable on macOS) and fail. */
static void *
apir_virgl_context_resource_map(struct virgl_context *base,
                                struct virgl_resource *res,
                                UNUSED void *addr,
                                UNUSED int32_t prot,
                                UNUSED int32_t flags)
{
   struct apir_context *actx = apir_context_lookup(base->ctx_id);
   if (!actx)
      return NULL;
   return (void *)apir_resource_get_shmem_ptr(actx, res->res_id);
}

static void
apir_virgl_context_init_base(struct apir_virgl_context *ctx)
{
   ctx->base.destroy = apir_virgl_context_destroy;
   ctx->base.attach_resource = apir_virgl_context_attach_resource;
   ctx->base.detach_resource = apir_virgl_context_detach_resource;
   ctx->base.get_blob = apir_virgl_context_get_blob;
   ctx->base.submit_cmd = apir_virgl_context_submit_cmd;
   ctx->base.resource_map = apir_virgl_context_resource_map;

   ctx->base.transfer_3d = NULL;
   ctx->base.get_fencing_fd = NULL;
   ctx->base.retire_fences = NULL;
   ctx->base.submit_fence = NULL;
}
