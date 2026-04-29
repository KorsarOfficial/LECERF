"""lecerf — Python wrapper for the Cortex-M emulator shared library.

Quick start:
    from lecerf import Board
    b = Board("stm32f103")
    result = b.flash("firmware.bin").run(timeout_ms=500)
    print(result.exit_cause, result.r[0])
"""

from .board import Board
from .run_result import RunResult

__all__ = ["Board", "RunResult"]
__version__ = "0.1.0"
