/*
 * egg: see egg_blob_sync.h.  A port of the in-process path in
 * egg-runner/bridge/egg_virgl.m (egg_virgl_track_blob_sync / sync_blobs /
 * untrack_blob_sync): same table, same locking contract, same cadence.
 *
 * One deliberate difference: the direction a blob is copied is measured rather
 * than guessed from its blob_id — see egg_blob_sync_guest_wrote.  The original
 * heuristic breaks as soon as an application allocates host-visible memory
 * before its first fence, which is common and which W11-2 hit immediately.
 */

#include "egg_blob_sync.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Matches MAX_BLOB_SYNCS in the runner's bridge. */
#define EGG_BLOB_SYNC_MAX 64

/* Blobs larger than this are swapchain images: they are GPU-written, and the
 * post-fence pass skips them so a frame is never half-copied.  Same number as
 * the in-process path. */
#define EGG_BLOB_SYNC_LARGE_BYTES 500000

/* Copy cadence, milliseconds.  The in-process path is driven by the runner's
 * 5 ms gl-thread poll; here we own the timer, so the default matches it and
 * EGG_VENUS_BLOB_SYNC_MS overrides. */
#define EGG_BLOB_SYNC_DEFAULT_MS 5

/* mincore() granularity used to notice a mapping that has gone away. */
#define EGG_BLOB_SYNC_PAGE 16384

struct egg_blob_sync_entry {
   uint32_t res_id;
   uint64_t blob_id;
   void *gpu_ptr;   /* the VkDeviceMemory's CPU mapping */
   void *guest_ptr; /* the same bytes in the hostmem window */
   /* Copies of both sides as they were after the last pass, used only until
    * the direction is known.  NULL for blobs too large to shadow. */
   void *guest_shadow;
   void *gpu_shadow;
   enum {
      EGG_BLOB_DIR_UNKNOWN = 0,
      EGG_BLOB_DIR_GPU_WRITES, /* proven: pull gpu -> guest, for good */
   } dir;
   uint32_t last_gpu0;
   uint32_t last_guest0;
   uint64_t size;
   bool active;
};

/* Serialises every access.  untrack() takes it for the whole call, so it can
 * only return once an in-progress memcpy on that entry has finished — which is
 * what makes it safe for the caller to free the memory right after. */
/* EGG_VENUS_BLOB_SYNC_TRACE=1 logs each blob's direction as it is decided. */
static bool egg_blob_sync_trace;

/* EGG_VENUS_BLOB_SYNC_NO_PUSH=1 never copies guest -> GPU.  Venus rewrites its
 * fence-feedback slots before every submission, so a push always has something
 * to copy and will overwrite the completion the GPU just wrote before the next
 * pass can notice it.  Pull-only makes feedback work at the cost of guest
 * writes to host-visible memory, which is the trade the in-process path makes
 * implicitly by classifying the feedback pool as GPU-written for its lifetime. */
static bool egg_blob_sync_no_push;

static pthread_mutex_t egg_blob_sync_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct egg_blob_sync_entry egg_blob_syncs[EGG_BLOB_SYNC_MAX];
static int egg_blob_sync_count;
static pthread_t egg_blob_sync_thread;
static bool egg_blob_sync_thread_started;

static unsigned
egg_blob_sync_period_ms(void)
{
   static unsigned cached;
   if (cached)
      return cached;
   const char *env = getenv("EGG_VENUS_BLOB_SYNC_MS");
   long v = env ? strtol(env, NULL, 10) : 0;
   if (v <= 0 || v > 1000)
      v = EGG_BLOB_SYNC_DEFAULT_MS;
   cached = (unsigned)v;
   return cached;
}

