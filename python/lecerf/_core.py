"""ctypes bindings for liblecerf shared library.

All argtypes and restype are declared before first call to avoid ctypes
default-int promotion surprises (especially on Win64 where pointers are 8B).
The DLL is loaded from the same directory as this file so pip-installed wheels
and editable installs both find it without PATH manipulation.
"""

import ctypes
import os
import sys

c_void_p  = ctypes.c_void_p
c_char_p  = ctypes.c_char_p
c_int     = ctypes.c_int
c_uint8_p = ctypes.POINTER(ctypes.c_uint8)
c_uint32  = ctypes.c_uint32
c_uint64  = ctypes.c_uint64


def _load() -> ctypes.CDLL:
    d = os.path.dirname(os.path.abspath(__file__))
    name_map = {
        "win32":  "liblecerf.dll",
        "darwin": "liblecerf.dylib",
    }
    name = name_map.get(sys.platform, "liblecerf.so")
    path = os.path.join(d, name)
    if not os.path.exists(path):
        raise OSError(
            f"liblecerf shared library not found at {path}.\n"
            "Run: pip install -e python/  or copy the built DLL there."
        )
    lib = ctypes.CDLL(path)
    return lib


_lib = _load()

# Sanity-check that required symbols are present immediately at import time.
# A missing symbol here means a stale or wrong DLL — surface early.
assert hasattr(_lib, "lecerf_board_create"), (
    "lecerf_board_create not found in liblecerf — wrong or stale DLL"
)

# ── lecerf_board_create ─────────────────────────────────────────────────────
# lecerf_board_t lecerf_board_create(const char* name)
_lib.lecerf_board_create.argtypes = [c_char_p]
_lib.lecerf_board_create.restype  = c_void_p

# ── lecerf_board_destroy ────────────────────────────────────────────────────
# void lecerf_board_destroy(lecerf_board_t b)
_lib.lecerf_board_destroy.argtypes = [c_void_p]
_lib.lecerf_board_destroy.restype  = None

# ── lecerf_board_flash ──────────────────────────────────────────────────────
# int lecerf_board_flash(lecerf_board_t b, const uint8_t* data, uint32_t sz)
_lib.lecerf_board_flash.argtypes = [c_void_p, ctypes.c_char_p, c_uint32]
_lib.lecerf_board_flash.restype  = c_int

# ── lecerf_board_run ────────────────────────────────────────────────────────
# uint64_t lecerf_board_run(lecerf_board_t b, uint64_t max_steps, int* exit_cause)
_lib.lecerf_board_run.argtypes = [c_void_p, c_uint64, ctypes.POINTER(c_int)]
_lib.lecerf_board_run.restype  = c_uint64

# ── lecerf_board_uart_drain ─────────────────────────────────────────────────
# uint32_t lecerf_board_uart_drain(lecerf_board_t b, uint8_t* dst, uint32_t cap)
_lib.lecerf_board_uart_drain.argtypes = [c_void_p, ctypes.c_char_p, c_uint32]
_lib.lecerf_board_uart_drain.restype  = c_uint32

# ── lecerf_board_gpio_get ───────────────────────────────────────────────────
# int lecerf_board_gpio_get(lecerf_board_t b, uint32_t port, uint32_t pin)
_lib.lecerf_board_gpio_get.argtypes = [c_void_p, c_uint32, c_uint32]
_lib.lecerf_board_gpio_get.restype  = c_int

# ── lecerf_board_cpu_reg ────────────────────────────────────────────────────
# uint32_t lecerf_board_cpu_reg(lecerf_board_t b, uint32_t n)
_lib.lecerf_board_cpu_reg.argtypes = [c_void_p, c_uint32]
_lib.lecerf_board_cpu_reg.restype  = c_uint32

# ── lecerf_board_cycles ─────────────────────────────────────────────────────
# uint64_t lecerf_board_cycles(lecerf_board_t b)
_lib.lecerf_board_cycles.argtypes = [c_void_p]
_lib.lecerf_board_cycles.restype  = c_uint64
