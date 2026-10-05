"""«Модель» — всё, что умеет принять историю и вернуть следующий шаг.

Шаг — это либо вызов тула {"tool": ..., "args": {...}},
либо финальный ответ {"final": "..."}.

Агентный цикл знает только этот интерфейс, поэтому MockModel потом
заменяется на OllamaModel без изменений в остальном коде.
"""
import json
import os
from typing import Protocol

import httpx

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "qwen3:8b")

SYSTEM_PROMPT = """Ты агент, который работает с файлами в песочнице.
На каждом шаге отвечай ТОЛЬКО одним JSON-объектом, без пояснений вокруг.
Доступные тулы:
  {"tool": "list_tree", "args": {"path": "."}}
  {"tool": "read_file", "args": {"path": "имя/файла"}}
  {"tool": "write_file", "args": {"path": "имя/файла", "content": "текст"}}
Имена файлов бери ТОЧНО как в list_tree (с расширением). Если не знаешь имени, сначала вызови list_tree.
Если спрашивают, какие файлы есть или сколько их, отвечай по результату list_tree и не читай файлы.
Если результат тула начинается с DENIED, пользователь отказал или тул запрещён. Так и скажи: «запись отменена пользователем». Не придумывай других причин.
Когда задача решена, ответь так:
  {"final": "короткий ответ пользователю"}
Других тулов не существует. Результаты тулов приходят сообщениями с ролью tool."""


class Model(Protocol):
    async def next_step(self, history: list[dict]) -> dict: ...


class ModelError(Exception):
    pass


def parse_step(raw: str) -> dict:
    """Проверяет JSON от модели. Плохой ответ — ошибка, а не молчаливый провал."""
    try:
        step = json.loads(raw)
    except json.JSONDecodeError as e:
        raise ModelError(f"model returned invalid JSON: {e}")
    if not isinstance(step, dict):
        raise ModelError(f"model step is not an object: {raw[:100]}")
    if "final" in step:
        return {"final": str(step["final"])}
    if "tool" in step and isinstance(step.get("args", {}), dict):
        return {"tool": step["tool"], "args": step.get("args", {})}
    raise ModelError(f"unknown step shape: {raw[:100]}")


class OllamaModel:
    """Модель через HTTP API Ollama (/api/chat).

    format="json" заставляет Ollama выдавать только валидный JSON.
    Это не гарантирует правильный тул, поэтому parse_step проверяет ответ.
    """

    def __init__(self, model: str = OLLAMA_MODEL, url: str = OLLAMA_URL, num_ctx: int = 8192):
        self.model = model
        self.num_ctx = num_ctx
        self.client = httpx.AsyncClient(base_url=url, timeout=120)

    async def next_step(self, history: list[dict]) -> dict:
        messages = [{"role": "system", "content": SYSTEM_PROMPT}]
        for h in history:
            if h["role"] == "tool":
                # Маленькие модели плохо понимают роль tool, поэтому результат
                # подаём как сообщение пользователя с явной подписью.
                messages.append({"role": "user", "content": f"Результат тула:\n{h['content']}"})
            else:
                messages.append({"role": h["role"], "content": h["content"]})

        resp = await self.client.post("/api/chat", json={
            "model": self.model,
            "messages": messages,
            "stream": False,
            "think": False,  # qwen3 иначе уходит в "размышление" и отдаёт пустой content
            "format": "json",
            "options": {"num_ctx": self.num_ctx, "temperature": 0},
        })
        resp.raise_for_status()
        return parse_step(resp.json()["message"]["content"])


class MockModel:
    """Заглушка без нейросети. Понимает три команды:

        ls                  -> list_tree
        read <path>         -> read_file
        write <path> <text> -> write_file

    После результата тула отвечает финальной фразой. Нужна, чтобы
    отлаживать поток запрос -> подтверждение -> результат без Ollama.
    """

    async def next_step(self, history: list[dict]) -> dict:
        last = history[-1]
        if last["role"] == "tool":
            return {"final": f"Готово: {last['content'][:200]}"}

        text = last["content"].strip()
        if text == "ls":
            return {"tool": "list_tree", "args": {}}
        if text.startswith("read "):
            return {"tool": "read_file", "args": {"path": text[5:].strip()}}
        if text.startswith("write "):
            parts = text.split(" ", 2)
            if len(parts) == 3:
                return {"tool": "write_file", "args": {"path": parts[1], "content": parts[2]}}
        return {"final": "Не понял. Команды: ls, read <path>, write <path> <text>"}
