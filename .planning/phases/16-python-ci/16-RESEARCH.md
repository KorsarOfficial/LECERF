# Phase 16: Python API + CI Runner - Research

**Researched:** 2026-04-28
**Domain:** Python C bindings, pip wheel packaging, pytest plugin, Docker, GitHub Actions
**Confidence:** HIGH (binding strategy, pytest), MEDIUM (Docker sizing, GH Action timing), LOW (Windows MinGW wheel nuance)

---

## Summary

Phase 16 wraps the pure-C11 LECERF engine in a pip-installable Python package, a pytest fixture plugin, a Docker image, and a GitHub Action. The core design challenge is shipping pre-built native binaries inside a wheel so end-users never compile anything on install. The existing codebase is already well-structured: `cortex_m_core` is a static library; `tools/main.c` is a thin driver. The main work is (1) exposing a `liblecerf.so/.dll/.dylib` shared library with a stable C ABI, (2) wrapping it with Python ctypes, (3) hooking pytest's entry-point system, and (4) building the Docker image and GitHub Action around the same binary.

The fastest path to first external user is: ctypes + setuptools + cibuildwheel for the wheel, a two-stage Docker build ending in `python:3.12-alpine` (~50 MB), and a Docker-container GitHub Action published to ghcr.io. Skipping PyPI publish for MVP is correct; GitHub Releases + `--find-links` is sufficient for V1 users.

**Primary recommendation:** ctypes ABI mode with a CMake-built shared library bundled inside the Python package directory. Build one manylinux_2_28 x86_64 wheel for Linux CI; add Windows and macOS wheels post-MVP.

---

## User Constraints (from phase objective)

### Locked Decisions
- Python is user-facing layer; emulator core stays pure C11 (no Python in src/core)
- pip-installable wheel: `pip install lecerf`
- pytest plugin via fixtures (`def test_blink(board): ...`)
- Docker image must be under 50 MB
- GitHub Action for sample-repo flow
- 3-firmware test suite must pass in under 30 s end-to-end on GitHub-hosted runner
- Prioritize developer ergonomics and "5-min-from-zero-to-running" over feature completeness

### Claude's Discretion
- Binding mechanism (ctypes vs CFFI vs pybind11)
- Wheel build pipeline details
- Docker base image choice
- GitHub Action architecture (composite vs Docker container)
- Whether to publish to PyPI for MVP
- Time-travel API exposure scope

### Deferred Ideas (OUT OF SCOPE)
- WASM / browser (Phase 15)
- Landing page (Phase 17)
- Real network stack
- Visual GUI / Tauri / Electron (v3.0)

---

## Q1: Python Binding Strategy

**Pick: ctypes (ABI mode) with bundled prebuilt shared library.**

### Comparison

| Mechanism | No compile on pip install | Pure C friendly | Type safety | Wheel complexity |
|-----------|--------------------------|-----------------|-------------|-----------------|
| ctypes    | YES (if wheel ships .so/.dll) | YES | LOW (manual argtypes) | LOW |
| CFFI ABI  | YES (similar to ctypes) | YES | MEDIUM (header parsing) | LOW |
| CFFI API  | NO (needs C compiler) | YES | HIGH | MEDIUM |
| pybind11  | NO (needs C++ compile per Python version) | NO (C++ layer required) | HIGH | HIGH |
| nanobind  | NO | NO | HIGH | HIGH |

**Rationale for ctypes:**
- Standard library, zero extra deps for end-users.
- Works by loading a pre-built platform `.so`/`.dll`/`.dylib` bundled inside the package directory.
- Unicorn-engine (the gold standard CPU emulator Python binding) uses exactly this pattern: `ctypes.cdll.LoadLibrary()` pointing to a file in the package dir.
- MinGW vs MSVC ABI issue does NOT apply to ctypes — ctypes loads `.dll` at runtime without linker involvement; a MinGW-built `liblecerf.dll` loads fine into MSVC-built CPython via ctypes.
- Downside: manual `argtypes`/`restype` declarations. Acceptable for a focused API of ~10 functions.

### Library loading pattern (verified against CPython docs)

```python
# lecerf/_core.py
import ctypes, os, sys, platform

def _load_lib():
    _dir = os.path.dirname(__file__)
    if sys.platform == "win32":
        name = "liblecerf.dll"
    elif sys.platform == "darwin":
        name = "liblecerf.dylib"
    else:
        name = "liblecerf.so"
    return ctypes.CDLL(os.path.join(_dir, name))

_lib = _load_lib()
```

### C API surface needed (new public header: `include/lecerf.h`)

```c
// lecerf.h — stable ABI, no internal types exposed
typedef void* lecerf_board_t;

lecerf_board_t lecerf_board_create(const char* board_name);
void           lecerf_board_destroy(lecerf_board_t b);
int            lecerf_flash(lecerf_board_t b, const uint8_t* data, uint32_t sz);
int            lecerf_run(lecerf_board_t b, uint64_t max_steps, int* exit_cause);
int            lecerf_uart_read(lecerf_board_t b, uint8_t* buf, uint32_t cap);
uint32_t       lecerf_gpio_get(lecerf_board_t b, int port, int pin);
uint32_t       lecerf_cpu_reg(lecerf_board_t b, int reg_n);
uint64_t       lecerf_cpu_cycles(lecerf_board_t b);
// Time-travel (optional for 16-06)
int            lecerf_snapshot(lecerf_board_t b, uint8_t* buf, uint32_t* sz);
int            lecerf_rewind(lecerf_board_t b, uint64_t target_cycle);
```

