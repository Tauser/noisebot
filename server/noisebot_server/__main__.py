"""NoiseBot server entrypoint.

For phase 1 this delegates to bridgev2's CLI. Keeping the same CLI surface
prevents behavior drift while the server package is introduced.
"""

from __future__ import annotations

from ._compat import ensure_bridgev2_path


def main() -> None:
    ensure_bridgev2_path()
    from bridgev2.__main__ import main as bridge_main

    bridge_main()


if __name__ == "__main__":
    main()
