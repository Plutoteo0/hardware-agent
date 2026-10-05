"""FastAPI + WebSocket. Запуск: uvicorn main:app --app-dir server"""
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from pydantic import BaseModel, ValidationError

from models import MockModel
from protocol import Cancel, Decision, Error, Task, client_adapter
from session import Session
from tools import SANDBOX

SANDBOX.mkdir(exist_ok=True)
app = FastAPI()


@app.get("/health")
def health():
    return {"ok": True}


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()

    async def send(msg: BaseModel) -> None:
        await ws.send_text(msg.model_dump_json())

    session = Session(MockModel(), send)
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
