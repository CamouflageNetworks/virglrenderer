#include "virglrenderer.h"

#define APIR_LIBRARY_CFG_KEY "apir.load_library.path"

VIRGL_EXPORT int virgl_apir_configure_kv(uint32_t ctx_id, const char *key, const char *value);
