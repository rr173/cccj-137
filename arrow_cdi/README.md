# arrow_cdi

In-process, zero-copy hand-off of Apache Arrow record batches between a C
producer, a Python cleaning stage and an independent native consumer, using
the **Apache Arrow C Data Interface** (`ArrowSchema` / `ArrowArray`).

The slice, project, import, export and release logic is implemented from
scratch in C; PyArrow is used only as the interoperability peer, never as a
source of slicing functions. Per-value Python object conversion is avoided
on the data path — values stay in shared native buffers, with JSON used only
for demonstration.

## What it does

* Builds **owning** record batches from caller-side C buffers.
* **Imports** batches exported by another producer (e.g. PyArrow) by moving
  the C ABI structs per the official release/move contract.
* `slice(start, length)` and `project(names)` as zero-copy views; slicing
  never copies values, offsets or validity, and composes correctly over
  arrays that already carry a nonzero offset.
* `export` produces independent, movable ABI shells (no internal struct
  address is exposed as a permanent handle).
* Explicit, idempotent, GC-safe `close`; schema and array sides can be
  released independently and in any order.

## Supported subset

* Top level: record-batch struct (`+s`, also accepts `+rs`).
* Columns: `int64` (`l`), UTF-8 string (`u`), and `int32`-indexed (`i`)
  dictionary over nullable UTF-8 (`u`). Every column is nullable.
* Null dictionary indices ignore their payload bits; valid indices must be
  in dictionary range; null dictionary values surface as null.
* Zero-column and zero-length batches are legal.

Full details and the rejection taxonomy:
[`docs/SUPPORTED_SUBSET.md`](docs/SUPPORTED_SUBSET.md).
Contract wording with traceable upstream links:
[`docs/INTERFACE.md`](docs/INTERFACE.md).
Ownership/refcount/failpoint design:
[`docs/OWNERSHIP.md`](docs/OWNERSHIP.md).

## Repository layout

```
include/arrow_cdata_interface.h  verbatim upstream ABI definitions
include/arrow_cdi.h              public C API
src/                             C library (alloc, payload/import, create, export)
py/arrowcdi/                     ctypes wrapper (Batch, slices, import/export)
tests/native_tests.c             ASan native suite (failpoints, orderings)
tests/py_tests.py                Python + PyArrow interop and edge-case suite
demo/consumer.c                  independent native ABI consumer (.so + CLI)
demo/run_demo.py                 end-to-end in-process demonstration
scripts/run_all.sh               one-shot reproducible build + verification
docs/                            contract references, subset, ownership
```

## Requirements

* gcc with C11 and AddressSanitizer, GNU make.
* Python 3.10+ with `pyarrow` available on the import path.

The sandboxed environment here installed pyarrow into the user site and
exposed it to isolated (`-I`) runs via a symlink under `py/`:

```sh
python3 get-pip.py --user --break-system-packages
python3 -m pip install --user --break-system-packages pyarrow
ln -s "$(python3 -c 'import site;print(site.getusersitepackages())')/pyarrow" \
      py/pyarrow
```

## Build and test

```sh
# Release shared/static libraries and the in-process consumer shared lib
make CC=gcc all

# ASan-instrumented libraries, native tests, ASan consumer binary + .so
make CC=gcc asan

# Native suite under AddressSanitizer (failpoints + destruction orderings)
make CC=gcc test

# Python/PyArrow interop suite (run with -I; the script inserts py/ itself)
python3 -I tests/py_tests.py

# End-to-end demo: C create -> PyArrow read -> PyArrow nonzero-offset batch
# -> import -> two slices -> reversed projection -> native C consumer
python3 -I demo/run_demo.py

# Everything, deterministically
./scripts/run_all.sh
```

## Demonstration guarantees

`demo/run_demo.py` prints, and asserts:

1. A C-created batch with interleaved nulls, an empty string, multibyte
   UTF-8 and repeated dictionary indices (including a null dictionary
   value) reads correctly through PyArrow.
2. Buffer addresses are **identical** on both sides of the boundary for
   values, offsets, bytes, validity, indices and dictionary bytes —
   proving zero copy rather than pointer aliasing of a copy.
3. A PyArrow chained-slice batch (nonzero physical offset) is imported,
   sliced twice and projected in reverse; logical values match the
   original rows and the offsets compose.
4. An independent C consumer (`demo/consumer.c`, linked only against the
   ABI header) verifies the rows from the two shell addresses in-process.
5. After closing the root, a projection and shells in arbitrary order, a
   surviving slice still reads, and the real allocation/free counters
   return to zero (`allocs == frees`, `live == 0`).

## Validation boundary

The C Data Interface carries no per-buffer allocation length, so no
consumer can prove pointer reachability from the contract alone. The
library validates everything the contract exposes (schema support,
child/buffer counts, non-negative length/offset, offset monotonicity,
dictionary index range, required non-NULL buffers) and distinguishes
rejected logical layouts (`AC_ELAYOUT`) from unsupported types
(`AC_EUNSUPPORTED`). Test producers guarantee readable, sufficiently
allocated memory and only inject logical errors.
