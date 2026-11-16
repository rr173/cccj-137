"""ctypes ABI definitions matching include/arrow_cdata_interface.h.

These mirror the upstream Apache Arrow C Data Interface structure layout
exactly; they are *not* PyArrow types. Keeping them in a dedicated module
makes the ABI boundary explicit and lets both the wrapper and the tests
inspect raw structs.
"""
from __future__ import annotations

import ctypes

# ARROW_FLAG_* from abi.h
ARROW_FLAG_DICTIONARY_ORDERED = 1
ARROW_FLAG_NULLABLE = 2
ARROW_FLAG_MAP_KEYS_SORTED = 4


class ArrowSchema(ctypes.Structure):
    pass


class ArrowArray(ctypes.Structure):
    pass


ReleaseSchema = ctypes.CFUNCTYPE(None, ctypes.POINTER(ArrowSchema))
ReleaseArray = ctypes.CFUNCTYPE(None, ctypes.POINTER(ArrowArray))

ArrowSchema._fields_ = [
    ("format", ctypes.c_char_p),
    ("name", ctypes.c_char_p),
    ("metadata", ctypes.c_void_p),
    ("flags", ctypes.c_int64),
    ("n_children", ctypes.c_int64),
    ("children", ctypes.POINTER(ctypes.POINTER(ArrowSchema))),
    ("dictionary", ctypes.POINTER(ArrowSchema)),
    ("release", ReleaseSchema),
    ("private_data", ctypes.c_void_p),
]

ArrowArray._fields_ = [
    ("length", ctypes.c_int64),
    ("null_count", ctypes.c_int64),
    ("offset", ctypes.c_int64),
    ("n_buffers", ctypes.c_int64),
    ("n_children", ctypes.c_int64),
    ("buffers", ctypes.POINTER(ctypes.c_void_p)),
    ("children", ctypes.POINTER(ctypes.POINTER(ArrowArray))),
    ("dictionary", ctypes.POINTER(ArrowArray)),
    ("release", ReleaseArray),
    ("private_data", ctypes.c_void_p),
]
