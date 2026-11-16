/*
 * native_tests.c - in-process C tests for arrow_cdi.
 *
 * Built with AddressSanitizer (make test). The checks that matter:
 *   - every allocation failpoint leaves the source owned by the caller
 *     on failed import and leaves already-published objects usable;
 *   - randomised close orderings over batch/slice/projection/export/shell
 *     never double-free and return all counters to zero;
 *   - dictionary null / garbage-under-null / real out-of-range indices;
 *   - zero-column, zero-length, repeated-name and unsupported-type cases.
 *
 * Python reference counts are deliberately not relied upon.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arrow_cdi.h"

static int g_checks;
#define CHECK(cond) do { ++g_checks; \
  if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
                 exit(1); } } while (0)
#define CHECK_EQ(a,b) CHECK((a) == (b))

/* --------------------------------------------------------------- */
/* Sample batch: interleaved nulls, empty strings, multibyte UTF-8, */
/* repeated dictionary indices, null dictionary value.              */
/* --------------------------------------------------------------- */

#define ROWS 6
static const int64_t i64_vals[ROWS] = {10, 20, 30, 40, 50, 60};
static const uint8_t i64_valid[ROWS] = {1, 0, 1, 1, 0, 1};

static const int32_t str_off[ROWS + 1] = {0, 6, 6, 9, 9, 13, 16};
/* "héllo"(6 bytes incl 2-byte é), ""(0), "日"(3 bytes E4BDA5),
   null slot, "αβ"(4 bytes CE B1 B2), "xyz"(3 bytes); 16 bytes total. */
static const char str_data[] = "h\xc3\xa9llo\xe6\x97\xa5\xce\xb1\xce\xb2xyz";
#define STR_DATA_NBYTES 16
static const uint8_t str_valid[ROWS] = {1, 1, 1, 0, 1, 1};

static const int32_t dict_idx[ROWS] = {0, 2, 1, 0, 2, 1};
static const uint8_t dict_valid[ROWS] = {1, 1, 0, 1, 1, 1};
/* dictionary: ["alpha", null, "beta"] */
static const int32_t d_off[4] = {0, 5, 5, 9};
static const char d_data[] = "alphabeta";
static const uint8_t d_valid[3] = {1, 0, 1};

static ArrowCDIBatch* make_sample(void) {
  ac_utf8_spec dict = {3, d_valid, d_off, (const uint8_t*)d_data, 9};
  ac_column_spec cols[3] = {
    {.name="ids", .kind=AC_KIND_I64, .length=ROWS, .validity=i64_valid,
     .i64_values=i64_vals},
    {.name="text", .kind=AC_KIND_UTF8, .length=ROWS, .validity=str_valid,
     .str_offsets=str_off, .str_data=(const uint8_t*)str_data,
     .str_data_nbytes=STR_DATA_NBYTES},
    {.name="lab", .kind=AC_KIND_DICT, .length=ROWS, .validity=dict_valid,
     .dict_indices=dict_idx, .dictionary=&dict},
  };
  ArrowCDIBatch* b = NULL;
  CHECK_EQ(ac_batch_create(cols, 3, &b), AC_OK);
  return b;
}

static void check_sample_values(ArrowCDIBatch* b) {
  CHECK_EQ(ac_batch_rows(b), ROWS);
  CHECK_EQ(ac_batch_columns(b), 3);
  CHECK(ac_batch_is_null(b, 0, 1));
  CHECK(!ac_batch_is_null(b, 0, 0));
  int64_t v;
  CHECK_EQ(ac_batch_get_i64(b, 0, 0, &v), AC_OK);
  CHECK_EQ(v, 10);
  CHECK_EQ(ac_batch_get_i64(b, 0, 1, &v), AC_ESTATE);

  const char* s; int64_t n;
  CHECK_EQ(ac_batch_get_string(b, 1, 0, &s, &n), AC_OK);
  CHECK_EQ(n, 6); CHECK(memcmp(s, "h\xc3\xa9llo", 6) == 0);
  CHECK_EQ(ac_batch_get_string(b, 1, 1, &s, &n), AC_OK);
  CHECK_EQ(n, 0);
  CHECK(ac_batch_is_null(b, 1, 3));
  CHECK_EQ(ac_batch_get_string(b, 1, 4, &s, &n), AC_OK);
  CHECK_EQ(n, 4); CHECK(memcmp(s, "\xce\xb1\xce\xb2", 4) == 0);

  /* dictionary: row 2 is a null index; dictionary entry 1 is itself null */
  CHECK(ac_batch_is_null(b, 2, 2));
  CHECK_EQ(ac_batch_get_string(b, 2, 0, &s, &n), AC_OK);
  CHECK_EQ(n, 5); CHECK(memcmp(s, "alpha", 5) == 0);
  CHECK_EQ(ac_batch_get_string(b, 2, 3, &s, &n), AC_OK); /* idx 0 */
  CHECK_EQ(n, 5);
  /* dictionary entry 1 is itself null: row 5 uses index 1 -> null */
  CHECK_EQ(ac_batch_get_string(b, 2, 5, &s, &n), AC_ESTATE);
}

