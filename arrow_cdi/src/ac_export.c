/*
 * ac_export.c - build fresh, independently movable ABI shells over an
 * existing payload, plus read-only logical accessors and JSON rendering.
 *
 * Export never returns addresses of internal ArrowSchema/ArrowArray
 * structs. It allocates a brand-new struct tree (root, every child and
 * every dictionary struct) whose buffer pointers reference the shared
 * payload bytes without copying them.
 *
 * Every exported struct carries a release callback, as required by the
 * C Data Interface move contract. A shared per-shell context holds one
 * payload reference per side (schema side, array side) and a live count;
 * releasing any subset of the child structs decrements the count, and the
 * payload reference is dropped only when the whole side is released.
 * Releasing the root does not free children - each struct is independently
 * movable - so reference counting, not tree-walk free, is used.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ac_internal.h"

typedef struct ac_shell_ctx {
  ac_payload* payload;
  int         side;       /* 0 = schema shell, 1 = array shell */
  int64_t     live;       /* number of structs still released-able */
  int64_t* col_indices;   /* view permutation, owned by context */
  int64_t  ncols;
  int64_t  view_offset;
  int64_t  view_length;
} ac_shell_ctx;

#define AC_SIDE_SCHEMA 0
#define AC_SIDE_ARRAY  1

static const char* kind_format(ac_kind k) {
  switch (k) {
    case AC_KIND_I64:  return "l";
    case AC_KIND_UTF8: return "u";
    case AC_KIND_DICT: return "i";
  }
  return "?";
}

/* Drop one live reference for a shell side. The payload reference is
 * released exactly once per side, on the last struct release. The root
 * struct's own strings/buffer-array are freed by the root release after
 * every child has gone, so the context only owns the column permutation. */
static void ctx_drop(ac_shell_ctx* ctx) {
  --ctx->live;
  if (ctx->live > 0) return;
  ac_payload* p = ctx->payload;
  int side = ctx->side;
  ac_free(ctx->col_indices);
  ac_free(ctx);
  if (side == AC_SIDE_SCHEMA) ac_payload_release_schema(p);
  else                        ac_payload_release_array(p);
}

/* ---- per-struct release callbacks -------------------------------- */

static void child_schema_release(struct ArrowSchema* s) {
  if (!s || !s->release) return;
  ac_shell_ctx* ctx = (ac_shell_ctx*)s->private_data;
  /* The dictionary struct carries its own release and live reference. */
  if (s->dictionary && s->dictionary->release)
    s->dictionary->release(s->dictionary);
  ac_free((void*)s->format);
  ac_free((void*)s->name);
  s->release = NULL;
  s->private_data = NULL;
  ctx_drop(ctx);
  ac_free(s);
}

static void root_schema_release(struct ArrowSchema* s) {
  if (!s || !s->release) return;
  ac_shell_ctx* ctx = (ac_shell_ctx*)s->private_data;
  /* C Data Interface consumers (PyArrow included) release a tree by
   * invoking only the root release; the root must therefore cascade to
   * every child. Each child release is still individually callable and
   * idempotent (it NULLs its own release pointer). */
  int64_t nch = s->n_children;
  struct ArrowSchema** kids = s->children;
  for (int64_t i = 0; i < nch; ++i) {
    struct ArrowSchema* cs = kids[i];
    if (cs && cs->release) cs->release(cs);
  }
  ac_free(s->children);
  ac_free((void*)s->format);
  ac_free((void*)s->name);
  s->release = NULL;
  s->private_data = NULL;
  ctx_drop(ctx);
}

/* Dictionary value structs are separately allocated and each carry their
 * own release/live reference. They are reached only via ->dictionary. */
static void dict_schema_release(struct ArrowSchema* s) {
  if (!s || !s->release) return;
  ac_shell_ctx* ctx = (ac_shell_ctx*)s->private_data;
  ac_free((void*)s->format);
  ac_free((void*)s->name);
  s->release = NULL;
  s->private_data = NULL;
  ctx_drop(ctx);
  ac_free(s);
}

static void dict_array_release(struct ArrowArray* a) {
  if (!a || !a->release) return;
  ac_shell_ctx* ctx = (ac_shell_ctx*)a->private_data;
  ac_free((void*)a->buffers);
  a->release = NULL;
  a->private_data = NULL;
  ctx_drop(ctx);
  ac_free(a);
}