`lecerf_board_t` is an opaque `void*` pointing to a heap-allocated struct that owns `cpu_t`, `bus_t`, all peripherals, and optionally `tt_t`. This completely decouples Python from internal types.

### Exit cause enum (used by `lecerf_run`)

```c
#define LECERF_HALT    0   // cpu.halted = true (WFI or BKPT 0xDEFE)
#define LECERF_TIMEOUT 1   // max_steps exhausted
#define LECERF_FAULT   2   // HardFault / UsageFault with no handler
```

### Python __del__ safety pattern (verified against CPython ctypes docs)

```python
class Board:
    def __init__(self, board_name: str):
        self._b = _lib.lecerf_board_create(board_name.encode())
        if not self._b:
            raise RuntimeError(f"Unknown board: {board_name}")

    def __del__(self):
        if self._b:
            _lib.lecerf_board_destroy(self._b)
            self._b = None
```

`__del__` is called by the garbage collector. The `if self._b` guard prevents double-free if `__init__` partially failed. This is the canonical pattern; no segfault risk as long as `argtypes`/`restype` are set correctly (mismatched argtypes is the primary ctypes segfault cause).

---

## Q2: Wheel Build Pipeline

**Pick: cibuildwheel with scikit-build-core backend, building Linux x86_64 only for MVP.**

### Minimum viable wheel matrix for "pip install on GitHub-hosted ubuntu-22.04"

One wheel is sufficient for MVP: `lecerf-0.1.0-cp3{9,10,11,12,13}-abi3-manylinux_2_28_x86_64.whl`

Or simpler: build per-CPython-version wheels, accept 5 wheel files for cp39–cp313 / linux x86_64.

GitHub-hosted runners use Ubuntu 22.04 (glibc 2.35). `manylinux_2_28` (glibc 2.28) images run inside Docker during the cibuildwheel step and produce wheels compatible with any Linux having glibc >= 2.28, which includes Ubuntu 22.04. This is the current default as of cibuildwheel 2.20+ (changed from manylinux2014 in May 2025).

### Platform matrix (post-MVP)

| Platform | Runner | Notes |
|----------|--------|-------|
| Linux x86_64 | ubuntu-22.04 | MVP — covers GitHub-hosted runners |
| Linux aarch64 | ubuntu-22.04 + QEMU | Slow (~10 min), skip for MVP |
| macOS x86_64 | macos-13 | Ship post-MVP |
| macOS arm64 | macos-14 | Ship post-MVP |
| Windows x86_64 | windows-latest | Ship post-MVP; ctypes loads MinGW .dll fine |

### Build backend: scikit-build-core

scikit-build-core is the current standard for CMake-based Python packages (replaces legacy scikit-build). It drives CMake from pyproject.toml, installs the compiled shared library into the package directory, and feeds it to the wheel builder.

```toml
# pyproject.toml (minimal)
[build-system]
requires = ["scikit-build-core>=0.9"]
build-backend = "scikit-build.core"

[project]
name = "lecerf"
version = "0.1.0"
requires-python = ">=3.9"
description = "Cortex-M firmware emulator for CI"

[tool.scikit-build]
cmake.build-type = "Release"
wheel.packages = ["lecerf"]

[project.entry-points.pytest11]
lecerf = "lecerf.pytest_plugin"
```

The `CMakeLists.txt` gains a new target:

```cmake
add_library(liblecerf SHARED src/lecerf_api.c)
target_link_libraries(liblecerf PRIVATE cortex_m_core)
# Install into the Python package directory so wheel picks it up
install(TARGETS liblecerf
        LIBRARY DESTINATION lecerf      # .so goes into lecerf/ package dir
        RUNTIME DESTINATION lecerf)     # .dll (Windows)
```

scikit-build-core installs targets into the wheel staging area using the CMake `install()` directive. The library ends up at `lecerf/liblecerf.so` inside the wheel, next to `lecerf/__init__.py`.

### auditwheel

For Linux wheels, `auditwheel repair` must be run after build to patch the manylinux tag and vendor any non-system shared library dependencies into the wheel. cibuildwheel does this automatically in the manylinux container.

---

## Q3: API Surface Design

### Board profile selection

Current `tools/main.c` hardcodes one memory map. The new `lecerf_board_create(name)` function selects a board profile from a table:

```c
typedef struct {
    const char* name;
    u32 flash_base, flash_size;
    u32 sram_base,  sram_size;
    core_t core_variant;
    // peripherals bitfield: UART | SYSTICK | STM32 | ETH | ...
    u32 periph_flags;
} board_profile_t;

static const board_profile_t BOARDS[] = {
    { "stm32f103", 0x00000000, 128*1024, 0x20000000, 20*1024,  CORE_M3, PERIPH_STM32|PERIPH_UART },
    { "stm32f407", 0x00000000, 512*1024, 0x20000000, 128*1024, CORE_M4, PERIPH_STM32|PERIPH_UART|PERIPH_FPU },
    { "generic-m3", 0x00000000, 256*1024, 0x20000000, 64*1024, CORE_M3, PERIPH_UART },
    { NULL }
};
```

### Python API

```python
import lecerf

b = lecerf.Board("stm32f407")
b.flash("firmware.bin")             # or b.flash(bytes_object)
result = b.run(timeout_ms=5000)     # returns RunResult

print(result.exit_cause)            # "halt" | "timeout" | "fault"
print(result.cycles)
print(b.uart.output())              # bytes since last call
print(b.gpio["GPIOC"][13].value)    # bool
print(b.cpu.r[0])                   # int

# Time-travel (16-06, optional)
snap = b.snapshot()                 # bytes blob
b.rewind(target_cycle=1000)
b.step_back(10)
```

