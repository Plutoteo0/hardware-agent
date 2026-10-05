"""Тесты озвучки без Piper: синтез подменён заглушкой."""
import asyncio
import json
from datetime import datetime

from fastapi.testclient import TestClient

import main
from models import today_line
from protocol import Result, SpeechEnd, SpeechStart
from session import Session
from tts import MAX_SPEECH_CHARS, speech_text


class FakeTTS:
    def __init__(self, seconds=1.0, rate=22050):
        self.pcm = b"\x01\x00" * int(seconds * rate)
        self.rate = rate
        self.text = None

    def synthesize(self, text):
        self.text = text
        return self.pcm, self.rate


class FinalModel:
    async def next_step(self, history):
        return {"final": "Готово, смотри https://example.com/page"}


def run_session(hello: bool, tts=None):
    async def run():
        events, audio = [], bytearray()

        async def send(m):
            events.append(m)

        async def send_bytes(b):
            audio.extend(b)

        s = Session(FinalModel(), send, tts=tts or FakeTTS(), send_bytes=send_bytes)
        if hello:
            s.on_hello(True)
        s.start_task("hi")
        await s.task
        return events, bytes(audio)

    return asyncio.run(run())


def test_no_hello_means_no_audio():
    # Терминальный клиент hello не шлёт: двоичные кадры его бы сломали
    events, audio = run_session(hello=False)
    assert audio == b""
    assert not any(isinstance(m, SpeechStart) for m in events)


def test_hello_speech_streams_audio_after_result():
    tts = FakeTTS(seconds=1.0)
    events, audio = run_session(hello=True, tts=tts)
    types = [type(m) for m in events]
    assert types == [Result, SpeechStart, SpeechEnd]   # сначала текст, потом звук
    assert events[1].sample_rate == 22050
    assert audio == tts.pcm                            # всё дошло, по порядку
    assert "ссылка" in tts.text and "https" not in tts.text


def test_speech_text_cleanup():
    assert speech_text("**жирный** и `код`") == "жирный и код"
    assert speech_text("см. http://a.b/c?d=1") == "см. ссылка"
    long = "Предложение. " * 100
    out = speech_text(long)
    assert len(out) <= MAX_SPEECH_CHARS + 30 and out.endswith("Остальное на экране.")


def test_today_line_has_weekday():
    assert today_line(datetime(2026, 10, 5, 9, 30)) == "Сейчас 2026-10-05 09:30, понедельник."


def test_hello_over_websocket(monkeypatch):
    tts = FakeTTS(seconds=0.5)
    monkeypatch.setattr(main, "SPEAKER", tts)
    tts.available = lambda: True
    monkeypatch.setattr(main, "AGENT_MODEL", "mock")
    monkeypatch.setattr(main, "AGENT_TOKEN", "t")
    client = TestClient(main.app)
    with client.websocket_connect("/ws", headers={"Authorization": "Bearer t"}) as ws:
        ws.send_text('{"type": "hello", "speech": true}')
        assert json.loads(ws.receive_text())["type"] == "project"   # после hello — текущий проект
        ws.send_text('{"type": "task", "text": "hello"}')
        assert json.loads(ws.receive_text())["type"] == "result"
        assert json.loads(ws.receive_text())["type"] == "speech_start"
        got = bytearray()
        while True:
            frame = ws.receive()
            if frame.get("bytes") is not None:
                got += frame["bytes"]
            else:
                assert json.loads(frame["text"])["type"] == "speech_end"
                break
    assert bytes(got) == tts.pcm
