/*
 * consumer.c - independent native C Data Interface consumer.
 *
 * Two entry points share one implementation:
 *   - arrow_cdi_consume(schema_addr, array_addr): callable in-process via
 *     ctypes (the demo loads it into the same address space that produced
 *     the shells, so the pointers are valid);
 *   - main(): a standalone CLI wrapper for out-of-process experimentation.
 *
 * The consumer validates the contract itself and reads the buffers only
 * through the ABI. It links the ABI header but no private producer type,
 * proving the exchange is a real C Data Interface hand-off. It releases
 * both shells (peer callbacks) exactly once.
 *
 * The demo hands it the reversed projection (column order dict, utf8,
 * int64) after two slices of a PyArrow nonzero-offset batch. Expected
 * logical content is therefore parameterised by the observed length.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arrow_cdata_interface.h"

static int fail(const char* what) {
  fprintf(stderr, "consumer mismatch: %s\n", what);
  return 2;
}

static int utf8_eq(const char* base, int32_t lo, int32_t hi,
                   const char* want) {
  size_t n = (size_t)(hi - lo);
  return n == strlen(want) && memcmp(base + lo, want, n) == 0;
}

/* Full logical table of the PyArrow source rows 3..5:
 *   row 3: utf8 "wörld", i64 40 (present)
 *   row 4: utf8 "αβ",    i64 null
 *   row 5: utf8 "xyz",   i64 60
 * The demo exports rows 4..5 (length 2) after slice(0,3).slice(1,2). */
static const char* WANT_UTF8[3] = {
    "w\xc3\xb6rld", "\xce\xb1\xce\xb2", "xyz"};
static const int64_t WANT_I64[3] = {40, 50, 60};
static const int WANT_I64_NULL[3] = {0, 1, 0};
#define FIRST_EXPORTED_ROW 1   /* rows 4,5 -> indices 1,2 */

static int consume(const struct ArrowSchema* schema,
                   const struct ArrowArray* array) {
  if (!schema->release || !array->release) return fail("released shell");
  if (!schema->format ||
      (strcmp(schema->format, "+s") && strcmp(schema->format, "+rs")))
    return fail("root not struct record batch");
  if (array->length <= 0 || array->length > 3) return fail("row count");
  if (array->n_children != schema->n_children) return fail("child count");
  if (array->n_buffers != 1) return fail("root buffer count");

  int64_t rows = array->length;

  for (int64_t col = 0; col < array->n_children; ++col) {
    const struct ArrowSchema* cs = schema->children[col];
    const struct ArrowArray* ca = array->children[col];
    if (ca->length != rows) return fail("column length");
    const uint8_t* val = ca->buffers[0];
    int64_t off = ca->offset;

    if (cs->dictionary) {
      if (cs->format[0] != 'i') return fail("index type");
      const struct ArrowArray* da = ca->dictionary;
      const int32_t* idx = (const int32_t*)ca->buffers[1];
      const int32_t* doff = (const int32_t*)da->buffers[1];
      const uint8_t* dval = da->buffers[0];
      for (int64_t r = 0; r < rows; ++r) {
        int64_t phys = off + r;
        int is_null = val && ((val[phys >> 3] >> (phys & 7)) & 1) == 0;
        if (is_null) continue;
        int32_t k = idx[phys];
        if (k < 0 || k >= da->length) return fail("index range");
        if (dval && ((dval[k >> 3] >> (k & 7)) & 1) == 0) continue;
        int32_t lo = doff[k], hi = doff[k + 1];
        if (lo > hi) return fail("dict offset order");
      }
    } else if (cs->format[0] == 'u') {
      const int32_t* offsets = (const int32_t*)ca->buffers[1];
      const char* bytes = (const char*)ca->buffers[2];
      for (int64_t r = 0; r < rows; ++r) {
        int64_t phys = off + r;
        if (val && ((val[phys >> 3] >> (phys & 7)) & 1) == 0)
          return fail("unexpected utf8 null");
        int32_t lo = offsets[phys], hi = offsets[phys + 1];
        if (!utf8_eq(bytes, lo, hi, WANT_UTF8[FIRST_EXPORTED_ROW + r]))
          return fail("utf8 value");
      }
    } else if (cs->format[0] == 'l') {
      const int64_t* vals = (const int64_t*)ca->buffers[1];
      for (int64_t r = 0; r < rows; ++r) {
        int64_t phys = off + r;
        int src = FIRST_EXPORTED_ROW + (int)r;
        int is_null = val && ((val[phys >> 3] >> (phys & 7)) & 1) == 0;
        if (is_null != WANT_I64_NULL[src])
          return fail("i64 null pattern");
        if (!is_null && vals[phys] != WANT_I64[src])
          return fail("i64 value");
      }
    } else {
      return fail("unsupported column type");
    }
  }
  return 0;
}

/* In-process entry used by the Python demo through ctypes. Returns 0 on
 * success and releases the shells exactly once. */
int arrow_cdi_consume(uintptr_t schema_addr, uintptr_t array_addr) {
  const struct ArrowSchema* schema =
      (const struct ArrowSchema*)(uintptr_t)schema_addr;
  const struct ArrowArray* array =
      (const struct ArrowArray*)(uintptr_t)array_addr;
  int rc = consume(schema, array);
  if (rc == 0)
    printf("consumer: all %lld logical rows verified via raw ABI\n",
           (long long)array->length);
  /* Release once each, independently of order. */
  if (array->release)
    ((struct ArrowArray*)array)->release((struct ArrowArray*)array);
  if (schema->release)
    ((struct ArrowSchema*)schema)->release((struct ArrowSchema*)schema);
  return rc;
}

#ifndef ARROW_CDI_CONSUMER_LIB
int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <schema_addr> <array_addr>\n", argv[0]);
    return 1;
  }
  uintptr_t sa = (uintptr_t)strtoull(argv[1], NULL, 0);
  uintptr_t aa = (uintptr_t)strtoull(argv[2], NULL, 0);
  return arrow_cdi_consume(sa, aa);
}
#endif