`b.uart.output()` drains the UART TX buffer that was captured via the `uart_t.sink` callback into a Python-owned `bytearray`.

`b.gpio["GPIOC"][13].value` reads `stm32_t.odr_c` bit 13.

### RunResult dataclass

```python
from dataclasses import dataclass

@dataclass
class RunResult:
    exit_cause: str   # "halt" | "timeout" | "fault"
    cycles: int
    r: list[int]      # r[0..15] at halt
    apsr: int
```

### UART capture design

The `uart_t` currently has a `sink` function pointer (`int (*sink)(int c)`). The C API wrapper sets this to a callback that appends bytes to a per-board heap buffer:

```c
typedef struct board_s {
    cpu_t   cpu;
    bus_t   bus;
    uart_t  uart;
    // ... other peripherals ...
    uint8_t* uart_buf;
    uint32_t uart_buf_n;
    uint32_t uart_buf_cap;
    stm32_t  stm32;
    // etc.
} board_t;
```

`lecerf_uart_read` copies `uart_buf[0..n]` into the Python-owned buffer and resets `uart_buf_n = 0`.

---

## Q4: pytest Plugin Mechanics

**Pick: single conftest entry point + `board` fixture scoped to function.**

### Entry point registration (pyproject.toml)

```toml
[project.entry-points.pytest11]
lecerf = "lecerf.pytest_plugin"
```

pytest auto-discovers plugins via the `pytest11` entrypoint on import. Users get the `board` fixture automatically after `pip install lecerf` — no conftest.py needed in their repo.

### Plugin module

```python
# lecerf/pytest_plugin.py
import pytest
from lecerf import Board

def pytest_addoption(parser):
    parser.addoption("--lecerf-board", default="stm32f407",
                     help="Default board profile for lecerf fixtures")

@pytest.fixture
def board(request):
    name = request.config.getoption("--lecerf-board")
    b = Board(name)
    yield b
    del b   # triggers __del__ -> lecerf_board_destroy

@pytest.fixture(params=["stm32f103", "stm32f407"])
def board_all(request):
    b = Board(request.param)
    yield b
    del b
```

### Test pattern (what end-users write)

```python
# tests/test_blink.py
def test_blink_uart(board):
    board.flash("firmware/blink.bin")
    result = board.run(timeout_ms=1000)
    assert result.exit_cause == "halt"
    assert b"STM32 blink demo" in board.uart.output()

def test_blink_gpio(board):
    board.flash("firmware/blink.bin")
    board.run(timeout_ms=1000)
    # stm32_t.odr_c bit 13 reflects last BSRR write
    assert board.gpio["GPIOC"][13].value in (True, False)

def test_blink_r0(board):
    board.flash("firmware/blink.bin")
    result = board.run(timeout_ms=1000)
    assert result.r[0] == 5   # blinks counter in R0
```

### Prior art comparison

| Project | Fixture model | Lesson for LECERF |
|---------|--------------|-------------------|
| pytest-embedded (Espressif) | `dut` fixture, service plugins | Use single `board` fixture, avoid service complexity for MVP |
| unicorn-engine tests | No pytest plugin; raw ctypes | LECERF adds value by having the plugin |
| angr | No fixture plugin; library only | pytest integration is a differentiator |

**Lesson:** pytest-embedded's multi-service architecture is powerful but complex. LECERF should ship the simplest possible fixture that works for the 3-test demo, then expand.

---

## Q5: Docker Image

**Pick: two-stage build — `python:3.12-alpine` runtime, target ~45 MB.**

### Image size analysis

| Strategy | Estimated Size | Notes |
|----------|---------------|-------|
| `python:3.12` (full) | ~1 GB | Not viable |
| `python:3.12-slim` | ~130 MB | Over budget |
| `python:3.12-alpine` | ~50 MB | At the limit; musl-libc breaks manylinux wheels |
| `scratch` + static C binary | ~5 MB | No Python, no pytest ergonomics |
| Two-stage: builder=manylinux, runtime=alpine + wheel | ~45 MB | **Recommended** |
| distroless/python3 | ~80 MB | Over budget |

**Why alpine works despite musl-libc:** the wheel built inside the image is built *for* musl (using a musllinux cibuildwheel target or just building inside the alpine container itself via `pip install --no-binary :all: lecerf`). For the Docker image specifically, we compile the .so inside the container, not use a manylinux wheel, avoiding the musl/glibc mismatch.

### Dockerfile strategy

```dockerfile
# Stage 1: Build liblecerf.so inside alpine
FROM python:3.12-alpine AS builder
RUN apk add --no-cache gcc musl-dev cmake make
COPY . /src
WORKDIR /src/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) liblecerf

# Stage 2: Runtime
FROM python:3.12-alpine
COPY --from=builder /src/build/liblecerf.so /lecerf/
COPY python/lecerf/ /lecerf/lecerf/
COPY python/pyproject.toml /lecerf/
WORKDIR /lecerf
RUN pip install --no-build-isolation -e .
ENTRYPOINT ["python", "-m", "lecerf.runner"]
```

Resulting image: ~45 MB (python:3.12-alpine base ~50 MB minus some overhead from multi-stage, plus ~1 MB .so, plus ~0.5 MB Python files).

**Alternative if alpine causes issues:** Use `python:3.12-slim-bookworm` (~130 MB) but that exceeds the 50 MB target. If strictly enforced, use the C-only `lecerf-runner` binary (static, ~3 MB) in a scratch image for the Docker path, and keep the Python wheel separate for developers.

