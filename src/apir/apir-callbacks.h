#pragma once

struct apir_context;

struct apir_callbacks {
   volatile uint32_t *(*get_shmem_ptr)(struct apir_context *ctx, uint32_t res_id);
};

struct apir_callbacks_context {
   struct apir_context *apir_ctx;

   struct apir_callbacks iface;
};
