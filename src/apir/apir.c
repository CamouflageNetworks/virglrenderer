#include <sys/stat.h>    // for struct stat, stat(), S_ISREG

#include "apir-protocol.h"
#include "apir-context.h"
#include "apir-resource.h"
#include "apir-codec.h"
#include "apir-impl.h"
#include "apir-callbacks.h"

static bool
dlopen_validated_library_name(struct apir_context *ctx, const char *library_name);

void
apir_HandShake(struct apir_context *ctx, UNUSED ApirCommandFlags flags)
{
   volatile uint32_t *atomic_reply_notif_p;
   struct apir_encoder *enc = get_response_stream(ctx, &atomic_reply_notif_p);

   if (!enc) {
      APIR_ERROR("Could not get the response stream :/");
      return;
   }

   /* *** */

   uint32_t guest_major = 0;
   uint32_t guest_minor = 0;
   if (!apir_decode_uint32_t(&ctx->decoder, &guest_major)) {
      APIR_ERROR("Failed to read the guest major version");
   }
   if (!apir_decode_uint32_t(&ctx->decoder, &guest_minor)) {
      APIR_ERROR("Failed to read the guest minor version");
   }
   APIR_INFO("Guest is running with %u.%u", guest_major, guest_minor);

   uint32_t host_major = APIR_PROTOCOL_MAJOR;
   uint32_t host_minor = APIR_PROTOCOL_MINOR;
   if (!apir_encode_uint32_t(enc, host_major)) {
      APIR_ERROR("Failed to write the host major version");
   }
   if (!apir_encode_uint32_t(enc, host_minor)) {
      APIR_ERROR("Failed to write the host minor version");
   }
   APIR_INFO("Host  is running with %u.%u", host_major, host_minor);

   if (guest_major != host_major) {
      APIR_ERROR("Host major (%d) and guest major (%d) version differ", host_major, guest_major);
   } else if (guest_minor != host_minor) {
      APIR_WARNING("Host minor (%d) and guest minor (%d) version differ", host_minor, guest_minor);
   }
   /* *** */

   uint32_t magic_ret_code = APIR_HANDSHAKE_MAGIC;
   send_response(ctx, atomic_reply_notif_p, magic_ret_code);

   APIR_INFO("Handshake with the guest library completed.");
}

void
apir_LoadLibrary(struct apir_context *ctx, ApirCommandFlags UNUSED flags)
{
   volatile uint32_t *atomic_reply_notif_p;
   struct apir_encoder *enc = get_response_stream(ctx, &atomic_reply_notif_p);

   if (!enc) {
      APIR_ERROR("Could not get the response stream :/");
      return;
   }

   const char *library_name = getenv(VIRGL_APIR_BACKEND_LIBRARY_ENV);
   if (!library_name) {
      APIR_ERROR("failed to load the library: %s env var not set", VIRGL_APIR_BACKEND_LIBRARY_ENV);
      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_ENV_VAR_MISSING);
      return;
   }

   if (ctx->library_handle) {
      APIR_INFO("APIR backend library already loaded.");

      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_ALREADY_LOADED);
      return;
   }

   /*
    * Load the API library
    */

   APIR_INFO("%s: loading the APIR backend library '%s' ...", __func__, library_name);



   if (!dlopen_validated_library_name(ctx, library_name)) {
      APIR_ERROR("cannot open the API Remoting library at %s (from %s): %s",
                 library_name, VIRGL_APIR_BACKEND_LIBRARY_ENV, dlerror());

      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_CANNOT_OPEN);
      return;
   }

   /*
    * Prepare the init function
    */

   apir_backend_initialize_t apir_init_fn;
   *(void**)(&apir_init_fn) = dlsym(ctx->library_handle, APIR_INITIALIZE_FN_NAME);

   const char* dlsym_error = dlerror();
   if (dlsym_error) {
      APIR_ERROR("cannot find the initialization symbol '%s': %s", APIR_INITIALIZE_FN_NAME, dlsym_error);

      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_SYMBOL_MISSING);
      return;
   }

   /*
    * Prepare the APIR dispatch function
    */

   *(void **)(&ctx->dispatch_fn) = dlsym(ctx->library_handle, APIR_DISPATCH_FN_NAME);

   dlsym_error = dlerror();
   if (dlsym_error) {
      APIR_ERROR("cannot find the dispatch symbol '%s': %s", APIR_DISPATCH_FN_NAME, dlsym_error);
      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_SYMBOL_MISSING);

      return;
   }

   /*
    * Initialize the APIR backend library
    */

   uint32_t apir_init_ret = apir_init_fn();
   if (apir_init_ret && apir_init_ret != APIR_LOAD_LIBRARY_INIT_BASE_INDEX) {
      if (apir_init_ret < APIR_LOAD_LIBRARY_INIT_BASE_INDEX) {
         APIR_ERROR("failed to initialize the APIR backend library: error %s (code %d)",
                    apir_load_library_error(apir_init_ret), apir_init_ret);
      } else {
         APIR_ERROR("failed to initialize the APIR backend library: API Remoting backend error: code %d", apir_init_ret);
      }

      send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_INIT_BASE_INDEX + apir_init_ret);

      return;
   }

   APIR_INFO("Loading the API Remoting backend library ... done.");
   send_response(ctx, atomic_reply_notif_p, APIR_LOAD_LIBRARY_SUCCESS);
}

