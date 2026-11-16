/*
 * ac_payload.c - payload lifecycle, C Data Interface validation and
 * move/commit of peer structs, plus export-shell release callbacks.
 *
 * The import sequence is strictly two-phased:
 *   phase 1  ac_validate_all  - read-only; the peer-owned source structs
 *                               are never mutated, so rejection leaves the
 *                               caller's ownership completely intact;
 *   phase 2  ac_commit        - allocates the payload and one holder per
 *                               moved struct. Every allocation is wrapped
 *                               so that ENOMEM unwinds the holders already
 *                               taken (restoring the peer callbacks without
 *                               invoking them) and returns the source to
 *                               the caller in its original released-state.
 */
#include <stdlib.h>
#include <string.h>

#include "ac_internal.h"

/* ================================================================== */
/* Source holders                                                     */
/* ================================================================== */

static ac_status holder_for_schema(struct ArrowSchema* s, ac_source** out) {
  ac_source* h = (ac_source*)ac_calloc(1, sizeof(*h));
  if (!h) return AC_ENOMEM;
  h->schema = s;
  h->orig_s_release = s->release;
  h->orig_s_private = s->private_data;
  s->private_data = h;
  *out = h;
  return AC_OK;
}

static ac_status holder_for_array(struct ArrowArray* a, ac_source** out) {
  ac_source* h = (ac_source*)ac_calloc(1, sizeof(*h));
  if (!h) return AC_ENOMEM;
  h->array = a;
  h->orig_a_release = a->release;
  h->orig_a_private = a->private_data;
  a->private_data = h;
  *out = h;
  return AC_OK;
}

/* Undo a holder that never completed the move: restore the peer
 * private_data and free the holder without invoking any peer callback.
 * Used while rolling back a failed commit. */
static void holder_abort(ac_source* h) {
  if (!h) return;
  if (h->schema) h->schema->private_data = h->orig_s_private;
  if (h->array)  h->array->private_data  = h->orig_a_private;
  ac_free(h);
}

/* Complete the move for a held struct: mark it released per the contract
 * ("The release callback must set the release field to NULL"). */
static void holder_mark_moved(ac_source* h) {
  if (!h) return;
  if (h->schema) h->schema->release = NULL;
  if (h->array)  h->array->release  = NULL;
}

/* Final destruction of a moved struct after our payload side is gone.
 * Restores the original callback/private pair and invokes the peer
 * release exactly once. */
static void holder_destroy(ac_source* h) {
  if (!h) return;
  if (h->schema) {
    struct ArrowSchema* s = h->schema;
    s->release = h->orig_s_release;
    s->private_data = h->orig_s_private;
    if (s->release) s->release(s);
  } else if (h->array) {
    struct ArrowArray* a = h->array;
    a->release = h->orig_a_release;
    a->private_data = h->orig_a_private;
    if (a->release) a->release(a);
  }
  ac_free(h);
}

/* ================================================================== */
/* Refcounting and teardown                                           */
/* ================================================================== */

void ac_payload_retain_both(ac_payload* p) {
  if (!p) return;
  ++p->schema_refs;
  ++p->array_refs;
}

static void free_library_buffers(ac_payload* p) {
  if (!p->owns_buffers) return;
  for (int64_t i = 0; i < p->n_owned_blocks; ++i) {
    ac_free(p->owned_blocks[i]);
  }
  ac_free(p->owned_blocks);
  p->owned_blocks = NULL;
  p->n_owned_blocks = 0;
}

static void release_array_side(ac_payload* p) {
  free_library_buffers(p);
  if (p->sources.root_array_holder)
    holder_destroy(p->sources.root_array_holder);
  for (int64_t i = 0; i < p->ncols; ++i) {
    ac_column* c = &p->cols[i];
    holder_destroy(c->col_array_holder);
    holder_destroy(c->dict_array_holder);
    c->col_array_holder = c->dict_array_holder = NULL;
  }
}

static void release_schema_side(ac_payload* p) {
  if (p->sources.root_schema_holder)
    holder_destroy(p->sources.root_schema_holder);
  for (int64_t i = 0; i < p->ncols; ++i) {
    ac_column* c = &p->cols[i];
    holder_destroy(c->col_schema_holder);
    holder_destroy(c->dict_schema_holder);
    ac_free(c->name);
  }
  ac_free(p->cols);
}