/* Which way a blob is copied.
 *
 * A periodic copy can only stand in for shared memory if each blob is written
 * by ONE side; if both write it, whichever copy runs last wins.  Real Venus
 * blobs are single-direction — fence feedback and swapchain images are written
 * by the GPU, staging and uniform buffers by the guest — so the job is to find
 * out which, per blob.
 *
 * The in-process path guesses from identity: lowest blob_id is assumed to be
 * the feedback pool, anything swapchain-sized is GPU-written, the rest is
 * guest-written.  That breaks as soon as an application allocates host-visible
 * memory before its first fence: it takes the lowest id, the real feedback
 * pool is pushed backwards, and no fence is ever seen to signal.  Observed on
 * W11-2.
 *
 * Watching both sides and latching the first one to change does not work
 * either, because the guest legitimately *initialises* its feedback slots
 * before the GPU ever writes them — that latches the pool guest-written and
 * loses every completion.  Also observed on W11-2.
 *
 * What is actually reliable: a change on the GPU side is proof.  Nothing
 * writes the VkDeviceMemory except the GPU and our own push, and a push
 * refreshes the shadow, so any other difference there means the GPU wrote it —
 * latch GPU-written for good.  Until that proof arrives, push whatever the
 * guest changes, which is right for staging buffers and harmless for a
 * feedback slot the guest is still setting up.  Caller holds the lock. */
static bool
egg_blob_sync_changed(const void *now, const void *shadow, uint64_t size)
{
   return shadow && memcmp(now, shadow, size) != 0;
}

static bool
egg_blob_sync_ptr_valid(void *ptr, uint64_t size)
{
   if (!ptr || !size)
      return false;
   char vec;
   uintptr_t page = (uintptr_t)ptr & ~(uintptr_t)(EGG_BLOB_SYNC_PAGE - 1);
   return mincore((void *)page, EGG_BLOB_SYNC_PAGE, &vec) == 0;
}

/* Log the first word that differs from the shadow, with its offset, so a write
 * at a slot offset is as visible as one at the start of the page. */
static void
egg_blob_sync_trace_side(const struct egg_blob_sync_entry *e,
                         const char *side,
                         const void *now,
                         const void *shadow)
{
   if (!shadow)
      return;
   const uint32_t *a = now, *b = shadow;
   for (uint64_t i = 0; i < e->size / sizeof(*a); i++) {
      if (a[i] == b[i])
         continue;
      fprintf(stderr, "egg blob sync: res %u %s[%llu] %08x->%08x\n", e->res_id, side,
              (unsigned long long)(i * sizeof(*a)), b[i], a[i]);
      return;
   }
}

/* Push the guest's writes into the GPU's copy of every blob it still owns.
 *
 * The periodic thread cannot do this on its own.  A guest that writes a
 * staging buffer and submits in the next instruction beats a 5 ms poll every
 * time, and the GPU then reads whatever was there before — zeroes, usually.
 * The same race is what silently breaks fences: Venus's feedback pool holds
 * the value the GPU is told to copy into the fence's slot, the guest writes it
 * once at pool setup, and if the push has not happened by the first submit the
 * GPU copies a zero into the slot.  Nothing on the GPU side ever changes after
 * that, so there is no write to detect and no fence to report — the fence just
 * never signals, and whether it signals at all comes down to whether the poll
 * happened to fall between the write and the submit.
 *
 * Vulkan's own ordering rule says what to do: host writes to mapped memory are
 * visible to a submission if they happen before it.  So push at the submit,
 * not on a timer. */
void
egg_blob_sync_push_before_gpu(void)
{
   pthread_mutex_lock(&egg_blob_sync_mutex);
   for (int i = 0; i < egg_blob_sync_count; i++) {
      struct egg_blob_sync_entry *e = &egg_blob_syncs[i];
      if (!e->active || e->dir == EGG_BLOB_DIR_GPU_WRITES || egg_blob_sync_no_push)
         continue;
      if (!egg_blob_sync_ptr_valid(e->gpu_ptr, e->size) ||
          !egg_blob_sync_ptr_valid(e->guest_ptr, e->size)) {
         e->active = false;
         continue;
      }
      if (!egg_blob_sync_changed(e->guest_ptr, e->guest_shadow, e->size))
         continue;

      memcpy(e->gpu_ptr, e->guest_ptr, e->size);
      if (egg_blob_sync_trace)
         fprintf(stderr, "egg blob sync: res %u pushed at submit\n", e->res_id);
      /* Both shadows, so the periodic pass does not read our own push back as
       * a write from the GPU and latch the blob the wrong way round. */
      if (e->guest_shadow)
         memcpy(e->guest_shadow, e->guest_ptr, e->size);
      if (e->gpu_shadow)
         memcpy(e->gpu_shadow, e->gpu_ptr, e->size);
   }
   pthread_mutex_unlock(&egg_blob_sync_mutex);
}

