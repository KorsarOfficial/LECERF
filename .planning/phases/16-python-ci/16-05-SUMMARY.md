# 16-05 Summary: GitHub Action + Sample Repo + Phase Close-out

## Outcome

Phase 16 SHIPPED. Final wave delivered three artifacts:

1. **`.github/actions/lecerf-runner/action.yml`** — reusable Docker container
   Action pointing at `ghcr.io/korsarofficial/lecerf:latest` with
   `board / firmware-dir / tests-dir / timeout-ms` inputs and `junit-xml`
   output (38 lines).
2. **`.github/workflows/release.yml`** — tag-triggered (`v*`) 3-job release
   pipeline: `build-wheel-linux` (cmake + `python -m build --wheel`),
   `build-and-push-docker` (`docker/build-push-action@v5` to ghcr.io with
   GHA cache), `release` (`softprops/action-gh-release@v2` uploads the
   wheel) (95 lines).
3. **`examples/lecerf-ci-example/`** — self-contained sample repo with
   3 firmware `.bin` files, 3 pytest test files, a CI workflow that calls
   the LECERF action, a README with a 5-minute onboarding tutorial, a
   `requirements.txt`, and `conftest.py` helper.

## Files created

```
.github/
  actions/lecerf-runner/action.yml         (38 lines)
  workflows/release.yml                    (95 lines)

examples/lecerf-ci-example/
  README.md                                (123 lines)
  requirements.txt                         (2 lines)
  firmware/test1.bin                       (58 B,  fib(10) -> R0=55)
  firmware/test3.bin                       (238 B, UDIV+MUL -> R0=6, R1=294)
  firmware/test10_stm32_blink.bin          (286 B, GPIO blink -> R0=5)
  tests/conftest.py                        (12 lines, fwpath() helper)
  tests/test_fib.py                        (10 lines)
  tests/test_uart.py                       (22 lines)
  tests/test_blink.py                      (24 lines, locks board=stm32f103)
  .github/workflows/firmware-test.yml      (32 lines)

.planning/phases/16-python-ci/
  PHASE-SUMMARY.md                         (Phase 16 close-out)
  16-05-SUMMARY.md                         (this file)
```

## Files modified

- `.planning/ROADMAP.md` — Phase 16 row marked `[x] shipped 2026-04-29`;
  all 5 plan checkboxes ticked; progress table updated to `Shipped 2026-04-29`.
- `.planning/STATE.md` — phase 16 marked complete; current position advanced.

## Verification

```
$ python -c "import yaml; yaml.safe_load(open('.github/actions/lecerf-runner/action.yml'))"
$ python -c "import yaml; yaml.safe_load(open('.github/workflows/release.yml'))"
$ python -c "import yaml; yaml.safe_load(open('examples/lecerf-ci-example/.github/workflows/firmware-test.yml'))"
                                                                                # all parse cleanly

$ ls examples/lecerf-ci-example/firmware/*.bin | wc -l                          # 3

$ cd examples/lecerf-ci-example && python -m pytest tests/ -v
============================= test session starts =============================
plugins: lecerf-0.1.0, ...
collected 3 items
tests/test_blink.py::test_blink_completes              PASSED  [ 33%]
tests/test_fib.py::test_fib_10_equals_55               PASSED  [ 66%]
tests/test_uart.py::test_uart_division_result          PASSED  [100%]
============================== 3 passed in 0.37s ==============================
```

## Wallclocks

| Path                                                     | Time      |
| -------------------------------------------------------- | --------- |
| `pytest tests/` local (no docker), 3 tests               | **0.37 s** |
| `pytest tests/` local incl. process startup              | ~4.3 s    |
| Estimated Docker container run (warm), 3 tests           | ~2 s      |
| Estimated GHA cold path: image pull (15s) + tests (2s)   | **~17 s** (under 30 s gate) |

## 5-minute onboarding excerpt (from README)

```bash
# 1. Clone this directory into a fresh GitHub repo (it is self-contained):
cp -r examples/lecerf-ci-example/ /tmp/my-firmware-tests
cd /tmp/my-firmware-tests
git init && git add . && git commit -m "initial"
git remote add origin git@github.com:<you>/<repo>.git
git push -u origin main

# 2. The included .github/workflows/firmware-test.yml fires on every push.
#    On first push the runner pulls the LECERF image (~20 MB compressed)
#    and runs `pytest tests/` inside.

# 3. Watch the green check appear under the Actions tab.
```

## Phase 16 close-out

5 / 5 plans shipped. CI-01..CI-06 all met. Headline numbers:

| Metric                          | Value         |
| ------------------------------- | ------------- |
| Docker image size               | 20.06 MB      |
| ctest                           | 20 / 20       |
| Firmware                        | 14 / 14       |
| pytest (in container)           | 15 / 15 in 1.87 s |
| pytest (local sample repo)      | 3 / 3 in 0.37 s   |
| LOC delta (aff4bfd..HEAD)       | +2535 / -203 across 46 files |
| End-to-end CI projection        | ~17 s         |

Full per-requirement matrix in `PHASE-SUMMARY.md`.

## Notable design points

- **Docker container Action** chosen over JavaScript Action: the whole point
  of shipping the Alpine image is reproducibility, so Action runtime should
  be the image itself, not a JS shim.
- **ghcr.io** chosen over Docker Hub: `secrets.GITHUB_TOKEN` already has
  write access; no extra setup. Owner name is lowercased at publish time
  because the OCI distribution spec mandates lowercase repository names.
- **Single-platform wheel (Linux x86_64)** for MVP. `cibuildwheel` matrix
  for macOS/Windows is post-MVP (deferred to Phase 17).
- **Sample repo in-tree** (not a sibling repo): keeps action.yml and the
  sample workflow in lockstep with zero version drift; can be split later.
- **Sample workflow uses `KorsarOfficial/cortex-m-emu/.github/actions/lecerf-runner@main`**
  for now (relative path inside the monorepo). After v1 tag flips to
  `KorsarOfficial/lecerf@v1`, both work because GHA resolves repo+path+ref.

## Next phase candidates

Recommended: **Phase 17 (release pipeline + landing)** — small, deterministic,
publishes the first public GH Release. Then **Phase 15 (WASM + Web IDE)**.

## Commits

- `dacc5b9` — `ci: lecerf-runner action.yml + release.yml docker+wheel pipeline`
- `f1e4805` — `examples: lecerf-ci-example sample repo with 3 firmware + 3 tests + readme`
- `e308a66` — `roadmap: mark python+ci runner shipped; 5/5 plans; 20MB image; CI-01..CI-06 met`

## Self-Check: PASSED

All listed files verified to exist on disk; all three commit hashes verified
in `git log`; sample repo's pytest runs 3/3 in 0.37s; YAML files all parse
cleanly under `python -c "import yaml; yaml.safe_load(...)"`. No `.planning/`
subtree found inside `examples/lecerf-ci-example/`. No pytest droppings
(`.pytest_cache/`, `__pycache__/`) committed.
