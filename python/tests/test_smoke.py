"""Smoke tests for the lecerf Python wrapper.

Six tests covering: create/destroy lifecycle, error handling, flash+run+regs,
UART drain, GPIO read, and two-board isolation. All must pass in < 5 s total.
"""

import lecerf
from .conftest import fw


def test_create_destroy():
    """Board creation and destruction must not crash."""
    b = lecerf.Board("stm32f103")
    assert b is not None
    del b  # __del__ -> lecerf_board_destroy; must not segfault


def test_unknown_board_raises():
    """Unknown board name must raise RuntimeError with 'Unknown board' message."""
    try:
        lecerf.Board("does-not-exist")
    except RuntimeError as e:
        assert "Unknown board" in str(e)
        return
    raise AssertionError("expected RuntimeError for unknown board name")


def test_fib_test1():
    """test1.bin computes fib(10) = 55 = 0x37, stores in R0, then halts."""
    b = lecerf.Board("generic-m4")
    result = b.flash(fw("test1")).run(timeout_ms=200)
    assert result.exit_cause == "halt", (
        f"expected exit_cause='halt', got {result.exit_cause!r}"
    )
    assert result.r[0] == 0x37, (
        f"expected R0=0x37 (fib(10)=55), got R0={result.r[0]:#x}"
    )


def test_uart_output():
    """test3.bin prints 'div=6 mul=294\\n' via UART sink; drain returns bytes."""
    b = lecerf.Board("generic-m4")
    b.flash(fw("test3")).run(timeout_ms=200)
    out = b.uart.output()
    assert isinstance(out, bytes), f"uart.output() must return bytes, got {type(out)}"
    # test3 always emits UDIV/MUL results — assert non-empty capture
    assert len(out) > 0, "uart.output() returned empty bytes; expected UART data"


def test_gpio_blink():
    """test10_stm32_blink toggles GPIOC[13]; gpio['GPIOC'][13].value is bool."""
    b = lecerf.Board("stm32f103")
    b.flash(fw("test10_stm32_blink")).run(timeout_ms=500)
    v = b.gpio["GPIOC"][13].value
    assert isinstance(v, bool), (
        f"gpio['GPIOC'][13].value must be bool, got {type(v)}"
    )


def test_two_board_isolation():
    """Two Board instances must have independent CPU state.

    Runs test1 on board A (R0=55), then test2 on board B, then asserts
    A's R0 was not mutated. This validates Phase 16-01's globals refactor.
    """
    a = lecerf.Board("generic-m4")
    a.flash(fw("test1")).run(timeout_ms=200)
    a_r0_after_run = a.cpu.r[0]
    assert a_r0_after_run == 55, f"sanity: A.R0 should be 55 after test1, got {a_r0_after_run}"

    b = lecerf.Board("generic-m4")
    b.flash(fw("test2")).run(timeout_ms=200)

    assert a.cpu.r[0] == a_r0_after_run, (
        f"Board A R0 mutated by Board B: expected {a_r0_after_run}, "
        f"got {a.cpu.r[0]}"
    )
