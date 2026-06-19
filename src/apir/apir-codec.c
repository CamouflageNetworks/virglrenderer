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

   /* DIAG: reply-ring target, handshake/loadlibrary phase only (gated on
    * dispatch_fn so per-op Forward stays silent). u.data is where the host writes
    * the reply: if externally_mapped is true it's egg's redirected SHM-BAR HVA
    * (guest-visible); if false it's this resource's private mmap (guest can't see
    * it). Diff this against egg's "SHM-redirect res N to <HVA> (GPA ...)" line. */
   if (reply_res && !ctx->dispatch_fn) {
      APIR_INFO("apir reply-ring: res_id=%u u.data=%p externally_mapped=%d size=%zu notif_before=%u",
                reply_res_id, (void *)reply_res->u.data, (int)reply_res->externally_mapped,
                reply_res->size, (unsigned)**atomic_reply_notif_p);
   }

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

   /* DIAG: confirm the host wrote the reply notif, and to which address. Gated on
    * dispatch_fn → handshake/loadlibrary only. readback proves the store landed
    * in the host's own mapping; compare the address to egg's redirected SHM HVA
    * to know whether the guest can see it. */
   if (!ctx->dispatch_fn) {
      APIR_INFO("apir reply sent: notif=%u -> %p (readback=%u)",
                reply_notif, (void *)atomic_reply_notif, (unsigned)*atomic_reply_notif);
   }

   /*
    * Reset the decoder, so that the next call starts at the beginning of the
    * buffer
    */

   apir_decoder_reset(&ctx->decoder);
}
