from __future__ import annotations

import sqlite3
from pathlib import Path

import pytest

from noisebot_server.internal.conversations import (
    ConversationStore,
    ConversationStoreError,
)


def _store(tmp_path: Path) -> ConversationStore:
    return ConversationStore(tmp_path / "conversations.sqlite3")


def _conversation(store: ConversationStore, *, user_id: str = "owner") -> dict:
    return store.create_conversation(
        user_id=user_id,
        title="Estudos de inglês",
        kind="study",
        language_policy="bilingual",
        response_mode="dashboard",
        metadata={"subject": "English"},
    )


def test_store_creates_schema_and_survives_restart(tmp_path: Path) -> None:
    path = tmp_path / "conversations.sqlite3"
    first = ConversationStore(path)
    created = _conversation(first)

    second = ConversationStore(path)
    loaded = second.get_conversation(created["id"], user_id="owner")

    assert second.schema_version == 1
    assert loaded["title"] == "Estudos de inglês"
    assert loaded["metadata"] == {"subject": "English"}


def test_store_rejects_newer_schema_without_modifying_it(tmp_path: Path) -> None:
    path = tmp_path / "conversations.sqlite3"
    store = ConversationStore(path)
    assert store.schema_version == 1
    with sqlite3.connect(path) as conn:
        conn.execute(
            "INSERT INTO schema_migrations(version, applied_at_ms) VALUES (99, 1)"
        )
        conn.commit()

    with pytest.raises(ConversationStoreError, match="mais novo"):
        ConversationStore(path)

    with sqlite3.connect(path) as conn:
        assert conn.execute(
            "SELECT MAX(version) FROM schema_migrations"
        ).fetchone()[0] == 99


def test_conversation_crud_and_user_ownership(tmp_path: Path) -> None:
    store = _store(tmp_path)
    created = _conversation(store)

    updated = store.update_conversation(
        created["id"],
        user_id="owner",
        title="English — conversation practice",
        response_mode="robot",
    )
    assert updated["title"] == "English — conversation practice"
    assert updated["response_mode"] == "robot"

    with pytest.raises(KeyError):
        store.get_conversation(created["id"], user_id="other")
    assert store.delete_conversation(created["id"], user_id="other") is False
    assert store.delete_conversation(created["id"], user_id="owner") is True


def test_active_conversation_is_scoped_by_user_and_cleared_on_delete(
    tmp_path: Path,
) -> None:
    store = _store(tmp_path)
    owner = _conversation(store, user_id="owner")
    guest = _conversation(store, user_id="guest")

    store.set_active_conversation(user_id="owner", conversation_id=owner["id"])
    store.set_active_conversation(user_id="guest", conversation_id=guest["id"])

    assert store.get_active_conversation(user_id="owner")["id"] == owner["id"]
    assert store.get_active_conversation(user_id="guest")["id"] == guest["id"]
    store.delete_conversation(owner["id"], user_id="owner")
    assert store.get_active_conversation(user_id="owner") is None


def test_begin_turn_is_idempotent_and_atomic(tmp_path: Path) -> None:
    store = _store(tmp_path)
    conversation = _conversation(store)

    first = store.begin_turn(
        conversation_id=conversation["id"],
        user_id="owner",
        content="Let's practice the present perfect.",
        origin="dashboard",
        response_mode="dashboard",
        client_request_id="request-1",
        runtime_turn_id=42,
        language="en",
    )
    repeated = store.begin_turn(
        conversation_id=conversation["id"],
        user_id="owner",
        content="texto que não deve ser duplicado",
        origin="dashboard",
        response_mode="dashboard",
        client_request_id="request-1",
    )

    assert repeated["id"] == first["id"]
    assert repeated["messages"] == first["messages"]
    assert first["status"] == "pending"
    assert first["messages"][0]["content"] == "Let's practice the present perfect."
    assert store.list_messages(
        conversation["id"], user_id="owner"
    ) == first["messages"]