void
apir_Forward(struct apir_context *ctx, ApirCommandFlags flags) {
   volatile uint32_t *atomic_reply_notif_p;
   struct apir_encoder *enc = get_response_stream(ctx, &atomic_reply_notif_p);

   if (!enc) {
      APIR_ERROR("Could not get the response stream :/");
      return;
   }

   if (!ctx->dispatch_fn) {
      APIR_ERROR("backend dispatch function (%s) not loaded :/", APIR_DISPATCH_FN_NAME);

      send_response(ctx, atomic_reply_notif_p, APIR_FORWARD_NO_DISPATCH_FN);
      return;
   }

   /* *** */

   static struct apir_callbacks callbacks = {
      /* get_shmem_ptr = */ apir_resource_get_shmem_ptr,
   };

   struct apir_callbacks_context apir_cb_ctx = {
      /* virgl_ctx = */ ctx,
      /* iface     = */ callbacks,
   };

   char *dec_cur;
   const char *dec_end;
   apir_decoder_get_stream(&ctx->decoder, &dec_cur, &dec_end);
   char *enc_cur;
   const char *enc_end;
   apir_encoder_get_stream(enc, &enc_cur, &enc_end);

   char *enc_cur_after;
   uint32_t apir_dispatch_ret;
   apir_dispatch_ret = ctx->dispatch_fn(
      flags, &apir_cb_ctx,
      dec_cur, dec_end,
      enc_cur, enc_end,
      &enc_cur_after
      );

   if (!apir_encoder_seek_stream((struct apir_encoder *) enc, (enc_cur_after - enc_cur))) {
      APIR_ERROR("Failed to sync the encoder stream");

      send_response(ctx, atomic_reply_notif_p, APIR_FORWARD_NO_DISPATCH_FN);
      return;
   }

   send_response(ctx, atomic_reply_notif_p,
                 APIR_FORWARD_BASE_INDEX + apir_dispatch_ret);
}

static bool
dlopen_validated_library_name(struct apir_context *ctx, const char *library_name) {
   if (!library_name || strlen(library_name) > PATH_MAX) {
      APIR_ERROR("Invalid library path");
      return false;
   }

   struct stat st;
   if (stat(library_name, &st) != 0 || !S_ISREG(st.st_mode)) {
      APIR_ERROR("Library file not found or not regular file: %s", library_name);
      return false;
   }

   ctx->library_handle = dlopen(library_name, RTLD_LAZY);

   return ctx->library_handle != NULL;
}