static void child_array_release(struct ArrowArray* a) {
  if (!a || !a->release) return;
  ac_shell_ctx* ctx = (ac_shell_ctx*)a->private_data;
  if (a->dictionary && a->dictionary->release)
    a->dictionary->release(a->dictionary);
  ac_free((void*)a->buffers);
  a->release = NULL;
  a->private_data = NULL;
  ctx_drop(ctx);
  ac_free(a);
}

static void root_array_release(struct ArrowArray* a) {
  if (!a || !a->release) return;
  ac_shell_ctx* ctx = (ac_shell_ctx*)a->private_data;
  /* Cascade-release every child (and their dictionary arrays); the root
   * is the single release entry point consumers are expected to call. */
  int64_t nch = a->n_children;
  struct ArrowArray** kids = a->children;
  for (int64_t i = 0; i < nch; ++i) {
    struct ArrowArray* ca = kids[i];
    if (ca && ca->release) ca->release(ca);
  }
  ac_free(a->children);
  ac_free((void*)a->buffers);
  a->release = NULL;
  a->private_data = NULL;
  ctx_drop(ctx);
}

/* Public release entry points. The root callbacks cascade through the
 * whole tree, so the wrappers only invoke the root and stay idempotent. */
void ac_export_schema_release(struct ArrowSchema* s) {
  if (!s || !s->release) return;
  s->release(s);
}

void ac_export_array_release(struct ArrowArray* a) {
  if (!a || !a->release) return;
  a->release(a);
}

/* ================================================================== */
/* Schema shell construction                                          */
/* ================================================================== */

ac_status ac_build_schema_shell(ac_payload* p, const int64_t* col_indices,
                                int64_t ncols, struct ArrowSchema* out) {
  memset(out, 0, sizeof(*out));
  ac_shell_ctx* ctx = (ac_shell_ctx*)ac_calloc(1, sizeof(*ctx));
  struct ArrowSchema** children =
      (struct ArrowSchema**)ac_calloc((size_t)(ncols > 0 ? ncols : 1),
                                      sizeof(*children));
  char* root_fmt = ac_strdup("+s");
  char* root_name = ac_strdup("");
  int64_t* idx_copy = (int64_t*)ac_malloc(sizeof(int64_t) *
                                          (size_t)(ncols > 0 ? ncols : 1));
  if (!ctx || !children || !root_fmt || !root_name || !idx_copy) {
    ac_free(ctx); ac_free(children); ac_free(root_fmt);
    ac_free(root_name); ac_free(idx_copy);
    return AC_ENOMEM;
  }
  memcpy(idx_copy, col_indices, sizeof(int64_t) * (size_t)ncols);
  ctx->payload = p;
  ctx->side = AC_SIDE_SCHEMA;
  ctx->col_indices = idx_copy;
  ctx->ncols = ncols;
  /* live count: root + one per child + one per dictionary schema */
  int64_t live = 1;
  for (int64_t i = 0; i < ncols; ++i)
    if (p->cols[col_indices[i]].kind == AC_KIND_DICT) ++live;
  live += ncols;
  ctx->live = live;

  for (int64_t i = 0; i < ncols; ++i) {
    ac_column* c = &p->cols[col_indices[i]];
    struct ArrowSchema* cs =
        (struct ArrowSchema*)ac_calloc(1, sizeof(*cs));
    char* fmt = ac_strdup(kind_format(c->kind));
    char* nm = ac_strdup(c->name);
    if (!cs || !fmt || !nm) {
      ac_free(cs); ac_free(fmt); ac_free(nm); goto enomem;
    }
    cs->format = fmt;
    cs->name = nm;
    cs->flags = c->nullable ? ARROW_FLAG_NULLABLE : 0;
    if (c->kind == AC_KIND_DICT && c->dict_ordered)
      cs->flags |= ARROW_FLAG_DICTIONARY_ORDERED;
    cs->n_children = 0;
    cs->release = child_schema_release;
    cs->private_data = ctx;
    if (c->kind == AC_KIND_DICT) {
      struct ArrowSchema* ds =
          (struct ArrowSchema*)ac_calloc(1, sizeof(*ds));
      char* dfmt = ac_strdup("u");
      char* dnm = ac_strdup("");
      if (!ds || !dfmt || !dnm) {
        ac_free(ds); ac_free(dfmt); ac_free(dnm);
        ac_free((void*)cs->format); ac_free((void*)cs->name); ac_free(cs);
        goto enomem;
      }
      ds->format = dfmt;
      ds->name = dnm;
      ds->flags = c->dict_nullable ? ARROW_FLAG_NULLABLE : 0;
      ds->n_children = 0;
      ds->release = dict_schema_release;
      ds->private_data = ctx;
      cs->dictionary = ds;
    }
    children[i] = cs;
  }

  out->format = root_fmt;
  out->name = root_name;
  out->metadata = NULL;
  out->flags = 0;
  out->n_children = ncols;
  out->children = children;
  out->dictionary = NULL;
  out->release = root_schema_release;
  out->private_data = ctx;
  ac_payload_retain_schema(p);
  return AC_OK;

enomem:
  for (int64_t i = 0; i < ncols; ++i) {
    struct ArrowSchema* cs = children[i];
    if (!cs) continue;
    if (cs->dictionary) {
      ac_free((void*)cs->dictionary->format);
      ac_free((void*)cs->dictionary->name);
      ac_free(cs->dictionary);
    }
    ac_free((void*)cs->format);
    ac_free((void*)cs->name);
    ac_free(cs);
  }
  ac_free(children);
  ac_free(root_fmt);
  ac_free(root_name);
  ac_free(idx_copy);
  ac_free(ctx);
  return AC_ENOMEM;
}

