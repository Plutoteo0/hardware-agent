"""Одна сессия = одно WebSocket-подключение клиента.

Здесь агентный цикл и логика подтверждений.
"""
import asyncio
import json
import re
import uuid
from contextlib import suppress
from typing import Awaitable, Callable, Protocol

from pydantic import BaseModel

import time

from memory import MemoryError_, Project, ProjectStore
from models import Model
from quick import REPLIES, quick_command
from protocol import (ConfirmRequest, Error, History, HistoryItem, ProjectInfo, ProjectItem, Projects,
                      Result, SpeechEnd, SpeechStart, Status, Transcript)
from stt import MAX_BYTES
from tools import RISK, run_tool
from tts import speech_text

CONFIRM_TIMEOUT_S = 30

# Модель говорит, что СДЕЛАЛА действие. Если при этом в ходе не было ни одного тула —
# это выдумка: без тула она ничего не может сделать на ПК.
# Только целые слова: «открытый язык», «включение питания» — это не утверждение о действии
ACTION_CLAIM = re.compile(
    r"\b(поставил[аи]?|включил[аи]?|включаю|открыл[аи]?|открываю|запустил[аи]?|запускаю|"
    r"выключил[аи]?|переключил[аи]?|записал[аи]?|сохранил[аи]?|запомнил[аи]?|"
    r"(поставлен|включен|открыт|запущен|установлен|увеличен|уменьшен|выключен|сохранен|сохранён)[аоы]?)\b",
    re.IGNORECASE,
)
# Искал, но не открыл ни одной страницы: сниппеты — обрывки, по ним модель путает факты
READ_PAGE_NUDGE = (
    "Ты ответил по коротким сниппетам поиска, не открыв ни одной страницы. Открой самую подходящую ссылку "
    'из результатов через fetch_url ({"tool": "fetch_url", "args": {"url": ...}}) и ответь по ней.'
)

# Не «ты ошибся» (тогда модель извиняется), а прямое указание, что делать дальше
FAKE_ACTION_NUDGE = (
    "Действие ещё не выполнено: тул не был вызван. Сейчас ответь JSON с вызовом нужного тула "
    '({"tool": ..., "args": ...}). Если для просьбы тул не нужен — ответь {"final": ...} без слов о действиях.'
)
MAX_STEPS = 8  # предохранитель от зацикливания модели

