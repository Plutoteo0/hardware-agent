"""Тесты токена: из сети без правильного токена подключиться нельзя.

TestClient представляется хостом "testclient", то есть "не с этого ПК",
поэтому на нём удобно проверять правила для сети.
"""
import json

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

import main


@pytest.fixture
def client(monkeypatch):
    monkeypatch.setattr(main, "AGENT_TOKEN", "secret-token")
    monkeypatch.setattr(main, "AGENT_MODEL", "mock")
    return TestClient(main.app)


def test_no_token_is_rejected(client):
    with pytest.raises(WebSocketDisconnect):
        with client.websocket_connect("/ws") as ws:
            ws.receive_text()


def test_wrong_token_is_rejected(client):
    with pytest.raises(WebSocketDisconnect):
        with client.websocket_connect("/ws", headers={"Authorization": "Bearer wrong"}) as ws:
            ws.receive_text()


def test_right_token_is_accepted(client):
    with client.websocket_connect("/ws", headers={"Authorization": "Bearer secret-token"}) as ws:
        ws.send_text('{"type": "task", "text": "hello"}')  # MockModel не знает команду -> сразу result
        assert json.loads(ws.receive_text())["type"] == "result"


def test_token_in_url_is_not_accepted(client):
    # Старый способ (токен в адресе) больше не работает: адрес попадает в логи
    with pytest.raises(WebSocketDisconnect):
        with client.websocket_connect("/ws?token=secret-token") as ws:
            ws.receive_text()


def test_empty_server_token_rejects_network(monkeypatch):
    monkeypatch.setattr(main, "AGENT_TOKEN", "")
    c = TestClient(main.app)
    with pytest.raises(WebSocketDisconnect):
        with c.websocket_connect("/ws", headers={"Authorization": "Bearer "}) as ws:
            ws.receive_text()