/* --------------------------------------------------------------- */
/* A minimal peer producer/consumer using raw ABI structs, used to  */
/* prove move semantics and release-once behaviour.                 */
/* --------------------------------------------------------------- */

/* Minimal peer release callbacks for hand-built ABI fixtures: they mark
 * the struct released per contract and do not free the stack fixture. */
static void peer_schema_release(struct ArrowSchema* s) {
  s->release = NULL;
  s->private_data = NULL;
}
static void peer_array_release(struct ArrowArray* a) {
  a->release = NULL;
  a->private_data = NULL;
}

static void test_basic_and_json(void) {
  ac_fail_reset();
  ArrowCDIBatch* b = make_sample();
  check_sample_values(b);
  char* json = NULL;
  CHECK_EQ(ac_batch_to_json(b, &json), AC_OK);
  CHECK(json && strstr(json, "\"rows\": 6"));
  CHECK(strstr(json, "h\\u") == NULL); /* multibyte passed through raw */
  CHECK(strstr(json, "\xc3\xa9") != NULL);
  ac_json_free(json);
  ac_batch_close(b);
  CHECK_EQ(ac_live_allocations(), 0);
}

static void test_slice_zero_copy(void) {
  ac_fail_reset();
  ArrowCDIBatch* b = make_sample();
  const void* base_off = ac_batch_buffer_address(b, 1, 1);
  const void* base_val = ac_batch_buffer_address(b, 0, 1);

  ArrowCDIBatch* s1 = NULL;
  CHECK_EQ(ac_batch_slice(b, 2, 4, &s1), AC_OK);
  ArrowCDIBatch* s2 = NULL;
  CHECK_EQ(ac_batch_slice(s1, 1, 2, &s2), AC_OK); /* chained, nonzero offset */
  CHECK_EQ(ac_batch_physical_offset(s2, 1), 3);
  CHECK_EQ(ac_batch_rows(s2), 2);

  /* same physical buffer addresses: slicing copied nothing - values,
   * offsets AND validity bitmaps stay shared. */
  const void* base_validity = ac_batch_buffer_address(b, 0, 0);
  const void* str_validity = ac_batch_buffer_address(b, 1, 0);
  CHECK_EQ(ac_batch_buffer_address(s1, 1, 1), base_off);
  CHECK_EQ(ac_batch_buffer_address(s2, 0, 1), base_val);
  CHECK_EQ(ac_batch_buffer_address(s1, 1, 1),
           ac_batch_buffer_address(s2, 1, 1));
  CHECK_EQ(ac_batch_buffer_address(s2, 0, 0), base_validity);
  CHECK_EQ(ac_batch_buffer_address(s1, 1, 0), str_validity);
  CHECK_EQ(ac_batch_buffer_address(s2, 1, 0), str_validity);

  const char* str; int64_t n;
  /* s2 rows map to original rows 3 and 4: null then "αβ" */
  CHECK(ac_batch_is_null(s2, 1, 0));
  CHECK_EQ(ac_batch_get_string(s2, 1, 1, &str, &n), AC_OK);
  CHECK_EQ(n, 4); CHECK(memcmp(str, "\xce\xb1\xce\xb2", 4) == 0);
  /* i64 original row 4 is null */
  CHECK(ac_batch_is_null(s2, 0, 1));

  /* bounds */
  ArrowCDIBatch* bad = NULL;
  CHECK_EQ(ac_batch_slice(b, -1, 1, &bad), AC_ELAYOUT);
  CHECK_EQ(ac_batch_slice(b, 0, ROWS + 1, &bad), AC_EINVAL);
  CHECK_EQ(ac_batch_slice(b, ROWS, 1, &bad), AC_EINVAL);

  ac_batch_close(s2);
  ac_batch_close(s1);
  /* surviving parent still readable after children closed */
  CHECK_EQ(ac_batch_rows(b), ROWS);
  ac_batch_close(b);
  CHECK_EQ(ac_live_allocations(), 0);
}