# Озвучка: шлём кусками по 0.2 с и не дальше чем на 2 с вперёд реального времени.
# Иначе 20 секунд речи прилетят за долю секунды, и у платы переполнится буфер.
HISTORY_ITEMS = 10          # сколько ходов отдаём плате в список истории
HISTORY_ANSWER_CHARS = 2000 # длинный ответ в списке истории обрезаем

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
                 send_bytes: SendBytes | None = None, projects: ProjectStore | None = None):
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
        # Без projects (старые тесты) — без памяти, тулы видят всю SANDBOX
        self.projects = projects
        self.project: Project | None = projects.current() if projects else None
        self._tools_log: list[dict] = []   # какие тулы вызвала текущая задача (для истории)

    def on_hello(self, speech: bool) -> None:
        self.speech = speech and self.tts is not None and self.send_bytes is not None
        if self.project:
            asyncio.create_task(self.send(self._project_info()))

    # ---- проекты ----

    def _project_info(self) -> ProjectInfo:
        return ProjectInfo(name=self.project.name, turns=len(self.project.turns()))

    def on_project_list(self) -> None:
        if not self.projects:
            return
        items = [ProjectItem(name=p.name, turns=len(p.turns())) for p in self.projects.list()]
        asyncio.create_task(self.send(Projects(current=self.project.name, items=items)))

    def on_project_switch(self, name: str) -> None:
        if not self.projects or self._busy():   # посреди задачи проект не меняем
            return
        project = self.projects.get(name)
        if project is None:
            asyncio.create_task(self.send(Error(text=f"no such project: {name}")))
            return
        self._use(project)

    def on_project_new(self, name: str | None) -> None:
        if not self.projects or self._busy():
            return
        self._use(self.projects.create(name))

    def on_project_delete(self, name: str) -> None:
        if not self.projects or self._busy():
            return
        project = self.projects.get(name)
        if project is None:
            asyncio.create_task(self.send(Error(text=f"no such project: {name}")))
            return
        try:
            self.projects.delete(project)
        except (MemoryError_, OSError) as e:
            asyncio.create_task(self.send(Error(text=f"cannot delete: {e}")))
            return
        if self.project.name == project.name:   # удалили текущий — на «общее»
            self._use(self.projects.current())
        self.on_project_list()

    def on_project_clear(self, name: str) -> None:
        if not self.projects or self._busy():
            return
        project = self.projects.get(name)
        if project is None:
            asyncio.create_task(self.send(Error(text=f"no such project: {name}")))
            return
        try:
            self.projects.clear_history(project)
        except OSError as e:
            asyncio.create_task(self.send(Error(text=f"cannot clear: {e}")))
            return
        if self.project.name == project.name:
            asyncio.create_task(self.send(self._project_info()))   # плате: ходов теперь 0
        self.on_project_list()

    def on_history_list(self) -> None:
        if not self.project:
            return
        items = [HistoryItem(q=t["q"], a=t["a"][:HISTORY_ANSWER_CHARS], error=t.get("error", False))
                 for t in reversed(self.project.turns()[-HISTORY_ITEMS:])]
        asyncio.create_task(self.send(History(items=items)))

    async def _auto_title(self, question: str) -> None:
        """Проект с именем по дате после первого ответа получает настоящее имя."""
        make_title = getattr(self.model, "make_title", None)
        project = self.project
        if not (make_title and self.projects and project and ProjectStore.is_auto_named(project)):
            return
        if len(project.turns()) != 1:
            return
        try:
            title = await make_title(question)
            if not title:
                return
            renamed = self.projects.rename(project, title)
        except Exception:
            return   # не придумалось или не переименовалось — останется имя по дате
        if self.project is project:
            self.project = renamed
            await self.send(self._project_info())

    def _use(self, project: Project) -> None:
        self.project = project
        self.projects.set_current(project)
        asyncio.create_task(self.send(self._project_info()))

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
        # Память проекта (заметки, сводка, последние ходы) идёт перед новым вопросом
        context = self.project.context_messages() if self.project else []
        history = context + [{"role": "user", "content": text}]
        self._tools_log: list[dict] = []
        nudged = False
        nudged_read = False
        try:
            # Короткие команды про музыку — без модели (см. quick.py)
            quick = quick_command(text)
            if quick:
                output = await self._call_tool(quick["tool"], quick["args"])
                ok = not output.startswith(("ERROR", "DENIED"))
                await self._finish(text, REPLIES[quick["args"]["action"]] if ok else output, error=not ok)
                return

            for _ in range(MAX_STEPS):
                step = await self.model.next_step(history)

                if "final" in step:
                    # Утверждает, что сделал, но тулов не было — один раз просим исправиться
                    if not self._tools_log and not nudged and ACTION_CLAIM.search(step["final"]):
                        nudged = True
                        history.append({"role": "assistant", "content": json.dumps(step, ensure_ascii=False)})
                        history.append({"role": "user", "content": FAKE_ACTION_NUDGE})
                        continue
                    # Был поиск, но ни одна страница не открыта — один раз просим прочитать источник
                    used = {c["tool"] for c in self._tools_log}
                    if "web_search" in used and "fetch_url" not in used and not nudged_read:
                        nudged_read = True
                        history.append({"role": "assistant", "content": json.dumps(step, ensure_ascii=False)})
                        history.append({"role": "user", "content": READ_PAGE_NUDGE})
                        continue
                    await self._finish(text, step["final"])
                    return

                name, args = step["tool"], step["args"]
                # Сохраняем и сам вызов, и результат: иначе модель не помнит,
                # что уже делала, и начинает повторяться.
                history.append({"role": "assistant", "content": json.dumps(step, ensure_ascii=False)})
                output = await self._call_tool(name, args)
                history.append({"role": "tool", "content": output})

            self._record(text, "step limit reached", error=True)
            await self.send(Error(text="step limit reached"))
        except asyncio.CancelledError:
            # При отключении клиента сокет уже закрыт, и send может упасть.
            with suppress(Exception):
                await self.send(Status(text="cancelled"))
            raise
        except Exception as e:  # ошибка в задаче не должна ронять соединение
            self._record(text, f"{type(e).__name__}: {e}", error=True)
            await self.send(Error(text=f"{type(e).__name__}: {e}"))

    async def _finish(self, question: str, answer: str, error: bool = False) -> None:
        self._record(question, answer, error)
        await self.send(Error(text=answer) if error else Result(text=answer))
        if error:
            return
        await self._auto_title(question)
        if self.speech:
            # Внутри той же задачи: cancel останавливает и озвучку
            await self._speak(answer)

    def _record(self, question: str, answer: str, error: bool = False) -> None:
        """Записать ход в историю проекта. Пишет сервер, не модель (см. memory.py)."""
        if self.project:
            with suppress(OSError):   # не смогли записать историю — ответ всё равно отдаём
                self.project.append_turn(question, answer, self._tools_log, error)

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
            output = await self._exec_tool(name, args)
            ok = True
        except Exception as e:
            output, ok = f"ERROR: {e}", False
        # В историю — что вызывали, без огромных значений (content файла, текст страницы)
        self._tools_log.append({
            "tool": name,
            "args": {k: (v[:200] if isinstance(v, str) else v) for k, v in args.items()},
            "ok": ok,
        })
        # Большой результат — в файл проекта, модели — начало и как дочитать
        # Поиск и погода и так короткие (5 результатов, 3 дня): их не обрезаем, иначе лишний шаг
        if ok and self.project and name not in ("read_output", "web_search", "weather"):
            output = self.project.offload(name, output)
        return output

    async def _exec_tool(self, name: str, args: dict) -> str:
        # Тулы памяти работают с проектом, а не с файлами песочницы
        if name in ("remember", "read_output"):
            if not self.project:
                raise MemoryError_("memory is not available")
            if name == "remember":
                if set(args) != {"fact"}:
                    raise MemoryError_("remember needs exactly one argument: fact")
                return self.project.remember(args["fact"])
            if not set(args) <= {"name", "offset"} or "name" not in args:
                raise MemoryError_("read_output needs: name, optional offset")
            return self.project.read_output(args["name"], args.get("offset", 0))
        root = self.project.files if self.project else None
        # В отдельном потоке: сетевые тулы могут ждать секунды, а цикл должен принимать Decision/Cancel
        return await asyncio.to_thread(run_tool, name, args, root)

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
