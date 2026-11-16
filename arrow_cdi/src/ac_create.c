/*
 * ac_create.c - build owning record batches from caller C buffers, and
 * create zero-copy views (slice, project).
 *
 * Library-created batches deep-copy the caller buffers once at creation
 * (the caller may free its memory immediately afterwards). Views never
 * copy value, offset or validity buffers: they carry new offset/length
 * and a column-index permutation over the same refcounted payload.
 */
#include <stdlib.h>
#include <string.h>

#include "ac_internal.h"

static ac_status remember_block(ac_payload* p, void* block) {
  if (p->n_owned_blocks >= p->cap_owned_blocks) return AC_ENOMEM;
  p->owned_blocks[p->n_owned_blocks++] = block;
  return AC_OK;
}

static void* copy_block(ac_payload* p, const void* src, size_t n) {
  void* dst = ac_malloc(n ? n : 1);
  if (!dst) return NULL;
  if (src && n) memcpy(dst, src, n);
  if (remember_block(p, dst) != AC_OK) { ac_free(dst); return NULL; }
  return dst;
}

static ac_status copy_validity(ac_payload* p, ac_column* c,
                               const uint8_t* validity, int64_t rows) {
  if (!validity) { c->has_validity = 0; c->validity = NULL; return AC_OK; }
  /* Caller supplies one byte per row; store the canonical bit-packed form
   * so exported shells are valid Arrow without further translation. */
  uint8_t* v = ac_pack_validity(validity, rows);
  if (!v) return AC_ENOMEM;
  if (remember_block(p, v) != AC_OK) { ac_free(v); return AC_ENOMEM; }
  c->has_validity = 1;
  c->validity = v;
  return AC_OK;
}

static ac_status build_i64(ac_payload* p, ac_column* c,
                           const ac_column_spec* s) {
  int64_t* vals = (int64_t*)copy_block(p, s->i64_values,
                                       sizeof(int64_t) * (size_t)s->length);
  if (!vals) return AC_ENOMEM;
  c->n_buffers = 2;
  c->buffers[0] = c->validity;
  c->buffers[1] = vals;
  return AC_OK;
}

static ac_status build_utf8(ac_payload* p, ac_column* c,
                            const int32_t* offsets, const uint8_t* data,
                            int64_t length, int64_t data_nbytes) {
  int32_t* off = (int32_t*)copy_block(p, offsets,
                                      sizeof(int32_t) * (size_t)(length + 1));
  if (!off) return AC_ENOMEM;
  uint8_t* bytes = (uint8_t*)copy_block(p, data, (size_t)data_nbytes);
  if (!bytes) return AC_ENOMEM;
  c->n_buffers = 3;
  c->buffers[0] = c->validity;
  c->buffers[1] = off;
  c->buffers[2] = bytes;
  return AC_OK;
}

static ac_status build_dict(ac_payload* p, ac_column* c,
                            const ac_column_spec* s) {
  const ac_utf8_spec* d = s->dictionary;
  /* index column buffers */
  int32_t* idx = (int32_t*)copy_block(p, s->dict_indices,
                                      sizeof(int32_t) * (size_t)s->length);
  if (!idx) return AC_ENOMEM;
  c->n_buffers = 2;
  c->buffers[0] = c->validity;
  c->buffers[1] = idx;

  /* dictionary value array: bit-packed validity / offsets / bytes */
  c->dict_length = d->length;
  c->dict_has_validity = (d->validity != NULL);
  if (d->validity) {
    uint8_t* dv = ac_pack_validity(d->validity, d->length);
    if (!dv) return AC_ENOMEM;
    if (remember_block(p, dv) != AC_OK) { ac_free(dv); return AC_ENOMEM; }
    c->dict_validity = dv;
  }
  int32_t* doff = (int32_t*)copy_block(p, d->offsets,
                                       sizeof(int32_t) * (size_t)(d->length + 1));
  if (!doff) return AC_ENOMEM;
  uint8_t* dbytes = (uint8_t*)copy_block(p, d->data, (size_t)d->data_nbytes);
  if (!dbytes) return AC_ENOMEM;
  c->dict_offsets = doff;
  c->dict_data = dbytes;
  c->dict_data_nbytes = d->data_nbytes;
  c->dict_nullable = 1;
  c->dict_ordered = 0;
  return AC_OK;
}

