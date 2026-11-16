/*
 * ac_internal.h - private structures of the arrow_cdi library.
 *
 * Ownership model (see docs/INTERFACE.md "Memory ownership"):
 *
 *   payload (refcounted, single allocation)
 *     |-- owns the logical column descriptors and the buffer pointers
 *     |-- for library-created batches: owns malloc'd buffer copies
 *     |-- for imported batches: owns "source holders" that keep the moved
 *     |     peer ArrowSchema/ArrowArray structs alive; payload destruction
 *     |     invokes the peer release callbacks exactly once
 *     +-- referenced by every batch view and every exported ABI shell
 *
 * Two independent refcounts: schema_refs (schema-side users) and
 * array_refs (array-side users). Buffers are freed only when array_refs
 * hits zero; schema-only metadata (copied format/name strings) is freed
 * with schema_refs. Releasing a schema can therefore never tear down an
 * array that is still alive.
 */
#ifndef AC_INTERNAL_H
#define AC_INTERNAL_H

#include "arrow_cdi.h"

#define AC_MAX_NBUFFERS 3 /* validity, values/offsets, data */

/* A moved peer ABI struct, kept alive until the payload side it belongs
 * to is torn down. Each holder owns exactly one struct (the other pointer
 * is NULL). While owned, the holder swaps the peer private_data for its
 * own bookkeeping and the commit clears the struct's release pointer; at
 * destruction the holder restores the original release/private pair and
 * invokes the peer callback exactly once. Holders are individually
 * allocated, so a failed commit can roll back one by one. */
typedef struct ac_source {
  struct ArrowSchema* schema; /* moved ArrowSchema, NULL for array holders */
  struct ArrowArray*  array;  /* moved ArrowArray,  NULL for schema holders */
  void (*orig_s_release)(struct ArrowSchema*);
  void*  orig_s_private;
  void (*orig_a_release)(struct ArrowArray*);
  void*  orig_a_private;
} ac_source;

typedef struct ac_column {
  char*    name;             /* copied, NUL terminated */
  ac_kind  kind;
  int64_t  length;           /* logical column length in the payload */
  int64_t  abs_offset;       /* physical start inside the buffers */
  int      has_validity;     /* payload validity buffer present */
  int      nullable;         /* schema ARROW_FLAG_NULLABLE bit */
  const uint8_t* validity;   /* physical base, NULL if none */
  const void*    buffers[AC_MAX_NBUFFERS]; /* physical bases */
  int64_t  n_buffers;
  /* dictionary columns only: */
  int64_t  dict_length;
  int      dict_has_validity;
  const uint8_t* dict_validity;
  const int32_t* dict_offsets;
  const uint8_t* dict_data;
  int64_t  dict_data_nbytes;
  int      dict_nullable;
  int      dict_ordered;
  /* Moved peer structs backing this column (imported payloads only).
   * Schema-side holders live until schema_refs dies, array-side holders
   * until array_refs dies. Non-dictionary columns leave dict_* NULL. */
  ac_source* col_schema_holder;
  ac_source* col_array_holder;
  ac_source* dict_schema_holder;
  ac_source* dict_array_holder;
} ac_column;

/* Root-level moved peer structs; unused slots stay NULL. */
typedef struct ac_root_sources {
  ac_source* root_schema_holder;
  ac_source* root_array_holder;
} ac_root_sources;

typedef struct ac_payload {
  int64_t  rows;             /* logical row count of the payload */
  int64_t  ncols;
  ac_column* cols;           /* ncols descriptors (projected order) */
  int      owns_buffers;     /* 1: library-created, free buffers */
  ac_root_sources sources;   /* imported peer structs, else zeroed */
  int64_t  schema_refs;
  int64_t  array_refs;
  /* library-created buffer copies, freed at array teardown: */
  void**   owned_blocks;
  int64_t  n_owned_blocks;
  int64_t  cap_owned_blocks;
} ac_payload;

/* Upper bound on copied buffer blocks per column: validity, values,
 * offsets, bytes, plus dictionary validity/offsets/bytes. */
#define AC_BLOCKS_PER_COLUMN 7

struct ArrowCDIBatch {
  ac_payload* payload;
  int64_t  rows;             /* view length */
  int64_t  offset;           /* view offset relative to payload columns */
  int64_t  ncols;
  int64_t* col_indices;      /* payload column index per view column */
  int      closed;
};

/* ---- allocation layer ---- */
void* ac_malloc(size_t n);          /* NULL == failure, counted */
void* ac_calloc(size_t n, size_t sz);
char* ac_strdup(const char* s);     /* NULL on failure/NULL input */
void  ac_payload_retain_schema(ac_payload* p);
void  ac_payload_retain_array(ac_payload* p);
void  ac_payload_retain_both(ac_payload* p);
void  ac_payload_release_schema(ac_payload* p);
void  ac_payload_release_array(ac_payload* p);
void  ac_payload_retain_array(ac_payload* p);
void  ac_payload_retain_both(ac_payload* p);
void  ac_payload_release_schema(ac_payload* p);
void  ac_payload_release_array(ac_payload* p);

/* ---- import: validation (read-only) then move/commit ---- */
ac_status ac_validate_and_import(struct ArrowSchema* schema,
                                 struct ArrowArray* array,
                                 ac_payload** out);

/* ---- export shell construction ---- */
ac_status ac_build_schema_shell(ac_payload* p, const int64_t* col_indices,
                                int64_t ncols, struct ArrowSchema* out);
ac_status ac_build_array_shell(ac_payload* p, const int64_t* col_indices,
                               int64_t ncols, int64_t view_offset,
                               int64_t view_length, struct ArrowArray* out);

/* Release callbacks installed on exported shells. */
void ac_export_schema_release(struct ArrowSchema* s);
void ac_export_array_release(struct ArrowArray* a);

/* ---- logical helpers ---- */
int ac_validity_is_set(const uint8_t* validity, int has_validity,
                       int64_t physical_row);

/* Arrow validity buffers are bit-packed (one bit per row, LSB-first).
 * Library-created batches accept caller convenience buffers of one byte
 * per row and pack them at creation; imported/exported payloads store the
 * canonical bit-packed form. These two helpers translate between them. */
uint8_t* ac_pack_validity(const uint8_t* byte_validity, int64_t length);
int      ac_bit_is_set(const uint8_t* bits, int64_t physical_row);

#endif