/* ================================================================== */
/* Array shell construction                                           */
/* ================================================================== */

ac_status ac_build_array_shell(ac_payload* p, const int64_t* col_indices,
                               int64_t ncols, int64_t view_offset,
                               int64_t view_length, struct ArrowArray* out) {
  memset(out, 0, sizeof(*out));
  ac_shell_ctx* ctx = (ac_shell_ctx*)ac_calloc(1, sizeof(*ctx));
  struct ArrowArray** children =
      (struct ArrowArray**)ac_calloc((size_t)(ncols > 0 ? ncols : 1),
                                     sizeof(*children));
  int64_t* idx_copy = (int64_t*)ac_malloc(sizeof(int64_t) *
                                          (size_t)(ncols > 0 ? ncols : 1));
  void** root_bufs = ac_calloc(1, sizeof(void*));
  if (!ctx || !children || !idx_copy || !root_bufs) {
    ac_free(ctx); ac_free(children); ac_free(idx_copy); ac_free(root_bufs);
    return AC_ENOMEM;
  }
  memcpy(idx_copy, col_indices, sizeof(int64_t) * (size_t)ncols);
  ctx->payload = p;
  ctx->side = AC_SIDE_ARRAY;
  ctx->col_indices = idx_copy;
  ctx->ncols = ncols;
  ctx->view_offset = view_offset;
  ctx->view_length = view_length;
  int64_t live = 1 + ncols;
  for (int64_t i = 0; i < ncols; ++i)
    if (p->cols[col_indices[i]].kind == AC_KIND_DICT) ++live;
  ctx->live = live;

  for (int64_t i = 0; i < ncols; ++i) {
    ac_column* c = &p->cols[col_indices[i]];
    struct ArrowArray* ca =
        (struct ArrowArray*)ac_calloc(1, sizeof(*ca));
    const void** bufs = (const void**)ac_calloc((size_t)c->n_buffers,
                                                sizeof(void*));
    if (!ca || !bufs) { ac_free(ca); ac_free(bufs); goto enomem; }
    ca->length = view_length;
    ca->null_count = -1; /* validity buffer is authoritative */
    ca->offset = c->abs_offset + view_offset;
    ca->n_buffers = c->n_buffers;
    ca->n_children = 0;
    ca->buffers = bufs;
    for (int64_t b = 0; b < c->n_buffers; ++b)
      ca->buffers[b] = c->buffers[b];
    ca->release = child_array_release;
    ca->private_data = ctx;
    if (c->kind == AC_KIND_DICT) {
      struct ArrowArray* da =
          (struct ArrowArray*)ac_calloc(1, sizeof(*da));
      const void** dbufs = (const void**)ac_calloc(3, sizeof(void*));
      if (!da || !dbufs) {
        ac_free(da); ac_free(dbufs);
        ac_free((void*)ca->buffers); ac_free(ca);
        goto enomem;
      }
      da->length = c->dict_length;
      da->null_count = -1;
      da->offset = 0;
      da->n_buffers = 3;
      da->n_children = 0;
      da->buffers = dbufs;
      dbufs[0] = c->dict_has_validity ? c->dict_validity : NULL;
      dbufs[1] = c->dict_offsets;
      dbufs[2] = c->dict_data;
      da->release = dict_array_release;
      da->private_data = ctx;
      ca->dictionary = da;
    }
    children[i] = ca;
  }

  out->length = view_length;
  out->null_count = 0;
  out->offset = 0;
  out->n_buffers = 1;
  out->n_children = ncols;
  out->buffers = (const void**)root_bufs;
  out->buffers[0] = NULL;
  out->children = children;
  out->dictionary = NULL;
  out->release = root_array_release;
  out->private_data = ctx;
  ac_payload_retain_array(p);
  return AC_OK;

enomem:
  for (int64_t i = 0; i < ncols; ++i) {
    struct ArrowArray* ca = children[i];
    if (!ca) continue;
    if (ca->dictionary) { ac_free((void*)ca->dictionary->buffers);
                          ac_free(ca->dictionary); }
    ac_free((void*)ca->buffers);
    ac_free(ca);
  }
  ac_free(children);
  ac_free(idx_copy);
  ac_free(root_bufs);
  ac_free(ctx);
  return AC_ENOMEM;
}

