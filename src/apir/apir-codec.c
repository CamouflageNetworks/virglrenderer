#include "apir-codec.h"
#include "apir-context.h"
#include "apir-renderer.h"

struct apir_encoder *
get_response_stream(struct apir_context *ctx, volatile uint32_t **atomic_reply_notif_p)
{
   /*
    * Look up the reply shared memory resource
    */

   uint32_t reply_res_id;

   struct apir_decoder *dec = &ctx->decoder;

   if (!apir_decode_uint32_t(dec, &reply_res_id)) {
      APIR_ERROR("%s: failed to read the reply stream ID", __func__);
      return NULL;
   }

   *atomic_reply_notif_p = apir_resource_get_shmem_ptr(ctx, reply_res_id);

   if (*atomic_reply_notif_p == NULL) {
      APIR_ERROR("%s: failed to find reply stream",  __func__);
      return NULL;
   }

   struct apir_resource *reply_res = apir_resource_get(ctx, reply_res_id);

   /*
    * Prepare the reply encoder and notif bit
    */

   // start the encoder right after the atomic bit
   if (!apir_encoder_set_stream(
          &ctx->encoder,
          reply_res->u.data,
          /* offset */ sizeof(**atomic_reply_notif_p),
          /* size */ reply_res->size - sizeof(**atomic_reply_notif_p)
          )) {
      APIR_ERROR("%s: failed to sync the encoder stream",  __func__);
      return NULL;
   }

   return &ctx->encoder;
}

void
send_response(struct apir_context *ctx,
              volatile uint32_t *atomic_reply_notif,
              uint32_t ret) {
   /*
    * Encode the return code with the reply notification flag
    */
   uint32_t reply_notif = 1 + ret;

   /*
    * Notify the guest that the reply is ready
    */

   *atomic_reply_notif = reply_notif;

   /*
    * Reset the decoder, so that the next call starts at the beginning of the
    * buffer
    */

   apir_decoder_reset(&ctx->decoder);
}