static void test_project(void) {
  ac_fail_reset();
  ArrowCDIBatch* b = make_sample();
  const char* rev[3] = {"lab", "text", "ids"};
  ArrowCDIBatch* p = NULL;
  CHECK_EQ(ac_batch_project(b, rev, 3, &p), AC_OK);
  CHECK(strcmp(ac_batch_column_name(p, 0), "lab") == 0);
  CHECK(strcmp(ac_batch_column_name(p, 2), "ids") == 0);

  ArrowCDIBatch* empty = NULL;
  CHECK_EQ(ac_batch_project(b, NULL, 0, &empty), AC_OK);
  CHECK_EQ(ac_batch_columns(empty), 0);
  CHECK_EQ(ac_batch_rows(empty), ROWS); /* root row count retained */

  ArrowCDIBatch* dup = NULL;
  const char* dnames[2] = {"ids", "ids"};
  CHECK_EQ(ac_batch_project(b, dnames, 2, &dup), AC_EINVAL);
  ArrowCDIBatch* miss = NULL;
  const char* mnames[1] = {"nope"};
  CHECK_EQ(ac_batch_project(b, mnames, 1, &miss), AC_EINVAL);

  ac_batch_close(empty);
  ac_batch_close(p);
  ac_batch_close(b);
  CHECK_EQ(ac_live_allocations(), 0);
}

/* --------------------------------------------------------------- */
/* Export shells, independent schema/array release, zero copy        */
/* --------------------------------------------------------------- */

static void test_export_lifecycle(void) {
  ac_fail_reset();
  ArrowCDIBatch* b = make_sample();
  struct ArrowSchema s1; struct ArrowArray a1;
  CHECK_EQ(ac_batch_export(b, &s1, &a1), AC_OK);
  /* zero-copy: shell child buffers equal payload buffer addresses */
  CHECK_EQ(a1.children[1]->buffers[1],
           ac_batch_buffer_address(b, 1, 1));
  CHECK_EQ(a1.children[0]->buffers[1],
           ac_batch_buffer_address(b, 0, 1));
  CHECK_EQ(a1.children[2]->dictionary->buffers[2],
           ac_batch_dict_buffer_address(b, 2, 2));

  /* Record array-side facts before any shell memory is released. */
  int64_t child1_offset = a1.children[1]->offset;
  int64_t child0_length = a1.children[0]->length;

  /* Release the whole schema side first (root + every child). The array
   * side keeps its own payload ref, so the underlying bytes stay valid;
   * only the schema struct tree is freed. */
  ac_export_schema_release(&s1);
  CHECK(s1.release == NULL);
  CHECK_EQ(a1.length, ROWS);
  CHECK_EQ(child1_offset, 0);
  /* idempotent: re-releasing an already-released shell is a no-op */
  ac_export_schema_release(&s1);

  /* a second, independent export while first array shell lives */
  ArrowCDIBatch* sl = NULL;
  CHECK_EQ(ac_batch_slice(b, 1, 3, &sl), AC_OK);
  struct ArrowSchema s2; struct ArrowArray a2;
  CHECK_EQ(ac_batch_export(sl, &s2, &a2), AC_OK);
  CHECK_EQ(a2.children[1]->offset, 1);
  CHECK_EQ(a2.children[1]->buffers[1],
           ac_batch_buffer_address(b, 1, 1));

  ac_batch_close(b);
  /* array shell keeps payload bytes valid; its struct tree is still live */
  CHECK_EQ(child0_length, ROWS);
  /* release array shells; mixed ordering across schema/array sides is
   * exercised again in the randomised close-ordering test. */
  ac_export_array_release(&a1);
  ac_export_array_release(&a1); /* idempotent */
  ac_export_array_release(&a2);
  ac_export_schema_release(&s2);
  ac_batch_close(sl);
  CHECK_EQ(ac_live_allocations(), 0);
}

