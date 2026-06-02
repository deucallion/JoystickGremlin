"""Application wiring: UDP listener -> speaker model -> overlay, plus a tray icon.

This is the process the user actually runs. It owns no game hooks and never
touches the game process, which is the whole point: it is the EAC-safe
replacement for Mumble's injected overlay.
"""

from __future__ import annotations

import sys

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QAction, QColor, QFont, QIcon, QPainter, QPixmap
from PyQt6.QtWidgets import QApplication, QMenu, QSystemTrayIcon

from .config import Config
from .ipc import IPCListener
from .overlay import VoiceOverlay
from .protocol import SpeakerModel


def _make_icon(accent: str) -> QIcon:
    """Draw a simple round mic badge so we don't ship a binary asset."""
    pixmap = QPixmap(64, 64)
    pixmap.fill(QColor(0, 0, 0, 0))
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setBrush(QColor(12, 18, 24))
    painter.setPen(QColor(accent))
    painter.drawEllipse(4, 4, 56, 56)
    painter.setPen(QColor(accent))
    font = QFont("Segoe UI Symbol", 30)
    painter.setFont(font)
    painter.drawText(pixmap.rect(), Qt.AlignmentFlag.AlignCenter, "\U0001F3A4")
    painter.end()
    return QIcon(pixmap)


class VoiceOverlayApp:
    def __init__(self) -> None:
        self.config = Config.load()
        self.model = SpeakerModel(show_self=self.config.show_self)
        self.overlay = VoiceOverlay(self.config)
        self.listener = IPCListener(self.config.host, self.config.port)
        self.listener.line_received.connect(self._on_line)
        self.tray: QSystemTrayIcon | None = None

    def start(self) -> None:
        self._build_tray()
        try:
            self.listener.start()
        except OSError as exc:
            self._notify(
                "Voice Overlay",
                f"Could not listen on {self.config.host}:{self.config.port} ({exc}). "
                "Is another copy already running?",
            )
        if not self.config.locked:
            self.overlay.show()

    # -- event handling ----------------------------------------------------

    def _on_line(self, line: str) -> None:
        speakers = self.model.ingest(line)
        if speakers is not None:
            self.overlay.set_speakers(speakers)

    # -- tray --------------------------------------------------------------

    def _build_tray(self) -> None:
        if not QSystemTrayIcon.isSystemTrayAvailable():
            return
        icon = _make_icon(self.config.accent_color)
        self.tray = QSystemTrayIcon(icon)
        self.tray.setToolTip("Mumble Voice Overlay")

        menu = QMenu()

        self.lock_action = QAction("Unlock (move/resize)", menu)
        self.lock_action.setCheckable(True)
        self.lock_action.setChecked(not self.config.locked)
        self.lock_action.toggled.connect(self._on_toggle_lock)
        menu.addAction(self.lock_action)

        self.self_action = QAction("Show my own card", menu)
        self.self_action.setCheckable(True)
        self.self_action.setChecked(self.config.show_self)
        self.self_action.toggled.connect(self._on_toggle_self)
        menu.addAction(self.self_action)

        menu.addSeparator()
        status = QAction(f"Listening on {self.config.host}:{self.config.port}", menu)
        status.setEnabled(False)
        menu.addAction(status)

        menu.addSeparator()
        quit_action = QAction("Quit", menu)
        quit_action.triggered.connect(self.quit)
        menu.addAction(quit_action)

        self.tray.setContextMenu(menu)
        self.tray.show()

    def _on_toggle_lock(self, unlocked: bool) -> None:
        self.lock_action.setText("Lock (click-through)" if unlocked else "Unlock (move/resize)")
        self.overlay.set_locked(not unlocked)

    def _on_toggle_self(self, show: bool) -> None:
        self.config.show_self = show
        self.config.save()
        self.model.show_self = show
        self.overlay.set_speakers(self.model.active_speakers())

    def _notify(self, title: str, message: str) -> None:
        if self.tray is not None:
            self.tray.showMessage(title, message)
        else:
            print(f"{title}: {message}", file=sys.stderr)

    def quit(self) -> None:
        self.listener.stop()
        if self.tray is not None:
            self.tray.hide()
        QApplication.quit()


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("Mumble Voice Overlay")
    app.setQuitOnLastWindowClosed(False)

    overlay_app = VoiceOverlayApp()
    overlay_app.start()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
