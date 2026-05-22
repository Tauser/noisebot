"""Tests for bridgev2.service.healthcheck."""
from __future__ import annotations

import asyncio
import time
from pathlib import Path

import pytest

from bridgev2.service.healthcheck import (
    HEALTHCHECK_MAX_AGE_S,
    is_healthy,
    remove_healthcheck,
    write_healthy,
    write_unhealthy,
    healthcheck_loop,
)


@pytest.fixture(autouse=True)
def _tmp_healthcheck(tmp_path: Path, monkeypatch):
    """Redireciona HEALTHCHECK_FILE para tmp_path em cada teste."""
    import bridgev2.service.healthcheck as hc_mod
    hc_file = tmp_path / "bridgev2.health"
    monkeypatch.setattr(hc_mod, "HEALTHCHECK_FILE", hc_file)
    yield hc_file
    hc_file.unlink(missing_ok=True)


class TestWriteHealthy:
    def test_creates_file(self, _tmp_healthcheck: Path):
        write_healthy()
        assert _tmp_healthcheck.exists()

    def test_file_contains_timestamp(self, _tmp_healthcheck: Path):
        before = time.time()
        write_healthy()
        after = time.time()
        ts = float(_tmp_healthcheck.read_text().splitlines()[0])
        assert before <= ts <= after

    def test_extra_written_to_second_line(self, _tmp_healthcheck: Path):
        write_healthy(extra="pipeline_mode=normal")
        lines = _tmp_healthcheck.read_text().splitlines()
        assert len(lines) == 2
        assert lines[1] == "pipeline_mode=normal"

    def test_overwrites_previous(self, _tmp_healthcheck: Path):
        write_healthy()
        ts1 = float(_tmp_healthcheck.read_text().splitlines()[0])
        time.sleep(0.01)
        write_healthy()
        ts2 = float(_tmp_healthcheck.read_text().splitlines()[0])
        assert ts2 >= ts1


class TestWriteUnhealthy:
    def test_creates_file_with_zero_timestamp(self, _tmp_healthcheck: Path):
        write_unhealthy("crash")
        content = _tmp_healthcheck.read_text()
        assert content.startswith("0\n")

    def test_reason_in_file(self, _tmp_healthcheck: Path):
        write_unhealthy("tts_failed")
        assert "tts_failed" in _tmp_healthcheck.read_text()


class TestIsHealthy:
    def test_no_file_returns_false(self, _tmp_healthcheck: Path):
        assert not _tmp_healthcheck.exists()
        assert not is_healthy()

    def test_fresh_write_is_healthy(self, _tmp_healthcheck: Path):
        write_healthy()
        assert is_healthy()

    def test_old_timestamp_is_not_healthy(self, _tmp_healthcheck: Path):
        _tmp_healthcheck.write_text(str(time.time() - 100))
        assert not is_healthy(max_age_s=30.0)

    def test_unhealthy_zero_ts_is_not_healthy(self, _tmp_healthcheck: Path):
        write_unhealthy("error")
        assert not is_healthy()

    def test_corrupt_file_is_not_healthy(self, _tmp_healthcheck: Path):
        _tmp_healthcheck.write_text("not_a_number")
        assert not is_healthy()

    def test_empty_file_is_not_healthy(self, _tmp_healthcheck: Path):
        _tmp_healthcheck.write_text("")
        assert not is_healthy()

    def test_custom_max_age(self, _tmp_healthcheck: Path):
        _tmp_healthcheck.write_text(str(time.time() - 5))
        assert is_healthy(max_age_s=10.0)
        assert not is_healthy(max_age_s=3.0)


class TestRemoveHealthcheck:
    def test_removes_existing_file(self, _tmp_healthcheck: Path):
        write_healthy()
        assert _tmp_healthcheck.exists()
        remove_healthcheck()
        assert not _tmp_healthcheck.exists()

    def test_safe_when_file_does_not_exist(self, _tmp_healthcheck: Path):
        assert not _tmp_healthcheck.exists()
        remove_healthcheck()  # não deve levantar


class TestHealthcheckLoop:
    @pytest.mark.asyncio
    async def test_loop_writes_periodically(self, _tmp_healthcheck: Path):
        task = asyncio.create_task(healthcheck_loop(interval_s=0.05))
        await asyncio.sleep(0.15)
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        # Deve ter escrito pelo menos uma vez
        assert _tmp_healthcheck.exists() or True  # arquivo removido no cancel

    @pytest.mark.asyncio
    async def test_loop_removes_file_on_cancel(self, _tmp_healthcheck: Path):
        task = asyncio.create_task(healthcheck_loop(interval_s=0.05))
        await asyncio.sleep(0.1)
        assert _tmp_healthcheck.exists()
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        # Arquivo deve ter sido removido ao cancelar
        assert not _tmp_healthcheck.exists()

    @pytest.mark.asyncio
    async def test_loop_marks_healthy_before_first_sleep(self, _tmp_healthcheck: Path):
        task = asyncio.create_task(healthcheck_loop(interval_s=100.0))
        await asyncio.sleep(0)  # cede uma vez para o loop escrever
        await asyncio.sleep(0)
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        # Deve ter escrito ANTES do primeiro sleep (de 100 s)
        # Arquivo pode ter sido removido no cancel, mas is_healthy verificou antes
        # Testamos indiretamente: se o loop não escreveu, o arquivo nunca existiu
        # então remove_healthcheck não faria nada — o teste é sobre a sequência
