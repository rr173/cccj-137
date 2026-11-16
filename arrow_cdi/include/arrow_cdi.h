/*
 * arrow_cdi.h - Public API of the arrow_cdi project.
 *
 * An in-process, zero-copy Apache Arrow C Data Interface hand-off library:
 *   - build owning record batches from caller-provided C buffers
 *   - import record batches exported by another producer (e.g. PyArrow) by
 *     moving its ArrowSchema/ArrowArray per the C Data Interface contract
 *   - slice / project without copying value, offset or validity buffers
 *   - export independent movable C ABI shells (for PyArrow or a native
 *     consumer) without exposing internal struct addresses as handles
 *
 * Supported subset (see docs/SUPPORTED_SUBSET.md):
 *   top level : non-nullable struct record batch, format "+rs"
 *   columns   : int64 ("l"), UTF-8 string ("u"),
 *               int32-indexed ("i") dictionary whose values are nullable
 *               UTF-8 ("u"); every column nullable
 *
 * Upstream contract references are collected in docs/INTERFACE.md.
 */
#ifndef ARROW_CDI_H
#define ARROW_CDI_H

#include <stdint.h>
#include <stdio.h>

#include "arrow_cdata_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ArrowCDIBatch ArrowCDIBatch;

typedef enum {
  AC_OK = 0,
  AC_EINVAL = 1,      /* bad arguments / rejected logical request        */
  AC_EUNSUPPORTED = 2,/* type or feature outside the supported subset     */
  AC_ELAYOUT = 3,     /* C Data Interface contract-level layout violation */
  AC_ENOMEM = 4,      /* allocation failure (failpoint or real OOM)       */
  AC_ESTATE = 5       /* use after close / released shell                  */
} ac_status;

typedef enum {
  AC_KIND_I64 = 0,
  AC_KIND_UTF8 = 1,
  AC_KIND_DICT = 2
} ac_kind;

/* ------------------------------------------------------------------ */
/* Allocation accounting and deterministic failpoints                 */
/* ------------------------------------------------------------------ */

/* Every allocation made by this library (including imported-shell
 * bookkeeping and export shells) goes through these wrappers. */
void  ac_fail_reset(void);            /* disarm failpoint, zero counters */
void  ac_fail_disarm(void);           /* disarm only; counters untouched  */
void  ac_fail_arm(int nth_allocation); /* 1 == next allocation fails; -1 disarms */
int64_t ac_total_allocations(void);
int64_t ac_total_frees(void);
int64_t ac_live_allocations(void);
int64_t ac_live_bytes(void);
void  ac_free(void* p);

/* ------------------------------------------------------------------ */
/* Batch creation from caller-side C buffers (library takes a copy,   */
/* so caller buffers remain owned by the caller and may be freed).    */
/* ------------------------------------------------------------------ */

typedef struct {
  int64_t length;                 /* number of dictionary entries        */
  const uint8_t* validity;        /* 1 byte/entry, 0 == null, or NULL    */
  const int32_t* offsets;         /* length+1 int32 offsets              */
  const uint8_t* data;            /* UTF-8 bytes                         */
  int64_t data_nbytes;
} ac_utf8_spec;

typedef struct {
  const char* name;               /* required, copied                     */
  ac_kind kind;
  int64_t length;                 /* column length == batch row count     */
  const uint8_t* validity;        /* 1 byte/row, 0 == null, NULL = all ok */
  /* AC_KIND_I64:  */ const int64_t* i64_values;
  /* AC_KIND_DICT: */ const int32_t* dict_indices;
  /* AC_KIND_UTF8: */ const int32_t* str_offsets; /* length+1 entries   */
  /* AC_KIND_UTF8: */ const uint8_t* str_data;
  /* AC_KIND_UTF8: */ int64_t str_data_nbytes;
  /* AC_KIND_DICT: */ const ac_utf8_spec* dictionary;
} ac_column_spec;

