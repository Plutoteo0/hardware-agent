"""Одна сессия = одно WebSocket-подключение клиента.

Здесь агентный цикл и логика подтверждений.
"""
import asyncio
import json
import uuid
from contextlib import suppress
from typing import Awaitable, Callable, Protocol

from pydantic import BaseModel

import time

from models import Model
from protocol import ConfirmRequest, Error, Result, SpeechEnd, SpeechStart, Status, Transcript
from stt import MAX_BYTES
from tools import RISK, run_tool
from tts import speech_text

CONFIRM_TIMEOUT_S = 30
MAX_STEPS = 8  # предохранитель от зацикливания модели

# Озвучка: шлём кусками по 0.2 с и не дальше чем на 2 с вперёд реального времени.
# Иначе 20 секунд речи прилетят за долю секунды, и у платы переполнится буфер.
SPEECH_CHUNK_S = 0.2
SPEECH_AHEAD_S = 2.0

Send = Callable[[BaseModel], Awaitable[None]]
SendBytes = Callable[[bytes], Awaitable[None]]


class Transcriber(Protocol):
    def transcribe(self, pcm: bytes) -> str: ...


class Speaker(Protocol):
    def synthesize(self, text: str) -> tuple[bytes, int]: ...


class Session:
    def __init__(self, model: Model, send: Send, confirm_timeout: float = CONFIRM_TIMEOUT_S,
                 stt: Transcriber | None = None, tts: Speaker | None = None,
                 send_bytes: SendBytes | None = None):
        self.model = model
        self.send = send
        self.confirm_timeout = confirm_timeout
        self.stt = stt
        self.tts = tts
        self.send_bytes = send_bytes
        self.speech = False  # клиент попросил озвучку (hello). По умолчанию нет
        self.pending: dict[str, asyncio.Future] = {}  # id подтверждения -> ожидание ответа
        self.task: asyncio.Task | None = None
        self.audio: bytearray | None = None  # не None = идёт запись голоса

    def on_hello(self, speech: bool) -> None:
        self.speech = speech and self.tts is not None and self.send_bytes is not None

    # ---- входящие сообщения от клиента ----

    def _busy(self) -> bool:
        if self.task and not self.task.done():
            # Асинхронно, поэтому create_task: см. комментарий в main.py
            asyncio.create_task(self.send(Error(text="task already running")))
            return True
        return False

    def start_task(self, text: str) -> None:
        if self._busy():
            return
        self.task = asyncio.create_task(self._run(text))

    def on_audio_start(self) -> None:
        self.audio = bytearray()

    def on_audio_chunk(self, data: bytes) -> None:
        if self.audio is None:
            return  # звук без audio_start игнорируем
        # Не больше MAX_BYTES (30 с): плата могла зависнуть с зажатой кнопкой
        room = MAX_BYTES - len(self.audio)
        if room > 0:
            self.audio += data[:room]

    def on_audio_end(self) -> None:
        pcm, self.audio = self.audio, None
        if pcm is None or self._busy():
            return
        if self.stt is None:
            asyncio.create_task(self.send(Error(text="speech recognition is not configured")))
            return
        self.task = asyncio.create_task(self._voice(bytes(pcm)))

    def on_decision(self, id: str, approve: bool) -> None:
        fut = self.pending.get(id)
        # Неизвестный или уже закрытый id молча игнорируем: так «опоздавшее»
        # решение после таймаута или рестарта не может одобрить чужой вызов.
        if fut and not fut.done():
            fut.set_result(approve)

    def cancel(self) -> None:
        if self.task and not self.task.done():
            self.task.cancel()

    def close(self) -> None:
        """Клиент отключился: отменяем задачу, все ожидания закроются сами."""
        self.cancel()

    # ---- голос ----

    async def _voice(self, pcm: bytes) -> None:
        """Распознать и запустить агента. Одна задача на оба шага, поэтому cancel работает и тут."""
        try:
            await self.send(Status(text="recognizing speech"))
            # Whisper считает секунды и держит CPU: в отдельном потоке, чтобы принимать cancel
            text = await asyncio.to_thread(self.stt.transcribe, pcm)
        except asyncio.CancelledError:
            with suppress(Exception):
                await self.send(Status(text="cancelled"))
            raise
        except Exception as e:
            await self.send(Error(text=f"speech recognition failed: {type(e).__name__}: {e}"))
            return

        if not text:
            await self.send(Error(text="не расслышал, попробуй ещё раз"))
            return
        await self.send(Transcript(text=text))
        await self._run(text)

    async def _speak(self, text: str) -> None:
        """Синтез и отправка озвучки. Ошибка озвучки не портит ответ: текст уже на экране."""
        text = speech_text(text)
        if not text:
            return
        try:
            pcm, rate = await asyncio.to_thread(self.tts.synthesize, text)
        except Exception as e:
            await self.send(Status(text=f"speech failed: {type(e).__name__}"))
            return

        await self.send(SpeechStart(sample_rate=rate))
        try:
            chunk = int(rate * SPEECH_CHUNK_S) * 2          # 2 байта на сэмпл
            started = time.monotonic()
            for i in range(0, len(pcm), chunk):
                sent_s = i / 2 / rate                         # сколько секунд звука уже отправили
                ahead = sent_s - (time.monotonic() - started)
                if ahead > SPEECH_AHEAD_S:
                    await asyncio.sleep(ahead - SPEECH_AHEAD_S)
                await self.send_bytes(pcm[i:i + chunk])
        finally:
            # И после отмены: плата должна узнать, что звука больше не будет
            with suppress(Exception):
                await self.send(SpeechEnd())

    # ---- агентный цикл ----

    async def _run(self, text: str) -> None:
        history = [{"role": "user", "content": text}]
        try:
            for _ in range(MAX_STEPS):
                step = await self.model.next_step(history)

                if "final" in step:
                    await self.send(Result(text=step["final"]))
                    if self.speech:
                        # Внутри той же задачи: cancel останавливает и озвучку
                        await self._speak(step["final"])
                    return

                name, args = step["tool"], step["args"]
                # Сохраняем и сам вызов, и результат: иначе модель не помнит,
                # что уже делала, и начинает повторяться.
                history.append({"role": "assistant", "content": json.dumps(step, ensure_ascii=False)})
                output = await self._call_tool(name, args)
                history.append({"role": "tool", "content": output})

            await self.send(Error(text="step limit reached"))
        except asyncio.CancelledError:
            # При отключении клиента сокет уже закрыт, и send может упасть.
            with suppress(Exception):
                await self.send(Status(text="cancelled"))
            raise
        except Exception as e:  # ошибка в задаче не должна ронять соединение
            await self.send(Error(text=f"{type(e).__name__}: {e}"))

    async def _call_tool(self, name: str, args: dict) -> str:
        # Неизвестный тул считаем запрещённым: безопасное значение по умолчанию.
        risk = RISK.get(name, "forbidden")

        if risk == "forbidden":
            return f"DENIED: tool '{name}' is forbidden"

        if risk == "ask":
            if not await self._confirm(name, args):
                return "DENIED: user rejected the call"

        await self.send(Status(text=f"running {name}"))
        # Любая ошибка тула (даже OSError при записи) уходит модели как текст,
        # а не роняет всю задачу: модель может исправить аргументы и попробовать снова.
        try:
            # В отдельном потоке: сетевые тулы могут ждать секунды, а цикл должен принимать Decision/Cancel
            return await asyncio.to_thread(run_tool, name, args)
        except Exception as e:
            return f"ERROR: {e}"

    async def _confirm(self, name: str, args: dict) -> bool:
        id = uuid.uuid4().hex[:8]
        fut = asyncio.get_running_loop().create_future()
        self.pending[id] = fut
        await self.send(ConfirmRequest(id=id, tool=name, args=args, timeout_s=int(self.confirm_timeout)))
        try:
            return await asyncio.wait_for(fut, self.confirm_timeout)
        except asyncio.TimeoutError:
            await self.send(Status(text="confirmation timed out -> denied"))
            return False  # нет ответа = отказ
        finally:
            self.pending.pop(id, None)
