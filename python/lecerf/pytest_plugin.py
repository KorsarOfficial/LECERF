"""lecerf pytest plugin — auto-discovered via pytest11 entry point.

Provides:
  board      — function-scoped Board using --lecerf-board (default generic-m4)
  board_all  — parametrized across stm32f103, stm32f407, generic-m4

Usage (no conftest.py needed after pip install lecerf):
    def test_fib(board):
        r = board.flash("firmware.bin").run(timeout_ms=200)
        assert r.r[0] == 55
"""

import pytest
from lecerf import Board


def pytest_addoption(parser):
    parser.addoption(
        "--lecerf-board",
        default="generic-m4",
        help="Default board profile for the `board` fixture (default: generic-m4)",
    )


@pytest.fixture
def board(request):
    """Per-test Board instance. Profile from --lecerf-board (default: generic-m4)."""
    name = request.config.getoption("--lecerf-board")
    b = Board(name)
    yield b
    del b  # triggers __del__ -> lecerf_board_destroy


@pytest.fixture(params=["stm32f103", "stm32f407", "generic-m4"])
def board_all(request):
    """Parametrized fixture: each consumer test runs once per board profile."""
    b = Board(request.param)
    yield b
    del b