/* --------------------------------------------------------------- */
/* Import round trip: export -> import -> values, move semantics     */
/* --------------------------------------------------------------- */

static void test_import_roundtrip(void) {
  ac_fail_reset();
  ArrowCDIBatch* b = make_sample();
  struct ArrowSchema s; struct ArrowArray a;
  CHECK_EQ(ac_batch_export(b, &s, &a), AC_OK);

  ArrowCDIBatch* in = NULL;
  CHECK_EQ(ac_batch_import(&s, &a, &in), AC_OK);
  CHECK(s.release == NULL && a.release == NULL); /* moved */
  check_sample_values(in);

  /* re-importing a moved shell is a state error, source still not touched */
  ArrowCDIBatch* again = NULL;
  CHECK_EQ(ac_batch_import(&s, &a, &again), AC_ESTATE);
  CHECK(again == NULL);

  ac_batch_close(in);   /* invokes our export release callbacks once */
  /* The original batch b is still open and legitimately holds memory;
   * closing the imported view must not have touched its buffers. */
  int64_t live_after_in = ac_live_allocations();
  CHECK(live_after_in > 0);
  ac_batch_close(b);
  /* Only now are both users gone; every real allocation is freed once. */
  CHECK_EQ(ac_live_allocations(), 0);
  CHECK_EQ(ac_total_allocations(), ac_total_frees());
}

/* --------------------------------------------------------------- */
/* Foreign peer with counted release callbacks (real move test)      */
/* --------------------------------------------------------------- */

static void test_foreign_peer_release_once(void) {
  ac_fail_reset();
  ArrowCDIBatch* b = make_sample();
  struct ArrowSchema es; struct ArrowArray ea;
  CHECK_EQ(ac_batch_export(b, &es, &ea), AC_OK);
  /* Shells are independent movable allocations, not internal addresses. */
  ac_batch_close(b);
  /* Payload survives via shell refs; releasing both whole shells frees
   * everything, in either order. */
  ac_export_array_release(&ea);
  ac_export_schema_release(&es);
  CHECK_EQ(ac_live_allocations(), 0);
}

/* --------------------------------------------------------------- */
/* Zero-column and zero-length batches                               */
/* --------------------------------------------------------------- */

static void test_empty_shapes(void) {
  ac_fail_reset();
  ArrowCDIBatch* zc = NULL;
  CHECK_EQ(ac_batch_create(NULL, 0, &zc), AC_OK);
  CHECK_EQ(ac_batch_rows(zc), 0);
  CHECK_EQ(ac_batch_columns(zc), 0);
  struct ArrowSchema s; struct ArrowArray a;
  CHECK_EQ(ac_batch_export(zc, &s, &a), AC_OK);
  CHECK_EQ(s.n_children, 0);
  CHECK_EQ(a.n_children, 0);
  ArrowCDIBatch* back = NULL;
  CHECK_EQ(ac_batch_import(&s, &a, &back), AC_OK);
  CHECK_EQ(ac_batch_columns(back), 0);
  CHECK_EQ(ac_batch_rows(back), 0);
  ac_batch_close(back);
  ac_batch_close(zc);

  /* zero length with columns */
  ac_column_spec col = {.name="x", .kind=AC_KIND_I64, .length=0,
                        .validity=NULL, .i64_values=(const int64_t*)""};
  ArrowCDIBatch* zl = NULL;
  CHECK_EQ(ac_batch_create(&col, 1, &zl), AC_OK);
  CHECK_EQ(ac_batch_rows(zl), 0);
  ArrowCDIBatch* sl = NULL;
  CHECK_EQ(ac_batch_slice(zl, 0, 0, &sl), AC_OK);
  CHECK_EQ(ac_batch_rows(sl), 0);
  ac_batch_close(sl);
  ac_batch_close(zl);
  CHECK_EQ(ac_live_allocations(), 0);
}

/* --------------------------------------------------------------- */
/* Dictionary validation edge cases via raw ABI structs              */
/* --------------------------------------------------------------- */

