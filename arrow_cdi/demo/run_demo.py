#!/usr/bin/env python3
"""End-to-end, fully in-process demonstration.

Flow (all within one process, one address space):

  1. The C library creates a record batch from caller C buffers, containing
     interleaved nulls, an empty string, multibyte UTF-8 and repeated
     dictionary indices (with a null dictionary value).
  2. Python exports it through the Apache Arrow C Data Interface and reads
     it with PyArrow (the interoperability peer) - zero copies.
  3. PyArrow produces a *nonzero-offset* batch (a chained slice); the
     library imports that moved pair, slices it twice, projects columns in
     reverse order and exports again.
  4. An independent native C consumer (demo/consumer.c) receives only the
     two shell addresses and verifies the original rows/values through the
     raw ABI - it links no private producer structure.
  5. Buffer addresses are compared to prove zero copy; string logical
     values carry hand-written expectations.
  6. The root batch, a schema shell and a projection are closed first; a
     surviving slice stays readable. After everything closes, the real
     allocation/free counters return to zero.

Run:  python3 -I demo/run_demo.py
"""
from __future__ import annotations

import ctypes
import gc
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "py"))

import arrowcdi as ac          # noqa: E402
from arrowcdi import abi       # noqa: E402
import pyarrow as pa           # noqa: E402

# Independent native C consumer, built as a shared library and loaded
# in-process so the shell pointers it receives stay valid. It reads only
# the ABI, never a private producer structure.
CONSUMER_LIB = os.path.join(ROOT, "build", "libconsumer.so")


def load_consumer() -> ctypes.CDLL:
    lib = ctypes.CDLL(CONSUMER_LIB)
    lib.arrow_cdi_consume.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.arrow_cdi_consume.restype = ctypes.c_int
    return lib


def hr(title: str) -> None:
    print("\n" + "=" * 72)
    print(title)
    print("=" * 72)


def build_c_batch() -> ac.Batch:
    """Step 1: C library creates the edge-case batch from C buffers."""
    n = 6
    ids_valid = (ctypes.c_uint8 * n)(1, 0, 1, 1, 0, 1)
    ids = (ctypes.c_int64 * n)(10, 20, 30, 40, 50, 60)

    text_valid = (ctypes.c_uint8 * n)(1, 1, 1, 0, 1, 1)
    text_off = (ctypes.c_int32 * 7)(0, 6, 6, 9, 9, 13, 16)
    text_bytes = b"h\xc3\xa9llo\xe6\x97\xa5\xce\xb1\xce\xb2xyz"  # 16 bytes
    text_data = (ctypes.c_uint8 * 16)(*text_bytes)

    idx_valid = (ctypes.c_uint8 * n)(1, 1, 0, 1, 1, 1)
    indices = (ctypes.c_int32 * n)(0, 2, 1, 0, 2, 1)
    dict_valid = (ctypes.c_uint8 * 3)(1, 0, 1)  # entry 1 is null
    dict_off = (ctypes.c_int32 * 4)(0, 5, 5, 9)
    dict_bytes = (ctypes.c_uint8 * 9)(*b"alphabeta")
    dictionary = ac.DictSpec(3, dict_valid, dict_off, dict_bytes, 9)

    columns = [
        ac.ColumnSpec("ids", ac.Kind.I64, n, ids_valid, i64_values=ids),
        ac.ColumnSpec("text", ac.Kind.UTF8, n, text_valid,
                      str_offsets=text_off, str_data=text_data,
                      str_data_nbytes=16),
        ac.ColumnSpec("lab", ac.Kind.DICT, n, idx_valid,
                      dict_indices=indices, dictionary=dictionary),
    ]
    return ac.Batch.from_c_buffers(columns)