ac_status ac_batch_export(ArrowCDIBatch* b, struct ArrowSchema* out_schema,
                          struct ArrowArray* out_array) {
  if (!b || b->closed || !out_schema || !out_array) return AC_ESTATE;
  ac_status st = ac_build_schema_shell(b->payload, b->col_indices,
                                       b->ncols, out_schema);
  if (st) return st;
  st = ac_build_array_shell(b->payload, b->col_indices, b->ncols,
                            b->offset, b->rows, out_array);
  if (st) {
    ac_export_schema_release(out_schema);
    return st;
  }
  return AC_OK;
}

/* ================================================================== */
/* Logical accessors                                                  */
/* ================================================================== */

static ac_column* view_column(const ArrowCDIBatch* b, int64_t col) {
  return &b->payload->cols[b->col_indices[col]];
}

int64_t ac_batch_rows(const ArrowCDIBatch* b) { return b ? b->rows : 0; }
int64_t ac_batch_columns(const ArrowCDIBatch* b) { return b ? b->ncols : 0; }

const char* ac_batch_column_name(const ArrowCDIBatch* b, int64_t col) {
  if (!b || col < 0 || col >= b->ncols) return NULL;
  return view_column(b, col)->name;
}

ac_kind ac_batch_column_kind(const ArrowCDIBatch* b, int64_t col) {
  return view_column(b, col)->kind;
}

int ac_batch_is_null(const ArrowCDIBatch* b, int64_t col, int64_t row) {
  ac_column* c = view_column(b, col);
  int64_t phys = c->abs_offset + b->offset + row;
  return !ac_validity_is_set(c->validity, c->has_validity, phys);
}

ac_status ac_batch_get_i64(const ArrowCDIBatch* b, int64_t col,
                           int64_t row, int64_t* out) {
  ac_column* c = view_column(b, col);
  if (c->kind != AC_KIND_I64) return AC_EINVAL;
  int64_t phys = c->abs_offset + b->offset + row;
  if (!ac_validity_is_set(c->validity, c->has_validity, phys))
    return AC_ESTATE;
  *out = ((const int64_t*)c->buffers[1])[phys];
  return AC_OK;
}

ac_status ac_batch_get_string(const ArrowCDIBatch* b, int64_t col,
                              int64_t row, const char** out_utf8,
                              int64_t* out_len) {
  ac_column* c = view_column(b, col);
  int64_t phys = c->abs_offset + b->offset + row;
  const int32_t* offsets;
  const uint8_t* data;
  const uint8_t* val;
  int has_val;
  if (c->kind == AC_KIND_UTF8) {
    offsets = (const int32_t*)c->buffers[1];
    data = (const uint8_t*)c->buffers[2];
    val = c->validity;
    has_val = c->has_validity;
  } else if (c->kind == AC_KIND_DICT) {
    if (!ac_validity_is_set(c->validity, c->has_validity, phys))
      return AC_ESTATE; /* null index: its bits are never interpreted */
    int32_t ix = ((const int32_t*)c->buffers[1])[phys];
    if (!ac_validity_is_set(c->dict_validity, c->dict_has_validity, ix))
      return AC_ESTATE; /* dictionary's own null -> null output */
    offsets = c->dict_offsets;
    data = c->dict_data;
    val = c->dict_validity;
    has_val = c->dict_has_validity;
    phys = ix;
  } else {
    return AC_EINVAL;
  }
  if (!ac_validity_is_set(val, has_val, phys)) return AC_ESTATE;
  int32_t lo = offsets[phys];
  int32_t hi = offsets[phys + 1];
  *out_utf8 = (const char*)(data + lo);
  *out_len = (int64_t)(hi - lo);
  return AC_OK;
}

