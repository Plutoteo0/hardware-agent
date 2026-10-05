"""FastAPI + WebSocket.

Только для этого ПК:  uvicorn main:app --app-dir server --port 8001
Для T-Embed по Wi-Fi: uvicorn main:app --app-dir server --port 8001 --host 0.0.0.0
"""
import os
import secrets
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from pydantic import BaseModel, ValidationError

from models import MockModel, Model, OllamaModel
from protocol import Cancel, Decision, Error, Task, client_adapter
from session import Session
from tools import SANDBOX


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
app = FastAPI()

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

    session = Session(make_model(), send)
    try:
        # Этот цикл обязан не блокироваться: пока агент ждёт подтверждения,
        # тут должно прийти Decision. Поэтому задача запускается через
        # create_task (в session.start_task), а не через await.
        while True:
            raw = await ws.receive_text()
            try:
                msg = client_adapter.validate_json(raw)
            except ValidationError:
                await send(Error(text="invalid message"))
                continue

            if isinstance(msg, Task):
                session.start_task(msg.text)
            elif isinstance(msg, Decision):
                session.on_decision(msg.id, msg.approve)
            elif isinstance(msg, Cancel):
                session.cancel()
    except WebSocketDisconnect:
        pass
    finally:
        session.close()
