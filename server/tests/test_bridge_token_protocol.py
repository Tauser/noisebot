"""SF-02: token compartilhado no HELLO do bridge firmware<->server.

`load_bridge_token` / `server_hello_capabilities` sao somente-leitura no
server: o token e gerado pelo firmware (NVS nb_sys/api_token) e copiado pelo
usuario para NOISEBOT_BRIDGE_TOKEN ou ~/.noisebot-server/bridge_token.
"""

from noisebot_server.internal.transport import protocol


def test_load_bridge_token_absent_by_default(monkeypatch, tmp_path):
    monkeypatch.delenv("NOISEBOT_BRIDGE_TOKEN", raising=False)
    monkeypatch.setattr(protocol, "_BRIDGE_TOKEN_FILE", tmp_path / "bridge_token")

    assert protocol.load_bridge_token() is None


def test_load_bridge_token_from_env(monkeypatch, tmp_path):
    monkeypatch.setenv("NOISEBOT_BRIDGE_TOKEN", "deadbeefcafef00d")
    monkeypatch.setattr(protocol, "_BRIDGE_TOKEN_FILE", tmp_path / "bridge_token")

    assert protocol.load_bridge_token() == "deadbeefcafef00d"


def test_load_bridge_token_from_file(monkeypatch, tmp_path):
    monkeypatch.delenv("NOISEBOT_BRIDGE_TOKEN", raising=False)
    token_file = tmp_path / "bridge_token"
    token_file.write_text("0123456789abcdef\n", encoding="utf-8")
    monkeypatch.setattr(protocol, "_BRIDGE_TOKEN_FILE", token_file)

    assert protocol.load_bridge_token() == "0123456789abcdef"


def test_server_hello_capabilities_without_token(monkeypatch, tmp_path):
    monkeypatch.delenv("NOISEBOT_BRIDGE_TOKEN", raising=False)
    monkeypatch.setattr(protocol, "_BRIDGE_TOKEN_FILE", tmp_path / "bridge_token")

    caps = protocol.server_hello_capabilities()

    assert "token" not in caps
    assert caps["protocol"] == protocol.SERVER_HELLO_CAPABILITIES["protocol"]
    # server_hello_capabilities() não deve mutar o dict global
    assert "token" not in protocol.SERVER_HELLO_CAPABILITIES


def test_server_hello_capabilities_with_token(monkeypatch, tmp_path):
    monkeypatch.setenv("NOISEBOT_BRIDGE_TOKEN", "deadbeefcafef00d")
    monkeypatch.setattr(protocol, "_BRIDGE_TOKEN_FILE", tmp_path / "bridge_token")

    caps = protocol.server_hello_capabilities()

    assert caps["token"] == "deadbeefcafef00d"
    assert "token" not in protocol.SERVER_HELLO_CAPABILITIES


def test_encode_hello_includes_token_when_present(monkeypatch, tmp_path):
    monkeypatch.setenv("NOISEBOT_BRIDGE_TOKEN", "deadbeefcafef00d")
    monkeypatch.setattr(protocol, "_BRIDGE_TOKEN_FILE", tmp_path / "bridge_token")

    caps = protocol.server_hello_capabilities()
    payload = protocol.encode_hello(caps)
    decoded = protocol.decode_hello(payload)

    assert decoded["token"] == "deadbeefcafef00d"
