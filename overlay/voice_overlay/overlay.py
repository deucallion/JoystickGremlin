"""The transparent, always-on-top voice overlay window.

This is the EAC-safe alternative to Mumble's injected overlay: an ordinary
top-level OS window the desktop compositor draws over the (borderless) game.
It stacks one :class:`SpeakerCard` per active speaker, lets cards linger briefly
after talking stops, and masks itself to just the cards so empty space is fully
transparent and click-through.
"""

from __future__ import annotations

from PyQt6.QtCore import QPoint, QRectF, Qt, QTimer
from PyQt6.QtGui import QFont, QPainterPath, QRegion
from PyQt6.QtWidgets import QLabel, QVBoxLayout, QWidget

from .config import Config
from .protocol import Speaker
from .widgets import SpeakerCard


class VoiceOverlay(QWidget):
    def __init__(self, config: Config):
        super().__init__()
        self.config = config
        self._cards: dict[int, SpeakerCard] = {}
        self._linger: dict[int, QTimer] = {}
        self._dragging = False
        self._drag_offset = QPoint()

        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.NoDropShadowWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self.setAutoFillBackground(False)

        self.setFixedWidth(config.overlay_width)
        self._build_ui()
        self.move(config.overlay_x, config.overlay_y)
        self._apply_lock_mode()

    # -- UI ----------------------------------------------------------------

    def _build_ui(self) -> None:
        self.root = QVBoxLayout(self)
        self.root.setContentsMargins(0, 0, 0, 0)
        self.root.setSpacing(8)
        self.root.setAlignment(Qt.AlignmentFlag.AlignTop)

        self.placeholder = QLabel("VOICE OVERLAY  //  DRAG TO POSITION", self)
        self.placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        font = QFont("Consolas", 9, QFont.Weight.Bold)
        font.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, 1.5)
        self.placeholder.setFont(font)
        self.placeholder.setStyleSheet(
            "QLabel {"
            " color: rgba(0,229,255,200);"
            " border: 1px dashed rgba(0,229,255,120);"
            " border-radius: 8px;"
            " background-color: rgba(0,229,255,16);"
            " padding: 14px; }"
        )
        self.root.addWidget(self.placeholder)

    def _apply_lock_mode(self) -> None:
        locked = self.config.locked
        self.placeholder.setVisible(not locked)
        # Locked overlay is click-through so it never steals input from the game.
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, locked)
        self._relayout()

    def set_locked(self, locked: bool) -> None:
        self.config.locked = locked
        self.config.save()
        self._apply_lock_mode()
        if locked and not self._cards:
            self.hide()
        else:
            self.show()
            self.raise_()

    # -- speaker updates ---------------------------------------------------

    def set_speakers(self, speakers: list[Speaker]) -> None:
        """Reconcile the visible cards with the current active-speaker list."""
        active_ids = set()
        for speaker in speakers[: self.config.max_cards]:
            active_ids.add(speaker.user.id)
            self._cancel_linger(speaker.user.id)
            card = self._cards.get(speaker.user.id)
            if card is None:
                card = SpeakerCard(speaker, self.config, self)
                self._cards[speaker.user.id] = card
                self.root.addWidget(card)
            else:
                card.update_from(speaker)

        # Anyone no longer active starts (or keeps) their linger countdown.
        for uid in list(self._cards):
            if uid not in active_ids and uid not in self._linger:
                self._start_linger(uid)

        self._refresh_visibility()

    def _start_linger(self, uid: int) -> None:
        card = self._cards.get(uid)
        if card is not None:
            card.stop_animation()
        timer = QTimer(self)
        timer.setSingleShot(True)
        timer.setInterval(max(0, self.config.linger_ms))
        timer.timeout.connect(lambda u=uid: self._remove_card(u))
        self._linger[uid] = timer
        timer.start()

    def _cancel_linger(self, uid: int) -> None:
        timer = self._linger.pop(uid, None)
        if timer is not None:
            timer.stop()
            timer.deleteLater()

    def _remove_card(self, uid: int) -> None:
        self._cancel_linger(uid)
        card = self._cards.pop(uid, None)
        if card is not None:
            self.root.removeWidget(card)
            card.deleteLater()
        self._refresh_visibility()

    # -- layout / masking --------------------------------------------------

    def _refresh_visibility(self) -> None:
        if self.config.locked and not self._cards:
            self.hide()
            return
        self.show()
        self.raise_()
        self._relayout()

    def _relayout(self) -> None:
        # Resize to the content's natural height, then mask to just the visible
        # widgets so the empty window area stays transparent and click-through.
        self.root.activate()
        self.setFixedHeight(max(self.sizeHint().height(), self.minimumSizeHint().height()))
        QTimer.singleShot(0, self._sync_mask)

    def _sync_mask(self) -> None:
        if not self.config.locked:
            # Unlocked: keep the whole window interactive for dragging.
            self.clearMask()
            return
        region = QRegion()
        for card in self._cards.values():
            if card.height() <= 0:
                continue
            origin = card.mapTo(self, QPoint(0, 0))
            path = QPainterPath()
            path.addRoundedRect(
                QRectF(origin.x(), origin.y(), card.width(), card.height()), 8, 8
            )
            region = region.united(QRegion(path.toFillPolygon().toPolygon()))
        self.setMask(region)

    def resizeEvent(self, event):  # noqa: D401 (Qt override)
        super().resizeEvent(event)
        QTimer.singleShot(0, self._sync_mask)

    # -- dragging (only while unlocked) ------------------------------------

    def mousePressEvent(self, event):  # noqa: D401 (Qt override)
        if event.button() == Qt.MouseButton.LeftButton and not self.config.locked:
            self._dragging = True
            self._drag_offset = event.globalPosition().toPoint() - self.pos()
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event):  # noqa: D401 (Qt override)
        if self._dragging:
            self.move(event.globalPosition().toPoint() - self._drag_offset)
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):  # noqa: D401 (Qt override)
        if self._dragging and event.button() == Qt.MouseButton.LeftButton:
            self._dragging = False
            self.config.overlay_x = self.x()
            self.config.overlay_y = self.y()
            self.config.save()
            event.accept()
            return
        super().mouseReleaseEvent(event)
