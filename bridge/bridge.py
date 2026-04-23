#!/usr/bin/env python3
"""NoiseBot bridge CLI.

Mantem compatibilidade com:
    python bridge.py --host noisebot.local --dry-run
"""

from noisebot_bridge.cli import main


if __name__ == "__main__":
    main()
