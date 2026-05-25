from __future__ import annotations

import importlib


def test_bridgev2_compat_path_allows_application_import() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    app_module = importlib.import_module("noisebot_server.app")

    assert hasattr(app_module, "NoiseBotServer")


def test_server_cli_delegates_to_bridgev2_entrypoint() -> None:
    compat = importlib.import_module("noisebot_server._compat")
    compat.ensure_bridgev2_path()

    cli_module = importlib.import_module("noisebot_server.__main__")

    assert callable(cli_module.main)