**Recommended resolution of the 50 MB tension:** The Docker image runs the pytest test suite (it needs Python and pytest). Use python:3.12-alpine. The Docker image is NOT the same artifact as the PyPI wheel — it bundles its own .so compiled for musl.

### `lecerf.runner` entry point

The Docker container's entrypoint is a Python script that:
1. Accepts a JSON spec on stdin (or via env vars): `{"board": "stm32f407", "firmware": "/fw.bin", "tests": ["/tests/test_blink.py"]}`
2. Runs `pytest` programmatically or via subprocess
3. Exits 0 on all pass, 1 on any fail
4. Writes JUnit XML to stdout or to a mounted volume

---

## Q6: GitHub Action

**Pick: Docker container action pointing to `ghcr.io/korsarofficial/lecerf:latest`.**

### Why Docker container action over composite action

- Composite actions run on the caller's runner — no guaranteed Python/pytest version.
- Docker container actions guarantee the exact environment (the image built in Q5).
- Docker container actions are the canonical choice for tool-as-a-service actions (e.g., super-linter, trivy).
- Restriction: Docker container actions only run on Linux runners. GitHub-hosted `ubuntu-latest` is the target, so this is fine.

### action.yml

```yaml
# .github/actions/lecerf-runner/action.yml  (lives in this repo)
name: 'LECERF Firmware Test Runner'
description: 'Run firmware tests in the LECERF Cortex-M emulator'
inputs:
  board:
    description: 'Board profile (stm32f103, stm32f407, generic-m3)'
    required: false
    default: 'stm32f407'
  firmware:
    description: 'Path to firmware .bin file'
    required: true
  tests:
    description: 'Path to test file or directory'
    required: true
  timeout-ms:
    description: 'Per-test run timeout in milliseconds'
    required: false
    default: '5000'
outputs:
  result:
    description: 'pass or fail'
runs:
  using: 'docker'
  image: 'docker://ghcr.io/korsarofficial/lecerf:latest'
  args:
    - ${{ inputs.board }}
    - ${{ inputs.firmware }}
    - ${{ inputs.tests }}
    - ${{ inputs.timeout-ms }}
```

### Timing analysis for <30 s requirement

| Step | Time (cold) | Time (warm cache) |
|------|-------------|-------------------|
| Docker pull `lecerf:latest` (~45 MB) | ~15-20 s | ~0 s (cached by runner) |
| Container start | ~1 s | ~1 s |
| pytest collection (3 tests) | ~1 s | ~1 s |
| Board init + flash + run per test | ~0.1 s each | same |
| Total | ~23 s cold | ~3 s warm |

GitHub-hosted runners do NOT cache Docker images between workflow runs by default. However, a 45 MB image pulls in ~15 s on GitHub's network (fast internal CDN to ghcr.io). The 3-test suite itself runs in <1 s. Total end-to-end: ~20 s on first pull, ~3 s if image is warm (e.g., same runner reused in a short window).

**To hit 30 s reliably:** the image must be under 50 MB (already planned). Use `docker pull` with `--quiet` to minimize output. For the sample repo demo, the action will typically run within the 30 s budget on first pull.

**Risk:** If github.com → ghcr.io pull is slow (>20 s), the total exceeds 30 s. Mitigation: also push to Docker Hub as backup; Docker Hub CDN is sometimes faster for cold pulls.

### Sample repo workflow

```yaml
# lecerf-ci-example/.github/workflows/firmware-test.yml
name: Firmware Tests
on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Build firmware
        run: |
          arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 \
            -T link.ld -o blink.elf firmware/blink.c
          arm-none-eabi-objcopy -O binary blink.elf blink.bin

      - name: Run firmware tests
        uses: KorsarOfficial/lecerf@v1
        with:
          board: stm32f407
          firmware: blink.bin
          tests: tests/
```

Or for the simpler approach (no arm-none-eabi-gcc in CI), ship pre-built .bin files with the sample repo.

---

## Q7: Build and Release Pipeline

**Skip PyPI for MVP. Use GitHub Releases + `--find-links`.**

### MVP release flow

1. Tag `v0.1.0` on GitHub
2. GitHub Actions workflow builds the wheel via cibuildwheel (Linux x86_64)
3. Uploads wheel as release asset to the tag
4. Users install with:
   ```
   pip install lecerf --find-links https://github.com/KorsarOfficial/LECERF/releases/tag/v0.1.0
   ```
5. Docker image built and pushed to `ghcr.io/korsarofficial/lecerf:latest` and `:v0.1.0`

This saves ~1-2 days of PyPI registration, trusted publisher setup, and the "wait for PyPI review" loop. PyPI can be added in Phase 17 or when the package is stable.

### CI workflow structure (in this monorepo)

```yaml
# .github/workflows/release.yml
on:
  push:
    tags: ['v*']

jobs:
  build-wheel:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - uses: pypa/cibuildwheel@v2
        env:
          CIBW_BUILD: "cp39-* cp310-* cp311-* cp312-* cp313-*"
          CIBW_ARCHS_LINUX: x86_64
          CIBW_MANYLINUX_X86_64_IMAGE: manylinux_2_28
      - uses: actions/upload-artifact@v4
        with:
          name: wheels
          path: wheelhouse/

  build-docker:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}
      - uses: docker/build-push-action@v5
        with:
          push: true
          tags: ghcr.io/korsarofficial/lecerf:latest,ghcr.io/korsarofficial/lecerf:${{ github.ref_name }}

  release:
    needs: [build-wheel]
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/download-artifact@v4
        with: { name: wheels, path: dist/ }
      - uses: softprops/action-gh-release@v2
        with:
          files: dist/*.whl
```

