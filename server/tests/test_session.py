"""Тесты подтверждений в Session: одобрение, таймаут и чужой id решения."""
import asyncio

import pytest

import tools
from protocol import ConfirmRequest, Status
from session import Session


class ScriptedModel:
    """Вместо Ollama: отдаёт заранее заданные шаги по очереди."""

    def __init__(self, steps):
        self.steps = list(steps)

    async def next_step(self, history):
        return self.steps.pop(0)


WRITE = {"tool": "write_file", "args": {"path": "out.txt", "content": "hi"}}
FINAL = {"final": "готово"}


@pytest.fixture
def sandbox(tmp_path, monkeypatch):
    root = tmp_path / "sandbox"
    root.mkdir()
    monkeypatch.setattr(tools, "SANDBOX", root)
    return root


async def wait_for_confirm(sent):
    """Ждём, пока сессия отправит ConfirmRequest, и возвращаем его."""
    while True:
        for m in sent:
            if isinstance(m, ConfirmRequest):
                return m
        await asyncio.sleep(0.01)


async def wait_for_result(sent):
    while not any(getattr(m, "type", "") in ("result", "error") for m in sent):
        await asyncio.sleep(0.01)


def test_approved_write_creates_file(sandbox):
    async def run():
        sent = []

        async def send(m):
            sent.append(m)

        s = Session(ScriptedModel([WRITE, FINAL]), send, confirm_timeout=2)
        s.start_task("запиши")
        req = await wait_for_confirm(sent)
        s.on_decision(req.id, True)
        await wait_for_result(sent)

    asyncio.run(run())
    assert (sandbox / "out.txt").read_text(encoding="utf-8") == "hi"


def test_timeout_denies_write(sandbox):
    async def run():
        sent = []

        async def send(m):
            sent.append(m)

        s = Session(ScriptedModel([WRITE, FINAL]), send, confirm_timeout=0.1)
        s.start_task("запиши")
        await wait_for_result(sent)
        return sent

    sent = asyncio.run(run())
    assert not (sandbox / "out.txt").exists()
    assert any(isinstance(m, Status) and "timed out" in m.text for m in sent)


def test_foreign_id_is_ignored(sandbox):
    async def run():
        sent = []

        async def send(m):
            sent.append(m)

        s = Session(ScriptedModel([WRITE, FINAL]), send, confirm_timeout=0.1)
        s.start_task("запиши")
        await wait_for_confirm(sent)
        s.on_decision("чужой-id", True)  # решение на несуществующий запрос
        await wait_for_result(sent)
        return sent

    asyncio.run(run())
    assert not (sandbox / "out.txt").exists()
