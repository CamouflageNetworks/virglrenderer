#pragma once

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define VIRGL_APIR_LOG_TO_FILE_ENV "VIRGL_APIR_LOG_TO_FILE"

#define APIR_INITIALIZE_FN_NAME "apir_backend_initialize"
#define APIR_DEINIT_FN_NAME "apir_backend_deinit"
#define APIR_DISPATCH_FN_NAME "apir_backend_dispatcher"

/*** *** ***/

struct apir_context;

struct apir_callbacks {
   const char *(*get_config)(uint32_t virgl_ctx_id, const char *key);
   volatile uint32_t *(*get_shmem_ptr)(uint32_t virgl_ctx_id, uint32_t res_id);
};

/*** *** ***/

typedef uint32_t (*apir_backend_dispatch_t)(uint32_t virgl_ctx_id,
                                            struct apir_callbacks *virgl_cbs,
                                            uint32_t cmd_type,
                                            char *dec_cur, const char *dec_end,
                                            char *enc_cur, const char *enc_end,
                                            char **enc_cur_after
   );

typedef uint32_t (*apir_backend_initialize_t)(uint32_t virgl_ctx_id, struct apir_callbacks *virgl_cbs);
typedef void (*apir_backend_deinit_t)(uint32_t virgl_ctx_id);