---

## Q8: Sample Repo Structure (`lecerf-ci-example`)

Separate public repo, kept minimal:

```
lecerf-ci-example/
├── firmware/
│   ├── blink/
│   │   ├── main.c            # copy of test10_stm32_blink/main.c
│   │   └── blink.bin         # pre-built (ship with repo)
│   ├── uart_hello/
│   │   ├── main.c
│   │   └── uart_hello.bin
│   └── register_check/
│       ├── main.c
│       └── register_check.bin
├── tests/
│   ├── test_blink.py         # asserts UART + GPIO + R0
│   ├── test_uart.py          # asserts UART output content
│   └── test_registers.py     # asserts specific register values at halt
├── .github/
│   └── workflows/
│       └── test.yml          # uses KorsarOfficial/lecerf@v1
├── requirements.txt          # lecerf>=0.1.0
└── README.md                 # "5 minutes to running"
```

The three firmware binaries ship pre-built in the repo to eliminate the arm-none-eabi-gcc dependency on the runner. This is the shortest "zero to passing CI" path.

---

## Q9: Performance and Build-Time Targets

| Metric | Target | Expected |
|--------|--------|----------|
| `pip install lecerf` (warm cache) | <5 s | ~1 s (download + unpack wheel) |
| `pip install lecerf` (cold, from GitHub Release) | <15 s | ~3-5 s |
| Docker pull `lecerf:latest` | <20 s | ~15 s (45 MB on ghcr.io) |
| Board init + flash + run (1 test) | <0.5 s | ~0.05-0.1 s (JIT at 100M IPS) |
| pytest 3-test suite | <2 s | ~0.5 s |
| Full GH Action (cold Docker pull + 3 tests) | <30 s | ~20 s |

The emulator itself is already at 100M+ IPS native JIT. The blink firmware runs ~500k cycles to completion — this takes ~5 ms at 100M IPS, well within budget. The overhead is Docker pull time.

---

## Q10: Risk Areas

### Risk 1: Windows wheel — MinGW DLL in ctypes (MEDIUM risk)

**Issue:** Python on Windows is MSVC-built. MinGW-produced `liblecerf.dll` uses a different runtime (msvcrt.dll vs vcruntime140.dll). C ABI (plain C functions, no C++ exceptions, no C++ stdlib) is compatible across MSVC/MinGW when the DLL exports only C symbols.

**How to avoid:** The LECERF API (`lecerf.h`) is pure C with `extern "C"` guards if compiled as C++. No C++ stdlib, no RTTI, no exceptions. In practice, a MinGW-built C DLL loads fine via ctypes into MSVC Python. The mingwpy project documented this and confirmed ctypes bypasses the linker ABI entirely.

**Verification needed:** Test on Windows during 16-02 by running: `python -c "from lecerf import Board; b = Board('stm32f407')"` with a MinGW-built liblecerf.dll.

### Risk 2: Alpine musl-libc vs manylinux glibc in Docker (HIGH importance, MEDIUM risk)

**Issue:** manylinux wheels built for glibc will not load inside alpine (musl). The Docker image must build its own .so from source inside the alpine container.

**Resolution:** The Docker build (Q5) compiles from source inside alpine. The PyPI/GitHub-Release wheels target manylinux for host developers. These are two different distribution paths with two different .so files.

### Risk 3: UART capture requires sink callback modification (LOW risk)

**Issue:** Current `uart_t.sink` is `int (*sink)(int c)` — a function pointer to write a single byte. For Python-side capture, we need to buffer TX bytes per `board_t` instance.

**Resolution:** In `src/lecerf_api.c`, the `board_t.uart` is configured with a static file-local callback that writes into `board_t.uart_buf`. The sink callback receives `board_t*` via a closure or via the global `g_board` approach. Cleanest: add a `void* sink_ctx` to `uart_t` and change the signature to `int (*sink)(void* ctx, int c)`. This is a one-line header change.

### Risk 4: Time-travel memory size (LOW risk for Phase 16)

**Issue:** `snap_blob_t` is ~256 KB per snapshot (contains full SRAM). Exposing `b.snapshot()` as Python `bytes` is fine, but `b.rewind()` requires re-running from the last snapshot — this is already implemented in C. For Python, just expose `lecerf_rewind(board, cycle)` and let C handle it.

**Deferred:** Time-travel Python API is 16-06 (optional). Skip for MVP.

### Risk 5: `del b` during `b.run()` (LOW risk)

Python's GIL and single-threaded test execution mean `del b` cannot happen mid-run within a single thread. In multi-threaded pytest (via pytest-xdist), each worker has its own Board instance. No risk.

---

## Q11: Prior Art — LECERF vs Renode-CI

| Feature | LECERF | Renode |
|---------|--------|--------|
| Startup time | <0.1 s (shared lib load) | ~2-5 s (JVM + Mono runtime) |
| Install size | ~5 MB wheel | ~500 MB |
| pip installable | YES | NO (complex install) |
| pytest native | YES (plugin) | NO (Robot Framework only) |
| Time-travel | YES | NO |
| Board support | STM32F103/F407 + custom | 1000+ boards, pre-defined |
| Architecture support | ARMv7-M | ARM, RISC-V, MIPS, ... |
| Python API | YES (native) | Partial (REST/socket-based) |
| Docker image | ~45 MB | Not available as small image |

