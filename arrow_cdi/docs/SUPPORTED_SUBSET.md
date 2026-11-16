# Supported subset

Everything the library accepts is listed here. Anything else is rejected
with an explicit status, never silently reinterpreted.

## Top level

* A **record batch** struct array. Schema format `+s` accepted (what
  PyArrow emits for `RecordBatch` export); the dense spelling `+rs` is also
  accepted. The root is non-nullable, has one buffer slot (unused) and one
  child per column, all sharing the root row count.
* Zero-column batches are legal and retain the root row count.
* Zero-length batches (including zero-length columns) are legal.

## Column types

| Kind | Schema format | Buffers (canonical order) | Notes |
| --- | --- | --- | --- |
| 64-bit signed integer | `l` | validity, `int64` values | nullable |
| UTF-8 string | `u` | validity, `int32` offsets, bytes | nullable; offsets element-based |
| Dictionary | `i` indices + `u` values | index: validity, `int32` indices; values: validity, `int32` offsets, bytes | nullable indices and nullable dictionary values |

All columns are nullable. A missing validity buffer means "all valid".

## Dictionary rules

* Index type is exactly `int32` (`i`); any other integer width is
  `AC_EUNSUPPORTED`.
* Dictionary value type is exactly UTF-8 (`u`); any other value type is
  `AC_EUNSUPPORTED`.
* For every **valid** row, `0 <= index < dictionary.length`. A violation is
  `AC_ELAYOUT`.
* A **null** row is skipped before reading the index, so arbitrary bits
  stored under a null index can never trigger an out-of-range rejection.
* A null dictionary value produces a null logical value even for a valid
  index.
* The `ARROW_FLAG_DICTIONARY_ORDERED` flag is preserved on export.

## String rules

* Offsets are `int32`, length + 1 of them, non-negative and monotonically
  non-decreasing. They are element offsets into the byte buffer, never byte
  offsets derived from the array's row `offset`.
* Empty strings (zero-width span) are supported and distinct from nulls.
* Multibyte UTF-8 is passed through unchanged; the JSON demo does not
  re-encode it.

## Operations

* `ac_batch_create` — owning batch from caller C buffers (buffers are copied
  once; validity is bit-packed).
* `ac_batch_import` — move an externally exported schema/array pair.
* `ac_batch_slice(start, length)` — zero-copy view; composes over
  already-offset arrays without copying values, offsets or validity.
* `ac_batch_project(names)` — zero-copy column selection/reordering;
  duplicate output names are rejected; an empty projection keeps the root
  row count.
* `ac_batch_export` — fresh, independently movable ABI shells whose buffer
  pointers alias the shared payload.
* `ac_batch_close` — release a view; idempotent and GC-safe on the Python
  side.

## Rejection status taxonomy

| Status | Meaning | Example |
| --- | --- | --- |
| `AC_EINVAL` | Bad caller arguments / request | slice out of bounds, unknown projection name, duplicate names |
| `AC_EUNSUPPORTED` | Outside the supported subset (not a contract violation) | float column, int64 dictionary indices, non-struct top level |
| `AC_ELAYOUT` | C Data Interface contract/layout violation | negative length/offset, wrong buffer/child count, non-monotonic offsets, out-of-range valid index |
| `AC_ENOMEM` | Allocation failure (including an armed failpoint) | any allocation point |
| `AC_ESTATE` | Use after close / already-moved shell | re-importing a released pair |

## Explicitly out of scope

* Stream interface (`ArrowArrayStream`).
* Nested types beyond the single record-batch struct (lists, maps, unions,
  nested structs).
* Non-int32 dictionary indices and non-UTF-8 dictionaries.
* Float, boolean, date/timestamp, decimal, binary, and other primitive
  formats.
* Physical validation of buffer allocation sizes or pointer reachability
  (not expressible in the interface; producer-guaranteed).