const void* ac_batch_buffer_address(const ArrowCDIBatch* b, int64_t col,
                                    int64_t buffer_index) {
  ac_column* c = view_column(b, col);
  if (buffer_index < 0 || buffer_index >= c->n_buffers) return NULL;
  return c->buffers[buffer_index];
}

const void* ac_batch_dict_buffer_address(const ArrowCDIBatch* b,
                                         int64_t col,
                                         int64_t buffer_index) {
  ac_column* c = view_column(b, col);
  if (c->kind != AC_KIND_DICT || buffer_index < 0 || buffer_index > 2)
    return NULL;
  if (buffer_index == 0)
    return c->dict_has_validity ? c->dict_validity : NULL;
  if (buffer_index == 1) return c->dict_offsets;
  return c->dict_data;
}

int64_t ac_batch_physical_offset(const ArrowCDIBatch* b, int64_t col) {
  ac_column* c = view_column(b, col);
  return c->abs_offset + b->offset;
}

const char* ac_status_name(ac_status st) {
  switch (st) {
    case AC_OK: return "AC_OK";
    case AC_EINVAL: return "AC_EINVAL";
    case AC_EUNSUPPORTED: return "AC_EUNSUPPORTED";
    case AC_ELAYOUT: return "AC_ELAYOUT";
    case AC_ENOMEM: return "AC_ENOMEM";
    case AC_ESTATE: return "AC_ESTATE";
  }
  return "AC_UNKNOWN";
}

/* ================================================================== */
/* JSON rendering (demo only; not a scalar-list conversion API)       */
/* ================================================================== */

static int emit_string(FILE* f, const char* s, int64_t len) {
  if (fputc('"', f) == EOF) return -1;
  for (int64_t i = 0; i < len; ++i) {
    unsigned char ch = (unsigned char)s[i];
    if (ch == '"' || ch == '\\') {
      if (fputc('\\', f) == EOF) return -1;
      if (fputc((int)ch, f) == EOF) return -1;
    } else if (ch < 0x20) {
      if (fprintf(f, "\\u%04x", ch) < 0) return -1;
    } else {
      if (fputc((int)ch, f) == EOF) return -1;
    }
  }
  return fputc('"', f) == EOF ? -1 : 0;
}

ac_status ac_batch_to_json(ArrowCDIBatch* b, char** out_json) {
  if (!b || b->closed || !out_json) return AC_ESTATE;
  *out_json = NULL;
  char* buf = NULL;
  size_t cap = 0;
  FILE* f = open_memstream(&buf, &cap);
  if (!f) return AC_ENOMEM;

  fprintf(f, "{\"rows\": %lld, \"columns\": [", (long long)b->rows);
  for (int64_t col = 0; col < b->ncols; ++col) {
    ac_column* c = view_column(b, col);
    if (col) fputc(',', f);
    emit_string(f, c->name, (int64_t)strlen(c->name));
    fprintf(f, ": {\"type\": \"%s\", \"values\": [",
            c->kind == AC_KIND_I64 ? "int64" :
            c->kind == AC_KIND_UTF8 ? "utf8" : "dict<int32,utf8>");
    for (int64_t row = 0; row < b->rows; ++row) {
      if (row) fputc(',', f);
      if (ac_batch_is_null(b, col, row)) {
        fputs("null", f);
      } else if (c->kind == AC_KIND_I64) {
        int64_t v;
        ac_batch_get_i64(b, col, row, &v);
        fprintf(f, "%lld", (long long)v);
      } else {
        const char* s;
        int64_t n;
        ac_status st = ac_batch_get_string(b, col, row, &s, &n);
        if (st == AC_ESTATE) fputs("null", f);
        else emit_string(f, s, n);
      }
    }
    fputs("]}", f);
  }
  fputs("]}", f);
  if (fclose(f) != 0) { free(buf); return AC_ENOMEM; }
  *out_json = buf; /* libc malloc; release with ac_json_free */
  return AC_OK;
}

void ac_json_free(char* json) {
  free(json);
}
