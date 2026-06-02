"""Painted widgets for the voice overlay: tactical panel chrome, glow labels,
an animated "speaking" pulse, and the per-speaker card.

The look intentionally matches the Overwatch tactical-HUD overlays so the voice
overlay feels like part of the same toolkit.
"""

from __future__ import annotations

from PyQt6.QtCore import QRectF, Qt, QTimer
from PyQt6.QtGui import (
    QBrush,
    QColor,
    QFont,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
    QRadialGradient,
)
from PyQt6.QtWidgets import QHBoxLayout, QLabel, QSizePolicy, QVBoxLayout, QWidget

from .protocol import Speaker

# Per talking-state visual theme: accent colour, short label, and glyph.
STATE_THEME: dict[str, dict[str, str]] = {
    "talking": {"color": "#00E5FF", "label": "TALKING", "glyph": "\U0001F3A4"},
    "whispering": {"color": "#A78BFA", "label": "WHISPER", "glyph": "\U0001F92B"},
    "shouting": {"color": "#FBBF24", "label": "SHOUT", "glyph": "\U0001F4E2"},
    "muted": {"color": "#FB7185", "label": "MIC MUTED", "glyph": "\U0001F507"},
}


def state_theme(state: str) -> dict[str, str]:
    return STATE_THEME.get(state, STATE_THEME["talking"])


class GlowLabel(QLabel):
    def __init__(self, text: str = "", color: str = "#FFFFFF", parent=None):
        super().__init__(text, parent)
        self.setStyleSheet(f"color: {color}; background: transparent;")

    def set_color(self, color: str) -> None:
        self.setStyleSheet(f"color: {color}; background: transparent;")


class TacticalPanel(QWidget):
    """Painted dark-glass panel with an accent bloom and corner brackets."""

    def __init__(self, accent: str = "#00E5FF", opacity: float = 0.92, radius: int = 8, parent=None):
        super().__init__(parent)
        self._accent = accent
        self._opacity = max(0.0, min(1.0, float(opacity)))
        self._radius = radius
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, False)
        self.setAutoFillBackground(False)

    def set_accent(self, accent: str) -> None:
        if accent != self._accent:
            self._accent = accent
            self.update()

    def paintEvent(self, event):  # noqa: D401 (Qt override)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        if w <= 0 or h <= 0:
            return

        rect = QRectF(0.5, 0.5, w - 1, h - 1)
        path = QPainterPath()
        path.addRoundedRect(rect, self._radius, self._radius)
        painter.setClipPath(path)

        op = self._opacity
        grad = QLinearGradient(0, 0, 0, h)
        grad.setColorAt(0.0, QColor(12, 18, 24, int(238 * op)))
        grad.setColorAt(1.0, QColor(5, 8, 12, int(208 * op)))
        painter.fillPath(path, QBrush(grad))

        accent = QColor(self._accent)
        bloom = QRadialGradient(w * 0.12, h * 0.5, max(140.0, w * 0.8))
        bc = QColor(accent)
        bc.setAlpha(int(55 * op))
        bloom.setColorAt(0.0, bc)
        bloom.setColorAt(1.0, QColor(0, 0, 0, 0))
        painter.fillPath(path, QBrush(bloom))

        # Accent rail down the left edge marks the "active speaker" feel.
        rail = QColor(accent)
        rail.setAlpha(int(230 * op))
        painter.fillRect(QRectF(0.0, 4.0, 3.0, h - 8.0), rail)

        # Corner brackets.
        painter.setClipping(False)
        bp = QColor(accent)
        bp.setAlpha(int(200 * op))
        pen = QPen(bp, 1.5)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        painter.setPen(pen)
        arm = 12
        painter.drawLine(w - 6 - arm, 5, w - 6, 5)
        painter.drawLine(w - 6, 5, w - 6, 5 + arm)
        painter.drawLine(w - 6, h - 6 - arm, w - 6, h - 6)
        painter.drawLine(w - 6 - arm, h - 6, w - 6, h - 6)