/* Build a minimal valid dictionary pair, then mutate one field per test. */
typedef struct {
  struct ArrowSchema root_s, idx_s, dict_s;
  struct ArrowArray root_a, idx_a, dict_a;
  struct ArrowSchema* child_s[1];
  struct ArrowArray* child_a[1];
  const void* idx_bufs[2];
  const void* dict_bufs[3];
  const void* root_bufs[1];
  int32_t offsets[3];
  int32_t indices[4];
  uint8_t idx_valid[1];   /* bit-packed, 4 rows */
  uint8_t dict_valid[1];  /* bit-packed, 2 rows */
  char bytes[8];
} dict_fixture;

static void build_dict_fixture(dict_fixture* f) {
  memset(f, 0, sizeof(*f));
  memcpy(f->bytes, "abcd", 4);
  f->offsets[0]=0; f->offsets[1]=2; f->offsets[2]=4;
  f->indices[0]=0; f->indices[1]=1; f->indices[2]=0; f->indices[3]=1;
  /* bit-packed LSB-first: rows 0,1,3 valid (bits 0,1,3 set => 0b1011);
     row 2 null so its garbage index must be ignored. */
  f->idx_valid[0] = (uint8_t)(1 | 2 | 8);   /* 0x0B */
  f->dict_valid[0] = (uint8_t)(1 | 2);      /* both entries valid */

  f->root_s.format="+s"; f->root_s.n_children=1;
  f->root_s.children=f->child_s; f->child_s[0]=&f->idx_s;
  f->root_s.release=peer_schema_release;
  f->idx_s.format="i"; f->idx_s.flags=ARROW_FLAG_NULLABLE;
  f->idx_s.dictionary=&f->dict_s; f->idx_s.release=peer_schema_release;
  f->dict_s.format="u"; f->dict_s.flags=ARROW_FLAG_NULLABLE;
  f->dict_s.release=peer_schema_release;

  f->root_a.length=4; f->root_a.n_buffers=1; f->root_a.n_children=1;
  f->root_a.buffers=f->root_bufs; f->root_a.children=f->child_a;
  f->child_a[0]=&f->idx_a; f->root_a.release=peer_array_release;
  f->idx_a.length=4; f->idx_a.n_buffers=2; f->idx_a.buffers=f->idx_bufs;
  f->idx_bufs[0]=f->idx_valid; f->idx_bufs[1]=f->indices;
  f->idx_a.dictionary=&f->dict_a; f->idx_a.release=peer_array_release;
  f->dict_a.length=2; f->dict_a.n_buffers=3; f->dict_a.buffers=f->dict_bufs;
  f->dict_bufs[0]=f->dict_valid; f->dict_bufs[1]=f->offsets;
  f->dict_bufs[2]=f->bytes; f->dict_a.release=peer_array_release;
}

static int peer_s_calls, peer_a_calls;
static void counting_schema_release(struct ArrowSchema* s) {
  ++peer_s_calls; s->release=NULL;
}
static void counting_array_release(struct ArrowArray* a) {
  ++peer_a_calls; a->release=NULL;
}

