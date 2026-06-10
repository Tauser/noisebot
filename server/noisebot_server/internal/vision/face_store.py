"""Face reference photo store.

One JPEG per enrolled user, stored at:
    ~/.noisebot-server/faces/{user_id}.jpg
"""
from __future__ import annotations

import os
import threading
from pathlib import Path
from typing import Any


class FaceStore:
    def __init__(self, path: str | Path | None = None) -> None:
        self._dir = Path(path) if path is not None else _default_faces_dir()
        self._lock = threading.Lock()

    # ── Write ──────────────────────────────────────────────────────────────

    def save(self, user_id: str, display_name: str, jpeg_bytes: bytes) -> None:
        if not _valid_id(user_id):
            raise ValueError(f"user_id inválido: {user_id!r}")
        if not jpeg_bytes:
            raise ValueError("jpeg_bytes vazio")
        with self._lock:
            self._dir.mkdir(parents=True, exist_ok=True)
            self._dir.joinpath(f"{user_id}.jpg").write_bytes(jpeg_bytes)
            meta = self._load_meta()
            meta[user_id] = {"display_name": display_name[:60]}
            self._save_meta(meta)

    def delete(self, user_id: str) -> bool:
        with self._lock:
            jpg = self._dir / f"{user_id}.jpg"
            if not jpg.exists():
                return False
            jpg.unlink(missing_ok=True)
            meta = self._load_meta()
            meta.pop(user_id, None)
            self._save_meta(meta)
        return True

    # ── Read ───────────────────────────────────────────────────────────────

    def load(self, user_id: str) -> bytes | None:
        jpg = self._dir / f"{user_id}.jpg"
        try:
            return jpg.read_bytes()
        except FileNotFoundError:
            return None

    def list_users(self) -> list[dict[str, Any]]:
        with self._lock:
            meta = self._load_meta()
        result = []
        for uid, info in meta.items():
            jpg = self._dir / f"{uid}.jpg"
            result.append({
                "user_id": uid,
                "display_name": info.get("display_name", uid),
                "enrolled": jpg.exists(),
            })
        return result

    def enrolled_ids(self) -> list[str]:
        with self._lock:
            meta = self._load_meta()
        return [uid for uid in meta if (self._dir / f"{uid}.jpg").exists()]

    def load_all(self) -> dict[str, bytes]:
        ids = self.enrolled_ids()
        result: dict[str, bytes] = {}
        for uid in ids:
            data = self.load(uid)
            if data:
                result[uid] = data
        return result

    # ── Internal ───────────────────────────────────────────────────────────

    def _meta_path(self) -> Path:
        return self._dir / "meta.json"

    def _load_meta(self) -> dict[str, Any]:
        import json
        try:
            return json.loads(self._meta_path().read_text("utf-8"))
        except (FileNotFoundError, ValueError):
            return {}

    def _save_meta(self, meta: dict[str, Any]) -> None:
        import json
        import tempfile
        payload = json.dumps(meta, ensure_ascii=False, indent=2, sort_keys=True)
        with tempfile.NamedTemporaryFile(
            "w", delete=False, dir=str(self._dir), encoding="utf-8"
        ) as fh:
            fh.write(payload)
            tmp = fh.name
        os.replace(tmp, self._meta_path())


def _default_faces_dir() -> Path:
    configured = os.environ.get("NOISEBOT_FACES_DIR")
    if configured:
        return Path(configured)
    return Path.home() / ".noisebot-server" / "faces"


def _valid_id(uid: str) -> bool:
    if not uid or len(uid) > 64:
        return False
    return all(c.isalnum() or c in "-_." for c in uid)


__all__ = ["FaceStore"]