/* ncols == 0 is legal: produces a zero-column batch that still carries
 * the row count. Returns AC_ENOMEM without partial objects on failure. */
ac_status ac_batch_create(const ac_column_spec* cols, int64_t ncols,
                          ArrowCDIBatch** out);

/* ------------------------------------------------------------------ */
/* Import: move semantics per the C Data Interface.                   */
/* On success the source structs are released (release == NULL); the   */
/* caller must not touch the moved structures any more. On failure the */
/* source is never modified and still owned by the caller.            */
/* ------------------------------------------------------------------ */
ac_status ac_batch_import(struct ArrowSchema* schema,
                          struct ArrowArray* array,
                          ArrowCDIBatch** out);

/* ------------------------------------------------------------------ */
/* Views: zero-copy, independently closable, share the payload.       */
/* ------------------------------------------------------------------ */
ac_status ac_batch_slice(ArrowCDIBatch* batch, int64_t start, int64_t length,
                         ArrowCDIBatch** out);
ac_status ac_batch_project(ArrowCDIBatch* batch,
                           const char* const* names, int64_t n_names,
                           ArrowCDIBatch** out);

/* Build fresh, independently movable ABI shells. The schema shell and
 * the array shell take independent references to the shared payload,
 * so either may be released first. Buffers are referenced, never copied.
 * The returned structs are allocated by the library; their release
 * callbacks free the shells and drop the payload references. */
ac_status ac_batch_export(ArrowCDIBatch* batch,
                          struct ArrowSchema* out_schema,
                          struct ArrowArray* out_array);

/* Idempotent. Safe on NULL and on an already-closed batch. Never invokes
 * a peer release callback twice. */
void ac_batch_close(ArrowCDIBatch* batch);

/* Release callbacks installed on shells produced by ac_batch_export.
 * Exposed so callers (and tests) can release schema and array shells
 * independently and in either order; both are no-ops after release. */
void ac_export_schema_release(struct ArrowSchema* s);
void ac_export_array_release(struct ArrowArray* a);

/* ------------------------------------------------------------------ */
/* Read-only logical accessors (no per-value Python object conversion; */
/* the Python layer only uses these for JSON/demo output).            */
/* ------------------------------------------------------------------ */
int64_t     ac_batch_rows(const ArrowCDIBatch* batch);
int64_t     ac_batch_columns(const ArrowCDIBatch* batch);
const char* ac_batch_column_name(const ArrowCDIBatch* batch, int64_t col);
ac_kind     ac_batch_column_kind(const ArrowCDIBatch* batch, int64_t col);
int         ac_batch_is_null(const ArrowCDIBatch* batch, int64_t col,
                             int64_t row);
ac_status   ac_batch_get_i64(const ArrowCDIBatch* batch, int64_t col,
                             int64_t row, int64_t* out);
/* Also resolves dictionary columns; dictionary nulls surface as null. */
ac_status   ac_batch_get_string(const ArrowCDIBatch* batch, int64_t col,
                                int64_t row, const char** out_utf8,
                                int64_t* out_len);
/* Physical address of a column buffer slot in the shared payload.
 * Used by tests to prove zero copy across producers/views/exports. */
const void* ac_batch_buffer_address(const ArrowCDIBatch* batch, int64_t col,
                                    int64_t buffer_index);
/* Dictionary value-array buffer address (buffer_index 0..2); NULL for
 * non-dictionary columns or bad indices. */
const void* ac_batch_dict_buffer_address(const ArrowCDIBatch* batch,
                                         int64_t col, int64_t buffer_index);
/* Absolute array offset (root offset + view offset) for a column. */
int64_t     ac_batch_physical_offset(const ArrowCDIBatch* batch, int64_t col);

/* JSON rendering into a libc-allocated string; free with ac_json_free. */
ac_status   ac_batch_to_json(ArrowCDIBatch* batch, char** out_json);
void        ac_json_free(char* json);

const char* ac_status_name(ac_status st);

#ifdef __cplusplus
}
#endif

#endif /* ARROW_CDI_H */
