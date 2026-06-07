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
            method="GET",
            path="/api/logs",
            purpose="Recent sanitized server log lines for the Dev console.",
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
            implemented=True,
        ),
        AppEndpoint(
            domain="vision",
            method="GET",
            path="/api/vision/observe",
            purpose="Current camera observation through server vision service.",
            implemented=True,
        ),
        AppEndpoint(
            domain="vision",
            method="GET",
            path="/api/vision/analyze",
            purpose="Scene and face analysis through server vision service.",
            implemented=True,
        ),
        AppEndpoint(
            domain="vision",
            method="GET",
            path="/api/vision/snapshot",
            purpose="Latest camera snapshot proxied by server.",
            implemented=True,
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
            implemented=True,
        ),
        AppEndpoint(
            domain="agenda",
            method="POST",
            path="/api/agenda/timers",
            purpose="Create a timer visible to the app.",
            auth_required=True,
            implemented=True,
        ),
        AppEndpoint(
            domain="agenda",
            method="POST",
            path="/api/agenda/alarms",
            purpose="Create an alarm visible to the app.",
            auth_required=True,
            implemented=True,
        ),
        AppEndpoint(
            domain="agenda",
            method="POST",
            path="/api/agenda/reminders",
            purpose="Create a reminder visible to the app.",
            auth_required=True,
            implemented=True,
        ),
        AppEndpoint(
            domain="settings",
            method="GET",
            path="/api/settings/basic",
            purpose="User-facing volume, LEDs and comfort settings.",
            implemented=True,
        ),
        AppEndpoint(
            domain="settings",
            method="PUT",
            path="/api/settings/basic",
            purpose="Persist user-facing volume, LEDs and comfort settings.",
            auth_required=True,
            implemented=True,
        ),
        AppEndpoint(
            domain="settings",
            method="GET",
            path="/api/settings/advanced",
            purpose="User-facing connectivity, maintenance and device preferences.",
            implemented=True,
        ),
        AppEndpoint(
            domain="settings",
            method="PUT",
            path="/api/settings/advanced",
            purpose="Persist connectivity, maintenance and device preferences.",
            auth_required=True,
            implemented=True,
        ),
        AppEndpoint(
            domain="profile",
            method="GET",
            path="/api/profile",
            purpose="Assistant identity, language, voice and tone.",
            implemented=True,
        ),
        AppEndpoint(
            domain="profile",
            method="PUT",
            path="/api/profile",
            purpose="Persist assistant identity, language, voice and tone.",
            auth_required=True,
            implemented=True,
        ),
        AppEndpoint(
            domain="profile",
            method="POST",
            path="/api/profile/test-voice",
            purpose="Ask the local server to synthesize a voice test.",
            auth_required=True,
            implemented=True,
        ),
    )


def implemented_endpoints() -> tuple[AppEndpoint, ...]:
    """Return endpoints already served by the current bridge-backed runtime."""
    return tuple(endpoint for endpoint in default_app_contract() if endpoint.implemented)
