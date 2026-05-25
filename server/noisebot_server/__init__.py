"""NoiseBot local companion server.

This package starts as a compatibility facade over ``bridge_v2``. The facade
lets us introduce the server architecture without moving runtime code in one
large risky change.
"""

from __future__ import annotations

__all__ = ["__version__"]

__version__ = "0.1.0"
