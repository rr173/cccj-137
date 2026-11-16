/*
 * ac_alloc.c - counted allocation layer with a deterministic failpoint.
 *
 * Every heap allocation performed by the library funnels through
 * ac_malloc/ac_calloc/ac_strdup so that:
 *   - live allocation / byte counts are exact (see ac_live_*),
 *   - tests can arm "the Nth allocation fails" and walk every failure
 *     branch, including branches inside import validation.
 * Real allocations and frees are counted independently; the test suite
 * requires both counters to return to zero after full teardown under
 * AddressSanitizer, not merely Python reference counts dropping.
 */
#include <stdlib.h>
#include <string.h>

#include "ac_internal.h"

static int64_t g_total_allocs;
static int64_t g_total_frees;
static int64_t g_live_allocs;
static int64_t g_live_bytes;
static int64_t g_fail_at = -1;   /* 1-based; -1 disarmed */
static int64_t g_alloc_seq;

void ac_fail_reset(void) {
  g_fail_at = -1;
  g_total_allocs = g_total_frees = 0;
  g_live_allocs = g_live_bytes = 0;
  g_alloc_seq = 0;
}

/* Disarm the failpoint without touching the allocation counters. Used
 * inside failpoint-walk loops so the cumulative real alloc/free counts
 * keep reflecting every live object across iterations. */
void ac_fail_disarm(void) {
  g_fail_at = -1;
}

void ac_fail_arm(int nth_allocation) {
  g_fail_at = nth_allocation;
}

int64_t ac_total_allocations(void) { return g_total_allocs; }
int64_t ac_total_frees(void)       { return g_total_frees; }
int64_t ac_live_allocations(void)  { return g_live_allocs; }
int64_t ac_live_bytes(void)        { return g_live_bytes; }

void* ac_malloc(size_t n) {
  ++g_alloc_seq;
  if (g_fail_at > 0 && g_alloc_seq == g_fail_at) {
    /* One-shot: disarm on firing so that failure-rollback allocations
     * inside the same call behave like a real transient OOM and are not
     * themselves forced to fail. */
    g_fail_at = -1;
    return NULL;
  }
  if (n == 0) n = 1;
  void* p = malloc(n);
  if (!p) return NULL;
  ++g_total_allocs;
  ++g_live_allocs;
  g_live_bytes += (int64_t)n;
  return p;
}

void* ac_calloc(size_t n, size_t sz) {
  /* Counted as one allocation, zero-filled. */
  if (n != 0 && sz > (size_t)-1 / n) return NULL;
  size_t total = n * sz;
  void* p = ac_malloc(total);
  if (p) memset(p, 0, total);
  return p;
}

char* ac_strdup(const char* s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char* p = (char*)ac_malloc(n);
  if (!p) return NULL;
  memcpy(p, s, n);
  return p;
}

void ac_free(void* p) {
  if (!p) return;
  free(p);
  ++g_total_frees;
  --g_live_allocs;
}

/* ------------------------------------------------------------------ */
/* Payload refcounting.                                               */
/* ------------------------------------------------------------------ */

void ac_payload_retain_schema(ac_payload* p) {
  if (p) ++p->schema_refs;
}

void ac_payload_retain_array(ac_payload* p) {
  if (p) ++p->array_refs;
}

/* ================================================================== */
/* Bit-packed validity helpers                                        */
/* ================================================================== */

uint8_t* ac_pack_validity(const uint8_t* byte_validity, int64_t length) {
  if (!byte_validity || length <= 0) return NULL;
  int64_t nbytes = (length + 7) / 8;
  uint8_t* packed = (uint8_t*)ac_calloc((size_t)nbytes, 1);
  if (!packed) return NULL;
  for (int64_t i = 0; i < length; ++i) {
    if (byte_validity[i] != 0)
      packed[i >> 3] |= (uint8_t)(1u << (i & 7)); /* LSB-first */
  }
  return packed;
}

int ac_bit_is_set(const uint8_t* bits, int64_t physical_row) {
  if (!bits) return 1;
  return (bits[physical_row >> 3] >> (physical_row & 7)) & 1;
}
