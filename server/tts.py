"""Озвучка ответов (text-to-speech) через Piper на CPU.

Piper — маленькая нейросеть для синтеза речи, работает в ~14 раз быстрее
реального времени на процессоре. Видеокарта занята моделью агента.

Голоса лежат в voices/ (в git не попадают, ~63 МБ каждый). Скачать:
    python -m piper.download_voices --download-dir voices ru_RU-irina-medium
Голос выбирается через TTS_VOICE, например ru_RU-denis-medium (по умолчанию irina).
Читается при создании объекта, а не при импорте: main.py грузит .env после импортов.
"""
import logging
import os
import re
import threading
from pathlib import Path

log = logging.getLogger("uvicorn.error")

VOICES_DIR = Path(__file__).resolve().parent.parent / "voices"
MAX_SPEECH_CHARS = 600   # длинный ответ слушать долго; полный текст и так на экране


def speech_text(text: str) -> str:
    """Готовит текст для чтения вслух: ссылки и разметку синтезатор читает по буквам."""
    text = re.sub(r"https?://\S+", "ссылка", text)
    text = re.sub(r"[*_#`>|]+", " ", text)          # markdown
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) > MAX_SPEECH_CHARS:
        cut = text.rfind(".", 0, MAX_SPEECH_CHARS)  # обрезаем по концу предложения
        text = text[: cut + 1 if cut > 0 else MAX_SPEECH_CHARS] + " Остальное на экране."
    return text


class PiperSpeaker:
    def __init__(self, voice: str | None = None, voices_dir: Path = VOICES_DIR):
        voice = voice or os.environ.get("TTS_VOICE", "ru_RU-irina-medium")
        self.path = voices_dir / f"{voice}.onnx"
        self._voice = None
        self._lock = threading.Lock()

    def available(self) -> bool:
        return self.path.is_file()

    def preload(self) -> None:
        if self.available():
            self._get_voice()
        else:
            log.warning("tts: voice not found: %s (speech disabled)", self.path)

    def _get_voice(self):
        with self._lock:
            if self._voice is None:
                from piper import PiperVoice  # тяжёлый импорт, только когда нужен
                self._voice = PiperVoice.load(str(self.path))
            return self._voice

    def synthesize(self, text: str) -> tuple[bytes, int]:
        """Блокирующий вызов: запускать через asyncio.to_thread.

        Возвращает PCM 16 бит моно и частоту (у medium-голосов 22050 Гц).
        """
        chunks = list(self._get_voice().synthesize(text))
        if not chunks:
            return b"", 22050
        return b"".join(c.audio_int16_bytes for c in chunks), chunks[0].sample_rate