/* Validate caller specs before any allocation. */
static ac_status validate_specs(const ac_column_spec* cols, int64_t ncols,
                                int64_t* rows_out) {
  if (ncols < 0) return AC_EINVAL;
  int64_t rows = ncols ? cols[0].length : 0;
  for (int64_t i = 0; i < ncols; ++i) {
    const ac_column_spec* s = &cols[i];
    if (!s->name || !*s->name) return AC_EINVAL;
    if (s->length < 0) return AC_ELAYOUT;
    if (i && s->length != rows) return AC_EINVAL;
    for (int64_t j = 0; j < i; ++j)
      if (strcmp(cols[j].name, s->name) == 0) return AC_EINVAL;
    switch (s->kind) {
      case AC_KIND_I64:
        if (!s->i64_values) return AC_EINVAL;
        break;
      case AC_KIND_UTF8:
        if (!s->str_offsets) return AC_EINVAL;
        if (s->str_data_nbytes < 0) return AC_ELAYOUT;
        if (s->str_offsets[0] != 0) return AC_ELAYOUT;
        for (int64_t k = 1; k <= s->length; ++k)
          if (s->str_offsets[k] < s->str_offsets[k - 1]) return AC_ELAYOUT;
        if (s->str_offsets[s->length] > s->str_data_nbytes) return AC_ELAYOUT;
        break;
      case AC_KIND_DICT: {
        if (!s->dict_indices || !s->dictionary) return AC_EINVAL;
        const ac_utf8_spec* d = s->dictionary;
        if (d->length < 0 || !d->offsets) return AC_ELAYOUT;
        if (d->offsets[0] != 0) return AC_ELAYOUT;
        for (int64_t k = 1; k <= d->length; ++k)
          if (d->offsets[k] < d->offsets[k - 1]) return AC_ELAYOUT;
        if (d->offsets[d->length] > d->data_nbytes) return AC_ELAYOUT;
        for (int64_t k = 0; k < s->length; ++k) {
          if (s->validity && s->validity[k] == 0) continue;
          int32_t v = s->dict_indices[k];
          if (v < 0 || v >= d->length) return AC_ELAYOUT;
        }
        break;
      }
      default:
        return AC_EUNSUPPORTED;
    }
  }
  *rows_out = rows;
  return AC_OK;
}

ac_status ac_batch_create(const ac_column_spec* cols, int64_t ncols,
                          ArrowCDIBatch** out) {
  if (!out) return AC_EINVAL;
  *out = NULL;
  if (ncols > 64) return AC_EUNSUPPORTED;
  int64_t rows;
  ac_status st = validate_specs(cols, ncols, &rows);
  if (st) return st;

  ac_payload* p = (ac_payload*)ac_calloc(1, sizeof(*p));
  ac_column* descs = (ac_column*)ac_calloc((size_t)(ncols > 0 ? ncols : 1),
                                           sizeof(*descs));
  if (!p || !descs) { ac_free(p); ac_free(descs); return AC_ENOMEM; }
  p->cols = descs;
  p->ncols = ncols;
  p->rows = rows;
  p->owns_buffers = 1;
  p->schema_refs = 1;
  p->array_refs = 1;
  p->cap_owned_blocks = (ncols > 0 ? ncols : 1) * AC_BLOCKS_PER_COLUMN;
  p->owned_blocks = (void**)ac_malloc(sizeof(void*) *
                                      (size_t)p->cap_owned_blocks);
  if (!p->owned_blocks) {
    ac_free(descs); ac_free(p);
    return AC_ENOMEM;
  }
  p->n_owned_blocks = 0;

  for (int64_t i = 0; i < ncols; ++i) {
    const ac_column_spec* s = &cols[i];
    ac_column* c = &p->cols[i];
    c->name = ac_strdup(s->name);
    if (!c->name) goto fail;
    c->kind = s->kind;
    c->length = rows;
    c->abs_offset = 0;
    c->nullable = 1;
    st = copy_validity(p, c, s->validity, rows);
    if (st) goto fail;
    if (s->kind == AC_KIND_I64) st = build_i64(p, c, s);
    else if (s->kind == AC_KIND_UTF8)
      st = build_utf8(p, c, s->str_offsets, s->str_data, rows,
                      s->str_data_nbytes);
    else st = build_dict(p, c, s);
    if (st) goto fail;
  }

  ArrowCDIBatch* b = (ArrowCDIBatch*)ac_calloc(1, sizeof(*b));
  int64_t* idx = (int64_t*)ac_malloc(sizeof(int64_t) *
                                     (size_t)(ncols > 0 ? ncols : 1));
  if (!b || !idx) { ac_free(b); ac_free(idx); goto fail; }
  for (int64_t i = 0; i < ncols; ++i) idx[i] = i;
  b->payload = p;
  b->rows = rows;
  b->offset = 0;
  b->ncols = ncols;
  b->col_indices = idx;
  *out = b;
  return AC_OK;

fail:
  /* Payload teardown owns all cleanup: release_schema_side() frees the
   * copied names and the column array, release_array_side() frees the
   * copied buffer blocks. Do not free names here or they double-free. */
  ac_payload_release_array(p);
  ac_payload_release_schema(p);
  return AC_ENOMEM;
}

