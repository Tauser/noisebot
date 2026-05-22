"""Tests for bridgev2.service.manager — ServiceManager."""
from __future__ import annotations

import platform
import subprocess
import sys
from unittest.mock import MagicMock, patch

import pytest

from bridgev2.service.manager import (
    get_manager,
    WindowsTaskSchedulerManager,
    SystemdManager,
    TASK_NAME,
    SERVICE_NAME,
)


class TestGetManager:
    def test_returns_windows_on_windows(self):
        with patch("platform.system", return_value="Windows"):
            mgr = get_manager()
        assert isinstance(mgr, WindowsTaskSchedulerManager)

    def test_returns_systemd_on_linux(self):
        with patch("platform.system", return_value="Linux"):
            mgr = get_manager()
        assert isinstance(mgr, SystemdManager)

    def test_returns_systemd_on_darwin(self):
        with patch("platform.system", return_value="Darwin"):
            mgr = get_manager()
        assert isinstance(mgr, SystemdManager)


class TestWindowsTaskSchedulerManager:
    def _run_ok(self, *args, **kwargs):
        result = MagicMock()
        result.returncode = 0
        result.stdout = "Ready\n"
        result.stderr = ""
        return result

    def _run_fail(self, *args, **kwargs):
        result = MagicMock()
        result.returncode = 1
        result.stderr = "Acesso negado"
        result.stdout = ""
        return result

    def test_install_calls_powershell(self):
        mgr = WindowsTaskSchedulerManager()
        with patch("subprocess.run", side_effect=self._run_ok) as mock_run:
            mgr.install()
        assert mock_run.called
        cmd = mock_run.call_args.args[0]
        assert cmd[0] == "powershell"

    def test_install_includes_task_name(self):
        mgr = WindowsTaskSchedulerManager()
        with patch("subprocess.run", side_effect=self._run_ok) as mock_run:
            mgr.install()
        script = mock_run.call_args.args[0][-1]
        assert TASK_NAME in script

    def test_install_raises_on_failure(self):
        mgr = WindowsTaskSchedulerManager()
        with patch("subprocess.run", side_effect=self._run_fail):
            with pytest.raises(RuntimeError):
                mgr.install()

    def test_uninstall_calls_powershell(self):
        mgr = WindowsTaskSchedulerManager()
        with patch("subprocess.run", side_effect=self._run_ok) as mock_run:
            mgr.uninstall()
        assert mock_run.called

    def test_status_returns_string(self):
        mgr = WindowsTaskSchedulerManager()
        with patch("subprocess.run", side_effect=self._run_ok):
            result = mgr.status()
        assert isinstance(result, str)
        assert len(result) > 0

    def test_status_not_installed_when_empty_stdout(self):
        mgr = WindowsTaskSchedulerManager()
        def _empty(*args, **kwargs):
            r = MagicMock()
            r.returncode = 0
            r.stdout = ""
            return r
        with patch("subprocess.run", side_effect=_empty):
            result = mgr.status()
        assert "instalado" in result.lower() or result == "Não instalado"

    def test_powershell_not_found_raises(self):
        mgr = WindowsTaskSchedulerManager()
        with patch("subprocess.run", side_effect=FileNotFoundError):
            with pytest.raises(RuntimeError, match="PowerShell"):
                mgr.install()


class TestSystemdManager:
    def _run_ok(self, *args, **kwargs):
        result = MagicMock()
        result.returncode = 0
        result.stdout = "active (running)\n"
        result.stderr = ""
        return result

    def test_unit_dir_path(self):
        mgr = SystemdManager()
        assert "systemd" in str(mgr._unit_dir)
        assert "user" in str(mgr._unit_dir)

    def test_unit_file_contains_service_name(self):
        mgr = SystemdManager()
        assert SERVICE_NAME in mgr._unit_file.name

    def test_install_writes_unit_file(self, tmp_path):
        mgr = SystemdManager()
        with patch.object(type(mgr), "_unit_dir", new_callable=lambda: property(lambda self: tmp_path)):
            with patch("subprocess.run", side_effect=self._run_ok):
                mgr.install()
        unit_file = tmp_path / f"{SERVICE_NAME}.service"
        assert unit_file.exists()
        content = unit_file.read_text()
        assert "ExecStart" in content
        assert sys.executable in content

    def test_install_unit_contains_restart_policy(self, tmp_path):
        mgr = SystemdManager()
        with patch.object(type(mgr), "_unit_dir", new_callable=lambda: property(lambda self: tmp_path)):
            with patch("subprocess.run", side_effect=self._run_ok):
                mgr.install()
        unit_file = tmp_path / f"{SERVICE_NAME}.service"
        content = unit_file.read_text()
        assert "Restart=on-failure" in content
        assert "RestartSec=5" in content

    def test_status_calls_systemctl(self):
        mgr = SystemdManager()
        with patch("subprocess.run", side_effect=self._run_ok) as mock_run:
            result = mgr.status()
        cmd = mock_run.call_args.args[0]
        assert "systemctl" in cmd
        assert "--user" in cmd
        assert "status" in cmd

    def test_status_returns_string(self):
        mgr = SystemdManager()
        with patch("subprocess.run", side_effect=self._run_ok):
            result = mgr.status()
        assert isinstance(result, str)

    def test_status_no_systemd_returns_message(self):
        mgr = SystemdManager()
        with patch("subprocess.run", side_effect=FileNotFoundError):
            result = mgr.status()
        assert "systemd" in result.lower() or "não disponível" in result.lower()

    def test_start_calls_systemctl_start(self):
        mgr = SystemdManager()
        with patch("subprocess.run", side_effect=self._run_ok) as mock_run:
            mgr.start()
        cmd = mock_run.call_args.args[0]
        assert "start" in cmd

    def test_stop_calls_systemctl_stop(self):
        mgr = SystemdManager()
        with patch("subprocess.run", side_effect=self._run_ok) as mock_run:
            mgr.stop()
        cmd = mock_run.call_args.args[0]
        assert "stop" in cmd
