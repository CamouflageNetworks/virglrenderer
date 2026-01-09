#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/mman.h>

#include "mesa/util/macros.h"
#include "virgl_resource.h"
#include "apir-context.h"

// Resource management API

struct apir_resource {
   uint32_t res_id;

   enum virgl_resource_fd_type fd_type;

   /* valid when fd_type is dma_buf or opaque */
   int fd;

   union {
      /* valid when fd_type is shm */
      uint8_t *data;
   } u;

   size_t size;
};

static inline struct apir_resource *
apir_resource_get(struct apir_context *ctx, uint32_t res_id)
{
   mtx_lock(&ctx->resource_mutex);
   const struct hash_entry *entry = _mesa_hash_table_search(ctx->resource_table, (void*)(uintptr_t)res_id);
   struct apir_resource *res = likely(entry) ? entry->data : NULL;
   mtx_unlock(&ctx->resource_mutex);

   return res;
}

void apir_resource_destroy(struct apir_context *ctx, uint32_t res_id);
void apir_resource_destroy_locked(struct apir_resource *res);
volatile uint32_t *apir_resource_get_shmem_ptr(struct apir_context *ctx, uint32_t res_id);

bool apir_resource_create_blob(uint64_t blob_size,
                               uint32_t blob_flags,
                               struct virgl_context_blob *out_blob);
