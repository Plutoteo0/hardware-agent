"""Протокол между сервером и клиентами (терминал, потом ESP32).

Каждое сообщение — JSON-объект с полем "type". Модели ниже описывают
поля каждого типа, pydantic проверяет их при разборе.
"""
from typing import Annotated, Literal, Union

from pydantic import BaseModel, Field, TypeAdapter


# ---- клиент -> сервер ----

class Task(BaseModel):
    type: Literal["task"] = "task"
    text: str


class Decision(BaseModel):
    """Ответ на confirm_request. Связь с запросом — через id."""
    type: Literal["decision"] = "decision"
    id: str
    approve: bool


class Cancel(BaseModel):
    type: Literal["cancel"] = "cancel"


# ---- сервер -> клиент ----

class Status(BaseModel):
    type: Literal["status"] = "status"
    text: str


class ConfirmRequest(BaseModel):
    """Карточка подтверждения. tool и args — реальный вызов, не пересказ модели."""
    type: Literal["confirm_request"] = "confirm_request"
    id: str
    tool: str
    args: dict
    risk: Literal["ask"] = "ask"
    timeout_s: int


class Result(BaseModel):
    type: Literal["result"] = "result"
    text: str


class Error(BaseModel):
    type: Literal["error"] = "error"
    text: str


ClientMessage = Annotated[Union[Task, Decision, Cancel], Field(discriminator="type")]
client_adapter = TypeAdapter(ClientMessage)
