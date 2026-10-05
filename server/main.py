"""FastAPI + WebSocket.

Только для этого ПК:  uvicorn main:app --app-dir server --port 8001
Для T-Embed по Wi-Fi: uvicorn main:app --app-dir server --port 8001 --host 0.0.0.0
"""
import os
import secrets
import threading
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from pydantic import BaseModel, ValidationError

from models import MockModel, Model, OllamaModel
from memory import ProjectStore
from protocol import (AudioEnd, AudioStart, Cancel, Decision, Error, Hello, ProjectList, ProjectNew,
                      ProjectSwitch, Task, client_adapter)
from session import Session
from stt import WhisperTranscriber
from tools import SANDBOX
from tts import PiperSpeaker


def load_dotenv(path: Path) -> None:
    """Читает KEY=VALUE из .env. Переменные, заданные в системе, важнее файла."""
    if not path.is_file():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip())


load_dotenv(Path(__file__).resolve().parent.parent / ".env")

SANDBOX.mkdir(exist_ok=True)

# Одна модель Whisper и один голос Piper на все подключения: они большие,
# грузить их на каждого клиента незачем
TRANSCRIBER = WhisperTranscriber()
SPEAKER = PiperSpeaker()

# Проекты: sandbox/projects/<имя>/ (см. memory.py)
PROJECTS = ProjectStore(SANDBOX / "projects")


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Грузим модели в фоне при старте: сервер сразу принимает подключения,
    # а первая голосовая задача не ждёт загрузку
    if os.environ.get("STT_PRELOAD", "1") == "1":
        threading.Thread(target=TRANSCRIBER.preload, daemon=True).start()
        threading.Thread(target=SPEAKER.preload, daemon=True).start()
    yield


app = FastAPI(lifespan=lifespan)

# AGENT_MODEL=ollama (по умолчанию) или mock — для отладки без Ollama.
AGENT_MODEL = os.environ.get("AGENT_MODEL", "ollama")

# Токен для подключений из сети (T-Embed). Подключения с самого ПК пускаем без него:
# терминальный клиент работает как раньше, а из сети без токена агент недоступен.
AGENT_TOKEN = os.environ.get("AGENT_TOKEN", "")
LOOPBACK = {"127.0.0.1", "::1"}


def is_allowed(ws: WebSocket) -> bool:
    if ws.client and ws.client.host in LOOPBACK:
        return True
    if not AGENT_TOKEN:
        return False  # токен не настроен — из сети не пускаем никого
    # Токен в заголовке, а не в адресе: адрес uvicorn пишет в лог, заголовки — нет
    auth = ws.headers.get("authorization", "")
    given = auth.removeprefix("Bearer ").strip()
    # compare_digest сравнивает за одинаковое время: по скорости ответа токен не подобрать
    return secrets.compare_digest(given, AGENT_TOKEN)


def make_model() -> Model:
    if AGENT_MODEL == "mock":
        return MockModel()
    return OllamaModel()


@app.get("/health")
def health():
    return {"ok": True}


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    if not is_allowed(ws):
        await ws.close(code=1008)  # 1008 = policy violation; до accept клиент получит отказ
        return
    await ws.accept()

    async def send(msg: BaseModel) -> None:
        await ws.send_text(msg.model_dump_json())

    async def send_bytes(data: bytes) -> None:
        await ws.send_bytes(data)

    session = Session(make_model(), send, stt=TRANSCRIBER,
                      tts=SPEAKER if SPEAKER.available() else None, send_bytes=send_bytes,
                      projects=PROJECTS)
    try:
        # Этот цикл обязан не блокироваться: пока агент ждёт подтверждения,
        # тут должно прийти Decision. Поэтому задача запускается через
        # create_task (в session.start_task), а не через await.
        while True:
            # receive() вместо receive_text(): кадр бывает и текстом (JSON), и байтами (звук)
            frame = await ws.receive()
            if frame["type"] == "websocket.disconnect":
                break
            if frame.get("bytes") is not None:
                session.on_audio_chunk(frame["bytes"])
                continue

            try:
                msg = client_adapter.validate_json(frame.get("text") or "")
            except ValidationError:
                await send(Error(text="invalid message"))
                continue

            if isinstance(msg, Task):
                session.start_task(msg.text)
            elif isinstance(msg, Decision):
                session.on_decision(msg.id, msg.approve)
            elif isinstance(msg, Cancel):
                session.cancel()
            elif isinstance(msg, Hello):
                session.on_hello(msg.speech)
            elif isinstance(msg, ProjectList):
                session.on_project_list()
            elif isinstance(msg, ProjectSwitch):
                session.on_project_switch(msg.name)
            elif isinstance(msg, ProjectNew):
                session.on_project_new(msg.name)
            elif isinstance(msg, AudioStart):
                session.on_audio_start()
            elif isinstance(msg, AudioEnd):
                session.on_audio_end()
    except WebSocketDisconnect:
        pass
    finally:
        session.close()