class SpeakerPulse(QWidget):
    """A small equaliser-style animation shown while a speaker is active."""

    _BARS = 4

    def __init__(self, color: str = "#00E5FF", parent=None):
        super().__init__(parent)
        self._color = color
        self._phase = 0
        self.setFixedSize(26, 30)
        self._timer = QTimer(self)
        self._timer.setInterval(110)
        self._timer.timeout.connect(self._tick)

    def set_color(self, color: str) -> None:
        self._color = color
        self.update()

    def start(self) -> None:
        if not self._timer.isActive():
            self._timer.start()

    def stop(self) -> None:
        self._timer.stop()
        self.update()

    def _tick(self) -> None:
        self._phase = (self._phase + 1) % 8
        self.update()

    def paintEvent(self, event):  # noqa: D401 (Qt override)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        color = QColor(self._color)
        bar_w = 3
        gap = (w - self._BARS * bar_w) / (self._BARS + 1)
        # Deterministic pseudo-random heights driven by the animation phase.
        heights = [0.35, 0.85, 0.55, 1.0, 0.45, 0.7, 0.9, 0.4]
        for i in range(self._BARS):
            frac = heights[(self._phase + i * 2) % len(heights)]
            if not self._timer.isActive():
                frac = 0.25
            bh = max(3.0, frac * (h - 6))
            x = gap + i * (bar_w + gap)
            y = (h - bh) / 2
            painter.setBrush(QBrush(color))
            painter.setPen(Qt.PenStyle.NoPen)
            painter.drawRoundedRect(QRectF(x, y, bar_w, bh), 1.5, 1.5)


class SpeakerCard(QWidget):
    """One speaker: animated pulse + name + state/channel + optional comment."""

    def __init__(self, speaker: Speaker, config, parent=None):
        super().__init__(parent)
        self.config = config
        self.user_id = speaker.user.id
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Maximum)
        self._build_ui()
        self.update_from(speaker)

    def _build_ui(self) -> None:
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)

        self.panel = TacticalPanel(accent=self.config.accent_color, opacity=self.config.opacity)
        outer.addWidget(self.panel)

        row = QHBoxLayout(self.panel)
        row.setContentsMargins(12, 9, 16, 9)
        row.setSpacing(10)

        self.pulse = SpeakerPulse(self.config.accent_color, self.panel)
        row.addWidget(self.pulse, 0, Qt.AlignmentFlag.AlignVCenter)

        body = QVBoxLayout()
        body.setSpacing(1)

        self.name_label = GlowLabel("", "#E6EDF3", self.panel)
        name_font = QFont("Segoe UI", self.config.font_size + 1, QFont.Weight.Bold)
        self.name_label.setFont(name_font)
        body.addWidget(self.name_label)

        self.meta_label = GlowLabel("", "#7DD3FC", self.panel)
        meta_font = QFont("Consolas", max(8, self.config.font_size - 3), QFont.Weight.Bold)
        meta_font.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, 1.2)
        self.meta_label.setFont(meta_font)
        body.addWidget(self.meta_label)

        self.comment_label = GlowLabel("", "#6B7280", self.panel)
        comment_font = QFont("Segoe UI", max(8, self.config.font_size - 3))
        comment_font.setItalic(True)
        self.comment_label.setFont(comment_font)
        self.comment_label.setWordWrap(True)
        body.addWidget(self.comment_label)

        row.addLayout(body, 1)

    def update_from(self, speaker: Speaker) -> None:
        theme = state_theme(speaker.state)
        color = theme["color"]
        self.panel.set_accent(color)
        self.pulse.set_color(color)
        if speaker.state == "muted":
            self.pulse.stop()
        else:
            self.pulse.start()

        name = speaker.user.name or f"User {speaker.user.id}"
        if speaker.is_self:
            name = f"{name}  (you)"
        self.name_label.setText(name)

        # Meta line: STATE  ·  channel  ·  status tags.
        parts = [theme["label"]]
        if self.config.show_channel and speaker.user.channel:
            parts.append(speaker.user.channel)
        tags = self._status_tags(speaker)
        parts.extend(tags)
        self.meta_label.setText("  ·  ".join(parts))
        self.meta_label.set_color(color)

        comment = speaker.user.comment if self.config.show_comment else ""
        if comment:
            if len(comment) > 90:
                comment = comment[:90].rstrip() + "…"
            self.comment_label.setText(f"“{comment}”")
            self.comment_label.setVisible(True)
        else:
            self.comment_label.clear()
            self.comment_label.setVisible(False)

    @staticmethod
    def _status_tags(speaker: Speaker) -> list[str]:
        tags: list[str] = []
        if speaker.user.locally_muted:
            tags.append("LOCAL MUTED")
        if speaker.is_self:
            if speaker.self_deafened:
                tags.append("DEAFENED")
            elif speaker.self_muted:
                tags.append("SELF MUTED")
        return tags

    def stop_animation(self) -> None:
        self.pulse.stop()
