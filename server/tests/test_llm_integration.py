"""Integration tests for the full LLM pipeline.

These tests exercise the complete path from FinalTranscript through the
Orchestrator, LLM provider, tool gateway, and second-step loop.

Design:
- MockLLMProvider returns predefined JSON responses in sequence.
- Tests are synchronous, using asyncio.run() to keep pytest config minimal.
- The orchestrator runs in a background task; we collect bus events until
  SpeechDone, then shutdown.
- Text chosen for LLM path: personal/conversational questions that don't
  match any local intent pattern.
"""
from __future__ import annotations

import asyncio
import importlib
from collections.abc import AsyncIterator
from typing import Any


# ---------------------------------------------------------------------------
# Test infrastructure
# ---------------------------------------------------------------------------

class MockLLMProvider:
    """Yields chars from predefined responses in sequence.

    Call N returns self._responses[N], or a neutral fallback if exhausted.
    """
    _provider_name = "mock"
    _model = "mock-test"

    def __init__(self, *responses: str) -> None:
        self._responses = list(responses)
        self._call_index = 0
        self.call_contexts: list[dict] = []

    async def generate_stream(self, text: str, context: dict) -> AsyncIterator[str]:
        self.call_contexts.append(context)
        if self._call_index < len(self._responses):
            response = self._responses[self._call_index]
        else:
            response = '{"expression_id":"neutral","reply":"...","tool_call":null}'
        self._call_index += 1
        await asyncio.sleep(0)  # yield to event loop
        for char in response:
            yield char


class MockAppState:
    def __init__(self) -> None:
        self.items: list[dict] = []
        self.facts: list[dict] = []

    def create_agenda_item(self, kind: str, payload: dict) -> dict:
        item: dict = {"id": f"{kind}_{len(self.items) + 1}", "kind": kind, **payload}
        self.items.append(item)
        return item

    def list_agenda(self) -> dict:
        return {"items": list(self.items), "summary": f"{len(self.items)} item(s)"}

    def add_fact(self, text: str) -> dict:
        fact: dict = {"id": f"fact_{len(self.facts) + 1}", "text": text, "created_at": "2026-06-09T00:00:00"}
        self.facts.append(fact)
        return fact

    def remove_fact(self, fact_id: str) -> bool:
        original = len(self.facts)
        self.facts = [f for f in self.facts if f["id"] != fact_id]
        return len(self.facts) != original

    def list_facts(self) -> list:
        return list(self.facts)


async def _do_turn(
    llm_provider: MockLLMProvider,
    text: str,
    app_state: Any = None,
    timeout: float = 4.0,
) -> list[Any]:
    """Run one orchestrator turn, return all bus events up to SpeechDone."""
    bus_mod = importlib.import_module("noisebot_server.internal.agent.runtime")
    orch_mod = importlib.import_module("noisebot_server.internal.agent.orchestrator")

    bus = bus_mod.EventBus()
    orch = orch_mod.Orchestrator(
        bus=bus,
        llm_provider=llm_provider,
        app_state_store=app_state,
    )

    sub = bus.subscribe(maxsize=500)
    loop_task = asyncio.create_task(orch.run())

    await bus.publish(bus_mod.FinalTranscript(
        turn_id=1,
        text=text,
        quality=bus_mod.TranscriptQuality.GOOD,
    ))

    collected: list[Any] = []

    async def _collect() -> None:
        while True:
            ev = await sub.get()
            collected.append(ev)
            if isinstance(ev, bus_mod.SpeechDone):
                break

    try:
        await asyncio.wait_for(_collect(), timeout=timeout)
    except asyncio.TimeoutError:
        pass

    await orch.shutdown()
    try:
        await asyncio.wait_for(loop_task, timeout=1.0)
    except (asyncio.CancelledError, asyncio.TimeoutError, Exception):
        loop_task.cancel()

    return collected


def _find(events: list[Any], cls: type) -> list[Any]:
    return [e for e in events if type(e).__name__ == cls.__name__]


def _first(events: list[Any], cls: type) -> Any | None:
    found = _find(events, cls)
    return found[0] if found else None


# ---------------------------------------------------------------------------
# Scenario 1 — Happy path: simple LLM reply
# ---------------------------------------------------------------------------