void ac_payload_release_array(ac_payload* p) {
  if (!p) return;
  --p->array_refs;
  if (p->array_refs > 0) return;
  release_array_side(p);
  if (p->schema_refs == 0) {
    release_schema_side(p);
    ac_free(p);
  }
}

void ac_payload_release_schema(ac_payload* p) {
  if (!p) return;
  --p->schema_refs;
  if (p->schema_refs > 0) return;
  if (p->array_refs == 0) {
    release_schema_side(p);
    ac_free(p);
  }
}

int ac_validity_is_set(const uint8_t* validity, int has_validity,
                       int64_t physical_row) {
  if (!has_validity || !validity) return 1;
  return ac_bit_is_set(validity, physical_row);
}

/* ================================================================== */
/* Phase 1: read-only validation                                      */
/* ================================================================== */

static int format_is(const struct ArrowSchema* s, char c) {
  return s->format && s->format[0] == c && s->format[1] == '\0';
}

static ac_status validate_common_array(const struct ArrowArray* a) {
  /* "The length and offset fields ... must be non-negative." */
  if (a->length < 0 || a->offset < 0) return AC_ELAYOUT;
  if (a->null_count < -1) return AC_ELAYOUT;
  if (a->n_buffers < 0 || a->n_children < 0) return AC_ELAYOUT;
  return AC_OK;
}

/* Distinguish "outside the supported subset" (AC_EUNSUPPORTED) from a
 * contract-level layout violation (AC_ELAYOUT). */
static ac_status classify_column(const struct ArrowSchema* s, ac_kind* kind) {
  if (s->dictionary) {
    if (!format_is(s, 'i')) return AC_EUNSUPPORTED; /* non-int32 indices */
    const struct ArrowSchema* d = s->dictionary;
    if (!format_is(d, 'u')) return AC_EUNSUPPORTED; /* non-utf8 values */
    if (d->n_children != 0) return AC_ELAYOUT;
    *kind = AC_KIND_DICT;
    return AC_OK;
  }
  if (format_is(s, 'l')) { *kind = AC_KIND_I64; return AC_OK; }
  if (format_is(s, 'u')) { *kind = AC_KIND_UTF8; return AC_OK; }
  return AC_EUNSUPPORTED;
}

static ac_status validate_utf8_array(const struct ArrowArray* a) {
  ac_status st = validate_common_array(a);
  if (st) return st;
  if (a->n_buffers != 3) return AC_ELAYOUT;
  if (a->n_children != 0) return AC_ELAYOUT;
  const int32_t* offsets = (const int32_t*)a->buffers[1];
  if (!offsets) return AC_ELAYOUT;
  /* Offsets are int32 *element* offsets into the byte buffer (C Data
   * Interface "Variable-size binary elements"), never array byte offsets. */
  int64_t base = a->offset;
  int32_t prev = offsets[base];
  if (prev < 0) return AC_ELAYOUT;
  for (int64_t i = 1; i <= a->length; ++i) {
    int32_t cur = offsets[base + i];
    if (cur < prev) return AC_ELAYOUT;
    prev = cur;
  }
  if (prev != 0 && a->buffers[2] == NULL) return AC_ELAYOUT;
  return AC_OK;
}

static ac_status validate_primitive_array(const struct ArrowArray* a,
                                          int64_t expected_buffers) {
  ac_status st = validate_common_array(a);
  if (st) return st;
  if (a->n_buffers != expected_buffers) return AC_ELAYOUT;
  if (a->n_children != 0) return AC_ELAYOUT;
  if (!a->buffers[1]) return AC_ELAYOUT;
  return AC_OK;
}

/* Index range validation. Null rows are skipped before touching the
 * index value: arbitrary bits under a null index must never be read as
 * an out-of-range integer. */
static ac_status validate_indices(const struct ArrowArray* idx,
                                  int64_t dict_length) {
  ac_status st = validate_primitive_array(idx, 2);
  if (st) return st;
  const uint8_t* val = idx->buffers[0] ? (const uint8_t*)idx->buffers[0] : NULL;
  const int32_t* indices = (const int32_t*)idx->buffers[1];
  for (int64_t i = 0; i < idx->length; ++i) {
    int64_t phys = idx->offset + i;
    /* Validity is bit-packed; a null row is skipped before reading the
     * index, so arbitrary bits under a null index are never interpreted. */
    if (val && !ac_bit_is_set(val, phys)) continue;
    int32_t v = indices[phys];
    if (v < 0 || (int64_t)v >= dict_length) return AC_ELAYOUT;
  }
  return AC_OK;
}

