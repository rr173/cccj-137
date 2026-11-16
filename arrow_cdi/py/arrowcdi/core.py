"""Pythonic, ownership-correct wrapper around the arrow_cdi C library.

Design goals (see docs/INTERFACE.md):

* No per-value conversion to Python scalar objects as a data path. Values
  stay in shared native buffers; only the demo JSON renderer and explicit
  helpers touch logical values, and the bulk hand-off goes through the
  Apache Arrow C Data Interface.
* Every Batch owns one C handle. ``close()`` releases it exactly once and
  is idempotent. The garbage collector runs the same release path through
  ``__del__``, so users never have to destroy views in a lucky order.
* Import *moves* the source structs. On success the source shells read
  ``release == NULL`` and the old owner must not release them; on failure
  the source is untouched and remains owned by the caller.
* Export produces independent movable shells; internal struct addresses
  are never handed out as permanent handles.
"""
from __future__ import annotations

import ctypes
import enum
import os
from typing import List, Optional, Sequence

from .abi import ARROW_FLAG_NULLABLE, ArrowArray, ArrowSchema

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = os.path.dirname(os.path.dirname(_HERE))


class Status(enum.IntEnum):
    OK = 0
    EINVAL = 1
    EUNSUPPORTED = 2
    ELAYOUT = 3
    ENOMEM = 4
    ESTATE = 5


class ArrowCdiError(RuntimeError):
    """Logical/contract rejection with the C status code."""

    def __init__(self, status: int, message: str = ""):
        self.status = Status(status) if status in Status._value2member_map_ \
            else status
        name = _LIB.ac_status_name(ctypes.c_int(status))
        name_s = ctypes.string_at(name).decode() if name else f"code {status}"
        super().__init__(f"{name_s}: {message}" if message else name_s)


class Kind(enum.IntEnum):
    I64 = 0
    UTF8 = 1
    DICT = 2


class _Utf8Spec(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_int64),
        ("validity", ctypes.POINTER(ctypes.c_uint8)),
        ("offsets", ctypes.POINTER(ctypes.c_int32)),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("data_nbytes", ctypes.c_int64),
    ]


class _ColumnSpec(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("kind", ctypes.c_int),
        ("length", ctypes.c_int64),
        ("validity", ctypes.POINTER(ctypes.c_uint8)),
        ("i64_values", ctypes.POINTER(ctypes.c_int64)),
        ("dict_indices", ctypes.POINTER(ctypes.c_int32)),
        ("str_offsets", ctypes.POINTER(ctypes.c_int32)),
        ("str_data", ctypes.POINTER(ctypes.c_uint8)),
        ("str_data_nbytes", ctypes.c_int64),
        ("dictionary", ctypes.POINTER(_Utf8Spec)),
    ]


