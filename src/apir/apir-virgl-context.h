#ifndef APIR_VIRGL_CONTEXT_H_
#define APIR_VIRGL_CONTEXT_H_

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "util/hash_table.h"
#include "util/list.h"
#include "util/macros.h"

#include "virgl_context.h"

#include "apir-renderer.h"

struct apir_virgl_context {
   struct virgl_context base;

   struct list_head head;

   /* this tracks resources early attached in get_blob */
   struct hash_table *resource_table;
};

struct virgl_context *
apir_virgl_context_create(uint32_t ctx_id,
                          const char *debug_name);

size_t
apir_get_capset(uint32_t set, void *caps);

#endif // APIR_VIRGL_CONTEXT_H_