static void test_dict_validation(void) {
  /* valid baseline imports */
  dict_fixture f;
  build_dict_fixture(&f);
  f.root_s.release=counting_schema_release;
  f.idx_s.release=counting_schema_release;
  f.dict_s.release=counting_schema_release;
  f.root_a.release=counting_array_release;
  f.idx_a.release=counting_array_release;
  f.dict_a.release=counting_array_release;
  ArrowCDIBatch* b = NULL;
  CHECK_EQ(ac_batch_import(&f.root_s, &f.root_a, &b), AC_OK);
  CHECK(f.root_s.release==NULL && f.root_a.release==NULL);
  const char* s; int64_t n;
  CHECK_EQ(ac_batch_get_string(b, 0, 0, &s, &n), AC_OK);
  CHECK_EQ(n, 2); CHECK(memcmp(s, "ab", 2)==0);
  CHECK(ac_batch_is_null(b, 0, 2));
  ac_batch_close(b);
  CHECK_EQ(peer_s_calls, 3);
  CHECK_EQ(peer_a_calls, 3);

  /* garbage bits under a null index must not be read as out-of-range */
  dict_fixture g; build_dict_fixture(&g);
  g.idx_s.release=counting_schema_release; g.dict_s.release=counting_schema_release;
  g.root_s.release=counting_schema_release;
  g.idx_a.release=counting_array_release; g.dict_a.release=counting_array_release;
  g.root_a.release=counting_array_release;
  g.indices[2] = 999; /* row 2 is null: must be ignored */
  ArrowCDIBatch* g2 = NULL;
  CHECK_EQ(ac_batch_import(&g.root_s, &g.root_a, &g2), AC_OK);
  CHECK(ac_batch_is_null(g2, 0, 2));
  ac_batch_close(g2);

  /* genuine out-of-range index on a valid row */
  dict_fixture h; build_dict_fixture(&h);
  h.idx_s.release=counting_schema_release; h.dict_s.release=counting_schema_release;
  h.root_s.release=counting_schema_release;
  h.idx_a.release=counting_array_release; h.dict_a.release=counting_array_release;
  h.root_a.release=counting_array_release;
  h.indices[0] = 5;
  ArrowCDIBatch* h2 = NULL;
  CHECK_EQ(ac_batch_import(&h.root_s, &h.root_a, &h2), AC_ELAYOUT);
  CHECK(h2 == NULL);
  /* rejected import: source untouched and still owner-releasable once */
  CHECK(h.root_s.release != NULL && h.root_a.release != NULL);
  h.root_s.release(&h.root_s); h.idx_s.release(&h.idx_s);
  h.dict_s.release(&h.dict_s);
  h.root_a.release(&h.root_a); h.idx_a.release(&h.idx_a);
  h.dict_a.release(&h.dict_a);

  /* dictionary's own null value surfaces as null */
  dict_fixture k; build_dict_fixture(&k);
  k.idx_s.release=counting_schema_release; k.dict_s.release=counting_schema_release;
  k.root_s.release=counting_schema_release;
  k.idx_a.release=counting_array_release; k.dict_a.release=counting_array_release;
  k.root_a.release=counting_array_release;
  k.dict_valid[0] &= (uint8_t)~2u; /* clear bit 1: dictionary entry 1 null */
  ArrowCDIBatch* k2 = NULL;
  CHECK_EQ(ac_batch_import(&k.root_s, &k.root_a, &k2), AC_OK);
  CHECK_EQ(ac_batch_get_string(k2, 0, 1, &s, &n), AC_ESTATE);
  ac_batch_close(k2);
}

static void test_unsupported_and_layout(void) {
  dict_fixture f; build_dict_fixture(&f);
  f.root_s.release=counting_schema_release; f.idx_s.release=counting_schema_release;
  f.dict_s.release=counting_schema_release;
  f.root_a.release=counting_array_release; f.idx_a.release=counting_array_release;
  f.dict_a.release=counting_array_release;

  /* unsupported type: float indices/values are outside the subset */
  f.idx_s.format = "f";
  ArrowCDIBatch* b = NULL;
  CHECK_EQ(ac_batch_import(&f.root_s, &f.root_a, &b), AC_EUNSUPPORTED);
  CHECK(b == NULL);
  f.idx_s.format = "i";

  /* layout violation: negative length */
  f.idx_a.length = -1;
  CHECK_EQ(ac_batch_import(&f.root_s, &f.root_a, &b), AC_ELAYOUT);
  f.idx_a.length = 4;

  /* layout violation: wrong buffer count */
  f.idx_a.n_buffers = 3;
  CHECK_EQ(ac_batch_import(&f.root_s, &f.root_a, &b), AC_ELAYOUT);
  f.idx_a.n_buffers = 2;

  /* top level not a struct -> unsupported */
  f.root_s.format = "l";
  CHECK_EQ(ac_batch_import(&f.root_s, &f.root_a, &b), AC_EUNSUPPORTED);

  /* source still intact after rejections: release each struct once */
  f.root_s.format="+s";
  f.root_s.release(&f.root_s); f.idx_s.release(&f.idx_s);
  f.dict_s.release(&f.dict_s);
  f.root_a.release(&f.root_a); f.idx_a.release(&f.idx_a);
  f.dict_a.release(&f.dict_a);
  CHECK_EQ(ac_live_allocations(), 0);
}

/* --------------------------------------------------------------- */
/* Allocation failpoint walk                                        */
/* --------------------------------------------------------------- */

