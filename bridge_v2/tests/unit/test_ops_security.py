"""Tests for bridgev2.ops.security — segurança da API local."""
from __future__ import annotations

import logging
import os
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from bridgev2.ops.security import (
    check_ip,
    check_token,
    load_or_create_token,
)


class TestLoadOrCreateToken:
    def test_uses_env_var_when_set(self, monkeypatch, tmp_path):
        monkeypatch.setenv("NOISEBOT_OPS_TOKEN", "my-secret-token")
        with patch("bridgev2.ops.security._TOKEN_FILE", tmp_path / "token"):
            token = load_or_create_token()
        assert token == "my-secret-token"

    def test_reads_from_file_when_no_env(self, monkeypatch, tmp_path):
        monkeypatch.delenv("NOISEBOT_OPS_TOKEN", raising=False)
        token_file = tmp_path / "ops_token"
        token_file.write_text("file-token-abc")
        with patch("bridgev2.ops.security._TOKEN_FILE", token_file):
            token = load_or_create_token()
        assert token == "file-token-abc"

    def test_generates_and_saves_when_neither(self, monkeypatch, tmp_path):
        monkeypatch.delenv("NOISEBOT_OPS_TOKEN", raising=False)
        token_file = tmp_path / "ops_token"
        with patch("bridgev2.ops.security._TOKEN_FILE", token_file):
            token = load_or_create_token()
        assert len(token) == 64   # hex(32) = 64 chars
        assert token_file.read_text() == token

    def test_generated_tokens_are_unique(self, monkeypatch, tmp_path):
        monkeypatch.delenv("NOISEBOT_OPS_TOKEN", raising=False)
        tokens = set()
        for i in range(5):
            tf = tmp_path / f"token_{i}"
            with patch("bridgev2.ops.security._TOKEN_FILE", tf):
                tokens.add(load_or_create_token())
        assert len(tokens) == 5


class TestCheckToken:
    def _make_request(self, auth_header: str = "", token_param: str = "") -> MagicMock:
        req = MagicMock()
        req.headers = {"Authorization": auth_header} if auth_header else {}
        req.rel_url.query = {"token": token_param} if token_param else {}
        return req

    def test_valid_bearer_token(self):
        req = self._make_request(auth_header="Bearer abc123")
        assert check_token(req, "abc123")

    def test_invalid_bearer_token(self):
        req = self._make_request(auth_header="Bearer wrong")
        assert not check_token(req, "abc123")

    def test_valid_query_param(self):
        req = self._make_request(token_param="abc123")
        assert check_token(req, "abc123")

    def test_invalid_query_param(self):
        req = self._make_request(token_param="bad")
        assert not check_token(req, "abc123")

    def test_empty_request_fails(self):
        req = self._make_request()
        assert not check_token(req, "abc123")

    def test_empty_expected_fails(self):
        req = self._make_request(auth_header="Bearer abc123")
        assert not check_token(req, "")

    def test_bearer_case_insensitive(self):
        req = self._make_request(auth_header="BEARER abc123")
        assert check_token(req, "abc123")


class TestCheckIp:
    def _make_request(self, remote: str) -> MagicMock:
        req = MagicMock()
        req.remote = remote
        return req

    def test_none_allowed_ips_allows_all(self):
        req = self._make_request("192.168.1.50")
        assert check_ip(req, allowed_ips=None)

    def test_localhost_always_allowed(self):
        for ip in ("127.0.0.1", "::1", "localhost"):
            req = self._make_request(ip)
            assert check_ip(req, allowed_ips=["10.0.0.1"])

    def test_allowed_ip_passes(self):
        req = self._make_request("192.168.1.50")
        assert check_ip(req, allowed_ips=["192.168.1.50"])

    def test_unlisted_ip_blocked(self):
        req = self._make_request("10.0.0.99")
        assert not check_ip(req, allowed_ips=["192.168.1.1"])
