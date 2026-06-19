"""Transactional SQLite store for persistent NoiseBot conversations.

This module is deliberately synchronous. Callers running in the asyncio event
loop must execute disk operations through ``asyncio.to_thread``.
"""

from __future__ import annotations

import json
import os
import sqlite3
import threading
import time
import uuid
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
_CONVERSATION_KINDS = frozenset({"general", "study"})
_CONVERSATION_STATUSES = frozenset({"active", "archived"})
_LANGUAGE_POLICIES = frozenset({"auto", "pt-BR", "en-US", "bilingual"})
_RESPONSE_MODES = frozenset({"dashboard", "robot"})
_TURN_ORIGINS = frozenset({"dashboard", "voice", "legacy_import"})
_TURN_STATUSES = frozenset({"pending", "complete", "failed", "interrupted"})
_MESSAGE_ROLES = frozenset({"user", "assistant", "tool"})


class ConversationStoreError(RuntimeError):
    """Raised when the persistent conversation store cannot be used safely."""


class ConversationStore:
    """Small SQLite-backed store with short, explicit transactions."""

    def __init__(self, path: str | Path | None = None) -> None:
        self._path = Path(path) if path is not None else _default_db_path()
        self._lock = threading.RLock()
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._migrate()

    @property
    def path(self) -> Path:
        return self._path

    @property
    def schema_version(self) -> int:
        with self._connect() as conn:
            row = conn.execute(
                "SELECT COALESCE(MAX(version), 0) AS version FROM schema_migrations"
            ).fetchone()
        return int(row["version"])

    def create_conversation(
        self,
        *,
        user_id: str,
        title: str,
        kind: str = "general",
        language_policy: str = "pt-BR",
        response_mode: str = "dashboard",
        metadata: dict[str, Any] | None = None,
        conversation_id: str | None = None,
    ) -> dict[str, Any]:
        user_id = _required_text(user_id, "user_id", 64)
        title = _required_text(title, "title", 120)
        kind = _choice(kind, _CONVERSATION_KINDS, "kind")
        language_policy = _choice(
            language_policy, _LANGUAGE_POLICIES, "language_policy"
        )
        response_mode = _choice(response_mode, _RESPONSE_MODES, "response_mode")
        conversation_id = _valid_uuid(conversation_id or str(uuid.uuid4()))
        now = _now_ms()
        metadata_json = _metadata_json(metadata)

        with self._write_transaction() as conn:
            conn.execute(
                """
                INSERT INTO conversations (
                    id, user_id, title, kind, status, language_policy,
                    response_mode, created_at_ms, updated_at_ms,
                    last_message_at_ms, metadata_json
                ) VALUES (?, ?, ?, ?, 'active', ?, ?, ?, ?, NULL, ?)
                """,
                (
                    conversation_id,
                    user_id,
                    title,
                    kind,
                    language_policy,
                    response_mode,
                    now,
                    now,
                    metadata_json,
                ),
            )
        return self.get_conversation(conversation_id, user_id=user_id)

    def get_conversation(
        self,
        conversation_id: str,
        *,
        user_id: str | None = None,
    ) -> dict[str, Any]:
        conversation_id = _valid_uuid(conversation_id)
        params: list[Any] = [conversation_id]
        where = "id = ?"
        if user_id is not None:
            where += " AND user_id = ?"
            params.append(_required_text(user_id, "user_id", 64))
        with self._connect() as conn:
            row = conn.execute(
                f"SELECT * FROM conversations WHERE {where}", params
            ).fetchone()
        if row is None:
            raise KeyError("conversa não encontrada")
        return _conversation_from_row(row)

    def list_conversations(
        self,
        *,
        user_id: str,
        status: str = "active",
        limit: int = 50,
    ) -> list[dict[str, Any]]:
        user_id = _required_text(user_id, "user_id", 64)
        status = _choice(status, _CONVERSATION_STATUSES, "status")
        limit = _bounded_limit(limit, maximum=100)
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT * FROM conversations
                WHERE user_id = ? AND status = ?
                ORDER BY COALESCE(last_message_at_ms, created_at_ms) DESC, id ASC
                LIMIT ?
                """,
                (user_id, status, limit),
            ).fetchall()
        return [_conversation_from_row(row) for row in rows]

    def update_conversation(
        self,
        conversation_id: str,
        *,
        user_id: str,
        title: str | None = None,
        status: str | None = None,
        language_policy: str | None = None,
        response_mode: str | None = None,
        metadata: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        conversation_id = _valid_uuid(conversation_id)
        user_id = _required_text(user_id, "user_id", 64)
        updates: list[str] = []
        values: list[Any] = []
        if title is not None:
            updates.append("title = ?")
            values.append(_required_text(title, "title", 120))
        if status is not None:
            updates.append("status = ?")
            values.append(_choice(status, _CONVERSATION_STATUSES, "status"))
        if language_policy is not None:
            updates.append("language_policy = ?")
            values.append(
                _choice(language_policy, _LANGUAGE_POLICIES, "language_policy")
            )
        if response_mode is not None:
            updates.append("response_mode = ?")
            values.append(_choice(response_mode, _RESPONSE_MODES, "response_mode"))
        if metadata is not None:
            updates.append("metadata_json = ?")
            values.append(_metadata_json(metadata))
        if not updates:
            return self.get_conversation(conversation_id, user_id=user_id)

        updates.append("updated_at_ms = ?")
        values.append(_now_ms())
        values.extend((conversation_id, user_id))
        with self._write_transaction() as conn:
            cursor = conn.execute(
                f"""
                UPDATE conversations SET {", ".join(updates)}
                WHERE id = ? AND user_id = ?
                """,
                values,
            )
            if cursor.rowcount != 1:
                raise KeyError("conversa não encontrada")
        return self.get_conversation(conversation_id, user_id=user_id)

    def delete_conversation(self, conversation_id: str, *, user_id: str) -> bool:
        conversation_id = _valid_uuid(conversation_id)
        user_id = _required_text(user_id, "user_id", 64)
        with self._write_transaction() as conn:
            cursor = conn.execute(
                "DELETE FROM conversations WHERE id = ? AND user_id = ?",
                (conversation_id, user_id),
            )
        return cursor.rowcount == 1

    def set_active_conversation(
        self,
        *,
        user_id: str,
        conversation_id: str,
    ) -> dict[str, Any]:
        conversation = self.get_conversation(conversation_id, user_id=user_id)
        if conversation["status"] != "active":
            raise ValueError("conversa arquivada não pode ficar ativa")
        now = _now_ms()
        with self._write_transaction() as conn:
            conn.execute(
                """
                INSERT INTO active_conversations (user_id, conversation_id, updated_at_ms)
                VALUES (?, ?, ?)
                ON CONFLICT(user_id) DO UPDATE SET
                    conversation_id = excluded.conversation_id,
                    updated_at_ms = excluded.updated_at_ms
                """,
                (user_id, conversation_id, now),
            )
        return conversation

    def get_active_conversation(self, *, user_id: str) -> dict[str, Any] | None:
        user_id = _required_text(user_id, "user_id", 64)
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT c.* FROM active_conversations a
                JOIN conversations c ON c.id = a.conversation_id
                WHERE a.user_id = ? AND c.user_id = ? AND c.status = 'active'
                """,
                (user_id, user_id),
            ).fetchone()
        return _conversation_from_row(row) if row is not None else None

    def begin_turn(
        self,
        *,
        conversation_id: str,
        user_id: str,
        content: str,
        origin: str,
        response_mode: str,
        client_request_id: str | None = None,
        runtime_turn_id: int | None = None,
        language: str | None = None,
        message_metadata: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Create a pending turn and its user message atomically.

        Repeating the same non-empty ``client_request_id`` within a conversation
        returns the already-created turn without inserting another message.
        """
        conversation_id = _valid_uuid(conversation_id)
        user_id = _required_text(user_id, "user_id", 64)
        content = _required_text(content, "content", 100_000)
        origin = _choice(origin, _TURN_ORIGINS, "origin")
        response_mode = _choice(response_mode, _RESPONSE_MODES, "response_mode")
        client_request_id = _optional_text(client_request_id, 128)
        language = _optional_text(language, 16)
        now = _now_ms()

        with self._write_transaction() as conn:
            conversation = conn.execute(
                """
                SELECT id FROM conversations
                WHERE id = ? AND user_id = ? AND status = 'active'
                """,
                (conversation_id, user_id),
            ).fetchone()
            if conversation is None:
                raise KeyError("conversa ativa não encontrada")

            if client_request_id:
                existing = conn.execute(
                    """
                    SELECT * FROM turns
                    WHERE conversation_id = ? AND client_request_id = ?
                    """,
                    (conversation_id, client_request_id),
                ).fetchone()
                if existing is not None:
                    return self._turn_with_messages(conn, str(existing["id"]))

            turn_id = str(uuid.uuid4())
            message_id = str(uuid.uuid4())
            sequence = _next_sequence(conn, conversation_id)
            conn.execute(
                """
                INSERT INTO turns (
                    id, conversation_id, runtime_turn_id, client_request_id,
                    origin, response_mode, route, status, error_code,
                    created_at_ms, completed_at_ms
                ) VALUES (?, ?, ?, ?, ?, ?, NULL, 'pending', NULL, ?, NULL)
                """,
                (
                    turn_id,
                    conversation_id,
                    runtime_turn_id,
                    client_request_id,
                    origin,
                    response_mode,
                    now,
                ),
            )
            conn.execute(
                """
                INSERT INTO messages (
                    id, conversation_id, turn_id, sequence, role, content,
                    language, created_at_ms, metadata_json
                ) VALUES (?, ?, ?, ?, 'user', ?, ?, ?, ?)
                """,
                (
                    message_id,
                    conversation_id,
                    turn_id,
                    sequence,
                    content,
                    language,
                    now,
                    _metadata_json(message_metadata),
                ),
            )
            conn.execute(
                """
                UPDATE conversations
                SET updated_at_ms = ?, last_message_at_ms = ?
                WHERE id = ?
                """,
                (now, now, conversation_id),
            )
            return self._turn_with_messages(conn, turn_id)

    def complete_turn(
        self,
        turn_id: str,
        *,
        assistant_content: str,
        route: str,
        language: str | None = None,
        message_metadata: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        turn_id = _valid_uuid(turn_id)
        assistant_content = _required_text(
            assistant_content, "assistant_content", 100_000
        )
        route = _required_text(route, "route", 40)
        language = _optional_text(language, 16)
        now = _now_ms()

        with self._write_transaction() as conn:
            turn = conn.execute("SELECT * FROM turns WHERE id = ?", (turn_id,)).fetchone()
            if turn is None:
                raise KeyError("turno não encontrado")
            if turn["status"] == "complete":
                return self._turn_with_messages(conn, turn_id)
            if turn["status"] != "pending":
                raise ValueError("somente turno pendente pode ser concluído")

            conversation_id = str(turn["conversation_id"])
            conn.execute(
                """
                INSERT INTO messages (
                    id, conversation_id, turn_id, sequence, role, content,
                    language, created_at_ms, metadata_json
                ) VALUES (?, ?, ?, ?, 'assistant', ?, ?, ?, ?)
                """,
                (
                    str(uuid.uuid4()),
                    conversation_id,
                    turn_id,
                    _next_sequence(conn, conversation_id),
                    assistant_content,
                    language,
                    now,
                    _metadata_json(message_metadata),
                ),
            )
            conn.execute(
                """
                UPDATE turns SET status = 'complete', route = ?,
                    completed_at_ms = ?, error_code = NULL
                WHERE id = ?
                """,
                (route, now, turn_id),
            )
            conn.execute(
                """
                UPDATE conversations
                SET updated_at_ms = ?, last_message_at_ms = ?
                WHERE id = ?
                """,
                (now, now, conversation_id),
            )
            return self._turn_with_messages(conn, turn_id)

    def finish_turn_with_error(
        self,
        turn_id: str,
        *,
        status: str = "failed",
        error_code: str,
    ) -> dict[str, Any]:
        turn_id = _valid_uuid(turn_id)
        status = _choice(status, frozenset({"failed", "interrupted"}), "status")
        error_code = _required_text(error_code, "error_code", 80)
        now = _now_ms()
        with self._write_transaction() as conn:
            cursor = conn.execute(
                """
                UPDATE turns SET status = ?, error_code = ?, completed_at_ms = ?
                WHERE id = ? AND status = 'pending'
                """,
                (status, error_code, now, turn_id),
            )
            if cursor.rowcount != 1:
                row = conn.execute("SELECT id FROM turns WHERE id = ?", (turn_id,)).fetchone()
                if row is None:
                    raise KeyError("turno não encontrado")
                raise ValueError("somente turno pendente pode receber erro")
            return self._turn_with_messages(conn, turn_id)

    def recover_stale_pending(
        self,
        *,
        older_than_ms: int,
        now_ms: int | None = None,
    ) -> int:
        if older_than_ms < 0:
            raise ValueError("older_than_ms inválido")
        now_ms = _now_ms() if now_ms is None else int(now_ms)
        cutoff = now_ms - int(older_than_ms)
        with self._write_transaction() as conn:
            cursor = conn.execute(
                """
                UPDATE turns
                SET status = 'interrupted', error_code = 'server_restart',
                    completed_at_ms = ?
                WHERE status = 'pending' AND created_at_ms <= ?
                """,
                (now_ms, cutoff),
            )
        return int(cursor.rowcount)

    def get_turn(self, turn_id: str) -> dict[str, Any]:
        turn_id = _valid_uuid(turn_id)
        with self._connect() as conn:
            row = conn.execute("SELECT id FROM turns WHERE id = ?", (turn_id,)).fetchone()
            if row is None:
                raise KeyError("turno não encontrado")
            return self._turn_with_messages(conn, turn_id)

    def list_messages(
        self,
        conversation_id: str,
        *,
        user_id: str,
        before_sequence: int | None = None,
        limit: int = 50,
    ) -> list[dict[str, Any]]:
        conversation_id = _valid_uuid(conversation_id)
        user_id = _required_text(user_id, "user_id", 64)
        limit = _bounded_limit(limit, maximum=200)
        with self._connect() as conn:
            owner = conn.execute(
                "SELECT id FROM conversations WHERE id = ? AND user_id = ?",
                (conversation_id, user_id),
            ).fetchone()
            if owner is None:
                raise KeyError("conversa não encontrada")
            params: list[Any] = [conversation_id]
            where = "conversation_id = ?"
            if before_sequence is not None:
                where += " AND sequence < ?"
                params.append(max(1, int(before_sequence)))
            params.append(limit)
            rows = conn.execute(
                f"""
                SELECT * FROM messages
                WHERE {where}
                ORDER BY sequence DESC
                LIMIT ?
                """,
                params,
            ).fetchall()
        return [_message_from_row(row) for row in reversed(rows)]

    def _turn_with_messages(
        self,
        conn: sqlite3.Connection,
        turn_id: str,
    ) -> dict[str, Any]:
        turn = conn.execute("SELECT * FROM turns WHERE id = ?", (turn_id,)).fetchone()
        if turn is None:
            raise KeyError("turno não encontrado")
        messages = conn.execute(
            "SELECT * FROM messages WHERE turn_id = ? ORDER BY sequence ASC",
            (turn_id,),
        ).fetchall()
        result = _turn_from_row(turn)
        result["messages"] = [_message_from_row(row) for row in messages]
        return result

    def _migrate(self) -> None:
        with self._lock:
            try:
                with self._connect() as conn:
                    conn.execute(
                        """
                        CREATE TABLE IF NOT EXISTS schema_migrations (
                            version INTEGER PRIMARY KEY,
                            applied_at_ms INTEGER NOT NULL
                        )
                        """
                    )
                    row = conn.execute(
                        "SELECT COALESCE(MAX(version), 0) AS version FROM schema_migrations"
                    ).fetchone()
                    version = int(row["version"])
                    if version > SCHEMA_VERSION:
                        raise ConversationStoreError(
                            f"schema de conversas {version} é mais novo que "
                            f"o suportado ({SCHEMA_VERSION})"
                        )
                    if version < 1:
                        _apply_schema_v1(conn)
                        conn.execute(
                            """
                            INSERT INTO schema_migrations (version, applied_at_ms)
                            VALUES (1, ?)
                            """,
                            (_now_ms(),),
                        )
                    conn.commit()
            except sqlite3.DatabaseError as exc:
                raise ConversationStoreError(
                    f"banco de conversas inválido: {self._path}"
                ) from exc

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self._path, timeout=5.0)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("PRAGMA synchronous = FULL")
        conn.execute("PRAGMA busy_timeout = 5000")
        return conn

    def _write_transaction(self):
        return _WriteTransaction(self)


class _WriteTransaction:
    def __init__(self, store: ConversationStore) -> None:
        self._store = store
        self._conn: sqlite3.Connection | None = None

    def __enter__(self) -> sqlite3.Connection:
        self._store._lock.acquire()
        self._conn = self._store._connect()
        self._conn.execute("BEGIN IMMEDIATE")
        return self._conn

    def __exit__(self, exc_type, exc, tb) -> None:
        assert self._conn is not None
        try:
            if exc_type is None:
                self._conn.commit()
            else:
                self._conn.rollback()
        finally:
            self._conn.close()
            self._store._lock.release()


def _apply_schema_v1(conn: sqlite3.Connection) -> None:
    conn.executescript(
        """
        CREATE TABLE conversations (
            id TEXT PRIMARY KEY,
            user_id TEXT NOT NULL,
            title TEXT NOT NULL,
            kind TEXT NOT NULL CHECK (kind IN ('general', 'study')),
            status TEXT NOT NULL CHECK (status IN ('active', 'archived')),
            language_policy TEXT NOT NULL
                CHECK (language_policy IN ('auto', 'pt-BR', 'en-US', 'bilingual')),
            response_mode TEXT NOT NULL
                CHECK (response_mode IN ('dashboard', 'robot')),
            created_at_ms INTEGER NOT NULL,
            updated_at_ms INTEGER NOT NULL,
            last_message_at_ms INTEGER,
            metadata_json TEXT NOT NULL DEFAULT '{}'
        );

        CREATE INDEX conversations_user_status_updated_idx
            ON conversations (
                user_id, status,
                COALESCE(last_message_at_ms, created_at_ms) DESC
            );

        CREATE TABLE turns (
            id TEXT PRIMARY KEY,
            conversation_id TEXT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
            runtime_turn_id INTEGER,
            client_request_id TEXT,
            origin TEXT NOT NULL
                CHECK (origin IN ('dashboard', 'voice', 'legacy_import')),
            response_mode TEXT NOT NULL
                CHECK (response_mode IN ('dashboard', 'robot')),
            route TEXT,
            status TEXT NOT NULL
                CHECK (status IN ('pending', 'complete', 'failed', 'interrupted')),
            error_code TEXT,
            created_at_ms INTEGER NOT NULL,
            completed_at_ms INTEGER,
            UNIQUE (conversation_id, client_request_id)
        );

        CREATE INDEX turns_conversation_created_idx
            ON turns (conversation_id, created_at_ms ASC);
        CREATE INDEX turns_pending_created_idx
            ON turns (status, created_at_ms)
            WHERE status = 'pending';

        CREATE TABLE messages (
            id TEXT PRIMARY KEY,
            conversation_id TEXT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
            turn_id TEXT NOT NULL REFERENCES turns(id) ON DELETE CASCADE,
            sequence INTEGER NOT NULL,
            role TEXT NOT NULL CHECK (role IN ('user', 'assistant', 'tool')),
            content TEXT NOT NULL,
            language TEXT,
            created_at_ms INTEGER NOT NULL,
            metadata_json TEXT NOT NULL DEFAULT '{}',
            UNIQUE (conversation_id, sequence)
        );

        CREATE INDEX messages_conversation_sequence_idx
            ON messages (conversation_id, sequence DESC);

        CREATE TABLE active_conversations (
            user_id TEXT PRIMARY KEY,
            conversation_id TEXT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
            updated_at_ms INTEGER NOT NULL
        );
        """
    )


def _next_sequence(conn: sqlite3.Connection, conversation_id: str) -> int:
    row = conn.execute(
        """
        SELECT COALESCE(MAX(sequence), 0) + 1 AS next_sequence
        FROM messages WHERE conversation_id = ?
        """,
        (conversation_id,),
    ).fetchone()
    return int(row["next_sequence"])


def _conversation_from_row(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": str(row["id"]),
        "user_id": str(row["user_id"]),
        "title": str(row["title"]),
        "kind": str(row["kind"]),
        "status": str(row["status"]),
        "language_policy": str(row["language_policy"]),
        "response_mode": str(row["response_mode"]),
        "created_at_ms": int(row["created_at_ms"]),
        "updated_at_ms": int(row["updated_at_ms"]),
        "last_message_at_ms": (
            int(row["last_message_at_ms"])
            if row["last_message_at_ms"] is not None
            else None
        ),
        "metadata": _parse_metadata(row["metadata_json"]),
    }


def _turn_from_row(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": str(row["id"]),
        "conversation_id": str(row["conversation_id"]),
        "runtime_turn_id": (
            int(row["runtime_turn_id"])
            if row["runtime_turn_id"] is not None
            else None
        ),
        "client_request_id": row["client_request_id"],
        "origin": str(row["origin"]),
        "response_mode": str(row["response_mode"]),
        "route": row["route"],
        "status": str(row["status"]),
        "error_code": row["error_code"],
        "created_at_ms": int(row["created_at_ms"]),
        "completed_at_ms": (
            int(row["completed_at_ms"])
            if row["completed_at_ms"] is not None
            else None
        ),
    }


def _message_from_row(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": str(row["id"]),
        "conversation_id": str(row["conversation_id"]),
        "turn_id": str(row["turn_id"]),
        "sequence": int(row["sequence"]),
        "role": str(row["role"]),
        "content": str(row["content"]),
        "language": row["language"],
        "created_at_ms": int(row["created_at_ms"]),
        "metadata": _parse_metadata(row["metadata_json"]),
    }


def _default_db_path() -> Path:
    configured = os.environ.get("NOISEBOT_CONVERSATIONS_DB_PATH", "").strip()
    if configured:
        return Path(configured)
    return Path.home() / ".noisebot-server" / "conversations.sqlite3"


def _metadata_json(value: dict[str, Any] | None) -> str:
    if value is None:
        return "{}"
    if not isinstance(value, dict):
        raise ValueError("metadata deve ser objeto")
    encoded = json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
    if len(encoded.encode("utf-8")) > 64 * 1024:
        raise ValueError("metadata excede 64 KB")
    return encoded


def _parse_metadata(value: str) -> dict[str, Any]:
    try:
        parsed = json.loads(value)
    except (TypeError, json.JSONDecodeError):
        return {}
    return parsed if isinstance(parsed, dict) else {}


def _required_text(value: Any, field: str, limit: int) -> str:
    text = str(value or "").strip()
    if not text:
        raise ValueError(f"{field} obrigatório")
    if len(text) > limit:
        raise ValueError(f"{field} excede {limit} caracteres")
    return text


def _optional_text(value: Any, limit: int) -> str | None:
    if value is None:
        return None
    text = str(value).strip()
    if not text:
        return None
    if len(text) > limit:
        raise ValueError(f"texto excede {limit} caracteres")
    return text


def _choice(value: Any, allowed: frozenset[str], field: str) -> str:
    text = str(value or "").strip()
    if text not in allowed:
        raise ValueError(f"{field} inválido")
    return text


def _valid_uuid(value: str) -> str:
    try:
        return str(uuid.UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise ValueError("id inválido") from exc


def _bounded_limit(value: int, *, maximum: int) -> int:
    limit = int(value)
    if limit < 1 or limit > maximum:
        raise ValueError(f"limit deve estar entre 1 e {maximum}")
    return limit


def _now_ms() -> int:
    return time.time_ns() // 1_000_000


__all__ = ["ConversationStore", "ConversationStoreError", "SCHEMA_VERSION"]