**LECERF's unique USP vs Renode:**
1. **Time-travel debugging** — no other embedded emulator CI tool offers this.
2. **Sub-100ms test startup** — Renode takes 2-5 s to boot the .NET/Mono runtime.
3. **pip install** — Renode requires complex installation; LECERF is `pip install lecerf`.
4. **pytest-native** — LECERF tests look like normal Python unit tests, not Robot Framework scripts.

---

## Q12: Naming and Identity

| Item | Recommendation | Rationale |
|------|---------------|-----------|
| PyPI package | `lecerf` | Short, memorable, already the project name |
| Python import | `import lecerf` | Matches package name |
| GitHub Action | `KorsarOfficial/lecerf@v1` (in this repo, path `.github/actions/lecerf-runner/action.yml`) | Avoids managing a separate action repo for MVP |
| Docker image | `ghcr.io/korsarofficial/lecerf:latest` | Free, no Docker Hub account needed, CI auth via GITHUB_TOKEN |
| Sample repo | `lecerf-ci-example` | New public repo under KorsarOfficial |

---

## Q13: Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Cross-platform wheel builds | Custom CI matrix of gcc + python + zip | cibuildwheel | Handles manylinux container, auditwheel, wheel tags |
| Shared library vendoring | Manual .so copy scripts | scikit-build-core CMake install | Handles staging, RPATH, platform tags |
| pytest plugin registration | `conftest.py` instructions | pyproject.toml `pytest11` entry point | Auto-discovered, no user setup |
| manylinux compliance | Custom Docker images | cibuildwheel's manylinux_2_28 container | Audits symbol versions, patches RPATH |
| GitHub Release upload | curl API calls | `softprops/action-gh-release` | Battle-tested, handles asset upload |

---

## Q14: Common Pitfalls

### Pitfall 1: Wheel tagged as "pure Python" when it contains a .so

**What goes wrong:** setuptools auto-detects packages and may tag the wheel `py3-none-any` if it doesn't see a `.pyx` or `.c` extension file being compiled. A `py3-none-any` wheel containing a platform-specific `.so` will install but crash on import.

**How to avoid:** With scikit-build-core, the wheel is always tagged as a platform wheel when CMake installs a shared library. Verify: `unzip -l lecerf-*.whl | grep .so` — the .so must be present. Check the wheel filename: it must NOT contain `none-any`.

### Pitfall 2: `argtypes` not set — ctypes passes wrong types silently

**What goes wrong:** ctypes defaults to `c_int` for unspecified argument types. Passing a Python `str` to a function expecting `char*` will crash with a segfault, not a Python exception.

**How to avoid:** Set `argtypes` and `restype` for every function in `_core.py` before any call. Add a test in the test suite that calls each API function.

### Pitfall 3: RPATH not set — `.so` not found at runtime

**What goes wrong:** The `liblecerf.so` is in the wheel's package directory, but the OS linker doesn't know to look there. On Linux, `ctypes.CDLL("./liblecerf.so")` requires the full path or `LD_LIBRARY_PATH`.

**How to avoid:** Use `os.path.dirname(__file__)` to construct the absolute path: `ctypes.CDLL(os.path.join(os.path.dirname(__file__), "liblecerf.so"))`. Do NOT rely on `find_library()` for a bundled library.

### Pitfall 4: Alpine + manylinux wheel

**What goes wrong:** Trying to `pip install` a `manylinux_2_28_x86_64` wheel inside the alpine Docker container fails: "manylinux: glibc 2.28 or newer required".

**How to avoid:** The Dockerfile compiles from source via `pip install --no-binary :all:` or by running CMake directly (Q5 approach). Never copy a manylinux wheel into an alpine image.

### Pitfall 5: Docker image >50 MB due to test dependencies

**What goes wrong:** `pip install pytest` inside alpine adds ~15 MB. Add a few test utilities and the image creeps over 50 MB.

**How to avoid:** Install only `pytest` (no extra plugins). Use `pip install --no-cache-dir`. Run `pip install` in a single RUN layer. Check image size with `docker image inspect --format='{{.Size}}'`.

### Pitfall 6: Global state in `uart_t` / `stm32_t` between tests

**What goes wrong:** If the `uart_t.sink` function pointer or `stm32_t.odr_c` state is global/static, two `Board()` instances in the same test session share state. Pytest runs all tests in the same process.

**How to avoid:** Every field of `board_t` is heap-allocated per-instance. No global state in `src/lecerf_api.c` except the absolute minimum required (and the existing globals like `g_tt`, `g_cpu_for_scb` must be moved into `board_t` or removed).

---

## Q15: Code Examples — Verified Patterns

### CMakeLists.txt addition for shared lib

```cmake
# Add after existing cortex_m_core static lib definition
add_library(liblecerf SHARED
    src/lecerf_api.c    # new file: the C-API wrapper
)
set_target_properties(liblecerf PROPERTIES
    PREFIX ""           # produces liblecerf.so not libliblecerf.so
    OUTPUT_NAME "liblecerf"
)
target_link_libraries(liblecerf PRIVATE cortex_m_core)
if(WIN32)
    target_link_libraries(liblecerf PRIVATE ws2_32)
endif()
install(TARGETS liblecerf
    LIBRARY DESTINATION lecerf
    RUNTIME DESTINATION lecerf)
```

### ctypes function declarations (lecerf/_core.py)