def _load_library() -> ctypes.CDLL:
    # Allow an explicit override; otherwise pick the build tree shared
    # object next to the project root.
    path = os.environ.get("ARROW_CDI_LIB")
    if not path:
        path = os.path.join(_PROJECT_ROOT, "build", "libarrow_cdi.so")
    lib = ctypes.CDLL(path)

    lib.ac_fail_reset.restype = None
    lib.ac_fail_disarm.restype = None
    lib.ac_fail_arm.argtypes = [ctypes.c_int]
    lib.ac_fail_arm.restype = None
    lib.ac_total_allocations.restype = ctypes.c_int64
    lib.ac_total_frees.restype = ctypes.c_int64
    lib.ac_live_allocations.restype = ctypes.c_int64
    lib.ac_live_bytes.restype = ctypes.c_int64

    lib.ac_batch_create.argtypes = [
        ctypes.POINTER(_ColumnSpec), ctypes.c_int64,
        ctypes.POINTER(ctypes.c_void_p)]
    lib.ac_batch_create.restype = ctypes.c_int

    lib.ac_batch_import.argtypes = [
        ctypes.POINTER(ArrowSchema), ctypes.POINTER(ArrowArray),
        ctypes.POINTER(ctypes.c_void_p)]
    lib.ac_batch_import.restype = ctypes.c_int

    lib.ac_batch_slice.argtypes = [
        ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64,
        ctypes.POINTER(ctypes.c_void_p)]
    lib.ac_batch_slice.restype = ctypes.c_int

    lib.ac_batch_project.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_int64,
        ctypes.POINTER(ctypes.c_void_p)]
    lib.ac_batch_project.restype = ctypes.c_int

    lib.ac_batch_export.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ArrowSchema),
        ctypes.POINTER(ArrowArray)]
    lib.ac_batch_export.restype = ctypes.c_int

    lib.ac_batch_close.argtypes = [ctypes.c_void_p]
    lib.ac_batch_close.restype = None
    lib.ac_export_schema_release.argtypes = [ctypes.POINTER(ArrowSchema)]
    lib.ac_export_schema_release.restype = None
    lib.ac_export_array_release.argtypes = [ctypes.POINTER(ArrowArray)]
    lib.ac_export_array_release.restype = None

    lib.ac_batch_rows.argtypes = [ctypes.c_void_p]
    lib.ac_batch_rows.restype = ctypes.c_int64
    lib.ac_batch_columns.argtypes = [ctypes.c_void_p]
    lib.ac_batch_columns.restype = ctypes.c_int64
    lib.ac_batch_column_name.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.ac_batch_column_name.restype = ctypes.c_char_p
    lib.ac_batch_column_kind.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.ac_batch_column_kind.restype = ctypes.c_int
    lib.ac_batch_is_null.argtypes = [ctypes.c_void_p, ctypes.c_int64,
                                     ctypes.c_int64]
    lib.ac_batch_is_null.restype = ctypes.c_int
    lib.ac_batch_get_i64.argtypes = [ctypes.c_void_p, ctypes.c_int64,
                                    ctypes.c_int64,
                                    ctypes.POINTER(ctypes.c_int64)]
    lib.ac_batch_get_i64.restype = ctypes.c_int
    lib.ac_batch_get_string.argtypes = [
        ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64,
        ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_int64)]
    lib.ac_batch_get_string.restype = ctypes.c_int
    lib.ac_batch_buffer_address.argtypes = [
        ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64]
    lib.ac_batch_buffer_address.restype = ctypes.c_void_p
    lib.ac_batch_dict_buffer_address.argtypes = [
        ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64]
    lib.ac_batch_dict_buffer_address.restype = ctypes.c_void_p
    lib.ac_batch_physical_offset.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.ac_batch_physical_offset.restype = ctypes.c_int64

    lib.ac_batch_to_json.argtypes = [ctypes.c_void_p,
                                     ctypes.POINTER(ctypes.c_char_p)]
    lib.ac_batch_to_json.restype = ctypes.c_int
    lib.ac_json_free.argtypes = [ctypes.c_char_p]
    lib.ac_json_free.restype = None
    lib.ac_status_name.argtypes = [ctypes.c_int]
    lib.ac_status_name.restype = ctypes.c_char_p
    return lib


_LIB = _load_library()


def _check(status: int, what: str) -> None:
    if status != Status.OK:
        raise ArrowCdiError(status, what)


