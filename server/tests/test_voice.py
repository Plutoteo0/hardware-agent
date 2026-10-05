"""Тесты голосового пути без Whisper: распознавание подменено заглушкой."""
import asyncio
import json

import pytest
from fastapi.testclient import TestClient

import main
from protocol import Error, Result, Transcript
from session import Session
from stt import MAX_BYTES


class FakeSTT:
    def __init__(self, text: str):
        self.text = text
        self.got: bytes | None = None

    def transcribe(self, pcm: bytes) -> str:
        self.got = pcm
        return self.text


class EchoModel:
    """Сразу отвечает тем, что услышал: проверяем, что текст дошёл до модели."""

    async def next_step(self, history):
        return {"final": f"heard: {history[0]['content']}"}


def run_voice(stt, chunks, end=True):
    async def run():
        sent = []

        async def send(m):
            sent.append(m)

        s = Session(EchoModel(), send, stt=stt)
        s.on_audio_start()
        for c in chunks:
            s.on_audio_chunk(c)
        if end:
            s.on_audio_end()
        if s.task:
            await s.task
        return sent

    return asyncio.run(run())


def test_audio_is_transcribed_and_sent_to_model():
    stt = FakeSTT("что в песочнице")
    sent = run_voice(stt, [b"\x01\x00" * 100, b"\x02\x00" * 100])
    assert stt.got == b"\x01\x00" * 100 + b"\x02\x00" * 100   # куски склеены по порядку
    assert any(isinstance(m, Transcript) and m.text == "что в песочнице" for m in sent)
    assert any(isinstance(m, Result) and m.text == "heard: что в песочнице" for m in sent)


def test_empty_transcript_gives_error_and_no_task():
    sent = run_voice(FakeSTT(""), [b"\x00\x00" * 100])
    assert any(isinstance(m, Error) for m in sent)
    assert not any(isinstance(m, Result) for m in sent)


def test_audio_is_capped():
    stt = FakeSTT("x")
    run_voice(stt, [b"\x00" * (MAX_BYTES // 2)] * 3)
    assert len(stt.got) == MAX_BYTES


def test_chunks_without_start_are_ignored():
    async def run():
        async def send(m):
            pass
        s = Session(EchoModel(), send, stt=FakeSTT("x"))
        s.on_audio_chunk(b"\x00\x00")
        return s.audio

    assert asyncio.run(run()) is None


def test_voice_over_websocket(monkeypatch):
    """Весь путь через настоящий /ws: audio_start, байты, audio_end."""
    stt = FakeSTT("привет")
    monkeypatch.setattr(main, "TRANSCRIBER", stt)
    monkeypatch.setattr(main, "AGENT_MODEL", "mock")
    monkeypatch.setattr(main, "AGENT_TOKEN", "t")
    client = TestClient(main.app)
    with client.websocket_connect("/ws", headers={"Authorization": "Bearer t"}) as ws:
        ws.send_text('{"type": "audio_start"}')
        ws.send_bytes(b"\x10\x00" * 50)
        ws.send_text('{"type": "audio_end"}')
        types = []
        while True:
            m = json.loads(ws.receive_text())
            types.append(m["type"])
            if m["type"] == "transcript":
                assert m["text"] == "привет"
            if m["type"] in ("result", "error"):
                break
    assert stt.got == b"\x10\x00" * 50
    assert types[-2:] == ["transcript", "result"]