/* ================================================================== */
/* Views                                                              */
/* ================================================================== */

ac_status ac_batch_slice(ArrowCDIBatch* b, int64_t start, int64_t length,
                         ArrowCDIBatch** out) {
  if (!b || !out || b->closed) return AC_ESTATE;
  *out = NULL;
  if (start < 0 || length < 0) return AC_ELAYOUT;
  if (start > b->rows) return AC_EINVAL;
  if (start + length > b->rows) return AC_EINVAL;

  ArrowCDIBatch* v = (ArrowCDIBatch*)ac_calloc(1, sizeof(*v));
  int64_t* idx = (int64_t*)ac_malloc(sizeof(int64_t) *
                                     (size_t)(b->ncols > 0 ? b->ncols : 1));
  if (!v || !idx) { ac_free(v); ac_free(idx); return AC_ENOMEM; }
  memcpy(idx, b->col_indices, sizeof(int64_t) * (size_t)b->ncols);
  v->payload = b->payload;
  v->rows = length;
  /* Relative offsets compose: re-slicing a nonzero-offset slice stays
   * correct without touching any buffer. */
  v->offset = b->offset + start;
  v->ncols = b->ncols;
  v->col_indices = idx;
  ac_payload_retain_both(v->payload);
  *out = v;
  return AC_OK;
}

ac_status ac_batch_project(ArrowCDIBatch* b, const char* const* names,
                           int64_t n_names, ArrowCDIBatch** out) {
  if (!b || !out || b->closed) return AC_ESTATE;
  *out = NULL;
  if (n_names < 0) return AC_ELAYOUT;
  int64_t* chosen = (int64_t*)ac_malloc(sizeof(int64_t) *
                                        (size_t)(n_names > 0 ? n_names : 1));
  if (!chosen) return AC_ENOMEM;
  for (int64_t i = 0; i < n_names; ++i) {
    if (!names[i]) { ac_free(chosen); return AC_EINVAL; }
    int found = -1;
    for (int64_t j = 0; j < b->ncols; ++j) {
      ac_column* c = &b->payload->cols[b->col_indices[j]];
      if (strcmp(c->name, names[i]) == 0) { found = (int)j; break; }
    }
    if (found < 0) { ac_free(chosen); return AC_EINVAL; }
    /* Reordering is allowed; duplicate names in the projection are not. */
    for (int64_t k = 0; k < i; ++k)
      if (chosen[k] == found) { ac_free(chosen); return AC_EINVAL; }
    chosen[i] = found;
  }

  ArrowCDIBatch* v = (ArrowCDIBatch*)ac_calloc(1, sizeof(*v));
  if (!v) { ac_free(chosen); return AC_ENOMEM; }
  v->payload = b->payload;
  v->rows = b->rows;       /* empty projection keeps root row count */
  v->offset = b->offset;
  v->ncols = n_names;
  v->col_indices = chosen;
  ac_payload_retain_both(v->payload);
  *out = v;
  return AC_OK;
}

void ac_batch_close(ArrowCDIBatch* b) {
  if (!b || b->closed) return;
  b->closed = 1;
  ac_payload_release_array(b->payload);
  ac_payload_release_schema(b->payload);
  ac_free(b->col_indices);
  ac_free(b);
}

ac_status ac_batch_import(struct ArrowSchema* schema,
                          struct ArrowArray* array,
                          ArrowCDIBatch** out) {
  if (!out) return AC_EINVAL;
  *out = NULL;
  ac_payload* p = NULL;
  ac_status st = ac_validate_and_import(schema, array, &p);
  if (st) return st;
  ArrowCDIBatch* b = (ArrowCDIBatch*)ac_calloc(1, sizeof(*b));
  int64_t* idx = (int64_t*)ac_malloc(sizeof(int64_t) *
                                     (size_t)(p->ncols > 0 ? p->ncols : 1));
  if (!b || !idx) {
    ac_free(b); ac_free(idx);
    ac_payload_release_array(p);
    ac_payload_release_schema(p);
    return AC_ENOMEM;
  }
  for (int64_t i = 0; i < p->ncols; ++i) idx[i] = i;
  b->payload = p;
  b->rows = p->rows;
  b->offset = 0;
  b->ncols = p->ncols;
  b->col_indices = idx;
  *out = b;
  return AC_OK;
}
