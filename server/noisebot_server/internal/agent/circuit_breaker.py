"""Circuit breaker for server LLM providers."""
from __future__ import annotations

import time
from enum import Enum


class CircuitState(Enum):
    CLOSED = "closed"        # Normal — chamadas passam
    OPEN = "open"            # Falhou — chamadas rejeitadas imediatamente
    HALF_OPEN = "half_open"  # Testando — uma sonda por vez


class CircuitOpenError(Exception):
    """Raised when the circuit is OPEN and the call must be rejected."""

    def __init__(self, provider: str, reset_in: float = 0.0) -> None:
        self.provider = provider
        self.reset_in = reset_in
        super().__init__(
            f"Circuit OPEN para '{provider}'. "
            f"Próxima sonda em {reset_in:.1f}s"
        )


class CircuitBreaker:
    """Circuit breaker simples por provider LLM.

    Transições de estado:
      CLOSED    → OPEN      quando failure_count >= failure_threshold
      OPEN      → HALF_OPEN após reset_timeout segundos
      HALF_OPEN → CLOSED    após uma sonda bem-sucedida
      HALF_OPEN → OPEN      após uma sonda com falha (failure_count reseta)

    Parâmetros
    ----------
    provider:
        Nome identificador — aparece nas mensagens de erro e logs.
    failure_threshold:
        Falhas consecutivas para abrir o circuito. Default: 3.
    reset_timeout:
        Segundos que o circuito fica OPEN antes de tentar HALF_OPEN. Default: 30.
    """

    def __init__(
        self,
        provider: str = "unknown",
        failure_threshold: int = 3,
        reset_timeout: float = 30.0,
    ) -> None:
        self._provider = provider
        self._failure_threshold = failure_threshold
        self._reset_timeout = reset_timeout

        self._state = CircuitState.CLOSED
        self._failure_count = 0
        self._opened_at: float = 0.0
        self._probe_active = False  # True → sonda HALF_OPEN em andamento

    # -- Propriedades ---------------------------------------------------------

    @property
    def state(self) -> CircuitState:
        """Estado atual (inclui checagem de timeout automática)."""
        self._maybe_half_open()
        return self._state

    @property
    def failure_count(self) -> int:
        return self._failure_count

    @property
    def is_closed(self) -> bool:
        return self.state == CircuitState.CLOSED

    @property
    def is_open(self) -> bool:
        return self.state == CircuitState.OPEN

    # -- API pública ----------------------------------------------------------

    def allow_request(self) -> bool:
        """Verifica se a chamada pode prosseguir.

        Retorna True se o circuito estiver CLOSED ou se for a sonda HALF_OPEN.
        Lança CircuitOpenError se o circuito estiver OPEN (ou sonda ocupada).
        """
        self._maybe_half_open()

        if self._state == CircuitState.CLOSED:
            return True

        if self._state == CircuitState.HALF_OPEN and not self._probe_active:
            self._probe_active = True
            return True

        # OPEN ou sonda já em andamento
        raise CircuitOpenError(self._provider, self._time_until_probe())

    def record_success(self) -> None:
        """Registra sucesso — fecha o circuito e zera contagem de falhas."""
        self._failure_count = 0
        self._probe_active = False
        self._state = CircuitState.CLOSED

    def record_failure(self) -> None:
        """Registra falha — pode abrir o circuito."""
        self._failure_count += 1
        self._probe_active = False
        self._opened_at = time.monotonic()
        if self._failure_count >= self._failure_threshold:
            self._state = CircuitState.OPEN

    def reset(self) -> None:
        """Reseta completamente (ex: reinicialização manual do provider)."""
        self._state = CircuitState.CLOSED
        self._failure_count = 0
        self._opened_at = 0.0
        self._probe_active = False

    # -- Internos -------------------------------------------------------------

    def _maybe_half_open(self) -> None:
        """Transiciona OPEN → HALF_OPEN se o timeout expirou."""
        if (
            self._state == CircuitState.OPEN
            and self._opened_at > 0
            and time.monotonic() - self._opened_at >= self._reset_timeout
        ):
            self._state = CircuitState.HALF_OPEN
            self._probe_active = False

    def _time_until_probe(self) -> float:
        """Segundos restantes até a próxima sonda HALF_OPEN."""
        if self._opened_at <= 0:
            return self._reset_timeout
        elapsed = time.monotonic() - self._opened_at
        return max(0.0, self._reset_timeout - elapsed)
