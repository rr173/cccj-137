# Apache Arrow C Data Interface — contract references

This project exchanges data in-process using the **Apache Arrow C Data
Interface** (the `ArrowSchema` / `ArrowArray` ABI), not the C Stream
Interface and not any private shared structure. The canonical header is
embedded verbatim at `include/arrow_cdata_interface.h`.

## Primary sources

| Topic | Upstream document |
| --- | --- |
| C Data Interface overview, structure definitions, release/move semantics | https://arrow.apache.org/docs/format/CDataInterface.html |
| Canonical ABI header `abi.h` | https://github.com/apache/arrow/blob/main/cpp/src/arrow/c/abi.h |
| Format strings (`+s`, `l`, `u`, `i`, dictionary encoding) | https://arrow.apache.org/docs/format/CDataInterface.html#data-type-description-format-strings |
| Buffer layouts and validity bitmaps | https://arrow.apache.org/docs/format/Columnar.html#physical-memory-layout |
| Variable-size binary / UTF-8 offset semantics | https://arrow.apache.org/docs/format/Columnar.html#variable-size-binary-layout |
| Dictionary encoding | https://arrow.apache.org/docs/format/CDataInterface.html#dictionary-encoded-arrays |
| PyArrow C interface entry points | https://arrow.apache.org/docs/python/integration/extending.html#the-arrow-array-and-arrow-schema-c-interface |
| PyArrow `RecordBatch._export_to_c` / `_import_from_c` | https://arrow.apache.org/docs/python/generated/pyarrow.RecordBatch.html |

The exact wording the implementation relies on is summarised below, each
with the section it comes from so the behaviour is traceable.

## 1. Structures and versioning

From *"The C Data Interface / The structure definitions"*:

> The C interface is defined in this header file … Any project can embed
> it and use it to exchange data in the Arrow format.

`ArrowSchema` describes a logical type (format string, flags, children, an
optional dictionary); `ArrowArray` describes one logical array (length,
null count, offset, buffer and child pointers, an optional dictionary).
The fields are declared in the same order as upstream `abi.h`; the layout
is not modified.

## 2. Release callback and move semantics

From *"Memory management / Release callbacks and lifetime"*:

> The release callback … provides a way for the producer to release the
> resources associated with the interface. … The release callback must set
> the release field to NULL.

Consequences implemented here:

* A struct with `release == NULL` means **released/moved** and must not be
  consumed again.
* On **successful import**, the library moves the source: it takes the
  structs and sets every source `release` to NULL. The previous holder must
  not free them.
* On **failed import**, validation runs before any takeover, so the source
  is left completely untouched (callback still set, still caller-owned).
* Releasing is idempotent at the shell level: invoking an already-released
  shell's release is a no-op, and a peer release callback is invoked at
  most once.

## 3. `length` and `offset` are signed and non-negative

From the array field description:

> The `length` and `offset` fields are signed integers, and must be
> non-negative.

The validator rejects negative `length`/`offset` as `AC_ELAYOUT` contract
violations (distinct from "type not in the supported subset", which is
`AC_EUNSUPPORTED`).

## 4. Buffers and the validity bitmap

From *Columnar Specification / Physical memory layout*:

> Buffers are contiguous memory regions … the first buffer of a nullable
> array is a validity bitmap.

* The validity buffer is **bit-packed**, one bit per value, least
  significant bit first. A set bit means valid. The library accepts
  one-byte-per-row convenience buffers at creation and bit-packs them;
  imported/exported payloads always carry the canonical bitmap.
* A null entry's other bits are arbitrary and must not be interpreted. The
  dictionary index validator skips null rows **before** reading the index,
  so garbage under a null index can never be reported as out of range.
* The interface does **not** carry the physical allocation length of any
  buffer. This library therefore does not claim to validate arbitrary
  malicious raw pointers; tests guarantee readable, sufficiently allocated
  memory and only construct *logical* errors.

## 5. Variable-size UTF-8 offsets are element offsets, not byte offsets

From *Variable-size binary layout*:

> The offsets buffer contains length + 1 signed integers … where the
> offsets are the positions where the array's values start in the values
> buffer.

`u` (UTF-8) columns therefore store `int32` *element* offsets into the byte
buffer. The array's own `offset` field is a logical row offset and is never
added to byte addresses. The validator requires non-negative, monotonically
non-decreasing offsets. Slicing changes only the array `offset`/`length`;
it never copies or rewrites the offset buffer.

## 6. Dictionary encoding

From *Dictionary-encoded arrays*:

> A dictionary array is an array of integers (indices) referencing a
> separate, usually longer, array of values (the dictionary). The schema's
> dictionary field holds the value type; the array's dictionary field holds
> the value array.

Supported here: `int32` indices (`i`) over nullable UTF-8 (`u`) values.
Every valid index must satisfy `0 <= index < dictionary.length`; null
indices are exempt. A dictionary value that is itself null surfaces as a
null logical value even when the index is valid.

## 7. Record batch top level

PyArrow exports a `RecordBatch` as a struct array whose schema format is
`+s`. The dense non-nullable spelling `+rs` is also accepted on import. The
root struct has one (possibly NULL) buffer and one child array per column;
all children share the root's logical row count.

## Threat model and validation boundary

The interface gives no per-buffer allocation size, so no consumer can
distinguish a short valid buffer from a dangling pointer using the contract
alone. This library validates everything the contract *does* expose —
supported schema, child/buffer counts, non-negative length/offset, offset
monotonicity, dictionary index range, presence of required non-NULL buffers
— and explicitly documents that physical pointer safety is the producer's
guarantee. The test producers honour that guarantee and only inject logical
errors.