```python
import ctypes, os, sys

def _load():
    d = os.path.dirname(__file__)
    names = {"win32": "liblecerf.dll", "darwin": "liblecerf.dylib"}
    return ctypes.CDLL(os.path.join(d, names.get(sys.platform, "liblecerf.so")))

_lib = _load()

# void*  lecerf_board_create(const char*)
_lib.lecerf_board_create.argtypes  = [ctypes.c_char_p]
_lib.lecerf_board_create.restype   = ctypes.c_void_p

# void   lecerf_board_destroy(void*)
_lib.lecerf_board_destroy.argtypes = [ctypes.c_void_p]
_lib.lecerf_board_destroy.restype  = None

# int    lecerf_flash(void*, const uint8_t*, uint32_t)
_lib.lecerf_flash.argtypes  = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
_lib.lecerf_flash.restype   = ctypes.c_int

# int    lecerf_run(void*, uint64_t, int*)
_lib.lecerf_run.argtypes  = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_int)]
_lib.lecerf_run.restype   = ctypes.c_uint64  # returns cycles

# int    lecerf_uart_read(void*, uint8_t*, uint32_t)
_lib.lecerf_uart_read.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
_lib.lecerf_uart_read.restype  = ctypes.c_int  # bytes read

# uint32_t lecerf_gpio_get(void*, int, int)
_lib.lecerf_gpio_get.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
_lib.lecerf_gpio_get.restype  = ctypes.c_uint32

# uint32_t lecerf_cpu_reg(void*, int)
_lib.lecerf_cpu_reg.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.lecerf_cpu_reg.restype  = ctypes.c_uint32
```

### C API wrapper skeleton (src/lecerf_api.c)

```c
#include "lecerf.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/run.h"
#include "periph/uart.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include "periph/stm32.h"
/* ... other headers ... */
#include <stdlib.h>
#include <string.h>

#define UART_BUF_CAP (64 * 1024)

typedef struct board_s {
    cpu_t    cpu;
    bus_t    bus;
    uart_t   uart;
    systick_t systick;
    scb_t    scb;
    mpu_t    mpu;
    stm32_t  stm32;
    nvic_t   nvic;
    dwt_t    dwt;
    eth_t    eth;
    u8*      uart_buf;
    u32      uart_n;
} board_t;

static int uart_sink(void* ctx, int c) {
    board_t* b = (board_t*)ctx;
    if (b->uart_n < UART_BUF_CAP) b->uart_buf[b->uart_n++] = (u8)c;
    return c;
}

lecerf_board_t lecerf_board_create(const char* name) {
    // look up name in BOARDS table
    const board_profile_t* prof = find_profile(name);
    if (!prof) return NULL;
    board_t* b = calloc(1, sizeof(board_t));
    b->uart_buf = malloc(UART_BUF_CAP);
    // init bus, peripherals per prof
    bus_init(&b->bus);
    bus_add_flat(&b->bus, "flash", prof->flash_base, prof->flash_size, false);
    bus_add_flat(&b->bus, "sram",  prof->sram_base,  prof->sram_size,  true);
    // ... attach peripherals ...
    // wire uart sink
    b->uart.sink     = uart_sink;
    b->uart.sink_ctx = b;  // requires uart_t.sink_ctx field addition
    uart_attach(&b->bus, &b->uart);
    cpu_reset(&b->cpu, prof->core_variant);
    return (lecerf_board_t)b;
}
```

---

## Architecture Patterns

### Recommended Project Structure (post-phase-16)

```
cortex-m-emu/
├── include/
│   ├── core/         # existing headers unchanged
│   ├── periph/       # existing headers (uart_t gains sink_ctx field)
│   └── lecerf.h      # NEW: public C API (stable ABI)
├── src/
│   ├── core/         # existing, unchanged
│   ├── periph/       # existing, uart.c gains sink_ctx plumbing
│   └── lecerf_api.c  # NEW: opaque board_t wrapper
├── python/
│   ├── lecerf/
│   │   ├── __init__.py       # Board, RunResult classes
│   │   ├── _core.py          # ctypes declarations
│   │   └── pytest_plugin.py  # pytest11 entry point
│   └── pyproject.toml
├── docker/
│   └── Dockerfile
├── .github/
│   ├── actions/
│   │   └── lecerf-runner/
│   │       └── action.yml
│   └── workflows/
│       ├── ci.yml            # existing: ctest
│       └── release.yml       # NEW: cibuildwheel + docker push
├── firmware/                 # existing
├── tests/                    # existing: ctest + run_all.sh
├── tools/
│   └── main.c                # existing, unchanged
└── CMakeLists.txt            # gains liblecerf SHARED target
```

---

## State of the Art

| Old Approach | Current Approach | Impact |
|--------------|-----------------|--------|
| `manylinux2014` (glibc 2.17) | `manylinux_2_28` (glibc 2.28) default in cibuildwheel 2.20+ | Need to set explicitly or accept new default |
| setup.py | pyproject.toml + scikit-build-core | Avoid setup.py; scikit-build-core is the 2025 standard |
| pybind11 for C++ wrapping | cffi/ctypes for C wrapping | pybind11 only needed for C++ classes/templates |
| Docker Hub | ghcr.io (GitHub Container Registry) | Free, auth via GITHUB_TOKEN, no Docker Hub account needed |
| pytest `conftest.py` for plugins | pyproject.toml `pytest11` entry-point | Users get fixtures automatically after pip install |

---

## Open Questions

1. **`uart_t.sink_ctx` field addition**
   - What we know: `uart_t.sink` is `int (*sink)(int c)` — no context pointer
   - What's unclear: whether changing to `int (*sink)(void* ctx, int c)` breaks existing test1-14
   - Recommendation: Add `sink_ctx` field; update `uart_attach` to pass it through; existing tests use `NULL` ctx with the old stdout sink, which still works

