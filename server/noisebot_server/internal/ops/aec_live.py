"""Live AEC probe wrapper for the real firmware diagnostics endpoint."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from .voice_ab import VoiceAbError, get_json


@dataclass(frozen=True)
class AecLiveTrial:
    ok: bool
    promotable: bool
    probe_ok: bool
    supported: bool
    blocked_no_reference: bool
    probe_error: str
    internal_free_kb: int | None
    dma_largest_kb: int | None
    psram_current_kb: int | None
    status_after_ok: bool
    recommendation: str

    def to_dict(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "promotable": self.promotable,
            "probe_ok": self.probe_ok,
            "supported": self.supported,
            "blocked_no_reference": self.blocked_no_reference,
            "probe_error": self.probe_error,
            "internal_free_kb": self.internal_free_kb,
            "dma_largest_kb": self.dma_largest_kb,
            "psram_current_kb": self.psram_current_kb,
            "status_after_ok": self.status_after_ok,
            "recommendation": self.recommendation,
        }


def run_aec_live_probe(*, firmware_url: str) -> AecLiveTrial:
    """Run the firmware AEC probe and classify whether it can be promoted."""

    firmware_url = firmware_url.rstrip("/")
    probe = _post_json_diagnostic(
        f"{firmware_url}/api/audio/processor/aec/probe",
        timeout_s=5.0,
    )
    status_after = get_json(f"{firmware_url}/api/audio/processor", timeout_s=3.0)

    probe_ok = bool(probe.get("ok") and probe.get("aec_probe_ok"))
    supported = bool(probe.get("aec_supported", True))
    blocked_no_reference = bool(probe.get("aec_blocked_no_reference"))
    status_after_ok = bool(status_after.get("ok"))
    probe_error = str(probe.get("probe_error") or probe.get("aec_last_error") or "")
    internal_free_kb = _optional_int(probe.get("internal_free_kb"))
    dma_largest_kb = _optional_int(probe.get("dma_largest_kb"))
    psram_current_kb = _optional_int(probe.get("shadow_psram_current_kb"))
    promotable = (
        probe_ok
        and supported
        and not blocked_no_reference
        and status_after_ok
        and (internal_free_kb is None or internal_free_kb >= 64)
        and (dma_largest_kb is None or dma_largest_kb >= 48)
    )
    ok = status_after_ok and bool(probe)
    return AecLiveTrial(
        ok=ok,
        promotable=promotable,
        probe_ok=probe_ok,
        supported=supported,
        blocked_no_reference=blocked_no_reference,
        probe_error=probe_error,
        internal_free_kb=internal_free_kb,
        dma_largest_kb=dma_largest_kb,
        psram_current_kb=psram_current_kb,
        status_after_ok=status_after_ok,
        recommendation=_recommendation(
            promotable=promotable,
            probe_ok=probe_ok,
            supported=supported,
            blocked_no_reference=blocked_no_reference,
            probe_error=probe_error,
        ),
    )


def format_aec_live_markdown(trial: AecLiveTrial) -> str:
    status = "OK" if trial.ok else "FALHOU"
    return "\n".join(
        [
            "# AEC Live Probe",
            "",
            f"- Status: {status}",
            f"- Promovivel: {'sim' if trial.promotable else 'nao'}",
            f"- Probe OK: {'sim' if trial.probe_ok else 'nao'}",
            f"- Suportado: {'sim' if trial.supported else 'nao'}",
            f"- Sem referencia limpa: {'sim' if trial.blocked_no_reference else 'nao'}",
            f"- Erro: {trial.probe_error}",
            f"- Internal livre: {_fmt_int(trial.internal_free_kb)} KB",
            f"- DMA maior bloco: {_fmt_int(trial.dma_largest_kb)} KB",
            f"- PSRAM atual: {_fmt_int(trial.psram_current_kb)} KB",
            f"- Recomendacao: {trial.recommendation}",
            "",
        ]
    )


def format_aec_live_json(trial: AecLiveTrial) -> str:
    return json.dumps(trial.to_dict(), ensure_ascii=False, indent=2)


def _recommendation(
    *,
    promotable: bool,
    probe_ok: bool,
    supported: bool,
    blocked_no_reference: bool,
    probe_error: str,
) -> str:
    if promotable:
        return "AEC passou no probe; ainda exigir teste longo antes de runtime."
    if blocked_no_reference or not supported:
        return "Nao promover AEC: placa sem referencia limpa de speaker."
    if not probe_ok:
        return f"Nao promover AEC: probe falhou ({probe_error or 'sem detalhe'})."
    return "Nao promover AEC: margem insuficiente ou status pos-probe inconclusivo."


def _post_json_diagnostic(url: str, timeout_s: float) -> dict[str, Any]:
    """POST JSON and accept diagnostic JSON bodies returned with HTTP errors."""

    request = Request(
        url,
        data=b"{}",
        method="POST",
        headers={
            "Content-Type": "application/json",
            "User-Agent": "NoiseBot-AecLive/0.1",
        },
    )
    try:
        with urlopen(request, timeout=timeout_s) as response:
            data = response.read().decode("utf-8")
    except HTTPError as exc:
        try:
            data = exc.read().decode("utf-8")
        except Exception as read_exc:
            raise VoiceAbError(f"{url}: HTTP Error {exc.code}: {exc.reason}") from read_exc
        return _decode_json_payload(url, data)
    except (URLError, TimeoutError, OSError) as exc:
        raise VoiceAbError(f"{url}: {exc}") from exc
    return _decode_json_payload(url, data)


def _decode_json_payload(url: str, data: str) -> dict[str, Any]:
    try:
        payload = json.loads(data)
    except json.JSONDecodeError as exc:
        raise VoiceAbError(f"{url}: resposta nao e JSON") from exc
    if not isinstance(payload, dict):
        raise VoiceAbError(f"{url}: resposta invalida")
    return payload


def _optional_int(value: object) -> int | None:
    if isinstance(value, bool) or value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _fmt_int(value: int | None) -> str:
    return "" if value is None else str(value)


__all__ = [
    "AecLiveTrial",
    "format_aec_live_json",
    "format_aec_live_markdown",
    "run_aec_live_probe",
]