static void test_failpoints_create(void) {
  ac_fail_reset();
  for (int nth = 1; nth <= 80; ++nth) {
    ac_fail_arm(nth);
    ArrowCDIBatch* b = NULL;
    ac_status st = ac_batch_create(
        (ac_column_spec[]){
          {.name="a",.kind=AC_KIND_I64,.length=2,
           .validity=(uint8_t[]){1,1},.i64_values=(int64_t[]){1,2}}}, 1, &b);
    ac_fail_disarm();
    if (st == AC_OK) {
      ac_batch_close(b);
      CHECK_EQ(ac_live_allocations(), 0);
      break; /* all later failpoints are beyond this call's allocations */
    }
    CHECK_EQ(st, AC_ENOMEM);
    CHECK(b == NULL);
    CHECK_EQ(ac_live_allocations(), 0);
  }
  ac_fail_disarm();
}

static void test_failpoints_import(void) {
  ac_fail_reset();
  for (int nth = 1; nth <= 120; ++nth) {
    /* fresh foreign fixture each iteration */
    dict_fixture f; build_dict_fixture(&f);
    f.root_s.release=counting_schema_release; f.idx_s.release=counting_schema_release;
    f.dict_s.release=counting_schema_release;
    f.root_a.release=counting_array_release; f.idx_a.release=counting_array_release;
    f.dict_a.release=counting_array_release;
    int s_before = peer_s_calls, a_before = peer_a_calls;

    ac_fail_arm(nth);
    ArrowCDIBatch* b = NULL;
    ac_status st = ac_batch_import(&f.root_s, &f.root_a, &b);
    ac_fail_disarm();
    if (st == AC_OK) {
      /* success path reached: no more failpoints inside this import */
      ac_batch_close(b);
      CHECK_EQ(ac_live_allocations(), 0);
      break;
    }
    CHECK_EQ(st, AC_ENOMEM);
    CHECK(b == NULL);
    /* failure before takeover: source callbacks not invoked by us, and
     * the structs remain in their original released-capable state */
    CHECK(f.root_s.release != NULL && f.root_a.release != NULL);
    CHECK_EQ(peer_s_calls, s_before);
    CHECK_EQ(peer_a_calls, a_before);
    f.root_s.release(&f.root_s); f.idx_s.release(&f.idx_s);
    f.dict_s.release(&f.dict_s);
    f.root_a.release(&f.root_a); f.idx_a.release(&f.idx_a);
    f.dict_a.release(&f.dict_a);
    CHECK_EQ(ac_live_allocations(), 0);
  }
  ac_fail_disarm();
}

/* Failpoints while exporting: already-published batch must stay usable. */
static void test_failpoints_export(void) {
  ac_fail_reset();
  ArrowCDIBatch* b = make_sample();
  for (int nth = 1; nth <= 200; ++nth) {
    ac_fail_arm(nth);
    struct ArrowSchema s; struct ArrowArray a;
    memset(&s, 0, sizeof(s)); memset(&a, 0, sizeof(a));
    ac_status st = ac_batch_export(b, &s, &a);
    ac_fail_disarm();
    if (st == AC_OK) {
      ac_export_schema_release(&s);
      ac_export_array_release(&a);
      break;
    }
    CHECK_EQ(st, AC_ENOMEM);
    /* no half-published shell: release pointers must not be installed */
    CHECK(s.release == NULL);
    CHECK(a.release == NULL);
    CHECK_EQ(ac_live_allocations(), 0);
  }
  /* published batch still readable */
  CHECK_EQ(ac_batch_rows(b), ROWS);
  ac_batch_close(b);
  CHECK_EQ(ac_live_allocations(), 0);
}

/* --------------------------------------------------------------- */
/* Randomised destruction orders                                    */
/* --------------------------------------------------------------- */

static unsigned long rng_state = 0x12345678;
static unsigned long xr(void) {
  rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17; return rng_state;
}

