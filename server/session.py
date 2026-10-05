"""Одна сессия = одно WebSocket-подключение клиента.

Здесь агентный цикл и логика подтверждений.
"""
import asyncio
import uuid
from typing import Awaitable, Callable

from pydantic import BaseModel

from models import Model
from protocol import ConfirmRequest, Error, Result, Status
from tools import RISK, ToolError, run_tool

CONFIRM_TIMEOUT_S = 30
MAX_STEPS = 8  # предохранитель от зацикливания модели

Send = Callable[[BaseModel], Awaitable[None]]


class Session:
    def __init__(self, model: Model, send: Send, confirm_timeout: float = CONFIRM_TIMEOUT_S):
        self.model = model
        self.send = send
        self.confirm_timeout = confirm_timeout
        self.pending: dict[str, asyncio.Future] = {}  # id подтверждения -> ожидание ответа
        self.task: asyncio.Task | None = None

    # ---- входящие сообщения от клиента ----

    def start_task(self, text: str) -> None:
        if self.task and not self.task.done():
            # Асинхронно, поэтому create_task: см. комментарий в main.py
            asyncio.create_task(self.send(Error(text="task already running")))
            return
        self.task = asyncio.create_task(self._run(text))

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

    # ---- агентный цикл ----

    async def _run(self, text: str) -> None:
        history = [{"role": "user", "content": text}]
        try:
            for _ in range(MAX_STEPS):
                step = await self.model.next_step(history)

                if "final" in step:
                    await self.send(Result(text=step["final"]))
                    return

                name, args = step["tool"], step["args"]
                output = await self._call_tool(name, args)
                history.append({"role": "tool", "content": output})

            await self.send(Error(text="step limit reached"))
        except asyncio.CancelledError:
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
        try:
            return run_tool(name, args)
        except ToolError as e:
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
