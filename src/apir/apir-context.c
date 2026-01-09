#include <dlfcn.h>
#include <string.h>

#include "apir-context.h"
#include "apir-resource.h"
#include "apir-impl.h"

#include "c11/threads.h"
#include "util/hash_table.h"

#define UNUSED __attribute__((unused))

static bool apir_context_init(struct apir_context *ctx);
static void apir_context_deinit(struct apir_context *ctx);
static void apir_context_remove(uint32_t ctx_id);
static void apir_context_add(struct apir_context *ctx);

static uint32_t
hash_uint32(const void *key)
{
   return (uintptr_t)key;
}

static bool
compare_uint32(const void *key1, const void *key2)
{
   return key1 == key2;
}

struct apir_context *
apir_context_create(uint32_t ctx_id, const char *debug_name)
{
   struct apir_context *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return NULL;

   // Basic context setup
   ctx->ctx_id = ctx_id;
   ctx->debug_name = debug_name ? strdup(debug_name) : NULL;
   ctx->fatal = false;  // Start with no fatal errors

   // Initialize resource management (following Venus pattern)
   mtx_init(&ctx->resource_mutex, mtx_plain);
   ctx->resource_table = _mesa_hash_table_create(NULL, hash_uint32, compare_uint32);
   if (!ctx->resource_table) {
      apir_context_destroy(ctx);
      return NULL;
   }

   // Initialize APIR-specific state
   // (encoder/decoder will be initialized in get_response_stream like before)
   ctx->dispatch_fn = NULL;

   APIR_INFO("APIR context created: ctx_id=%u, debug_name=%s", ctx_id, debug_name ? debug_name : "unknown");

   if (!apir_context_init(ctx)) {
      APIR_WARNING("APIR context initialization failed.");
      apir_context_destroy(ctx);
      return NULL;
   }

   return ctx;
}

void
apir_context_destroy(struct apir_context *ctx)
{
   if (!ctx)
      return;

   apir_context_deinit(ctx);

   APIR_INFO("APIR context destroyed: ctx_id=%u, debug_name=%s",
             ctx->ctx_id, ctx->debug_name ? ctx->debug_name : "unknown");

   // Clean up resources
   if (ctx->resource_table) {
      mtx_lock(&ctx->resource_mutex);

      hash_table_foreach(ctx->resource_table, entry) {
         struct apir_resource *res = (struct apir_resource *)entry->data;

         apir_resource_destroy_locked(res);
      }

      mtx_unlock(&ctx->resource_mutex);
      _mesa_hash_table_destroy(ctx->resource_table, NULL);
   }

   mtx_destroy(&ctx->resource_mutex);
   free(ctx->debug_name);
   free(ctx);
}

void
apir_context_set_fatal(struct apir_context *ctx)
{
   APIR_ERROR("APIR context fatal error: ctx_id=%u", ctx->ctx_id);

   ctx->fatal = true;
}

bool
apir_context_get_fatal(UNUSED struct apir_context *ctx)
{
   return ctx ? ctx->fatal : true;  // NULL context is considered fatal
}


bool
apir_context_init(struct apir_context *ctx)
{
   apir_context_add(ctx);

   return true;
}

void
apir_context_deinit(struct apir_context *ctx)
{
   apir_context_remove(ctx->ctx_id);

   if (ctx->library_handle) {
      APIR_INFO("%s: The APIR backend library was loaded. Unloading it.", __func__);

      apir_backend_deinit_t apir_deinit_fct;
      *(void**)(&apir_deinit_fct) = dlsym(ctx->library_handle, APIR_DEINIT_FN_NAME);

      if (apir_deinit_fct) {
         apir_deinit_fct();
      } else {
         APIR_WARNING("the APIR backend library does not provide a deinit function.", __func__);
      }

      dlclose(ctx->library_handle);
      ctx->library_handle = NULL;
   } else {
      APIR_INFO("The backend library was NOT loaded.");
   }
}

/* Global context lookup hash map */

static struct hash_table *apir_context_table = NULL;

void apir_context_table_init(void)
{
   apir_context_table = _mesa_hash_table_create(NULL, hash_uint32, compare_uint32);
}

static void apir_context_add(struct apir_context *ctx)
{
   _mesa_hash_table_insert(apir_context_table, (void*)(uintptr_t)ctx->ctx_id, ctx);
}

static void apir_context_remove(uint32_t ctx_id)
{
   struct hash_entry *entry = _mesa_hash_table_search(apir_context_table,
                                                      (void*)(uintptr_t)ctx_id);

   _mesa_hash_table_remove(apir_context_table, entry); // safe if entry is null
}

struct apir_context *apir_context_lookup(uint32_t ctx_id)
{
   const struct hash_entry *entry = _mesa_hash_table_search(apir_context_table,
                                                            (void*)(uintptr_t)ctx_id);
   return entry ? entry->data : NULL;
}
