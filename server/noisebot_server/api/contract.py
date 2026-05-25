"""App-facing API contract.

The app must talk to the local server boundary, not directly to ESP32 firmware
endpoints. Firmware REST can still exist for diagnostics, but app workflows
should be routed through server-owned APIs.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class AppEndpoint:
    """One endpoint visible to the external app/dashboard."""

    domain: str
    method: str
    path: str
    purpose: str
    auth_required: bool = False
    implemented: bool = False


def default_app_contract() -> tuple[AppEndpoint, ...]:
    """Return the stable app-to-server API contract.

    ``implemented=False`` marks endpoints reserved by architecture but not yet
    served by the current bridge-backed runtime.
    """
    return (
        AppEndpoint(
            domain="ops",
            method="GET",
            path="/health",
            purpose="Server health and uptime.",
            implemented=True,
        ),
        AppEndpoint(
            domain="ops",
            method="GET",
            path="/ai/status",
            purpose="Agent, provider and firmware connection status.",
            implemented=True,
        ),
        AppEndpoint(
            domain="ops",
            method="GET",
            path="/ai/metrics",
            purpose="Agent latency and turn metrics.",
            implemented=True,
        ),
        AppEndpoint(
            domain="ops",
            method="GET",
            path="/ai/errors",
            purpose="Recent sanitized runtime errors.",
            implemented=True,
        ),
        AppEndpoint(
            domain="ops",
            method="GET",
            path="/ai/config",
            purpose="Safe runtime configuration snapshot.",
            implemented=True,
        ),
        AppEndpoint(
            domain="ops",
            method="POST",
            path="/ai/config",
            purpose="Apply safe runtime configuration changes.",
            auth_required=True,
            implemented=True,
        ),
        AppEndpoint(
            domain="ops",
            method="POST",
            path="/debug/transcript",
            purpose="Inject a transcript into the agent for testing.",
            auth_required=True,
            implemented=True,
        ),
        AppEndpoint(
            domain="vision",
            method="GET",
            path="/api/vision/status",
            purpose="Server-owned vision status.",
        ),
        AppEndpoint(
            domain="vision",
            method="GET",
            path="/api/vision/observe",
            purpose="Current camera observation through server vision service.",
        ),
        AppEndpoint(
            domain="vision",
            method="GET",
            path="/api/vision/snapshot",
            purpose="Latest camera snapshot proxied by server.",
        ),
        AppEndpoint(
            domain="agent",
            method="POST",
            path="/api/agent/turn",
            purpose="Start an app-originated conversation turn.",
            auth_required=True,
        ),
        AppEndpoint(
            domain="device",
            method="GET",
            path="/api/device/status",
            purpose="Firmware/device status normalized by server.",
        ),
        AppEndpoint(
            domain="agenda",
            method="GET",
            path="/api/agenda/items",
            purpose="Timers, reminders and alarms visible to the app.",
        ),
    )


def implemented_endpoints() -> tuple[AppEndpoint, ...]:
    """Return endpoints already served by the current bridge-backed runtime."""
    return tuple(endpoint for endpoint in default_app_contract() if endpoint.implemented)
