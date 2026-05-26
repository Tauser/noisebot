"""Persistent app-owned state for routine and basic controls.

This module belongs to the local server boundary. It intentionally keeps user
dashboard state out of the ESP32 firmware until a setting has an explicit
firmware command available.
"""
from __future__ import annotations

import json
import os
import secrets
import tempfile
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DEFAULT_BASIC_SETTINGS: dict[str, bool | int] = {
    "volume": 62,
    "display_brightness": 74,
    "led_brightness": 48,
    "silent_mode": False,
    "do_not_disturb": False,
    "night_mode": False,
    "reduce_brightness_at_night": True,
    "confirm_loud_sounds": True,
    "subtle_leds": False,
}

AGENDA_KINDS = {"timer", "alarm", "reminder"}


class AppStateStore:
    """Small JSON-backed store for app-facing routine and settings."""

    def __init__(self, path: str | Path | None = None) -> None:
        self._path = Path(path) if path is not None else _default_state_path()
        self._lock = threading.Lock()
        self._state = self._load()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            items = list(self._state["agenda"]["items"])
            settings = dict(self._state["settings"]["basic"])
        return {
            "routine": {
                "items": items,
                "summary": _routine_summary(items),
            },
            "settings": settings,
        }

    def list_agenda(self) -> dict[str, Any]:
        with self._lock:
            items = list(self._state["agenda"]["items"])
        return {"items": items, "summary": _routine_summary(items)}

    def import_firmware_agenda(self, payload: dict[str, Any]) -> int:
        imported = _items_from_firmware_agenda(payload)
        imported_ids = {item["id"] for item in imported}
        with self._lock:
            current = list(self._state["agenda"]["items"])
            created_at_by_id = {
                str(item.get("id")): str(item.get("created_at", ""))
                for item in current
                if isinstance(item, dict)
            }
            local_items = [
                item for item in current
                if item.get("source") != "firmware" or item.get("id") in imported_ids
            ]
            by_id = {item["id"]: item for item in local_items}
            for item in imported:
                previous_created_at = created_at_by_id.get(item["id"])
                if previous_created_at:
                    item["created_at"] = previous_created_at
                by_id[item["id"]] = item

            local_order = [
                item for item in local_items
                if item.get("source") != "firmware" and item.get("id") not in imported_ids
                and not _matches_imported_item(item, imported)
            ]
            next_items = imported + local_order
            if next_items != current:
                self._state["agenda"]["items"] = next_items
                self._save_locked()
        return len(imported)

    def get_agenda_item(self, item_id: str) -> dict[str, Any] | None:
        with self._lock:
            for item in self._state["agenda"]["items"]:
                if item["id"] == item_id:
                    return dict(item)
        return None

    def create_agenda_item(self, kind: str, payload: dict[str, Any]) -> dict[str, Any]:
        if kind not in AGENDA_KINDS:
            raise ValueError("tipo de agenda inválido")

        item = _agenda_item_from_payload(kind, payload)
        with self._lock:
            self._state["agenda"]["items"].insert(0, item)
            self._save_locked()
        return item

    def update_agenda_item(self, item_id: str, payload: dict[str, Any]) -> dict[str, Any] | None:
        with self._lock:
            for index, current in enumerate(self._state["agenda"]["items"]):
                if current["id"] != item_id:
                    continue
                updated = dict(current)
                if "enabled" in payload:
                    updated["enabled"] = bool(payload["enabled"])
                    updated["status"] = _status_for_item(updated)
                if "title" in payload:
                    updated["title"] = _clean_text(payload["title"], 80) or updated["title"]
                if "detail" in payload:
                    updated["detail"] = _clean_text(payload["detail"], 160)
                if "time" in payload:
                    updated["time"] = _clean_text(payload["time"], 16)
                if "repeat" in payload:
                    updated["repeat"] = _clean_text(payload["repeat"], 40)
                if updated.get("kind") == "alarm" and (
                    "repeat" in payload or "weekdays_mask" in payload
                ):
                    updated["weekdays_mask"] = _repeat_to_weekdays_mask(
                        payload.get("repeat", updated.get("repeat")),
                        payload.get("weekdays_mask"),
                    )
                if updated.get("kind") == "alarm" and ("time" in payload or "repeat" in payload):
                    updated["detail"] = f"{updated.get('repeat', 'diário')}, {updated.get('time', '07:30')}"
                self._state["agenda"]["items"][index] = updated
                self._save_locked()
                return updated
        return None

    def delete_agenda_item(self, item_id: str) -> bool:
        with self._lock:
            items = self._state["agenda"]["items"]
            original_len = len(items)
            self._state["agenda"]["items"] = [item for item in items if item["id"] != item_id]
            changed = len(self._state["agenda"]["items"]) != original_len
            if changed:
                self._save_locked()
            return changed

    def get_basic_settings(self) -> dict[str, bool | int]:
        with self._lock:
            return dict(self._state["settings"]["basic"])

    def update_basic_settings(self, payload: dict[str, Any]) -> dict[str, bool | int]:
        with self._lock:
            settings = dict(self._state["settings"]["basic"])
            for key in ("volume", "display_brightness", "led_brightness"):
                if key in payload:
                    settings[key] = _clamp_int(payload[key], 0, 100)

            for key in (
                "silent_mode",
                "do_not_disturb",
                "night_mode",
                "reduce_brightness_at_night",
                "confirm_loud_sounds",
                "subtle_leds",
            ):
                if key in payload:
                    settings[key] = bool(payload[key])

            self._state["settings"]["basic"] = settings
            self._save_locked()
            return dict(settings)

    def _load(self) -> dict[str, Any]:
        try:
            with self._path.open("r", encoding="utf-8") as handle:
                raw = json.load(handle)
        except FileNotFoundError:
            raw = {}
        except (OSError, json.JSONDecodeError):
            raw = {}

        agenda = raw.get("agenda") if isinstance(raw, dict) else None
        settings = raw.get("settings") if isinstance(raw, dict) else None
        basic = settings.get("basic") if isinstance(settings, dict) else None

        return {
            "version": 1,
            "agenda": {
                "items": _normalize_items(agenda.get("items") if isinstance(agenda, dict) else []),
            },
            "settings": {
                "basic": _normalize_settings(basic if isinstance(basic, dict) else {}),
            },
        }

    def _save_locked(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        payload = json.dumps(self._state, ensure_ascii=False, indent=2, sort_keys=True)
        with tempfile.NamedTemporaryFile(
            "w",
            delete=False,
            dir=str(self._path.parent),
            encoding="utf-8",
        ) as handle:
            handle.write(payload)
            temp_name = handle.name
        os.replace(temp_name, self._path)


def _default_state_path() -> Path:
    configured = os.environ.get("NOISEBOT_APP_STATE_PATH")
    if configured:
        return Path(configured)
    return Path.home() / ".noisebot-server" / "app_state.json"


def _agenda_item_from_payload(kind: str, payload: dict[str, Any]) -> dict[str, Any]:
    title = _clean_text(payload.get("title"), 80)
    if not title:
        title = {
            "timer": "Timer",
            "alarm": "Alarme",
            "reminder": "Lembrete",
        }[kind]

    duration_min = _clamp_int(payload.get("duration_min", 5), 1, 1440)
    item = {
        "id": f"{kind[:3]}_{secrets.token_hex(4)}",
        "kind": kind,
        "title": title,
        "detail": _clean_text(payload.get("detail"), 160),
        "enabled": bool(payload.get("enabled", True)),
        "created_at": _now_iso(),
    }

    if kind == "timer":
        item["duration_min"] = duration_min
        item["detail"] = item["detail"] or f"{duration_min} min"
    elif kind == "alarm":
        item["time"] = _clean_text(payload.get("time"), 16) or "07:30"
        item["repeat"] = _clean_text(payload.get("repeat"), 40) or "diário"
        item["weekdays_mask"] = _repeat_to_weekdays_mask(
            item["repeat"],
            payload.get("weekdays_mask"),
        )
        item["detail"] = item["detail"] or f"{item['repeat']}, {item['time']}"
    else:
        item["duration_min"] = duration_min
        item["time"] = _clean_text(payload.get("time"), 16) or ""
        item["detail"] = item["detail"] or f"em {duration_min} min"

    item["status"] = _status_for_item(item)
    return item


def _normalize_items(raw_items: Any) -> list[dict[str, Any]]:
    if not isinstance(raw_items, list):
        return []
    normalized: list[dict[str, Any]] = []
    for raw in raw_items:
        if not isinstance(raw, dict):
            continue
        kind = str(raw.get("kind", "reminder"))
        if kind not in AGENDA_KINDS:
            kind = "reminder"
        item = {
            "id": _clean_text(raw.get("id"), 32) or f"{kind[:3]}_{secrets.token_hex(4)}",
            "kind": kind,
            "title": _clean_text(raw.get("title"), 80) or kind,
            "detail": _clean_text(raw.get("detail"), 160),
            "enabled": bool(raw.get("enabled", True)),
            "created_at": _clean_text(raw.get("created_at"), 40) or _now_iso(),
        }
        for key in ("time", "repeat"):
            if key in raw:
                item[key] = _clean_text(raw.get(key), 40)
        if raw.get("source") == "firmware":
            item["source"] = "firmware"
        if "firmware_id" in raw:
            item["firmware_id"] = _clamp_int(raw.get("firmware_id"), 0, 255)
        if "weekdays_mask" in raw:
            item["weekdays_mask"] = _clamp_int(raw.get("weekdays_mask"), 0, 127)
        elif kind == "alarm":
            item["weekdays_mask"] = _repeat_to_weekdays_mask(item.get("repeat"), None)
        if kind in {"timer", "reminder"}:
            item["duration_min"] = _clamp_int(raw.get("duration_min", 5), 1, 1440)
        item["status"] = _status_for_item(item)
        normalized.append(item)
    return normalized


def _items_from_firmware_agenda(payload: dict[str, Any]) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []

    for raw in _list_from_payload(payload, "alarms"):
        fw_id = _clamp_int(raw.get("id"), 0, 255)
        hour = _clamp_int(raw.get("hour"), 0, 23)
        minute = _clamp_int(raw.get("minute"), 0, 59)
        weekdays_mask = _clamp_int(raw.get("weekdays_mask"), 0, 127)
        time_text = f"{hour:02d}:{minute:02d}"
        repeat = _weekdays_label(weekdays_mask)
        item = {
            "id": f"fw_alarm_{fw_id}",
            "kind": "alarm",
            "title": _clean_text(raw.get("label"), 80) or "Alarme",
            "detail": f"{repeat}, {time_text}",
            "enabled": bool(raw.get("enabled", True)),
            "status": "ligado" if bool(raw.get("enabled", True)) else "desligado",
            "time": time_text,
            "repeat": repeat,
            "created_at": _now_iso(),
            "source": "firmware",
            "firmware_id": fw_id,
            "weekdays_mask": weekdays_mask,
        }
        items.append(item)

    for raw in _list_from_payload(payload, "timers"):
        fw_id = _clamp_int(raw.get("id"), 0, 255)
        duration_min = _ms_to_min(raw.get("duration_ms"))
        remaining_min = _ms_to_min(raw.get("remaining_ms"))
        items.append({
            "id": f"fw_timer_{fw_id}",
            "kind": "timer",
            "title": _clean_text(raw.get("label"), 80) or "Timer",
            "detail": f"restam {remaining_min} min",
            "enabled": True,
            "status": "ativo",
            "duration_min": duration_min,
            "created_at": _now_iso(),
            "source": "firmware",
            "firmware_id": fw_id,
        })

    for raw in _list_from_payload(payload, "reminders"):
        fw_id = _clamp_int(raw.get("id"), 0, 255)
        duration_min = _ms_to_min(raw.get("delay_ms"))
        remaining_min = _ms_to_min(raw.get("remaining_ms"))
        items.append({
            "id": f"fw_reminder_{fw_id}",
            "kind": "reminder",
            "title": _clean_text(raw.get("text"), 80) or "Lembrete",
            "detail": f"restam {remaining_min} min",
            "enabled": True,
            "status": "pendente",
            "duration_min": duration_min,
            "created_at": _now_iso(),
            "source": "firmware",
            "firmware_id": fw_id,
        })

    return items


def _list_from_payload(payload: dict[str, Any], key: str) -> list[dict[str, Any]]:
    raw = payload.get(key, [])
    if not isinstance(raw, list):
        return []
    return [item for item in raw if isinstance(item, dict)]


def _matches_imported_item(item: dict[str, Any], imported: list[dict[str, Any]]) -> bool:
    for candidate in imported:
        if item.get("kind") != candidate.get("kind"):
            continue
        item_title = _clean_text(item.get("title"), 80).lower()
        candidate_title = _clean_text(candidate.get("title"), 80).lower()
        if item_title != candidate_title:
            continue
        if item.get("kind") == "alarm" and item.get("time") != candidate.get("time"):
            continue
        return True
    return False


def _ms_to_min(value: Any) -> int:
    try:
        ms = int(value)
    except (TypeError, ValueError):
        ms = 60000
    return max(1, min(1440, round(ms / 60000)))


def _weekdays_label(mask: int) -> str:
    if mask == 0:
        return "uma vez"
    if mask == 0x7F:
        return "diário"
    if mask == 0x3E:
        return "dias úteis"
    names = ["dom", "seg", "ter", "qua", "qui", "sex", "sab"]
    return ", ".join(name for index, name in enumerate(names) if mask & (1 << index))


def _repeat_to_weekdays_mask(repeat: Any, explicit_mask: Any) -> int:
    if explicit_mask is not None:
        return _clamp_int(explicit_mask, 0, 127)
    normalized = _clean_text(repeat, 80).lower()
    if normalized in {"diario", "diário", "todo dia", "todos os dias"}:
        return 0x7F
    if normalized in {"dias uteis", "dias úteis", "seg-sex", "segunda a sexta"}:
        return 0x3E
    if normalized in {"fim de semana", "sab-dom", "sáb-dom"}:
        return 0x41
    names = {
        "dom": 0,
        "domingo": 0,
        "seg": 1,
        "segunda": 1,
        "ter": 2,
        "terça": 2,
        "terca": 2,
        "qua": 3,
        "quarta": 3,
        "qui": 4,
        "quinta": 4,
        "sex": 5,
        "sexta": 5,
        "sab": 6,
        "sábado": 6,
        "sabado": 6,
    }
    mask = 0
    for token in normalized.replace(";", ",").replace("/", ",").split(","):
        day = names.get(token.strip())
        if day is not None:
            mask |= 1 << day
    return mask


def _normalize_settings(raw: dict[str, Any]) -> dict[str, bool | int]:
    settings = dict(DEFAULT_BASIC_SETTINGS)
    for key in ("volume", "display_brightness", "led_brightness"):
        if key in raw:
            settings[key] = _clamp_int(raw[key], 0, 100)
    for key in (
        "silent_mode",
        "do_not_disturb",
        "night_mode",
        "reduce_brightness_at_night",
        "confirm_loud_sounds",
        "subtle_leds",
    ):
        if key in raw:
            settings[key] = bool(raw[key])
    return settings


def _routine_summary(items: list[dict[str, Any]]) -> dict[str, Any]:
    enabled = [item for item in items if item.get("enabled", True)]
    next_item = enabled[0] if enabled else None
    return {
        "next": next_item["title"] if next_item else "Nenhum compromisso ativo",
        "timers": sum(1 for item in enabled if item.get("kind") == "timer"),
        "alarms": sum(1 for item in enabled if item.get("kind") == "alarm"),
        "reminders": sum(1 for item in enabled if item.get("kind") == "reminder"),
    }


def _status_for_item(item: dict[str, Any]) -> str:
    if not item.get("enabled", True):
        return "desligado"
    kind = item.get("kind")
    if kind == "timer":
        return "ativo"
    if kind == "alarm":
        return "ligado"
    return "pendente"


def _clean_text(value: Any, max_len: int) -> str:
    if value is None:
        return ""
    return str(value).strip()[:max_len]


def _clamp_int(value: Any, minimum: int, maximum: int) -> int:
    try:
        number = int(value)
    except (TypeError, ValueError):
        number = minimum
    return max(minimum, min(maximum, number))


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
