#pragma once

#include <unistd.h>

#include "apir-protocol.h"

// Forward declaration to break circular dependency
struct apir_context;

void apir_HandShake(struct apir_context *ctx, ApirCommandFlags flags);
void apir_Forward(struct apir_context *ctx, ApirCommandFlags flags);
void apir_LoadLibrary(struct apir_context *ctx, ApirCommandFlags flags);

typedef void (*apir_dispatch_command_t) (struct apir_context *, ApirCommandFlags);
static inline apir_dispatch_command_t apir_protocol_dispatch_command(ApirCommandType type)
{
    switch (type) {
    case APIR_COMMAND_TYPE_HandShake: return apir_HandShake;
    case APIR_COMMAND_TYPE_LoadLibrary: return apir_LoadLibrary;
    case APIR_COMMAND_TYPE_Forward: return apir_Forward;
    default: return NULL;
    }
}
