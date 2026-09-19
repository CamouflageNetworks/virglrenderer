/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Neptune shared-blob bookkeeping tests.  These exercise
 * npt_context_register_pending_blob directly against a minimal
 * hand-built context -- no render server, no D3D libraries -- to pin the
 * security-relevant invariants of the pending-blob table: the untrusted
 * guest can drive SHARED_EXPORT_BLOB, and the table must neither grow
 * without bound nor leak an fd when a blob_id is re-exported before it is
 * claimed.  Uses the same assert-lite harness as test_neptune_init.c.
 */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "c11/threads.h"
#include "util/hash_table.h"
#include "neptune/npt_context.h"
#include "neptune/npt_shared.h"

/* Must match NPT_MAX_PENDING_BLOBS in src/neptune/npt_context.c. */
#define TEST_MAX_PENDING_BLOBS 256u

static int tests_run = 0;
static int tests_failed = 0;

#define RUN_TEST(fn)                                     \
   do {                                                  \
      tests_run++;                                       \
      printf("  %-50s ", #fn);                           \
      fflush(stdout);                                    \
      if (fn()) {                                        \
         printf("PASS\n");                               \
      } else {                                           \
         printf("FAIL\n");                               \
         tests_failed++;                                 \
      }                                                  \
   } while (0)

#define EXPECT(cond)                                     \
   do {                                                  \
      if (!(cond)) {                                     \
         fprintf(stderr, "    EXPECT failed: %s:%d: %s\n", \
                 __FILE__, __LINE__, #cond);             \
         return 0;                                       \
      }                                                  \
   } while (0)

/* The pending-blob table is keyed by a pointer to the uint64 blob_id;
 * any consistent hash of the pointed-to value works. */
static uint32_t
test_hash_u64(const void *key)
{
   const uint64_t v = *(const uint64_t *)key;
   return (uint32_t)(v ^ (v >> 32));
}

static bool
test_equal_u64(const void *a, const void *b)
{
   return *(const uint64_t *)a == *(const uint64_t *)b;
}

/* A context with only the fields npt_context_register_pending_blob
 * touches initialised. */
static bool
minimal_ctx_init(struct npt_context *ctx)
{
   memset(ctx, 0, sizeof(*ctx));
   if (mtx_init(&ctx->pending_blob_mutex, mtx_plain) != thrd_success)
      return false;
   ctx->pending_blob_table =
      _mesa_hash_table_create(NULL, test_hash_u64, test_equal_u64);
   return ctx->pending_blob_table != NULL;
}

static void
minimal_ctx_fini(struct npt_context *ctx)
{
   hash_table_foreach(ctx->pending_blob_table, entry) {
      struct npt_pending_blob *pb = entry->data;
      if (pb->fd >= 0)
         close(pb->fd);
      free(pb);
   }
   _mesa_hash_table_destroy(ctx->pending_blob_table, NULL);
   mtx_destroy(&ctx->pending_blob_mutex);
}

static bool
fd_is_open(int fd)
{
   return fcntl(fd, F_GETFD) != -1;
}

/* A guest that loops SHARED_EXPORT_BLOB with fresh blob_ids and never
 * claims them must be capped, not allowed to grow the table forever. */
static int
test_pending_blob_cap(void)
{
   struct npt_context ctx;
   EXPECT(minimal_ctx_init(&ctx));

   for (uint64_t id = 1; id <= TEST_MAX_PENDING_BLOBS; id++)
      EXPECT(npt_context_register_pending_blob(&ctx, id, NPT_SHARED_FD_TYPE,
                                               -1, 0, 0));

   EXPECT(_mesa_hash_table_num_entries(ctx.pending_blob_table) ==
          TEST_MAX_PENDING_BLOBS);

   /* One past the cap is rejected and the table does not grow. */
   EXPECT(!npt_context_register_pending_blob(&ctx, TEST_MAX_PENDING_BLOBS + 1,
                                             NPT_SHARED_FD_TYPE, -1, 0, 0));
   EXPECT(_mesa_hash_table_num_entries(ctx.pending_blob_table) ==
          TEST_MAX_PENDING_BLOBS);

   minimal_ctx_fini(&ctx);
   return 1;
}

/* Re-exporting an unclaimed blob_id must replace the entry and close the
 * stale entry's fd rather than orphan it. */
static int
test_pending_blob_duplicate_closes_old_fd(void)
{
   struct npt_context ctx;
   EXPECT(minimal_ctx_init(&ctx));

   int fd_a = dup(STDERR_FILENO);
   EXPECT(fd_a >= 0);
   EXPECT(npt_context_register_pending_blob(&ctx, 7, NPT_SHARED_FD_TYPE,
                                            fd_a, 0, 0));

   int fd_b = dup(STDERR_FILENO);
   EXPECT(fd_b >= 0 && fd_b != fd_a);
   EXPECT(npt_context_register_pending_blob(&ctx, 7, NPT_SHARED_FD_TYPE,
                                            fd_b, 0, 0));

   /* Same key => no growth; old fd closed, new fd retained. */
   EXPECT(_mesa_hash_table_num_entries(ctx.pending_blob_table) == 1);
   EXPECT(!fd_is_open(fd_a));
   EXPECT(fd_is_open(fd_b));

   minimal_ctx_fini(&ctx);
   /* fini closed fd_b. */
   EXPECT(!fd_is_open(fd_b));
   return 1;
}

/* A re-export that lands on a full table must still succeed (it replaces,
 * not adds) so a legitimate retry of an unclaimed blob is never wedged. */
static int
test_pending_blob_duplicate_at_cap(void)
{
   struct npt_context ctx;
   EXPECT(minimal_ctx_init(&ctx));

   for (uint64_t id = 1; id <= TEST_MAX_PENDING_BLOBS; id++)
      EXPECT(npt_context_register_pending_blob(&ctx, id, NPT_SHARED_FD_TYPE,
                                               -1, 0, 0));

   /* blob_id 1 already present: re-export replaces it even though the
    * table is at the cap. */
   EXPECT(npt_context_register_pending_blob(&ctx, 1, NPT_SHARED_FD_TYPE,
                                            -1, 0, 0));
   EXPECT(_mesa_hash_table_num_entries(ctx.pending_blob_table) ==
          TEST_MAX_PENDING_BLOBS);

   minimal_ctx_fini(&ctx);
   return 1;
}

int
main(void)
{
   printf("Neptune shared-blob tests:\n");

   RUN_TEST(test_pending_blob_cap);
   RUN_TEST(test_pending_blob_duplicate_closes_old_fd);
   RUN_TEST(test_pending_blob_duplicate_at_cap);

   printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
   return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
