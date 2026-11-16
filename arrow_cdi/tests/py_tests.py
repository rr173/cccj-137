"""Self-contained Python tests (no pytest dependency).

Run with:  python3 -I tests/py_tests.py
The script inserts py/ on sys.path itself so it works under ``-I``.
These tests complement the ASan-native suite: they exercise the ctypes
wrapper, PyArrow interop, move semantics on failure, and GC-driven close.
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

try:
    import pyarrow as pa       # noqa: E402
    HAVE_PA = True
except Exception:
    HAVE_PA = False

PASS = 0
FAIL = 0


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        raise AssertionError(msg)


def eq(a, b, msg):
    check(a == b, f"{msg}: {a!r} != {b!r}")


# ---------------------------------------------------------------------
# Sample batch builders
# ---------------------------------------------------------------------

def sample_batch():
    n = 6
    iv = (ctypes.c_uint8 * n)(1, 0, 1, 1, 0, 1)
    ivals = (ctypes.c_int64 * n)(10, 20, 30, 40, 50, 60)
    sv = (ctypes.c_uint8 * n)(1, 1, 1, 0, 1, 1)
    soff = (ctypes.c_int32 * 7)(0, 6, 6, 9, 9, 13, 16)
    sdata = (ctypes.c_uint8 * 16)(
        *b"h\xc3\xa9llo\xe6\x97\xa5\xce\xb1\xce\xb2xyz")
    dv = (ctypes.c_uint8 * n)(1, 1, 0, 1, 1, 1)
    didx = (ctypes.c_int32 * n)(0, 2, 1, 0, 2, 1)
    dval = (ctypes.c_uint8 * 3)(1, 0, 1)
    doff = (ctypes.c_int32 * 4)(0, 5, 5, 9)
    ddata = (ctypes.c_uint8 * 9)(*b"alphabeta")
    dspec = ac.DictSpec(3, dval, doff, ddata, 9)
    cols = [
        ac.ColumnSpec("ids", ac.Kind.I64, n, iv, i64_values=ivals),
        ac.ColumnSpec("text", ac.Kind.UTF8, n, sv, str_offsets=soff,
                      str_data=sdata, str_data_nbytes=16),
        ac.ColumnSpec("lab", ac.Kind.DICT, n, dv, dict_indices=didx,
                      dictionary=dspec),
    ]
    # Keep backing arrays alive for the call; the library copies anyway.
    return ac.Batch.from_c_buffers(cols)


def test_basic_values():
    ac.fail_reset()
    b = sample_batch()
    eq(b.rows, 6, "rows")
    eq(b.columns, 3, "cols")
    eq(b.column_name(1), "text", "name")
    check(b.is_null(0, 1), "i64 row1 null")
    eq(b.get_i64(0, 0), 10, "i64 value")
    check(b.get_i64(0, 1) is None, "null i64 -> None")
    eq(b.get_string(1, 0), "héllo", "utf8 multibyte")
    eq(b.get_string(1, 1), "", "empty string")
    check(b.get_string(1, 3) is None, "null string")
    eq(b.get_string(1, 4), "αβ", "greek utf8")
    # dictionary: row 2 null index; dict entry 1 itself null
    check(b.is_null(2, 2), "null index")
    eq(b.get_string(2, 0), "alpha", "dict resolve")
    check(b.get_string(2, 5) is None, "dict null entry -> null")
    b.close()
    eq(ac.live_allocations(), 0, "zero after close")


def test_slice_zero_copy_and_compose():
    ac.fail_reset()
    b = sample_batch()
    base_off = b.buffer_address(1, 1)
    base_val = b.buffer_address(0, 1)
    s1 = b.slice(2, 4)
    s2 = s1.slice(1, 2)  # chained nonzero offset
    eq(b.physical_offset(1), 0, "root offset")
    eq(s2.physical_offset(1), 3, "composed offset")
    eq(s2.rows, 2, "sliced rows")
    # buffer addresses identical -> no copy of values/offsets/validity
    eq(s1.buffer_address(1, 1), base_off, "no copy offsets")
    eq(s2.buffer_address(0, 1), base_val, "no copy values")
    check(s2.is_null(1, 0), "original row3 null via chained slice")
    eq(s2.get_string(1, 1), "αβ", "chained slice value")
    # bounds
    try:
        b.slice(-1, 1); check(False, "negative start rejected")
    except ac.ArrowCdiError as e:
        eq(int(e.status), ac.Status.ELAYOUT, "negative -> ELAYOUT")
    try:
        b.slice(0, 99); check(False, "overlong rejected")
    except ac.ArrowCdiError as e:
        eq(int(e.status), ac.Status.EINVAL, "overlong -> EINVAL")
    s2.close(); s1.close(); b.close()
    eq(ac.live_allocations(), 0, "zero after all close")


def test_project_rules():
    ac.fail_reset()
    b = sample_batch()
    rev = b.project(["lab", "text", "ids"])
    eq(rev.column_name(0), "lab", "reorder first")
    eq(rev.column_name(2), "ids", "reorder last")
    empty = b.project([])
    eq(empty.columns, 0, "zero columns")
    eq(empty.rows, 6, "empty projection keeps row count")
    try:
        b.project(["ids", "ids"]); check(False, "dup name rejected")
    except ac.ArrowCdiError as e:
        eq(int(e.status), ac.Status.EINVAL, "dup -> EINVAL")
    try:
        b.project(["nope"]); check(False, "missing name rejected")
    except ac.ArrowCdiError as e:
        eq(int(e.status), ac.Status.EINVAL, "missing -> EINVAL")
    rev.close(); empty.close(); b.close()
    eq(ac.live_allocations(), 0, "zero")


def test_zero_column_and_zero_length():
    ac.fail_reset()
    zc = ac.Batch.from_c_buffers([])
    eq(zc.rows, 0, "zero-col rows")
    eq(zc.columns, 0, "zero-col count")
    sh = zc.export()
    back = ac.Batch.import_move(sh.schema, sh.array)
    eq(back.columns, 0, "roundtrip zero-col")
    eq(back.rows, 0, "roundtrip zero-col rows")
    back.close(); zc.close()
    eq(ac.live_allocations(), 0, "zero after zero-col")

    vals = (ctypes.c_int64 * 0)()
    spec = ac.ColumnSpec("x", ac.Kind.I64, 0, None, i64_values=vals)
    zl = ac.Batch.from_c_buffers([spec])
    eq(zl.rows, 0, "zero-length rows")
    sl = zl.slice(0, 0)
    eq(sl.rows, 0, "zero slice")
    sl.close(); zl.close()
    eq(ac.live_allocations(), 0, "zero after zero-length")


def test_json_demo():
    ac.fail_reset()
    b = sample_batch()
    js = b.to_json()
    check('"rows": 6' in js, "json has rows")
    check("héllo" in js, "json preserves multibyte")
    check("null" in js, "json renders nulls")
    b.close()
    eq(ac.live_allocations(), 0, "zero after json")


def test_gc_close_and_idempotent():
    ac.fail_reset()
    b = sample_batch()
    s = b.slice(1, 3)
    p = b.project(["ids"])
    # Drop references without explicit close; GC must run C release.
    del s, p
    gc.collect()
    # b still open keeps payload alive
    eq(b.rows, 6, "parent survives child GC")
    del b
    gc.collect()
    eq(ac.live_allocations(), 0, "GC released everything")

    # explicit double close is a safe no-op
    b2 = sample_batch()
    b2.close()
    b2.close()
    eq(ac.live_allocations(), 0, "double close safe")


def test_failpoint_import_keeps_source():
    if not HAVE_PA:
        return
    ac.fail_reset()
    b = sample_batch()
    # Arm failpoints across import of a fresh shell each iteration.
    reached_success = False
    for nth in range(1, 120):
        sh = b.export()
        ac.fail_arm(nth)
        try:
            imported = ac.Batch.import_move(sh.schema, sh.array)
        except ac.ArrowCdiError as e:
            ac.fail_disarm()
            eq(int(e.status), ac.Status.ENOMEM, "only ENOMEM from failpoint")
            # Failed import must not have taken over the source.
            check(bool(sh.schema.release), "source schema still owned")
            check(bool(sh.array.release), "source array still owned")
            sh.release()
            continue
        ac.fail_disarm()
        reached_success = True
        eq(imported.rows, 6, "imported rows")
        # success moved the source
        check(not sh.schema.release, "schema moved on success")
        check(not sh.array.release, "array moved on success")
        imported.close()
        break
    check(reached_success, "import success path reached")
    b.close()
    eq(ac.live_allocations(), 0, "zero after failpoint walk")


def test_pyarrow_roundtrip_and_rejected_imports():
    if not HAVE_PA:
        print("  (skip PyArrow tests: not installed)")
        return
    ac.fail_reset()
    b = sample_batch()
    sh = b.export()
    rb = pa.RecordBatch._import_from_c(sh.array_address(),
                                       sh.schema_address())
    eq(rb.num_rows, 6, "pa rows")
    eq(rb.num_columns, 3, "pa cols")
    # zero-copy address equality on every buffer class
    eq(rb.column(1).buffers()[1].address, b.buffer_address(1, 1),
       "utf8 offsets zero-copy")
    eq(rb.column(1).buffers()[2].address, b.buffer_address(1, 2),
       "utf8 data zero-copy")
    eq(rb.column(0).buffers()[1].address, b.buffer_address(0, 1),
       "i64 values zero-copy")
    eq(rb.column(2).indices.buffers()[1].address,
       b.buffer_address(2, 1), "dict index zero-copy")
    eq(rb.column(2).dictionary.buffers()[2].address,
       b.dict_buffer_address(2, 2), "dict data zero-copy")
    # PyArrow took over the moved shells; dropping rb runs their release.
    del rb
    gc.collect()
    check(not sh.schema.release, "pyarrow moved the schema shell")
    b.close()
    gc.collect()
    eq(ac.live_allocations(), 0, "zero after pa roundtrip")


def test_import_bad_index_and_unsupported():
    if not HAVE_PA:
        return
    # Build a PyArrow dictionary batch, export raw, then corrupt one index.
    idx = pa.array([0, 1, 0], pa.int32())
    dic = pa.array(["a", "b"], pa.utf8())
    da = pa.DictionaryArray.from_arrays(idx, dic)
    rb = pa.RecordBatch.from_arrays([da], names=["d"])
    s = abi.ArrowSchema(); a = abi.ArrowArray()
    rb._export_to_c(ctypes.addressof(a), ctypes.addressof(s))
    # valid index range is [0,2); force an out-of-range value on row 0
    child = a.children[0].contents
    indices_ptr = ctypes.cast(child.buffers[1],
                              ctypes.POINTER(ctypes.c_int32))
    original = indices_ptr[child.offset]
    indices_ptr[child.offset] = 99
    try:
        ac.Batch.import_move(s, a)
        check(False, "out-of-range index must be rejected")
    except ac.ArrowCdiError as e:
        eq(int(e.status), ac.Status.ELAYOUT, "bad index -> ELAYOUT")
    indices_ptr[child.offset] = original
    # source untouched by failed import: PyArrow can still release it
    check(bool(s.release), "source schema intact after rejection")
    check(bool(a.release), "source array intact after rejection")
    s.release(s); a.release(a)

    # unsupported type rejection (float)
    rb2 = pa.RecordBatch.from_arrays([pa.array([1.0], pa.float64())],
                                     names=["f"])
    s2 = abi.ArrowSchema(); a2 = abi.ArrowArray()
    rb2._export_to_c(ctypes.addressof(a2), ctypes.addressof(s2))
    try:
        ac.Batch.import_move(s2, a2)
        check(False, "float must be unsupported")
    except ac.ArrowCdiError as e:
        eq(int(e.status), ac.Status.EUNSUPPORTED, "float -> EUNSUPPORTED")
    check(bool(s2.release), "unsupported source intact")
    s2.release(s2); a2.release(a2)
    eq(ac.live_allocations(), 0, "zero after rejection tests")


def test_pyarrow_nonzero_offset_import():
    if not HAVE_PA:
        return
    ac.fail_reset()
    # PyArrow produces a nonzero-offset (and chained-slice) batch.
    text = pa.array(["a", "bb", "ccc", "dddd", "eeeee", "ffffff"],
                    pa.utf8())
    nums = pa.array([1, 2, 3, 4, 5, 6], pa.int64())
    rb = pa.RecordBatch.from_arrays([text, nums], names=["t", "n"])
    sliced = rb.slice(2, 4).slice(1, 3)   # logical rows 3..5
    s = abi.ArrowSchema(); a = abi.ArrowArray()
    sliced._export_to_c(ctypes.addressof(a), ctypes.addressof(s))
    off0 = a.children[0].contents.offset
    off1 = a.children[1].contents.offset
    check(off0 != 0 or off1 != 0,
          "expected nonzero physical offset from PyArrow slice")
    b = ac.Batch.import_move(s, a)
    eq(b.rows, 3, "imported sliced rows")
    eq(b.get_string(0, 0), "dddd", "offset-adjusted string 0")
    eq(b.get_string(0, 2), "ffffff", "offset-adjusted string 2")
    eq(b.get_i64(1, 1), 5, "offset-adjusted i64")
    # slice again on our side and confirm composition
    s2 = b.slice(1, 2)
    eq(s2.get_string(0, 0), "eeeee", "re-sliced string")
    eq(s2.physical_offset(0), b.physical_offset(0) + 1, "offset composes")
    s2.close(); b.close()
    eq(ac.live_allocations(), 0, "zero after nonzero-offset import")


def test_dictionary_null_and_garbage_under_null_index():
    if not HAVE_PA:
        return
    # 4 rows; row 2 is a null index. Place an out-of-range garbage value
    # exactly under that null bit: it must be ignored, not rejected.
    n = 4
    # ColumnSpec/DictSpec accept one byte per row; the library bit-packs.
    idx_valid = (ctypes.c_uint8 * n)(1, 1, 0, 1)
    indices = (ctypes.c_int32 * n)(0, 1, 999, 1)  # 999 under null row 2
    dict_valid = (ctypes.c_uint8 * 2)(1, 1)        # both entries valid
    dict_off = (ctypes.c_int32 * 3)(0, 2, 4)
    dict_data = (ctypes.c_uint8 * 4)(*b"abcd")
    dspec = ac.DictSpec(2, dict_valid, dict_off, dict_data, 4)
    spec = ac.ColumnSpec("d", ac.Kind.DICT, n, idx_valid,
                         dict_indices=indices, dictionary=dspec)
    ac.fail_reset()
    b = ac.Batch.from_c_buffers([spec])
    check(b.is_null(0, 2), "garbage sits under a null index")
    eq(b.get_string(0, 0), "ab", "valid index 0 resolves")
    eq(b.get_string(0, 3), "cd", "valid index 1 resolves")
    # round-trip through export/import: garbage must still be tolerated
    sh = b.export()
    back = ac.Batch.import_move(sh.schema, sh.array)
    check(back.is_null(0, 2), "garbage tolerated after round-trip")
    back.close(); b.close()
    eq(ac.live_allocations(), 0, "zero")

    # Now make the same garbage value genuinely out-of-range on a *valid*
    # row: creation must reject it as a layout violation.
    idx_valid2 = (ctypes.c_uint8 * n)(1, 1, 1, 1)  # all rows valid
    spec2 = ac.ColumnSpec("d2", ac.Kind.DICT, n, idx_valid2,
                          dict_indices=(ctypes.c_int32 * n)(0, 1, 999, 1),
                          dictionary=dspec)
    try:
        ac.Batch.from_c_buffers([spec2])
        check(False, "real out-of-range index rejected at creation")
    except ac.ArrowCdiError as e:
        eq(int(e.status), ac.Status.ELAYOUT, "create bad index -> ELAYOUT")
    eq(ac.live_allocations(), 0, "no leak after rejected creation")


def test_utf8_array_offset_is_not_byte_offset():
    if not HAVE_PA:
        return
    ac.fail_reset()
    # Varying byte widths make element-vs-byte confusion observable.
    arr = pa.array(["a", "bb", "héllo", "日", "αβ", "xyz"], pa.utf8())
    rb = pa.RecordBatch.from_arrays([arr], names=["t"])
    sl = rb.slice(2, 4).slice(1, 3)      # logical rows 3,4,5
    s = abi.ArrowSchema(); a = abi.ArrowArray()
    sl._export_to_c(ctypes.addressof(a), ctypes.addressof(s))
    phys = a.children[0].contents.offset
    check(phys != 0, "nonzero array offset present")
    b = ac.Batch.import_move(s, a)
    # The array offset selects rows; int32 offsets still address bytes.
    eq([b.get_string(0, r) for r in range(b.rows)],
       ["日", "αβ", "xyz"], "element offsets decoded under row offset")
    # offsets buffer is shared, not rewritten by the import
    b.close()
    eq(ac.live_allocations(), 0, "zero")


def main():
    tests = [
        test_basic_values,
        test_slice_zero_copy_and_compose,
        test_project_rules,
        test_zero_column_and_zero_length,
        test_json_demo,
        test_gc_close_and_idempotent,
        test_failpoint_import_keeps_source,
        test_pyarrow_roundtrip_and_rejected_imports,
        test_import_bad_index_and_unsupported,
        test_pyarrow_nonzero_offset_import,
        test_dictionary_null_and_garbage_under_null_index,
        test_utf8_array_offset_is_not_byte_offset,
    ]
    for t in tests:
        ac.fail_reset()
        gc.collect()
        before = ac.live_allocations()
        try:
            t()
            gc.collect()
            after = ac.live_allocations()
            eq(after, before, f"[{t.__name__}] no leaked allocations")
            print(f"PASS {t.__name__}")
        except Exception as exc:  # noqa: BLE001
            print(f"FAIL {t.__name__}: {exc}")
            global FAIL
            FAIL += 1
    print(f"\n{FAIL} failures, {PASS} checks passed")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
