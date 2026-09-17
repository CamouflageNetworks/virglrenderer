#include "egg_hostmem.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "c11/threads.h"

#include "virgl_util.h"

static struct {
   int fd;
   void *ptr;
   uint64_t size;
} egg_hostmem = { .fd = -1 };

static once_flag egg_hostmem_once = ONCE_FLAG_INIT;

static void
egg_hostmem_init_once(void)
{
   const char *fd_env = getenv("EGG_HOSTMEM_FD");
   const char *size_env = getenv("EGG_HOSTMEM_SIZE");
   if (!fd_env || !size_env)
      return;
   int fd = atoi(fd_env);
   uint64_t size = strtoull(size_env, NULL, 0);
   if (fd < 0 || !size)
      return;
   void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (ptr == MAP_FAILED) {
      virgl_error("egg hostmem: mmap(fd=%d, %" PRIu64 " bytes) failed: %s\n", fd, size,
                strerror(errno));
      return;
   }
   egg_hostmem.fd = fd;
   egg_hostmem.ptr = ptr;
   egg_hostmem.size = size;
   virgl_info("egg hostmem: shared window fd=%d size=%" PRIu64 " MiB mapped at %p\n", fd,
             size >> 20, ptr);
}

void
egg_hostmem_init(void)
{
   call_once(&egg_hostmem_once, egg_hostmem_init_once);
}

bool
egg_hostmem_place(uint64_t offset, uint64_t size, void **out_ptr, int *out_fd)
{
   if (offset == UINT64_MAX || !egg_hostmem.ptr)
      return false;
   if (offset > egg_hostmem.size || size > egg_hostmem.size - offset) {
      virgl_warn("egg hostmem: blob offset %" PRIu64 " size %" PRIu64
                " outside the %" PRIu64 "-byte window\n",
                offset, size, egg_hostmem.size);
      return false;
   }
   *out_ptr = (uint8_t *)egg_hostmem.ptr + offset;
   *out_fd = egg_hostmem.fd;
   return true;
}