class Batch:
    """A refcounted record-batch view over shared native Arrow buffers."""

    __slots__ = ("_handle", "_closed")

    def __init__(self, handle: int) -> None:
        self._handle = ctypes.c_void_p(handle)
        self._closed = False

    # ---- construction -------------------------------------------------

    @classmethod
    def from_c_buffers(cls, columns: Sequence["ColumnSpec"]) -> "Batch":
        """Create an owning batch, deep-copying the supplied C buffers."""
        n = len(columns)
        arr = (_ColumnSpec * max(n, 1))()
        for i, spec in enumerate(columns):
            arr[i] = spec._underlying()
        out = ctypes.c_void_p()
        _check(_LIB.ac_batch_create(arr, n, ctypes.byref(out)),
               "ac_batch_create")
        return cls(out.value)

    @classmethod
    def import_move(cls, schema: ArrowSchema, array: ArrowArray) -> "Batch":
        """Move a released-looking C Data Interface pair into the library.

        On success the source structs are released (``release == NULL``).
        On failure they are left untouched and still owned by the caller.
        """
        out = ctypes.c_void_p()
        st = _LIB.ac_batch_import(ctypes.byref(schema),
                                  ctypes.byref(array), ctypes.byref(out))
        _check(st, "ac_batch_import")
        return cls(out.value)

    # ---- views --------------------------------------------------------

    def slice(self, start: int, length: int) -> "Batch":
        self._require_open()
        out = ctypes.c_void_p()
        _check(_LIB.ac_batch_slice(self._handle, start, length,
                                   ctypes.byref(out)), "ac_batch_slice")
        return Batch(out.value)

    def project(self, names: Sequence[str]) -> "Batch":
        self._require_open()
        n = len(names)
        arr = (ctypes.c_char_p * max(n, 1))()
        encoded = [nm.encode("utf-8") for nm in names]
        for i, nm in enumerate(encoded):
            arr[i] = nm
        out = ctypes.c_void_p()
        _check(_LIB.ac_batch_project(self._handle, arr, n,
                                     ctypes.byref(out)),
               "ac_batch_project")
        return Batch(out.value)

    # ---- export -------------------------------------------------------

    def export(self) -> "ExportedShells":
        """Build fresh, independently movable ABI shells (zero copy)."""
        self._require_open()
        schema = ArrowSchema()
        array = ArrowArray()
        _check(_LIB.ac_batch_export(self._handle, ctypes.byref(schema),
                                    ctypes.byref(array)),
               "ac_batch_export")
        return ExportedShells(schema, array)

    # ---- introspection (no scalar-list conversion) -------------------

    @property
    def rows(self) -> int:
        self._require_open()
        return int(_LIB.ac_batch_rows(self._handle))

    @property
    def columns(self) -> int:
        self._require_open()
        return int(_LIB.ac_batch_columns(self._handle))

    def column_name(self, col: int) -> str:
        self._require_open()
        raw = _LIB.ac_batch_column_name(self._handle, col)
        return ctypes.string_at(raw).decode("utf-8")

    def column_kind(self, col: int) -> Kind:
        self._require_open()
        return Kind(int(_LIB.ac_batch_column_kind(self._handle, col)))

    def is_null(self, col: int, row: int) -> bool:
        self._require_open()
        return bool(_LIB.ac_batch_is_null(self._handle, col, row))

    def get_i64(self, col: int, row: int) -> Optional[int]:
        self._require_open()
        out = ctypes.c_int64()
        st = _LIB.ac_batch_get_i64(self._handle, col, row,
                                   ctypes.byref(out))
        if st == Status.ESTATE:
            return None
        _check(st, "ac_batch_get_i64")
        return int(out.value)

    def get_string(self, col: int, row: int) -> Optional[str]:
        self._require_open()
        ptr = ctypes.c_char_p()
        n = ctypes.c_int64()
        st = _LIB.ac_batch_get_string(self._handle, col, row,
                                      ctypes.byref(ptr), ctypes.byref(n))
        if st == Status.ESTATE:
            return None
        _check(st, "ac_batch_get_string")
        return ctypes.string_at(ptr, size=n.value).decode("utf-8")

    def buffer_address(self, col: int, buffer_index: int) -> int:
        self._require_open()
        return int(_LIB.ac_batch_buffer_address(
            self._handle, col, buffer_index) or 0)

    def dict_buffer_address(self, col: int, buffer_index: int) -> int:
        self._require_open()
        return int(_LIB.ac_batch_dict_buffer_address(
            self._handle, col, buffer_index) or 0)

    def physical_offset(self, col: int) -> int:
        self._require_open()
        return int(_LIB.ac_batch_physical_offset(self._handle, col))

    def to_json(self) -> str:
        """Demo rendering only; not a scalar-list extraction API."""
        self._require_open()
        raw = ctypes.c_char_p()
        _LIB.ac_json_free  # ensure bound
        _check(_LIB.ac_batch_to_json(self._handle, ctypes.byref(raw)),
               "ac_batch_to_json")
        try:
            return ctypes.string_at(raw).decode("utf-8")
        finally:
            _LIB.ac_json_free(raw)

    # ---- lifecycle ----------------------------------------------------

    def _require_open(self) -> None:
        if self._closed:
            raise ArrowCdiError(Status.ESTATE, "batch is closed")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        _LIB.ac_batch_close(self._handle)
        self._handle = None

    def __enter__(self) -> "Batch":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        # Safe on the GC path: close() is idempotent and the C release
        # callbacks do not take the GIL beyond ctypes' own handling.
        try:
            self.close()
        except Exception:
            pass


class ExportedShells:
    """A pair of freshly allocated, independently movable ABI structs.

    The schema and array sides reference the payload separately; either
    may be released first. Releasing via :meth:`release` is idempotent.
    These structs are suitable for PyArrow's ``_import_from_c``.
    """

    __slots__ = ("schema", "array", "_released")

    def __init__(self, schema: ArrowSchema, array: ArrowArray) -> None:
        self.schema = schema
        self.array = array
        self._released = False

    def schema_address(self) -> int:
        return ctypes.addressof(self.schema)

    def array_address(self) -> int:
        return ctypes.addressof(self.array)

    def release(self) -> None:
        if self._released:
            return
        self._released = True
        _LIB.ac_export_array_release(ctypes.byref(self.array))
        _LIB.ac_export_schema_release(ctypes.byref(self.schema))

    def __enter__(self) -> "ExportedShells":
        return self

    def __exit__(self, *exc) -> None:
        self.release()

    def __del__(self) -> None:
        try:
            self.release()
        except Exception:
            pass