static ac_status ac_validate_all(const struct ArrowSchema* schema,
                                 const struct ArrowArray* array,
                                 ac_kind* kinds, int* nullable) {
  if (!schema->release || !array->release) return AC_ESTATE;
  if (!schema->format) return AC_ELAYOUT;
  /* Top level: struct record batch. Accept both "+s" (what PyArrow emits
   * for RecordBatch export) and "+rs" (dense non-nullable spelling). */
  if (strcmp(schema->format, "+s") != 0 &&
      strcmp(schema->format, "+rs") != 0) {
    return AC_EUNSUPPORTED;
  }
  ac_status st = validate_common_array(array);
  if (st) return st;
  if (array->n_buffers != 1) return AC_ELAYOUT;
  if (array->offset != 0) return AC_ELAYOUT;
  if (schema->n_children < 0) return AC_ELAYOUT;
  if (array->n_children != schema->n_children) return AC_ELAYOUT;

  for (int64_t i = 0; i < schema->n_children; ++i) {
    const struct ArrowSchema* cs = schema->children[i];
    const struct ArrowArray* ca = array->children[i];
    if (!cs || !ca || !cs->release || !ca->release) return AC_ELAYOUT;
    if (ca->length != array->length) return AC_ELAYOUT;
    ac_kind k;
    st = classify_column(cs, &k);
    if (st) return st;
    kinds[i] = k;
    nullable[i] = (cs->flags & ARROW_FLAG_NULLABLE) != 0;

    if (k == AC_KIND_I64) {
      st = validate_primitive_array(ca, 2);
    } else if (k == AC_KIND_UTF8) {
      st = validate_utf8_array(ca);
    } else {
      if (!cs->dictionary || !ca->dictionary || !ca->dictionary->release)
        return AC_ELAYOUT;
      st = validate_indices(ca, ca->dictionary->length);
      if (st) return st;
      st = validate_utf8_array(ca->dictionary);
      if (st) return st;
      if (ca->dictionary->n_children != 0) return AC_ELAYOUT;
    }
    if (st) return st;
  }
  return AC_OK;
}

/* ================================================================== */
/* Phase 2: commit                                                    */
/* ================================================================== */

static void fill_column(ac_column* c, const struct ArrowSchema* cs,
                        const struct ArrowArray* ca, ac_kind kind,
                        int nullable) {
  memset(c, 0, sizeof(*c));
  c->kind = kind;
  c->nullable = nullable;
  c->length = ca->length;
  c->abs_offset = ca->offset;
  c->has_validity = (ca->buffers[0] != NULL);
  c->validity = (const uint8_t*)ca->buffers[0];
  c->n_buffers = ca->n_buffers;
  for (int64_t b = 0; b < ca->n_buffers && b < AC_MAX_NBUFFERS; ++b)
    c->buffers[b] = ca->buffers[b];
  if (kind == AC_KIND_DICT) {
    const struct ArrowArray* da = ca->dictionary;
    c->dict_length = da->length;
    c->dict_has_validity = (da->buffers[0] != NULL);
    c->dict_validity = (const uint8_t*)da->buffers[0];
    c->dict_offsets = (const int32_t*)da->buffers[1];
    c->dict_data = (const uint8_t*)da->buffers[2];
    c->dict_data_nbytes = c->dict_offsets[da->length];
    c->dict_nullable =
        (cs->dictionary->flags & ARROW_FLAG_NULLABLE) != 0;
    c->dict_ordered = (cs->flags & ARROW_FLAG_DICTIONARY_ORDERED) != 0;
  }
}