def test_complete_turn_preserves_order_and_is_idempotent(tmp_path: Path) -> None:
    store = _store(tmp_path)
    conversation = _conversation(store)
    turn = store.begin_turn(
        conversation_id=conversation["id"],
        user_id="owner",
        content="Where did we stop?",
        origin="dashboard",
        response_mode="dashboard",
    )

    completed = store.complete_turn(
        turn["id"],
        assistant_content="We stopped at the present perfect.",
        route="dashboard",
        language="en",
        message_metadata={"model": "test"},
    )
    repeated = store.complete_turn(
        turn["id"],
        assistant_content="não deve criar nova resposta",
        route="dashboard",
    )

    assert completed["status"] == "complete"
    assert [item["role"] for item in completed["messages"]] == ["user", "assistant"]
    assert [item["sequence"] for item in completed["messages"]] == [1, 2]
    assert repeated["messages"] == completed["messages"]


def test_message_pagination_uses_sequence_cursor(tmp_path: Path) -> None:
    store = _store(tmp_path)
    conversation = _conversation(store)
    for index in range(4):
        turn = store.begin_turn(
            conversation_id=conversation["id"],
            user_id="owner",
            content=f"question {index}",
            origin="dashboard",
            response_mode="dashboard",
        )
        store.complete_turn(
            turn["id"],
            assistant_content=f"answer {index}",
            route="dashboard",
        )

    latest = store.list_messages(
        conversation["id"], user_id="owner", limit=3
    )
    previous = store.list_messages(
        conversation["id"],
        user_id="owner",
        before_sequence=latest[0]["sequence"],
        limit=3,
    )

    assert [item["sequence"] for item in latest] == [6, 7, 8]
    assert [item["sequence"] for item in previous] == [3, 4, 5]


def test_pending_turn_can_fail_without_losing_user_message(tmp_path: Path) -> None:
    store = _store(tmp_path)
    conversation = _conversation(store)
    turn = store.begin_turn(
        conversation_id=conversation["id"],
        user_id="owner",
        content="Please continue.",
        origin="dashboard",
        response_mode="dashboard",
    )

    failed = store.finish_turn_with_error(
        turn["id"], error_code="llm_unavailable"
    )

    assert failed["status"] == "failed"
    assert failed["error_code"] == "llm_unavailable"
    assert len(failed["messages"]) == 1
    assert failed["messages"][0]["content"] == "Please continue."


def test_recover_stale_pending_marks_only_old_turns(tmp_path: Path) -> None:
    store = _store(tmp_path)
    conversation = _conversation(store)
    old_turn = store.begin_turn(
        conversation_id=conversation["id"],
        user_id="owner",
        content="old",
        origin="dashboard",
        response_mode="dashboard",
    )
    new_turn = store.begin_turn(
        conversation_id=conversation["id"],
        user_id="owner",
        content="new",
        origin="dashboard",
        response_mode="dashboard",
    )

    with sqlite3.connect(store.path) as conn:
        conn.execute(
            "UPDATE turns SET created_at_ms = 1000 WHERE id = ?",
            (old_turn["id"],),
        )
        conn.execute(
            "UPDATE turns SET created_at_ms = 9500 WHERE id = ?",
            (new_turn["id"],),
        )
        conn.commit()

    assert store.recover_stale_pending(older_than_ms=2000, now_ms=10_000) == 1
    assert store.get_turn(old_turn["id"])["status"] == "interrupted"
    assert store.get_turn(old_turn["id"])["error_code"] == "server_restart"
    assert store.get_turn(new_turn["id"])["status"] == "pending"


def test_archived_conversation_rejects_new_turns(tmp_path: Path) -> None:
    store = _store(tmp_path)
    conversation = _conversation(store)
    store.update_conversation(
        conversation["id"], user_id="owner", status="archived"
    )

    with pytest.raises(KeyError, match="ativa"):
        store.begin_turn(
            conversation_id=conversation["id"],
            user_id="owner",
            content="hello",
            origin="dashboard",
            response_mode="dashboard",
        )