void
egg_blob_sync_run(bool small_only)
{
   pthread_mutex_lock(&egg_blob_sync_mutex);
   for (int i = 0; i < egg_blob_sync_count; i++) {
      struct egg_blob_sync_entry *e = &egg_blob_syncs[i];
      if (!e->active)
         continue;
      if (small_only && e->size > EGG_BLOB_SYNC_LARGE_BYTES)
         continue;
      if (!egg_blob_sync_ptr_valid(e->gpu_ptr, e->size) ||
          !egg_blob_sync_ptr_valid(e->guest_ptr, e->size)) {
         fprintf(stderr, "egg blob sync: res %u unmapped underneath us — dropping\n",
                 e->res_id);
         e->active = false;
         continue;
      }
      if (egg_blob_sync_trace) {
         /* Report movement anywhere in the blob, not just at offset 0: a
          * feedback pool hands out slots at an offset the guest picks, so
          * watching the first word alone reports "nothing happened" for the
          * one blob whose behaviour is in question. */
         egg_blob_sync_trace_side(e, "gpu", e->gpu_ptr, e->gpu_shadow);
         egg_blob_sync_trace_side(e, "guest", e->guest_ptr, e->guest_shadow);
      }

      if (e->dir == EGG_BLOB_DIR_UNKNOWN) {
         /* No shadow means too large to watch: that is a swapchain image. */
         if (!e->gpu_shadow ||
             egg_blob_sync_changed(e->gpu_ptr, e->gpu_shadow, e->size)) {
            e->dir = EGG_BLOB_DIR_GPU_WRITES;
            if (egg_blob_sync_trace)
               fprintf(stderr, "egg blob sync: res %u is GPU-written\n", e->res_id);
         }
      }

      if (e->dir == EGG_BLOB_DIR_GPU_WRITES) {
         memcpy(e->guest_ptr, e->gpu_ptr, e->size);
      } else if (!egg_blob_sync_no_push &&
                 egg_blob_sync_changed(e->guest_ptr, e->guest_shadow, e->size)) {
         memcpy(e->gpu_ptr, e->guest_ptr, e->size);
      }

      if (e->guest_shadow)
         memcpy(e->guest_shadow, e->guest_ptr, e->size);
      if (e->gpu_shadow)
         memcpy(e->gpu_shadow, e->gpu_ptr, e->size);
   }
   pthread_mutex_unlock(&egg_blob_sync_mutex);
}

static void *
egg_blob_sync_loop(void *arg)
{
   (void)arg;
   const unsigned ms = egg_blob_sync_period_ms();
   const struct timespec period = {
      .tv_sec = ms / 1000,
      .tv_nsec = (long)(ms % 1000) * 1000000L,
   };
   for (;;) {
      nanosleep(&period, NULL);
      egg_blob_sync_run(false);
   }
   return NULL;
}

