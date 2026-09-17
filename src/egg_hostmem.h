/*
 * egg: the VMM's hostmem BAR backing, shared with the render server as a
 * file descriptor (EGG_HOSTMEM_FD / EGG_HOSTMEM_SIZE) and mapped whole, once.
 * A blob create that carries a hostmem_offset lives at window + offset, so
 * the guest (which chose the offset at RESOURCE_MAP_BLOB) and the server share
 * the pages with no mapping step in the VMM. Used by Neptune and by Venus
 * when it runs in the render server (Windows eggs).
 */
#ifndef EGG_HOSTMEM_H
#define EGG_HOSTMEM_H

#include <stdbool.h>
#include <stdint.h>

/* Map the window if the environment names one; idempotent, thread-safe. */
void
egg_hostmem_init(void);

/* Resolve (offset, size) inside the window. False when there is no window,
 * the offset is UINT64_MAX (no placement requested) or the range does not fit.
 * *out_fd is the window's fd (not dup'ed); *out_ptr the placed address. */
bool
egg_hostmem_place(uint64_t offset, uint64_t size, void **out_ptr, int *out_fd);

#endif /* EGG_HOSTMEM_H */
