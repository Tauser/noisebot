"""Tests for bridgev2.llm.circuit_breaker."""
from __future__ import annotations

import time
import pytest

from bridgev2.llm.circuit_breaker import (
    CircuitBreaker,
    CircuitOpenError,
    CircuitState,
)


class TestInitialState:
    def test_starts_closed(self):
        cb = CircuitBreaker()
        assert cb.state == CircuitState.CLOSED

    def test_is_closed_true(self):
        cb = CircuitBreaker()
        assert cb.is_closed is True

    def test_is_open_false(self):
        cb = CircuitBreaker()
        assert cb.is_open is False

    def test_failure_count_zero(self):
        cb = CircuitBreaker()
        assert cb.failure_count == 0

    def test_allow_request_returns_true_when_closed(self):
        cb = CircuitBreaker()
        assert cb.allow_request() is True


class TestFailureAccumulation:
    def test_one_failure_stays_closed(self):
        cb = CircuitBreaker(failure_threshold=3)
        cb.record_failure()
        assert cb.state == CircuitState.CLOSED

    def test_two_failures_stays_closed(self):
        cb = CircuitBreaker(failure_threshold=3)
        cb.record_failure()
        cb.record_failure()
        assert cb.state == CircuitState.CLOSED

    def test_threshold_failures_opens(self):
        cb = CircuitBreaker(failure_threshold=3)
        for _ in range(3):
            cb.record_failure()
        assert cb.state == CircuitState.OPEN

    def test_failure_count_increments(self):
        cb = CircuitBreaker(failure_threshold=10)
        cb.record_failure()
        cb.record_failure()
        assert cb.failure_count == 2

    def test_success_resets_failure_count(self):
        cb = CircuitBreaker(failure_threshold=3)
        cb.record_failure()
        cb.record_failure()
        cb.record_success()
        assert cb.failure_count == 0

    def test_success_keeps_closed(self):
        cb = CircuitBreaker()
        cb.record_success()
        assert cb.state == CircuitState.CLOSED


class TestOpenState:
    def test_allow_request_raises_when_open(self):
        cb = CircuitBreaker(failure_threshold=1)
        cb.record_failure()
        with pytest.raises(CircuitOpenError):
            cb.allow_request()

    def test_circuit_open_error_has_provider(self):
        cb = CircuitBreaker(provider="openai/gpt-4", failure_threshold=1)
        cb.record_failure()
        with pytest.raises(CircuitOpenError) as exc_info:
            cb.allow_request()
        assert exc_info.value.provider == "openai/gpt-4"

    def test_circuit_open_error_has_reset_in(self):
        cb = CircuitBreaker(failure_threshold=1, reset_timeout=30.0)
        cb.record_failure()
        with pytest.raises(CircuitOpenError) as exc_info:
            cb.allow_request()
        assert exc_info.value.reset_in > 0

    def test_is_open_true(self):
        cb = CircuitBreaker(failure_threshold=1)
        cb.record_failure()
        assert cb.is_open is True

    def test_is_closed_false_when_open(self):
        cb = CircuitBreaker(failure_threshold=1)
        cb.record_failure()
        assert cb.is_closed is False


class TestHalfOpen:
    def test_transitions_to_half_open_after_timeout(self):
        cb = CircuitBreaker(failure_threshold=1, reset_timeout=0.01)
        cb.record_failure()
        assert cb.state == CircuitState.OPEN
        time.sleep(0.02)
        assert cb.state == CircuitState.HALF_OPEN

    def test_half_open_allows_one_probe(self):
        cb = CircuitBreaker(failure_threshold=1, reset_timeout=0.01)
        cb.record_failure()
        time.sleep(0.02)
        assert cb.allow_request() is True  # sonda permitida

    def test_half_open_blocks_second_request(self):
        cb = CircuitBreaker(failure_threshold=1, reset_timeout=0.01)
        cb.record_failure()
        time.sleep(0.02)
        cb.allow_request()  # primeira sonda
        with pytest.raises(CircuitOpenError):
            cb.allow_request()  # segunda bloqueada

    def test_half_open_success_closes(self):
        cb = CircuitBreaker(failure_threshold=1, reset_timeout=0.01)
        cb.record_failure()
        time.sleep(0.02)
        cb.allow_request()  # sonda
        cb.record_success()
        assert cb.state == CircuitState.CLOSED

    def test_half_open_failure_reopens(self):
        cb = CircuitBreaker(failure_threshold=1, reset_timeout=0.01)
        cb.record_failure()
        time.sleep(0.02)
        cb.allow_request()  # sonda
        cb.record_failure()
        assert cb.state == CircuitState.OPEN


class TestReset:
    def test_reset_from_open(self):
        cb = CircuitBreaker(failure_threshold=1)
        cb.record_failure()
        assert cb.is_open
        cb.reset()
        assert cb.is_closed

    def test_reset_clears_failure_count(self):
        cb = CircuitBreaker(failure_threshold=10)
        cb.record_failure()
        cb.record_failure()
        cb.reset()
        assert cb.failure_count == 0

    def test_reset_allows_requests(self):
        cb = CircuitBreaker(failure_threshold=1)
        cb.record_failure()
        cb.reset()
        assert cb.allow_request() is True


class TestProviderName:
    def test_default_provider_name(self):
        cb = CircuitBreaker()
        cb._failure_threshold = 1
        cb.record_failure()
        with pytest.raises(CircuitOpenError) as exc_info:
            cb.allow_request()
        assert "unknown" in exc_info.value.provider

    def test_custom_provider_name(self):
        cb = CircuitBreaker(provider="gemini/flash", failure_threshold=1)
        cb.record_failure()
        with pytest.raises(CircuitOpenError) as exc_info:
            cb.allow_request()
        assert "gemini/flash" in exc_info.value.provider