void
egg_blob_sync_track(uint32_t res_id,
                    uint64_t blob_id,
                    void *gpu_ptr,
                    void *guest_ptr,
                    uint64_t size)
{
   if (!gpu_ptr || !guest_ptr || !size)
      return;

   pthread_mutex_lock(&egg_blob_sync_mutex);

   int slot = -1;
   for (int i = 0; i < egg_blob_sync_count; i++) {
      if (egg_blob_syncs[i].active && egg_blob_syncs[i].res_id == res_id) {
         slot = i; /* re-created under the same id: replace it */
         break;
      }
      if (!egg_blob_syncs[i].active && slot < 0)
         slot = i;
   }
   if (slot < 0 && egg_blob_sync_count < EGG_BLOB_SYNC_MAX)
      slot = egg_blob_sync_count++;

   if (slot < 0) {
      fprintf(stderr, "egg blob sync: no free slot for res %u (max %d)\n", res_id,
              EGG_BLOB_SYNC_MAX);
      pthread_mutex_unlock(&egg_blob_sync_mutex);
      return;
   }

   free(egg_blob_syncs[slot].guest_shadow);
   free(egg_blob_syncs[slot].gpu_shadow);
   void *guest_shadow = NULL, *gpu_shadow = NULL;
   if (size <= EGG_BLOB_SYNC_LARGE_BYTES) {
      guest_shadow = malloc(size);
      gpu_shadow = malloc(size);
      if (guest_shadow)
         memcpy(guest_shadow, guest_ptr, size);
      if (gpu_shadow)
         memcpy(gpu_shadow, gpu_ptr, size);
   }
   egg_blob_syncs[slot] = (struct egg_blob_sync_entry){
      .res_id = res_id,
      .blob_id = blob_id,
      .gpu_ptr = gpu_ptr,
      .guest_ptr = guest_ptr,
      .guest_shadow = guest_shadow,
      .gpu_shadow = gpu_shadow,
      .size = size,
      .active = true,
   };

   if (!egg_blob_sync_thread_started) {
      const char *t = getenv("EGG_VENUS_BLOB_SYNC_TRACE");
      egg_blob_sync_trace = t && t[0] == '1';
      const char *np = getenv("EGG_VENUS_BLOB_SYNC_NO_PUSH");
      egg_blob_sync_no_push = np && np[0] == '1';
   }
   if (egg_blob_sync_trace) {
      fprintf(stderr,
              "egg blob sync: track res %u blob_id %llu gpu %p guest %p size %llu\n",
              res_id, (unsigned long long)blob_id, gpu_ptr, guest_ptr,
              (unsigned long long)size);
      if (pthread_create(&egg_blob_sync_thread, NULL, egg_blob_sync_loop, NULL) == 0) {
         pthread_detach(egg_blob_sync_thread);
         egg_blob_sync_thread_started = true;
      } else {
         fprintf(stderr, "egg blob sync: failed to start the sync thread\n");
      }
   }
   pthread_mutex_unlock(&egg_blob_sync_mutex);
}

void
egg_blob_sync_untrack(uint32_t res_id)
{
   pthread_mutex_lock(&egg_blob_sync_mutex);
   for (int i = 0; i < egg_blob_sync_count; i++) {
      if (egg_blob_syncs[i].active && egg_blob_syncs[i].res_id == res_id) {
         egg_blob_syncs[i].active = false;
         free(egg_blob_syncs[i].guest_shadow);
         free(egg_blob_syncs[i].gpu_shadow);
         egg_blob_syncs[i].guest_shadow = NULL;
         egg_blob_syncs[i].gpu_shadow = NULL;
         break;
      }
   }
   pthread_mutex_unlock(&egg_blob_sync_mutex);
}

void
egg_blob_sync_untrack_by_gpu_ptr(void *gpu_ptr)
{
   if (!gpu_ptr)
      return;
   pthread_mutex_lock(&egg_blob_sync_mutex);
   for (int i = 0; i < egg_blob_sync_count; i++) {
      if (egg_blob_syncs[i].active && egg_blob_syncs[i].gpu_ptr == gpu_ptr) {
         egg_blob_syncs[i].active = false;
         free(egg_blob_syncs[i].guest_shadow);
         free(egg_blob_syncs[i].gpu_shadow);
         egg_blob_syncs[i].guest_shadow = NULL;
         egg_blob_syncs[i].gpu_shadow = NULL;
         break;
      }
   }
   pthread_mutex_unlock(&egg_blob_sync_mutex);
}
