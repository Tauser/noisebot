"""Operating-system service management for ``noisebot_server``."""

from __future__ import annotations

import platform
import re
import subprocess
import sys
from abc import ABC, abstractmethod
from pathlib import Path

TASK_NAME = "NoiseBot Server"
SERVICE_NAME = "noisebot-server"

# SF-09 (docs/ANALISE_SERVER_FINDINGS_2026-06-11.md): TASK_NAME e interpolado
# em scripts PowerShell via f-string. Hoje e uma constante interna (risco
# baixo), mas garantimos na borda que ele nunca contem aspas ou metacaracteres
# de shell, caso um dia vire configuravel.
_TASK_NAME_RE = re.compile(r"^[A-Za-z0-9 _-]+$")
assert _TASK_NAME_RE.match(TASK_NAME), f"TASK_NAME invalido: {TASK_NAME!r}"


class ServiceManager(ABC):
    """Interface for local service managers."""

    @abstractmethod
    def install(self) -> None:
        """Register the server service."""

    @abstractmethod
    def uninstall(self) -> None:
        """Remove the server service."""

    @abstractmethod
    def status(self) -> str:
        """Return a human-readable service status."""

    @abstractmethod
    def start(self) -> None:
        """Start the service."""

    @abstractmethod
    def stop(self) -> None:
        """Stop the service."""


def service_workdir() -> Path:
    """Use the install-time cwd so editable/dev installs keep their .env nearby."""
    return Path.cwd()


def get_manager() -> ServiceManager:
    """Return the service manager for the current OS."""
    if platform.system() == "Windows":
        return WindowsTaskSchedulerManager()
    return SystemdManager()


class WindowsTaskSchedulerManager(ServiceManager):
    """Manage the server through Windows Task Scheduler."""

    def install(self) -> None:
        python = sys.executable
        workdir = service_workdir()
        ps_script = f"""
$action  = New-ScheduledTaskAction `
    -Execute '{python}' `
    -Argument '-m noisebot_server' `
    -WorkingDirectory '{workdir}'
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 999 `
    -RestartInterval (New-TimeSpan -Seconds 5) `
    -StartWhenAvailable $true
$principal = New-ScheduledTaskPrincipal `
    -UserId $env:USERNAME `
    -LogonType Interactive `
    -RunLevel Highest
Register-ScheduledTask `
    -TaskName '{TASK_NAME}' `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -Principal $principal `
    -Force
"""
        self._run_ps(ps_script, f"Instalar tarefa '{TASK_NAME}'")
        print(f"Servico '{TASK_NAME}' instalado no Task Scheduler.")
        print("Para iniciar agora: python -m noisebot_server service start")

    def uninstall(self) -> None:
        ps_script = f"Unregister-ScheduledTask -TaskName '{TASK_NAME}' -Confirm:$false"
        self._run_ps(ps_script, f"Remover tarefa '{TASK_NAME}'")
        print(f"Servico '{TASK_NAME}' removido.")

    def status(self) -> str:
        ps_script = (
            f"Get-ScheduledTask -TaskName '{TASK_NAME}' 2>$null "
            f"| Select-Object -ExpandProperty State"
        )
        try:
            result = subprocess.run(
                ["powershell", "-NonInteractive", "-Command", ps_script],
                capture_output=True,
                text=True,
                timeout=10,
            )
            state = result.stdout.strip()
            if not state:
                return "Nao instalado"
            return f"Task Scheduler: {state}"
        except Exception as exc:
            return f"Erro ao verificar status: {exc}"

    def start(self) -> None:
        ps_script = f"Start-ScheduledTask -TaskName '{TASK_NAME}'"
        self._run_ps(ps_script, f"Iniciar tarefa '{TASK_NAME}'")

    def stop(self) -> None:
        ps_script = f"Stop-ScheduledTask -TaskName '{TASK_NAME}'"
        self._run_ps(ps_script, f"Parar tarefa '{TASK_NAME}'")

    @staticmethod
    def _run_ps(script: str, description: str) -> None:
        try:
            result = subprocess.run(
                ["powershell", "-NonInteractive", "-Command", script],
                capture_output=True,
                text=True,
                timeout=30,
            )
            if result.returncode != 0:
                raise RuntimeError(result.stderr.strip() or f"Falha ao: {description}")
        except FileNotFoundError:
            raise RuntimeError("PowerShell nao encontrado. Necessario Windows 7+.")


SYSTEMD_TEMPLATE = """\
[Unit]
Description=NoiseBot Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart={python} -m noisebot_server
WorkingDirectory={workdir}
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=noisebot-server

[Install]
WantedBy=default.target
"""


class SystemdManager(ServiceManager):
    """Manage the server as a user systemd service."""

    @property
    def _unit_dir(self) -> Path:
        return Path.home() / ".config" / "systemd" / "user"

    @property
    def _unit_file(self) -> Path:
        return self._unit_dir / f"{SERVICE_NAME}.service"

    def install(self) -> None:
        self._unit_dir.mkdir(parents=True, exist_ok=True)
        content = SYSTEMD_TEMPLATE.format(
            python=sys.executable,
            workdir=service_workdir(),
        )
        self._unit_file.write_text(content, encoding="utf-8")
        self._systemctl("daemon-reload")
        self._systemctl("enable", SERVICE_NAME)
        print(f"Servico systemd '{SERVICE_NAME}' instalado e habilitado.")
        print(f"Arquivo: {self._unit_file}")
        print("Para iniciar agora: python -m noisebot_server service start")

    def uninstall(self) -> None:
        self._systemctl("disable", "--now", SERVICE_NAME)
        if self._unit_file.exists():
            self._unit_file.unlink()
        self._systemctl("daemon-reload")
        print(f"Servico systemd '{SERVICE_NAME}' removido.")

    def status(self) -> str:
        try:
            result = subprocess.run(
                ["systemctl", "--user", "status", SERVICE_NAME],
                capture_output=True,
                text=True,
                timeout=10,
            )
            return result.stdout.strip() or result.stderr.strip()
        except FileNotFoundError:
            return "systemd nao disponivel neste sistema."
        except Exception as exc:
            return f"Erro: {exc}"

    def start(self) -> None:
        self._systemctl("start", SERVICE_NAME)

    def stop(self) -> None:
        self._systemctl("stop", SERVICE_NAME)

    @staticmethod
    def _systemctl(*args: str) -> None:
        cmd = ["systemctl", "--user", *args]
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
            if result.returncode != 0:
                raise RuntimeError(result.stderr.strip())
        except FileNotFoundError:
            raise RuntimeError("systemctl nao encontrado. Necessario systemd.")
