"""Tool Gateway — safe pipeline from LLM tool_call to executor.

Flow:
  LLM tool_call dict
    → 1. catalog lookup       (unknown → veto)
    → 2. schema validation    (invalid args → veto)
    → 3. state policy         (incompatible FSM state → veto)
    → 4. risk level           (blocked → veto | confirmation_required → pending)
    → 5. special policies     (analyze_vision without camera → veto)
    → 6. executor call        (low risk, all checks pass)
    → ToolResult (always populated, never raises)

The second LLM step (Phase 7) uses ToolResult.result as context.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

from .catalog import CATALOG, validate_arguments
from .executors import EXECUTOR_MAP

log = logging.getLogger(__name__)


@dataclass
class ToolResult:
    """Structured result from a gateway execution attempt.

    Always has a populated audit_log regardless of outcome.
    The second LLM step uses result (when success=True) as context.
    sandbox=True means the tool was validated but NOT executed (sandbox mode).
    """
    tool_name: str
    success: bool
    result: dict[str, Any] | None
    error: str | None
    audit_log: dict[str, Any]
    vetoed: bool = False
    veto_reason: str | None = None
    requires_confirmation: bool = False
    sandbox: bool = False


def execute_tool_call(
    tool_call: dict[str, Any],
    *,
    current_state: str | None = None,
    vision_available: bool = False,
    adapter: Any | None = None,
    app_state: Any | None = None,
    vision_client: Any | None = None,
    turn_id: int = 0,
    sandbox: bool = False,
) -> ToolResult:
    """Execute a tool call through the full safety pipeline.

    Never raises. All errors are captured inside ToolResult.
    One tool per call — the orchestrator enforces one tool per turn.

    Args:
        tool_call:       {"name": str, "arguments": dict} from parse_llm_json
        current_state:   FSM state name ("IDLE", "THINKING", etc.)
        vision_available: True when VisionClient is active
        adapter:         FirmwareAdapter | None
        app_state:       AppStateStore | None
        vision_client:   VisionClient | None
        turn_id:         turn number for audit logs
    """
    name = str(tool_call.get("name", "")).strip()
    arguments: dict[str, Any] = dict(tool_call.get("arguments") or {})

    audit: dict[str, Any] = {
        "tool_name": name,
        "turn_id": turn_id,
        "timestamp_iso": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "arguments_keys": sorted(arguments.keys()),
    }

    # ── Stage 1: Catalog lookup ────────────────────────────────────────────
    if not name or name not in CATALOG:
        return _veto(audit, name, f"tool desconhecida: '{name}'", turn_id)

    spec = CATALOG[name]

    # ── Stage 2: Schema validation ─────────────────────────────────────────
    errors = validate_arguments(spec, arguments)
    if errors:
        audit["validation_errors"] = errors
        return _veto(
            audit, name,
            "argumentos invalidos: " + "; ".join(errors),
            turn_id,
        )

    # ── Stage 3: State policy ──────────────────────────────────────────────
    if spec.allowed_states is not None and current_state not in spec.allowed_states:
        audit["current_state"] = current_state
        return _veto(
            audit, name,
            f"estado '{current_state}' incompativel com '{name}'",
            turn_id,
        )

    # ── Stage 4: Risk level ────────────────────────────────────────────────
    if spec.risk_level == "blocked":
        return _veto(audit, name, f"tool '{name}' bloqueada por politica", turn_id)

    if spec.risk_level == "confirmation_required":
        audit["outcome"] = "confirmation_pending"
        log.info(
            "ToolGateway turn_id=%d confirmation_required tool=%s", turn_id, name
        )
        return ToolResult(
            tool_name=name,
            success=False,
            result=None,
            error=None,
            audit_log=audit,
            requires_confirmation=True,
        )

    # ── Stage 5: Special per-tool policies ────────────────────────────────
    if name == "analyze_vision" and not vision_available:
        return _veto(
            audit, name,
            "analyze_vision: visao indisponivel no momento",
            turn_id,
        )

    # ── Stage 6: Executor (or sandbox simulation) ─────────────────────────
    if sandbox:
        audit["outcome"] = "sandboxed"
        log.info("ToolGateway turn_id=%d SANDBOX tool=%s args=%s", turn_id, name, sorted(arguments.keys()))
        return ToolResult(
            tool_name=name,
            success=True,
            result={"sandboxed": True, "would_execute": name, "arguments": arguments},
            error=None,
            audit_log=audit,
            sandbox=True,
        )

    executor = EXECUTOR_MAP.get(name)
    if executor is None:
        return _veto(audit, name, f"executor ausente para '{name}'", turn_id)

    exec_context: dict[str, Any] = {
        "adapter": adapter,
        "app_state": app_state,
        "vision_client": vision_client,
        "turn_id": turn_id,
    }

    try:
        result = executor(arguments, exec_context)
        audit["outcome"] = "success"
        log.info("ToolGateway turn_id=%d success tool=%s", turn_id, name)
        return ToolResult(
            tool_name=name,
            success=True,
            result=result,
            error=None,
            audit_log=audit,
        )
    except Exception as exc:
        msg = f"executor falhou: {exc}"
        audit["outcome"] = "error"
        audit["error"] = msg
        log.exception("ToolGateway turn_id=%d error tool=%s: %s", turn_id, name, exc)
        return ToolResult(
            tool_name=name,
            success=False,
            result=None,
            error=msg,
            audit_log=audit,
        )


def _veto(
    audit: dict[str, Any],
    name: str,
    reason: str,
    turn_id: int,
) -> ToolResult:
    audit["outcome"] = "vetoed"
    audit["veto_reason"] = reason
    log.warning("ToolGateway turn_id=%d veto tool=%s reason=%s", turn_id, name, reason)
    return ToolResult(
        tool_name=name,
        success=False,
        result=None,
        error=reason,
        audit_log=audit,
        vetoed=True,
        veto_reason=reason,
    )


__all__ = ["ToolResult", "execute_tool_call"]