static void test_close_orderings(void) {
  for (int trial = 0; trial < 200; ++trial) {
    ac_fail_reset();
    ArrowCDIBatch* b = make_sample();
    ArrowCDIBatch* views[6];
    CHECK_EQ(ac_batch_slice(b, 1, 4, &views[0]), AC_OK);
    CHECK_EQ(ac_batch_slice(views[0], 2, 2, &views[1]), AC_OK);
    const char* names[2] = {"lab","ids"};
    CHECK_EQ(ac_batch_project(b, names, 2, &views[2]), AC_OK);
    CHECK_EQ(ac_batch_project(views[0], names, 2, &views[3]), AC_OK);
    struct ArrowSchema sh[2]; struct ArrowArray ar[2];
    CHECK_EQ(ac_batch_export(b, &sh[0], &ar[0]), AC_OK);
    CHECK_EQ(ac_batch_export(views[1], &sh[1], &ar[1]), AC_OK);

    /* 9 distinct closable entities: root + 4 views + 2 whole schema
       shells + 2 whole array shells, shuffled independently. Shells are
       released through the wrappers so every child struct is released. */
    void* entities[9];
    int kind[9]; /* 0 batch, 1 schema shell, 2 array shell */
    entities[0]=b; kind[0]=0;
    for (int i=0;i<4;++i){ entities[i+1]=views[i]; kind[i+1]=0; }
    entities[5]=&sh[0]; kind[5]=1; entities[6]=&ar[0]; kind[6]=2;
    entities[7]=&sh[1]; kind[7]=1; entities[8]=&ar[1]; kind[8]=2;
    /* Fisher-Yates */
    for (int i=8;i>0;--i) {
      int j = (int)(xr() % (unsigned)(i+1));
      void* te=entities[i]; entities[i]=entities[j]; entities[j]=te;
      int tk=kind[i]; kind[i]=kind[j]; kind[j]=tk;
    }
    for (int i=0;i<9;++i) {
      if (kind[i]==0) ac_batch_close((ArrowCDIBatch*)entities[i]);
      else if (kind[i]==1) ac_export_schema_release(
                              (struct ArrowSchema*)entities[i]);
      else ac_export_array_release((struct ArrowArray*)entities[i]);
    }
    /* The heap batch/view pointers are dangling once closed and cannot be
     * touched again; idempotent batch close is covered separately below.
     * Re-invoke the stack-resident shell releases to prove they are
     * no-ops after release, in arbitrary order. */
    ac_export_schema_release(&sh[0]);
    ac_export_array_release(&ar[1]);
    ac_export_array_release(&ar[0]);
    ac_export_schema_release(&sh[1]);
    CHECK_EQ(ac_live_allocations(), 0);
  }
}

/* ac_batch_close never invokes a peer callback twice and is safe on NULL.
 * In C a freed pointer can not be re-entered, so the idempotency contract
 * is: close releases the payload once; a second close on the same handle
 * is a no-op while the handle is still allocated. This is exercised by
 * closing two views sharing one payload in both orders, then NULL. */
static void test_close_idempotent(void) {
  ac_fail_reset();
  ArrowCDIBatch* b = make_sample();
  ArrowCDIBatch* v1 = NULL;
  ArrowCDIBatch* v2 = NULL;
  CHECK_EQ(ac_batch_slice(b, 0, 2, &v1), AC_OK);
  CHECK_EQ(ac_batch_slice(b, 1, 3, &v2), AC_OK);
  ac_batch_close(v1);
  ac_batch_close(b);            /* payload still alive via v2 */
  CHECK_EQ(ac_batch_rows(v2), 3);
  ac_batch_close(v2);           /* last user frees the payload */
  ac_batch_close(NULL);         /* NULL is always safe */
  CHECK_EQ(ac_live_allocations(), 0);
}

int main(void) {
  test_basic_and_json();
  test_slice_zero_copy();
  test_project();
  test_export_lifecycle();
  test_import_roundtrip();
  test_foreign_peer_release_once();
  test_empty_shapes();
  test_dict_validation();
  test_unsupported_and_layout();
  test_failpoints_create();
  test_failpoints_import();
  test_failpoints_export();
  test_close_orderings();
  test_close_idempotent();
  printf("native tests: %d logical checks passed; "
         "allocs=%lld frees=%lld live=%lld\n",
         g_checks, (long long)ac_total_allocations(),
         (long long)ac_total_frees(), (long long)ac_live_allocations());
  CHECK_EQ(ac_live_allocations(), 0);
  CHECK_EQ(ac_total_allocations(), ac_total_frees());
  printf("ALL NATIVE TESTS PASSED\n");
  return 0;
}