def main() -> int:
    ac.fail_reset()

    hr("STEP 1-2  C library batch -> C Data Interface -> PyArrow")
    batch = build_c_batch()
    print("library JSON (demo rendering, not a scalar-list data path):")
    print(" ", batch.to_json())

    shells = batch.export()
    pa_batch = pa.RecordBatch._import_from_c(
        shells.array_address(), shells.schema_address())
    print("\nPyArrow read the moved pair as:")
    print(pa_batch.schema)
    print("pydict:", pa_batch.to_pydict())

    # Zero-copy proof across the C -> PyArrow boundary.
    checks = [
        ("utf8 offsets", pa_batch.column(1).buffers()[1].address,
         batch.buffer_address(1, 1)),
        ("utf8 bytes", pa_batch.column(1).buffers()[2].address,
         batch.buffer_address(1, 2)),
        ("i64 values", pa_batch.column(0).buffers()[1].address,
         batch.buffer_address(0, 1)),
        ("dict indices", pa_batch.column(2).indices.buffers()[1].address,
         batch.buffer_address(2, 1)),
        ("dict values bytes",
         pa_batch.column(2).dictionary.buffers()[2].address,
         batch.dict_buffer_address(2, 2)),
    ]
    print("\nzero-copy buffer address comparison:")
    all_shared = True
    for label, pa_addr, lib_addr in checks:
        same = pa_addr == lib_addr
        all_shared &= same
        print(f"  {label:16s} pyarrow={pa_addr:#x} library={lib_addr:#x} "
              f"shared={same}")
    assert all_shared, "expected every buffer to be shared, not copied"

    # Hand-written logical expectations for the string column.
    expected_text = ["héllo", "", "日", None, "αβ", "xyz"]
    actual_text = [batch.get_string(1, r) for r in range(batch.rows)]
    assert actual_text == expected_text, (actual_text, expected_text)
    print("hand-written string expectations:", expected_text, "OK")
    del pa_batch  # PyArrow ran the moved shell release callbacks

    hr("STEP 3  PyArrow nonzero-offset batch -> import -> slice/project")
    # Build a fresh PyArrow batch and apply a chained slice so the exported
    # array carries a nonzero absolute offset.
    pa_text = pa.array(
        ["aa", "bb", "cc", "wörld", "αβ", "xyz", "zz"], pa.utf8())
    pa_ids = pa.array([10, 20, 30, 40, None, 60, 70], pa.int64())
    pa_lab_idx = pa.array([0, 1, 2, 0, 1, 2, 0], pa.int32())
    pa_lab_dict = pa.array(["alpha", "beta", "gamma"], pa.utf8())
    pa_lab = pa.DictionaryArray.from_arrays(pa_lab_idx, pa_lab_dict)
    source = pa.RecordBatch.from_arrays(
        [pa_text, pa_ids, pa_lab], names=["text", "ids", "lab"])
    sliced = source.slice(2, 5).slice(1, 3)   # logical rows 3,4,5

    raw_schema = abi.ArrowSchema()
    raw_array = abi.ArrowArray()
    sliced._export_to_c(ctypes.addressof(raw_array),
                        ctypes.addressof(raw_schema))
    phys_offsets = [raw_array.children[i].contents.offset for i in range(3)]
    print("PyArrow chained slice physical child offsets:", phys_offsets)
    assert any(o != 0 for o in phys_offsets), "expected a nonzero offset"

    imported = ac.Batch.import_move(raw_schema, raw_array)
    assert not raw_schema.release and not raw_array.release, \
        "successful import must move (release == NULL) the source"
    print("import moved the pair; imported rows =", imported.rows)

    # Two slices, then a reversed projection - all zero-copy views.
    slice_a = imported.slice(0, 3)
    slice_b = slice_a.slice(1, 2)   # re-slicing a nonzero-offset view
    reversed_view = slice_b.project(["lab", "text", "ids"])
    print("reversed projection column order:",
          [reversed_view.column_name(i) for i in range(3)])
    print("physical offset of text column after both slices:",
          reversed_view.physical_offset(1))

    # Logical values must match the original PyArrow rows 4 and 5.
    assert reversed_view.get_string(1, 0) == "αβ"
    assert reversed_view.get_string(1, 1) == "xyz"
    assert reversed_view.get_i64(2, 0) is None      # original row 4 null
    assert reversed_view.get_i64(2, 1) == 60
    print("re-sliced logical values verified against original rows")

    hr("STEP 4  Independent native C consumer over the raw ABI")
    out = reversed_view.export()
    print("exported fresh movable shells:",
          hex(out.schema_address()), hex(out.array_address()))
    # The consumer only gets two integers and is loaded in the same
    # address space; it validates and reads the structs through the ABI.
    if not os.path.exists(CONSUMER_LIB):
        print("consumer library not found; run 'make asan' first")
        return 2
    consumer = load_consumer()
    rc = consumer.arrow_cdi_consume(out.schema_address(),
                                    out.array_address())
    if rc != 0:
        print("consumer reported failure", rc)
        return rc
    # The consumer released the shells it was given, exactly once.
    assert not out.schema.release and not out.array.release

    hr("STEP 5-6  Destruction order and allocation accounting")
    print("close root batch, then one schema-style view, then projection")
    proj_keep = imported.project(["text"])
    batch.close()                 # original C-created batch
    imported.close()              # imported root
    reversed_view.close()
    # A surviving slice must still read its shared bytes.
    print("surviving slice_b rows =", slice_b.rows,
          "text[0] =", slice_b.get_string(0, 0))
    assert slice_b.get_string(0, 0) == "αβ"
    slice_a.close()
    slice_b.close()
    proj_keep.close()
    out.release()  # idempotent no-op: consumer already released them
    out.release()

    # GC sweep to prove the Python release path needs no lucky ordering.
    gc_guard = [build_c_batch().slice(0, 2) for _ in range(5)]
    del gc_guard
    gc.collect()

    live = ac.live_allocations()
    print(f"\nreal allocation counters: allocs={ac.total_allocations()} "
          f"frees={ac.total_frees()} live={live}")
    assert live == 0, "all native allocations must be released exactly once"
    assert ac.total_allocations() == ac.total_frees()
    print("\nDEMO COMPLETE: zero-copy hand-off, correct values, and full "
          "cleanup verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
