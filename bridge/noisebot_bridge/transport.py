from __future__ import annotations

import logging
import socket
import threading
import time

from .protocol import FRAME_OVERHEAD, MSG_HELLO, decode_frames, encode_frame

log = logging.getLogger("noisebot_bridge.transport")

TCP_CONNECT_TIMEOUT_S = 5.0
TCP_RECV_TIMEOUT_S = 0.1
TCP_SEND_TIMEOUT_S = 2.0
TCP_KEEPALIVE_IDLE_S = 8
TCP_KEEPALIVE_INTERVAL_S = 2
TCP_KEEPALIVE_COUNT = 3


def _configure_tcp_keepalive(sock: socket.socket):
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)

    if hasattr(socket, "TCP_KEEPIDLE"):
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, TCP_KEEPALIVE_IDLE_S)
    if hasattr(socket, "TCP_KEEPINTVL"):
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, TCP_KEEPALIVE_INTERVAL_S)
    if hasattr(socket, "TCP_KEEPCNT"):
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, TCP_KEEPALIVE_COUNT)
    if hasattr(socket, "SIO_KEEPALIVE_VALS"):
        sock.ioctl(
            socket.SIO_KEEPALIVE_VALS,
            (
                1,
                TCP_KEEPALIVE_IDLE_S * 1000,
                TCP_KEEPALIVE_INTERVAL_S * 1000,
            ),
        )


class TcpTransport:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.sock = None
        self._send_lock = threading.Lock()

    def connect(self, timeout: float = TCP_CONNECT_TIMEOUT_S) -> bool:
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=timeout)
            self.sock.settimeout(TCP_RECV_TIMEOUT_S)
            _configure_tcp_keepalive(self.sock)
            return True
        except Exception as e:
            log.error("TCP connect %s:%d falhou: %s", self.host, self.port, e)
            self.close()
            return False

    def send(self, data: bytes):
        if self.sock is None:
            raise ConnectionError("TCP socket fechado")
        with self._send_lock:
            try:
                self.sock.settimeout(TCP_SEND_TIMEOUT_S)
                self.sock.sendall(data)
            except Exception as e:
                log.warning("TCP send erro: %s", e)
                raise
            finally:
                if self.sock is not None:
                    try:
                        self.sock.settimeout(TCP_RECV_TIMEOUT_S)
                    except Exception:
                        pass

    def recv(self, max_bytes: int = 4096) -> bytes:
        if self.sock is None:
            raise ConnectionError("TCP socket fechado")
        try:
            data = self.sock.recv(max_bytes)
            if data == b"":
                raise ConnectionError("TCP peer closed")
            return data
        except socket.timeout:
            return b""

    def close(self):
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None


class UartTransport:
    def __init__(self, port: str, baud: int = 921600):
        import serial

        self.ser = serial.Serial(port, baud, timeout=0.1)

    def send(self, data: bytes):
        self.ser.write(data)

    def recv(self, max_bytes: int = 4096) -> bytes:
        return self.ser.read(max_bytes)

    def close(self):
        self.ser.close()


class NullTransport:
    """Transporte para replay/teste offline."""

    def __init__(self):
        self.sent: list[tuple[int | None, bytes]] = []

    def send(self, data: bytes):
        self.sent.append((None, data))

    def recv(self, max_bytes: int = 4096) -> bytes:
        time.sleep(0.1)
        return b""

    def close(self):
        return None


def do_handshake(transport, timeout: float = 1.0) -> bool:
    try:
        transport.send(encode_frame(MSG_HELLO))
    except Exception as e:
        log.warning("Handshake send falhou: %s", e)
        return False

    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data = transport.recv(FRAME_OVERHEAD)
        except Exception as e:
            log.warning("Handshake recv falhou: %s", e)
            return False
        if data:
            buf.extend(data)
        if len(buf) >= FRAME_OVERHEAD:
            frames = decode_frames(buf)
            if frames and frames[0][0] == MSG_HELLO:
                return True
    return False


def discover_mdns(timeout: float = 5.0) -> str | None:
    try:
        return socket.gethostbyname("noisebot.local")
    except Exception:
        return None
