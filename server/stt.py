"""Распознавание речи (speech-to-text) через faster-whisper на CPU.

Видеокарта занята моделью агента (qwen3:8b ~6.2 из 8 ГБ), поэтому Whisper
работает на процессоре. compute_type="int8" — урезанная точность весов:
в разы быстрее на CPU, качество для коротких фраз почти не страдает.

Звук приходит с платы как сырой PCM: 16 бит со знаком, little-endian, 16 кГц, моно.
Это ровно то, что ждёт Whisper, поэтому перекодировать ничего не нужно.
"""
import logging
import os
import threading

import numpy as np

log = logging.getLogger("uvicorn.error")  # этот логгер uvicorn выводит по умолчанию

SAMPLE_RATE = 16000
BYTES_PER_SAMPLE = 2
MAX_SECONDS = 30
MAX_BYTES = SAMPLE_RATE * BYTES_PER_SAMPLE * MAX_SECONDS

# Настройки читаются при создании объекта, а не при импорте: main.py грузит .env после импортов.
#   STT_MODEL    — tiny / base / small / medium (по умолчанию small)
#   STT_LANGUAGE — ru по умолчанию: короткие фразы язык угадывают плохо


class WhisperTranscriber:
    """Модель грузится один раз при первом вызове (или заранее через preload)."""

    def __init__(self, model_name: str | None = None, language: str | None = None):
        self.model_name = model_name or os.environ.get("STT_MODEL", "small")
        self.language = language or os.environ.get("STT_LANGUAGE", "ru")
        self._model = None
        self._lock = threading.Lock()  # две записи подряд не должны грузить модель дважды

    def preload(self) -> None:
        self._get_model()

    def _get_model(self):
        with self._lock:
            if self._model is None:
                from faster_whisper import WhisperModel  # тяжёлый импорт, только когда нужен
                # cpu_threads: по умолчанию 4; на всех ядрах small быстрее в ~1.5 раза
                # (замер: фраза 3.3 с — 4.7 с на 4 потоках, 3.2 с на 12)
                self._model = WhisperModel(self.model_name, device="cpu", compute_type="int8",
                                           cpu_threads=os.cpu_count() or 4)
            return self._model

    def transcribe(self, pcm: bytes) -> str:
        """Блокирующий вызов: запускать через asyncio.to_thread."""
        if len(pcm) < SAMPLE_RATE * BYTES_PER_SAMPLE // 4:  # меньше 0.25 с — это не речь
            return ""
        # int16 -> float32 в диапазоне [-1, 1], так Whisper принимает звук из памяти
        audio = np.frombuffer(pcm[: len(pcm) // 2 * 2], dtype="<i2").astype(np.float32) / 32768.0
        # Длина и громкость в логе: если распознаёт плохо, сразу видно, тихо ли было
        log.info("voice: %.1f s, peak %.2f, rms %.3f",
                 len(audio) / SAMPLE_RATE, float(np.abs(audio).max()), float(np.sqrt(np.mean(audio ** 2))))
        segments, _ = self._get_model().transcribe(
            audio,
            language=self.language,
            beam_size=1,         # жадный поиск: быстрее, для команд хватает
            vad_filter=True,     # вырезать тишину до и после фразы
        )
        text = " ".join(s.text.strip() for s in segments).strip()
        log.info("voice -> %r", text)
        return text
