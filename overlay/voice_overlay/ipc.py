"""Localhost UDP listener that receives datagrams from the Mumble plugin.

Runs the blocking socket recv on a daemon thread and marshals each decoded line
onto the Qt event loop via a signal, so the overlay only ever touches widgets
from the GUI thread.
"""

from __future__ import annotations

import socket
import threading

from PyQt6.QtCore import QObject, pyqtSignal


class IPCListener(QObject):
    """Emits :pyattr:`line_received` (str) for every datagram from the plugin."""

    line_received = pyqtSignal(str)

    def __init__(self, host: str, port: int, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._host = host
        self._port = port
        self._sock: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._running = False

    @property
    def port(self) -> int:
        return self._port

    def start(self) -> None:
        if self._running:
            return
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self._host, self._port))
        # Wake up periodically so stop() can join the thread promptly.
        sock.settimeout(0.5)
        self._sock = sock
        self._running = True
        self._thread = threading.Thread(target=self._loop, name="voice-overlay-ipc", daemon=True)
        self._thread.start()

    def _loop(self) -> None:
        assert self._sock is not None
        while self._running:
            try:
                data, _addr = self._sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                continue
            text = data.decode("utf-8", errors="replace")
            # A single datagram may carry one or more newline-delimited objects.
            for line in text.splitlines():
                if line.strip():
                    self.line_received.emit(line)

    def stop(self) -> None:
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