ac_status ac_validate_and_import(struct ArrowSchema* schema,
                                 struct ArrowArray* array,
                                 ac_payload** out) {
  int64_t n = schema->n_children;
  size_t wn = (size_t)(n > 0 ? n : 1);

  ac_kind* kinds = (ac_kind*)ac_calloc(wn, sizeof(*kinds));
  int* nullable = (int*)ac_calloc(wn, sizeof(int));
  if (!kinds || !nullable) {
    ac_free(kinds); ac_free(nullable);
    return AC_ENOMEM;
  }
  /* Phase 1: source structs untouched. */
  ac_status st = ac_validate_all(schema, array, kinds, nullable);
  if (st) { ac_free(kinds); ac_free(nullable); return st; }

  /* Phase 2: allocate all bookkeeping up front where possible. */
  ac_payload* p = (ac_payload*)ac_calloc(1, sizeof(*p));
  ac_column* cols = (ac_column*)ac_calloc(wn, sizeof(*cols));
  if (!p || !cols) {
    ac_free(p); ac_free(cols); ac_free(kinds); ac_free(nullable);
    return AC_ENOMEM;
  }
  p->cols = cols;
  p->ncols = n;
  p->rows = array->length;
  p->owns_buffers = 0;

  /* Take holders, remembering every success for rollback. Schema holders
   * and array holders are distinct allocations per struct. */
  ac_source* taken[128];
  int64_t ntaken = 0;
  if (n > 64) { ac_free(cols); ac_free(p); ac_free(kinds); ac_free(nullable);
                return AC_EUNSUPPORTED; }

#define TAKE_SCHEMA(stptr, slot) do {                                   \
    ac_source* _h = NULL;                                               \
    if (holder_for_schema((stptr), &_h) != AC_OK) goto enomem;          \
    (slot) = _h; taken[ntaken++] = _h;                                  \
  } while (0)
#define TAKE_ARRAY(aptr, slot) do {                                     \
    ac_source* _h = NULL;                                               \
    if (holder_for_array((aptr), &_h) != AC_OK) goto enomem;            \
    (slot) = _h; taken[ntaken++] = _h;                                  \
  } while (0)

  TAKE_SCHEMA(schema, p->sources.root_schema_holder);
  TAKE_ARRAY(array, p->sources.root_array_holder);

  /* Logical descriptors first: fill_column() memsets the slot, so holder
   * pointers must be installed only afterwards to avoid being wiped. */
  for (int64_t i = 0; i < n; ++i)
    fill_column(&cols[i], schema->children[i], array->children[i],
                kinds[i], nullable[i]);

  for (int64_t i = 0; i < n; ++i) {
    struct ArrowSchema* cs = schema->children[i];
    struct ArrowArray* ca = array->children[i];
    TAKE_SCHEMA(cs, cols[i].col_schema_holder);
    TAKE_ARRAY(ca, cols[i].col_array_holder);
    if (kinds[i] == AC_KIND_DICT) {
      TAKE_SCHEMA(cs->dictionary, cols[i].dict_schema_holder);
      TAKE_ARRAY(ca->dictionary, cols[i].dict_array_holder);
    }
  }

  /* Copy names defensively so payload teardown never depends on peer
   * memory outliving the peer's own release. */
  for (int64_t i = 0; i < n; ++i) {
    const char* nm = schema->children[i]->name ? schema->children[i]->name : "";
    cols[i].name = ac_strdup(nm);
    if (!cols[i].name) goto enomem;
  }

  /* Complete the move: every peer struct now reads release == NULL. */
  holder_mark_moved(p->sources.root_schema_holder);
  holder_mark_moved(p->sources.root_array_holder);
  for (int64_t i = 0; i < n; ++i) {
    holder_mark_moved(cols[i].col_schema_holder);
    holder_mark_moved(cols[i].col_array_holder);
    if (cols[i].dict_schema_holder)
      holder_mark_moved(cols[i].dict_schema_holder);
    if (cols[i].dict_array_holder)
      holder_mark_moved(cols[i].dict_array_holder);
  }

  p->schema_refs = 1;
  p->array_refs = 1;
  ac_free(kinds);
  ac_free(nullable);
  *out = p;
  return AC_OK;

enomem:
  /* Roll back every holder without invoking peer callbacks; the source
   * structs return to their original, still-released-by-peer state. */
  for (int64_t i = 0; i < ntaken; ++i) holder_abort(taken[i]);
  for (int64_t i = 0; i < n; ++i) ac_free(cols[i].name);
  ac_free(cols);
  ac_free(p);
  ac_free(kinds);
  ac_free(nullable);
  return AC_ENOMEM;
}
