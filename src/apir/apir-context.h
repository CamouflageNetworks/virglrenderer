#pragma once

#include "c11/threads.h"
#include "util/hash_table.h"

#include "virgl_context.h"

#include "apir-callbacks.h"
#include "apir-impl.h"

struct apir_decoder {
   const uint8_t *data;
   const uint8_t *end;
   const uint8_t *cur;
};

struct apir_encoder {
   uint8_t *data;
   uint8_t *end;
   uint8_t *cur;
};

struct apir_context {
   uint32_t ctx_id;
   char *debug_name;

   /* Resource management */
   mtx_t resource_mutex;
   struct hash_table *resource_table;

   /* Configuration key-value storage */
   mtx_t config_mutex;
   struct hash_table *config_table;

   /* APIR-specific state */
   struct apir_encoder encoder;
   struct apir_decoder decoder;

   /* Error state */
   bool fatal;

   void *library_handle;

   apir_backend_dispatch_t dispatch_fn;

   /* virglrenderer base integration */
   struct virgl_context base;
};

// Context management
struct apir_context *apir_context_create(uint32_t ctx_id, const char *debug_name);
void apir_context_destroy(struct apir_context *ctx);

// Error handling
void apir_context_set_fatal(struct apir_context *ctx);
bool apir_context_get_fatal(struct apir_context *ctx);

struct apir_context *apir_context_lookup(uint32_t ctx_id);

void apir_context_table_init(void);

// Configuration management
const char *apir_context_get_config(struct apir_context *ctx, const char *key);
