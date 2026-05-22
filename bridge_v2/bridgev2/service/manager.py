"""bridgev2.service.manager — Gerenciador de serviço por SO (Fase 9).

Abstração de instalação/remoção/status do bridge como serviço local.
Implementações:
  - Windows: Task Scheduler (schtasks / PowerShell)
  - Linux/Pi: systemd (systemctl --user)

Uso via CLI:
    python -m bridgev2 service install
    python -m bridgev2 service uninstall
    python -m bridgev2 service status
    python -m bridgev2 service start
    python -m bridgev2 service stop
"""
from __future__ import annotations

import logging
import platform
import subprocess
import sys
from abc import ABC, abstractmethod
from pathlib import Path

log = logging.getLogger(__name__)

TASK_NAME   = "NoiseBot Bridge v2"     # Windows Task Scheduler
SERVICE_NAME = "bridgev2"              # systemd unit name


# ---------------------------------------------------------------------------
# Interface base
# ---------------------------------------------------------------------------

class ServiceManager(ABC):
    """Interface de gerenciamento de serviço."""

    @abstractmethod
    def install(self) -> None:
        """Registra o serviço no SO. Requer privilégios se necessário."""

    @abstractmethod
    def uninstall(self) -> None:
        """Remove o serviço do SO."""

    @abstractmethod
    def status(self) -> str:
        """Retorna string de status legível."""

    @abstractmethod
    def start(self) -> None:
        """Inicia o serviço se parado."""

    @abstractmethod
    def stop(self) -> None:
        """Para o serviço se rodando."""


def get_manager() -> ServiceManager:
    """Retorna o manager correto para o SO atual."""
    if platform.system() == "Windows":
        return WindowsTaskSchedulerManager()
    return SystemdManager()


# ---------------------------------------------------------------------------
# Windows — Task Scheduler
# ---------------------------------------------------------------------------

class WindowsTaskSchedulerManager(ServiceManager):
    """Gerencia o bridge como tarefa agendada do Windows Task Scheduler.

    Vantagens sobre NSSM/WinSW:
    - Nenhuma dependência externa
    - Restart automático configurável nas propriedades da tarefa
    - Integrado ao Windows
    """

    def install(self) -> None:
        python = sys.executable
        # Monta comando de instalação via PowerShell
        ps_script = f"""
$action  = New-ScheduledTaskAction `
    -Execute '{python}' `
    -Argument '-m bridgev2' `
    -WorkingDirectory '{Path.home()}'
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
        print(f"Serviço '{TASK_NAME}' instalado no Task Scheduler.")
        print("Para iniciar agora: python -m bridgev2 service start")

    def uninstall(self) -> None:
        ps_script = f"Unregister-ScheduledTask -TaskName '{TASK_NAME}' -Confirm:$false"
        self._run_ps(ps_script, f"Remover tarefa '{TASK_NAME}'")
        print(f"Serviço '{TASK_NAME}' removido.")

    def status(self) -> str:
        ps_script = (
            f"Get-ScheduledTask -TaskName '{TASK_NAME}' 2>$null "
            f"| Select-Object -ExpandProperty State"
        )
        try:
            result = subprocess.run(
                ["powershell", "-NonInteractive", "-Command", ps_script],
                capture_output=True, text=True, timeout=10,
            )
            state = result.stdout.strip()
            if not state:
                return "Não instalado"
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
                capture_output=True, text=True, timeout=30,
            )
            if result.returncode != 0:
                raise RuntimeError(result.stderr.strip() or f"Falha ao: {description}")
        except FileNotFoundError:
            raise RuntimeError("PowerShell não encontrado. Necessário Windows 7+.")


# ---------------------------------------------------------------------------
# Linux / Pi — systemd (user service)
# ---------------------------------------------------------------------------

_SYSTEMD_TEMPLATE = """\
[Unit]
Description=NoiseBot Bridge v2 — pipeline de voz async
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart={python} -m bridgev2
WorkingDirectory={workdir}
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=bridgev2

[Install]
WantedBy=default.target
"""


class SystemdManager(ServiceManager):
    """Gerencia o bridge como serviço systemd de usuário."""

    @property
    def _unit_dir(self) -> Path:
        return Path.home() / ".config" / "systemd" / "user"

    @property
    def _unit_file(self) -> Path:
        return self._unit_dir / f"{SERVICE_NAME}.service"

    def install(self) -> None:
        self._unit_dir.mkdir(parents=True, exist_ok=True)
        content = _SYSTEMD_TEMPLATE.format(
            python=sys.executable,
            workdir=Path.home(),
        )
        self._unit_file.write_text(content)
        self._systemctl("daemon-reload")
        self._systemctl("enable", SERVICE_NAME)
        print(f"Serviço systemd '{SERVICE_NAME}' instalado e habilitado.")
        print(f"Arquivo: {self._unit_file}")
        print("Para iniciar agora: python -m bridgev2 service start")

    def uninstall(self) -> None:
        self._systemctl("disable", "--now", SERVICE_NAME)
        if self._unit_file.exists():
            self._unit_file.unlink()
        self._systemctl("daemon-reload")
        print(f"Serviço systemd '{SERVICE_NAME}' removido.")

    def status(self) -> str:
        try:
            result = subprocess.run(
                ["systemctl", "--user", "status", SERVICE_NAME],
                capture_output=True, text=True, timeout=10,
            )
            return result.stdout.strip() or result.stderr.strip()
        except FileNotFoundError:
            return "systemd não disponível neste sistema."
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
            raise RuntimeError("systemctl não encontrado. Necessário systemd.")