def test_simple_llm_reply_emits_llm_reply_complete() -> None:
    """FinalTranscript → LLM reply → LlmReplyComplete published."""
    llm = MockLLMProvider(
        '{"expression_id":"happy","reply":"Boa pergunta! Prefiro jazz.","tool_call":null}'
    )

    events = asyncio.run(_do_turn(llm, "você prefere música clássica ou jazz?"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    assert complete is not None, "LlmReplyComplete não foi emitido"
    assert "jazz" in complete.reply


def test_simple_llm_reply_emits_sentence_ready() -> None:
    """SentenceReady deve ser emitido com o texto da resposta."""
    llm = MockLLMProvider(
        '{"expression_id":"neutral","reply":"Gosto muito de astronomia.","tool_call":null}'
    )

    events = asyncio.run(_do_turn(llm, "qual é o seu assunto favorito?"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    sentences = _find(events, runtime.SentenceReady)
    assert len(sentences) >= 1
    all_text = " ".join(s.sentence for s in sentences)
    assert "astronomia" in all_text


def test_simple_llm_reply_emits_speech_done() -> None:
    """SpeechDone deve ser emitido ao final do turno."""
    llm = MockLLMProvider(
        '{"expression_id":"curious","reply":"Não sei, mas adoraria descobrir!","tool_call":null}'
    )

    events = asyncio.run(_do_turn(llm, "você sonha enquanto dorme?"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    assert _first(events, runtime.SpeechDone) is not None


def test_llm_reply_expression_id_translated_correctly() -> None:
    """expression_id semântico 'happy' → int 1 em LlmReplyComplete."""
    llm = MockLLMProvider(
        '{"expression_id":"happy","reply":"Fico feliz com isso!","tool_call":null}'
    )

    events = asyncio.run(_do_turn(llm, "o que você acha da filosofia estoica?"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    assert complete is not None
    assert complete.expression_id == 1  # happy → 1


def test_llm_reply_null_expression_id_propagates() -> None:
    """expression_id ausente no JSON → None em LlmReplyComplete."""
    llm = MockLLMProvider(
        '{"reply":"Resposta sem expressao.","tool_call":null}'
    )

    events = asyncio.run(_do_turn(llm, "me fale algo interessante."))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    assert complete is not None
    assert complete.expression_id is None


# ---------------------------------------------------------------------------
# Scenario 2 — Tool call + second step
# ---------------------------------------------------------------------------

def test_tool_call_triggers_second_llm_step() -> None:
    """Step 1 com tool_call → step 2 é chamado (call_index >= 2)."""
    llm = MockLLMProvider(
        '{"expression_id":"curious","reply":"Claro, criando!","tool_call":{"name":"set_expression","arguments":{"expression_id":"happy"}}}',
        '{"expression_id":"happy","reply":"Expressei felicidade!","tool_call":null}',
    )

    asyncio.run(_do_turn(llm, "como você abordaria um problema matemático difícil?"))

    assert llm._call_index >= 2, "Segundo passo LLM não foi chamado"


def test_tool_call_second_step_reply_is_final() -> None:
    """Resposta do segundo passo substitui a do primeiro em LlmReplyComplete."""
    llm = MockLLMProvider(
        '{"expression_id":"curious","reply":"Vou criar o timer.","tool_call":{"name":"create_timer","arguments":{"duration_s":300,"label":"Estudos"}}}',
        '{"expression_id":"happy","reply":"Timer de 5 minutos criado com sucesso!","tool_call":null}',
    )
    app_state = MockAppState()

    events = asyncio.run(_do_turn(llm, "de que forma você aprende coisas novas?", app_state=app_state))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    assert complete is not None
    assert "5 minutos" in complete.reply or "criado" in complete.reply


def test_tool_call_second_step_receives_tool_result_context() -> None:
    """O contexto do segundo passo deve incluir tool_call_result."""
    llm = MockLLMProvider(
        '{"expression_id":"neutral","reply":"Ok.","tool_call":{"name":"set_expression","arguments":{"expression_id":"curious"}}}',
        '{"expression_id":"curious","reply":"Fiz a expressão!","tool_call":null}',
    )

    asyncio.run(_do_turn(llm, "por qual razão você escolheria aprender astronomia?"))

    assert llm._call_index >= 2
    second_context = llm.call_contexts[1] if len(llm.call_contexts) > 1 else {}
    assert "tool_call_result" in second_context


def test_tool_call_vetoed_second_step_explains() -> None:
    """Tool vetada → segundo passo recebe veto_reason no contexto."""
    # analyze_vision será vetada porque vision_available=False (sem config)
    llm = MockLLMProvider(
        '{"expression_id":"curious","reply":"Vou verificar.","tool_call":{"name":"analyze_vision","arguments":{}}}',
        '{"expression_id":"sad","reply":"Desculpe, minha câmera não está disponível agora.","tool_call":null}',
    )

    events = asyncio.run(_do_turn(llm, "qual é o seu conceito preferido de beleza?"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    assert complete is not None

    # Segundo passo deve ter sido chamado
    assert llm._call_index >= 2
    second_context = llm.call_contexts[1] if len(llm.call_contexts) > 1 else {}
    tcr = second_context.get("tool_call_result", {})
    assert tcr.get("vetoed") is True


def test_tool_second_step_empty_reply_falls_back_to_first() -> None:
    """Se segundo passo retorna reply vazio, usa o reply do primeiro passo."""
    llm = MockLLMProvider(
        '{"expression_id":"happy","reply":"Aqui está!","tool_call":{"name":"set_expression","arguments":{"expression_id":"happy"}}}',
        '{"expression_id":"neutral","reply":"","tool_call":null}',  # reply vazio
    )

    events = asyncio.run(_do_turn(llm, "qual seria seu plano para um fim de semana perfeito?"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    assert complete is not None
    # Deve cair de volta para o reply do primeiro passo
    assert complete.reply.strip() != ""


# ---------------------------------------------------------------------------
# Scenario 3 — JSON resilience
# ---------------------------------------------------------------------------

def test_bad_json_corrective_retry_succeeds() -> None:
    """JSON inválido no primeiro stream → corrective retry → resposta válida."""
    llm = MockLLMProvider(
        "Desculpe, não entendi bem.",  # não é JSON → parse falha
        '{"expression_id":"neutral","reply":"Peço desculpas, pode repetir?","tool_call":null}',  # corrective
    )

    events = asyncio.run(_do_turn(llm, "qual é a velocidade da luz?"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    # Deve ter resposta — do corrective ou do fallback
    assert complete is not None
    assert complete.reply.strip() != ""


def test_bad_json_both_fail_uses_fallback_text() -> None:
    """JSON inválido em ambas as tentativas → fallback extrai texto direto."""
    llm = MockLLMProvider(
        "Esta é uma resposta sem JSON algum.",
        "Ainda sem JSON.",  # corrective também falha
    )

    events = asyncio.run(_do_turn(llm, "me fale sobre a lua"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    # Fallback deve extrair texto ou retornar empty graciosamente
    # O turn deve concluir sem crash (SpeechDone ou apenas sem LlmReplyComplete)
    assert runtime.SpeechDone in [type(e) for e in events] or complete is not None


def test_truncated_json_still_produces_reply() -> None:
    """JSON parcialmente formado (tipo stream cortado) usa recover_llm_reply_text."""
    llm = MockLLMProvider(
        '{"expression_id":"neutral","reply":"Fragmento de resposta'  # cortado
    )

    events = asyncio.run(_do_turn(llm, "você consegue me ajudar?"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    # Não deve travar — deve produzir algum resultado ou encerrar o turno
    speech_done = _first(events, runtime.SpeechDone)
    assert speech_done is not None


# ---------------------------------------------------------------------------
# Scenario 4 — Turn payload contains turn context
# ---------------------------------------------------------------------------

def test_llm_receives_turn_payload_in_context() -> None:
    """O contexto passado ao LLM deve conter turn_payload estruturado."""
    llm = MockLLMProvider(
        '{"expression_id":"neutral","reply":"Ok.","tool_call":null}'
    )

    asyncio.run(_do_turn(llm, "o que define uma boa amizade para você?"))

    assert llm._call_index >= 1
    ctx = llm.call_contexts[0] if llm.call_contexts else {}
    assert "turn_payload" in ctx
    turn_payload = ctx["turn_payload"]
    assert "turn" in turn_payload
    assert "robot" in turn_payload
    assert "mood" in turn_payload


def test_turn_payload_user_text_matches_transcript() -> None:
    """turn_payload.turn.user_text deve bater com o texto transcrito."""
    llm = MockLLMProvider(
        '{"expression_id":"curious","reply":"Claro!","tool_call":null}'
    )
    text = "qual é a sua opinião sobre o cosmos?"

    asyncio.run(_do_turn(llm, text))

    ctx = llm.call_contexts[0] if llm.call_contexts else {}
    tp = ctx.get("turn_payload", {})
    assert tp.get("turn", {}).get("user_text") == text


def test_turn_payload_has_no_raw_floats() -> None:
    """turn_payload.mood deve ser string natural, não float."""
    llm = MockLLMProvider(
        '{"expression_id":"neutral","reply":"Ok.","tool_call":null}'
    )

    asyncio.run(_do_turn(llm, "como você definiria sucesso pessoal?"))

    ctx = llm.call_contexts[0] if llm.call_contexts else {}
    tp = ctx.get("turn_payload", {})
    mood = tp.get("mood", "")
    assert isinstance(mood, str)
    assert mood != ""
    # Não deve conter floats crus
    import re
    assert not re.search(r"\d+\.\d{3}", mood), f"Float cru no mood: {mood!r}"


# ---------------------------------------------------------------------------
# Scenario 5 — Sequential turns
# ---------------------------------------------------------------------------

def test_two_sequential_turns() -> None:
    """Dois turnos em sequência devem produzir dois LlmReplyComplete."""

    async def _two_turns() -> list:
        bus_mod = importlib.import_module("noisebot_server.internal.agent.runtime")
        orch_mod = importlib.import_module("noisebot_server.internal.agent.orchestrator")

        llm = MockLLMProvider(
            '{"expression_id":"happy","reply":"Adoro física!","tool_call":null}',
            '{"expression_id":"curious","reply":"Boa escolha de tema!","tool_call":null}',
        )
        bus = bus_mod.EventBus()
        orch = orch_mod.Orchestrator(bus=bus, llm_provider=llm)
        sub = bus.subscribe(maxsize=500)
        loop_task = asyncio.create_task(orch.run())

        all_events: list = []

        for turn_id, text in [
            (1, "você gosta de física?"),
            (2, "qual tema você prefere?"),
        ]:
            await bus.publish(bus_mod.FinalTranscript(
                turn_id=turn_id, text=text, quality=bus_mod.TranscriptQuality.GOOD,
            ))
            async def _collect(turn_id: int = turn_id) -> None:
                while True:
                    ev = await sub.get()
                    all_events.append(ev)
                    if isinstance(ev, bus_mod.SpeechDone) and ev.turn_id == turn_id:
                        break

            try:
                await asyncio.wait_for(_collect(), timeout=4.0)
            except asyncio.TimeoutError:
                break
            # Brief pause so orchestrator returns to IDLE
            await asyncio.sleep(0.05)

        await orch.shutdown()
        loop_task.cancel()
        return all_events

    events = asyncio.run(_two_turns())

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    replies = _find(events, runtime.LlmReplyComplete)
    assert len(replies) == 2
    assert "física" in replies[0].reply or "Adoro" in replies[0].reply


# ---------------------------------------------------------------------------
# Scenario 6 — Provider name in LlmReplyComplete
# ---------------------------------------------------------------------------

def test_llm_reply_complete_has_provider_name() -> None:
    """LlmReplyComplete.provider deve refletir o _provider_name do provider."""
    llm = MockLLMProvider(
        '{"expression_id":"neutral","reply":"Aqui estou.","tool_call":null}'
    )

    events = asyncio.run(_do_turn(llm, "você está online?"))

    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    assert complete is not None
    assert complete.provider == "mock"


# ---------------------------------------------------------------------------
# Scenario 7 — requires_confirmation skips second step
# ---------------------------------------------------------------------------

def test_requires_confirmation_skips_second_step() -> None:
    """tool_call request_confirmation não deve disparar segundo passo LLM."""
    # request_confirmation tem risk_level='confirmation_required' no gateway —
    # o gateway emite requires_confirmation=True e não chega ao executor.
    llm = MockLLMProvider(
        '{"expression_id":"curious","reply":"Posso fazer isso?","tool_call":{"name":"request_confirmation","arguments":{"question":"Confirmar?","action_description":"test"}}}',
        '{"expression_id":"neutral","reply":"Segundo passo indevido.","tool_call":null}',
    )

    asyncio.run(_do_turn(llm, "pode confirmar algo para mim?"))

    # Segundo passo NÃO deve ter sido chamado para requires_confirmation
    assert llm._call_index == 1, (
        f"Segundo passo foi chamado indevidamente (call_index={llm._call_index})"
    )


# ---------------------------------------------------------------------------
# Scenario 8 — show_message and list_agenda tools
# ---------------------------------------------------------------------------

def test_show_message_tool_second_step_confirms_display() -> None:
    """show_message executa (sem adapter) e segundo passo confirma via reply."""
    llm = MockLLMProvider(
        '{"expression_id":"neutral","reply":"Mostrando.","tool_call":{"name":"show_message","arguments":{"text":"Olá mundo!"}}}',
        '{"expression_id":"happy","reply":"Mensagem exibida no display!","tool_call":null}',
    )

    events = asyncio.run(_do_turn(llm, "escreve algo no seu display"))

    assert llm._call_index >= 2
    runtime = importlib.import_module("noisebot_server.internal.agent.runtime")
    complete = _first(events, runtime.LlmReplyComplete)
    assert complete is not None
    assert "display" in complete.reply or "exibida" in complete.reply or "Mensagem" in complete.reply


def test_list_agenda_tool_returns_items_in_context() -> None:
    """list_agenda com app_state populado injeta itens no contexto do step 2."""
    llm = MockLLMProvider(
        '{"expression_id":"curious","reply":"Consultando.","tool_call":{"name":"list_agenda","arguments":{}}}',
        '{"expression_id":"neutral","reply":"Você tem 1 timer ativo.","tool_call":null}',
    )
    app_state = MockAppState()
    app_state.create_agenda_item("timer", {"title": "café", "duration_min": 5})

    asyncio.run(_do_turn(llm, "quais itens você está monitorando?", app_state=app_state))

    assert llm._call_index >= 2
    second_context = llm.call_contexts[1] if len(llm.call_contexts) > 1 else {}
    tcr = second_context.get("tool_call_result", {})
    assert tcr.get("success") is True
    result = tcr.get("result", {})
    assert result.get("count", 0) >= 1


# ---------------------------------------------------------------------------
# Scenario 9 — Phase 15: long-term memory tools
# ---------------------------------------------------------------------------

def test_remember_fact_persists_to_app_state() -> None:
    """remember_fact armazena fato em app_state e step 2 confirma."""
    llm = MockLLMProvider(
        '{"expression_id":"happy","reply":"Vou lembrar.","tool_call":{"name":"remember_fact","arguments":{"text":"usuário prefere respostas curtas"}}}',
        '{"expression_id":"happy","reply":"Memorizado: você prefere respostas curtas!","tool_call":null}',
    )
    app_state = MockAppState()

    asyncio.run(_do_turn(llm, "pode guardar que gosto de respostas curtas?", app_state=app_state))

    assert llm._call_index >= 2
    assert len(app_state.facts) == 1
    assert "curtas" in app_state.facts[0]["text"]


def test_forget_fact_removes_from_app_state() -> None:
    """forget_fact remove fato existente e step 2 confirma remoção."""
    llm = MockLLMProvider(
        '{"expression_id":"neutral","reply":"Vou esquecer.","tool_call":{"name":"forget_fact","arguments":{"fact_id":"fact_1"}}}',
        '{"expression_id":"neutral","reply":"Fato removido da memória.","tool_call":null}',
    )
    app_state = MockAppState()
    app_state.add_fact("usuário prefere café")

    asyncio.run(_do_turn(llm, "esquece o que guardou sobre mim", app_state=app_state))

    assert llm._call_index >= 2
    assert len(app_state.facts) == 0


def test_recall_user_preferences_returns_facts() -> None:
    """recall_user_preferences injeta lista de fatos no contexto do step 2."""
    llm = MockLLMProvider(
        '{"expression_id":"curious","reply":"Consultando.","tool_call":{"name":"recall_user_preferences","arguments":{}}}',
        '{"expression_id":"happy","reply":"Lembro que você prefere café.","tool_call":null}',
    )
    app_state = MockAppState()
    app_state.add_fact("usuário prefere café")
    app_state.add_fact("usuário acorda cedo")

    asyncio.run(_do_turn(llm, "o que você sabe sobre mim?", app_state=app_state))

    assert llm._call_index >= 2
    second_context = llm.call_contexts[1] if len(llm.call_contexts) > 1 else {}
    tcr = second_context.get("tool_call_result", {})
    assert tcr.get("success") is True
    result = tcr.get("result", {})
    assert result.get("count", 0) == 2


def test_memory_facts_appear_in_turn_payload() -> None:
    """Fatos memorados aparecem em turn_payload.memory para o LLM."""
    llm = MockLLMProvider(
        '{"expression_id":"neutral","reply":"Ok.","tool_call":null}'
    )
    app_state = MockAppState()
    app_state.add_fact("usuário prefere jazz")

    asyncio.run(_do_turn(llm, "me fale algo sobre música", app_state=app_state))

    ctx = llm.call_contexts[0] if llm.call_contexts else {}
    tp = ctx.get("turn_payload", {})
    memory = tp.get("memory", [])
    assert len(memory) >= 1
    assert any("jazz" in f.get("text", "") for f in memory)