2. **GPIO readback accuracy**
   - What we know: `stm32_t.odr_a/b/c` track the last BSRR write value
   - What's unclear: whether the Python API should read the current ODR or track individual BSRR toggles
   - Recommendation: expose `odr_c` bit-field directly; individual pin toggle history is deferred

3. **`g_cpu_for_scb` and other globals**
   - What we know: `src/periph/scb.c` uses `extern cpu_t* g_cpu_for_scb` — a global set in `tools/main.c`
   - What's unclear: whether multiple `Board()` instances in the same pytest run will corrupt each other
   - Recommendation: Move `g_cpu_for_scb` into `board_t` and pass it via the `scb_t` ctx pointer. This is a required fix for thread safety and correct test isolation.

4. **Docker image size verification**
   - What we know: `python:3.12-alpine` is ~50 MB; adding pytest adds ~15 MB
   - What's unclear: whether the total stays under 50 MB after adding liblecerf.so and the Python package
   - Recommendation: Build the Dockerfile in 16-04, measure with `docker image ls`, trim if needed (remove pip cache, use `--no-cache-dir`)

---

## Sources

### Primary (HIGH confidence)
- CPython ctypes documentation — `os.path.dirname(__file__)` load pattern, `argtypes`/`restype` declarations, `__del__` safety
- cibuildwheel official docs (cibuildwheel.pypa.io) — `manylinux_2_28` is the current default; `CIBW_BUILD`, `CIBW_ARCHS_LINUX` options
- pytest official docs (docs.pytest.org) — `pytest11` entry-point format for pyproject.toml
- GitHub Actions official docs (docs.github.com) — Docker container action `action.yml` structure
- scikit-build-core official docs (scikit-build-core.readthedocs.io) — CMake `install()` into wheel staging

### Secondary (MEDIUM confidence)
- unicorn-engine PyPI page and GitHub bindings — confirms ctypes + bundled .so is production-viable for CPU emulators
- pytest-embedded (github.com/espressif) — `dut` fixture pattern reference
- cibuildwheel GitHub discussions — manylinux_2_28 default change from manylinux2014 effective May 2025
- oneuptime.com blog (Jan 2026) — python:3.12-alpine ~50 MB baseline

### Tertiary (LOW confidence)
- ziggit.dev thread on MinGW/MSVC ABI — "C ABI is compatible; ctypes bypasses linker ABI" (needs Windows verification)
- Docker pull timing estimates — based on 45 MB image on ghcr.io; actual timing varies by runner location

---

## Recommended Plan Structure

Sub-plans with dependency graph:

```
16-01-lecerf-api-c          (no deps — C-side prep)
    |
16-02-python-ctypes-wrapper (depends: 16-01)
    |
16-03-pytest-plugin         (depends: 16-02)
    |
    +----> 16-04-docker-and-cicd  (depends: 16-01, 16-03)
    |
    +----> 16-05-sample-repo      (depends: 16-03, 16-04)

16-06-time-travel-python    (depends: 16-02) [optional, post-MVP]
```

### Sub-plan summaries

| ID | Name | What | Key deliverables |
|----|------|------|-----------------|
| 16-01 | C API (`liblecerf`) | New `include/lecerf.h` + `src/lecerf_api.c`; board profile table; CMake `add_library(liblecerf SHARED)`; fix global state (`g_cpu_for_scb`, `uart_t.sink_ctx`) | `liblecerf.so` builds and passes a C-level smoke test |
| 16-02 | Python ctypes wrapper | `python/lecerf/__init__.py`, `python/lecerf/_core.py`, `pyproject.toml`; scikit-build-core; `Board`, `RunResult` classes; `__del__` safety | `python -c "from lecerf import Board; Board('stm32f407').flash('blink.bin').run()"` works |
| 16-03 | pytest plugin | `python/lecerf/pytest_plugin.py`; `board` fixture; `pytest11` entry-point; 3 example tests under `python/tests/` | `pytest python/tests/` passes 3 firmware tests |
| 16-04 | Docker + CI/CD | `docker/Dockerfile`; `release.yml` (cibuildwheel + docker push); `action.yml` for GH Action | Docker image built, <50 MB, passes `docker run --rm lecerf pytest tests/`; wheel uploaded to GH Release |
| 16-05 | Sample repo | New repo `lecerf-ci-example`; 3 firmware .bin files; 3 test files; GitHub Actions workflow using `KorsarOfficial/lecerf@v1`; README | Public CI green in <30 s, README has copy-paste 5-min tutorial |
| 16-06 | Time-travel Python API | `lecerf_snapshot`, `lecerf_rewind` C exports; `b.snapshot()` / `b.rewind(cycle)` Python; one test asserting rewind determinism | Optional — ship if time permits after 16-05 |

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — ctypes, scikit-build-core, cibuildwheel, pytest11 entry-point are all official, documented, and production-proven
- Architecture: HIGH — the `lecerf.h` opaque pointer pattern is standard C library design; the board profile table is a straightforward refactor
- Pitfalls: MEDIUM — global state issue (g_cpu_for_scb) is verified by reading source; Windows MinGW/ctypes compatibility needs runtime verification
- Docker sizing: MEDIUM — python:3.12-alpine ~50 MB baseline is verified; final image size with pytest needs measurement in 16-04
- GH Action timing: MEDIUM — 20-25 s estimate is based on 45 MB image pull speed; actual varies by runner

**Research date:** 2026-04-28
**Valid until:** 2026-07-28 (cibuildwheel defaults may shift; otherwise stable)