# ----------------------------------------------------------------------
# Builder helpers for from_c_buffers
# ----------------------------------------------------------------------

class ColumnSpec:
    """Python-side description of a caller-owned C buffer column.

    The library deep-copies every referenced buffer at creation time, so
    the backing ctypes arrays may be released once ``from_c_buffers``
    returns. Validity arrays use one byte per row (0 = null).
    """

    def __init__(self, name: str, kind: Kind, length: int,
                 validity: Optional[ctypes.Array],
                 i64_values: Optional[ctypes.Array] = None,
                 dict_indices: Optional[ctypes.Array] = None,
                 str_offsets: Optional[ctypes.Array] = None,
                 str_data: Optional[ctypes.Array] = None,
                 str_data_nbytes: Optional[int] = None,
                 dictionary: Optional["DictSpec"] = None) -> None:
        self.name = name
        self.kind = Kind(kind)
        self.length = length
        self.validity = validity
        self.i64_values = i64_values
        self.dict_indices = dict_indices
        self.str_offsets = str_offsets
        self.str_data = str_data
        self.str_data_nbytes = str_data_nbytes
        self.dictionary = dictionary

    def _underlying(self) -> _ColumnSpec:
        spec = _ColumnSpec()
        spec.name = self.name.encode("utf-8")
        spec.kind = int(self.kind)
        spec.length = self.length
        spec.validity = (ctypes.cast(self.validity,
                                     ctypes.POINTER(ctypes.c_uint8))
                         if self.validity is not None else None)
        spec.i64_values = (ctypes.cast(self.i64_values,
                                       ctypes.POINTER(ctypes.c_int64))
                           if self.i64_values is not None else None)
        spec.dict_indices = (ctypes.cast(self.dict_indices,
                                         ctypes.POINTER(ctypes.c_int32))
                             if self.dict_indices is not None else None)
        spec.str_offsets = (ctypes.cast(self.str_offsets,
                                        ctypes.POINTER(ctypes.c_int32))
                            if self.str_offsets is not None else None)
        if self.str_data is not None:
            spec.str_data = ctypes.cast(self.str_data,
                                        ctypes.POINTER(ctypes.c_uint8))
            spec.str_data_nbytes = (self.str_data_nbytes
                                    if self.str_data_nbytes is not None
                                    else len(bytes(self.str_data)))
        if self.dictionary is not None:
            spec.dictionary = ctypes.pointer(self.dictionary._underlying())
        return spec


class DictSpec:
    """The nullable UTF-8 dictionary referenced by int32 indices."""

    def __init__(self, length: int, validity: Optional[ctypes.Array],
                 offsets: ctypes.Array, data: ctypes.Array,
                 data_nbytes: Optional[int] = None) -> None:
        self.length = length
        self.validity = validity
        self.offsets = offsets
        self.data = data
        self.data_nbytes = data_nbytes

    def _underlying(self) -> _Utf8Spec:
        spec = _Utf8Spec()
        spec.length = self.length
        spec.validity = (ctypes.cast(self.validity,
                                     ctypes.POINTER(ctypes.c_uint8))
                         if self.validity is not None else None)
        spec.offsets = ctypes.cast(self.offsets,
                                   ctypes.POINTER(ctypes.c_int32))
        spec.data = ctypes.cast(self.data, ctypes.POINTER(ctypes.c_uint8))
        spec.data_nbytes = (self.data_nbytes
                            if self.data_nbytes is not None
                            else len(bytes(self.data)))
        return spec


# Convenience re-exports for tests and the failpoint harness.
def fail_reset() -> None:
    _LIB.ac_fail_reset()


def fail_disarm() -> None:
    _LIB.ac_fail_disarm()


def fail_arm(nth: int) -> None:
    _LIB.ac_fail_arm(nth)


def total_allocations() -> int:
    return int(_LIB.ac_total_allocations())


def total_frees() -> int:
    return int(_LIB.ac_total_frees())


def live_allocations() -> int:
    return int(_LIB.ac_live_allocations())


def live_bytes() -> int:
    return int(_LIB.ac_live_bytes())


__all__ = [
    "Status", "ArrowCdiError", "Kind", "Batch", "ExportedShells",
    "ColumnSpec", "DictSpec", "ArrowSchema", "ArrowArray",
    "fail_reset", "fail_disarm", "fail_arm",
    "total_allocations", "total_frees", "live_allocations", "live_bytes",
    "ARROW_FLAG_NULLABLE",
]
