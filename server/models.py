"""«Модель» — всё, что умеет принять историю и вернуть следующий шаг.

Шаг — это либо вызов тула {"tool": ..., "args": {...}},
либо финальный ответ {"final": "..."}.

Агентный цикл знает только этот интерфейс, поэтому MockModel потом
заменяется на OllamaModel без изменений в остальном коде.
"""
from typing import Protocol


class Model(Protocol):
    async def next_step(self, history: list[dict]) -> dict: ...


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
