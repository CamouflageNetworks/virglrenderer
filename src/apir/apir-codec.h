#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "apir-resource.h"
#include "apir-context.h"
#include "apir-protocol.h"

struct apir_encoder *get_response_stream(struct apir_context *ctx,
                                         volatile uint32_t **atomic_reply_notif_p);

void send_response(struct apir_context *ctx,
                   volatile uint32_t *atomic_reply_notif,
                   uint32_t ret);

// Decoder functions
static inline void
apir_decoder_init(struct apir_decoder *dec, const void *data, size_t size)
{
   dec->data = (const uint8_t *)data;
   dec->cur = dec->data;
   dec->end = dec->data + size;
}

static inline void
apir_decoder_reset(struct apir_decoder *dec)
{
   dec->cur = dec->data;
}

static inline bool
apir_decode_uint32_t(struct apir_decoder *dec, uint32_t *value)
{
   if (dec->cur + sizeof(uint32_t) > dec->end) {
      return false; // Buffer overflow
   }

   memcpy(value, dec->cur, sizeof(uint32_t));
   dec->cur += sizeof(uint32_t);

   return true;
}

// Encoder functions
static inline void
apir_encoder_init(struct apir_encoder *enc, void *data, size_t size)
{
   enc->data = (uint8_t *)data;
   enc->cur = enc->data;
   enc->end = enc->data + size;
}

static inline bool
apir_encoder_set_stream(struct apir_encoder *enc, void *data,
                        size_t offset, size_t available_size)
{
   if (!enc || !data || available_size == 0) {
      return false;
   }
   if (offset > SIZE_MAX - available_size) {
      return false; // Overflow check
   }
   enc->data = (uint8_t *)data + offset;
   enc->cur = enc->data;
   enc->end = enc->data + available_size;

   return true;
}

static inline bool
apir_encode_uint32_t(struct apir_encoder *enc, uint32_t value)
{
   if (enc->cur + sizeof(uint32_t) > enc->end) {
      return false; // Buffer overflow
   }
   memcpy(enc->cur, &value, sizeof(uint32_t));
   enc->cur += sizeof(uint32_t);
   return true;
}

static inline size_t
apir_encoder_get_used_size(struct apir_encoder *enc)
{
   return enc->cur - enc->data;
}

static inline bool
apir_encoder_seek_stream(struct apir_encoder *enc, size_t offset)
{
   if (enc->cur + offset > enc->end) {
      return false; // Buffer overflow
   }
   enc->cur = enc->data + offset;
   return true;
}

// Stream access helpers (for backend forwarding)
static inline void
apir_decoder_get_stream(struct apir_decoder *dec, char **cur, const char **end)
{
   *cur = (char *)dec->cur;
   *end = (const char *)dec->end;
}

static inline void
apir_encoder_get_stream(struct apir_encoder *enc, char **cur, const char **end)
{
   *cur = (char *)enc->cur;
   *end = (const char *)enc->end;
}

// Command type and flags decoding

static inline bool
apir_decode_command_type(struct apir_decoder *dec, ApirCommandType *cmd_type)
{
   uint32_t value;
   if (!apir_decode_uint32_t(dec, &value)) {
      return false;
   }
   *cmd_type = (ApirCommandType)value;
   return true;
}

static inline bool
apir_decode_command_flags(struct apir_decoder *dec, ApirCommandFlags *cmd_flags)
{
   uint32_t value;
   if (!apir_decode_uint32_t(dec, &value)) {
      return false;
   }
   *cmd_flags = (ApirCommandFlags)value;
   return true;
}
