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


class Hello(BaseModel):
    """Первое сообщение клиента: что он умеет. speech=True — присылать озвучку ответов.

    Без hello звук не шлём: терминальный клиент не умеет двоичные кадры.
    """
    type: Literal["hello"] = "hello"
    speech: bool = False


class AudioStart(BaseModel):
    """Начало голосовой задачи. Дальше идут двоичные кадры: PCM 16 бит, 16 кГц, моно."""
    type: Literal["audio_start"] = "audio_start"


class AudioEnd(BaseModel):
    """Кнопку отпустили: звук закончился, можно распознавать."""
    type: Literal["audio_end"] = "audio_end"


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


class Transcript(BaseModel):
    """Что сервер услышал. Показываем пользователю до ответа модели."""
    type: Literal["transcript"] = "transcript"
    text: str


class SpeechStart(BaseModel):
    """Дальше идут двоичные кадры с озвучкой ответа: PCM 16 бит, моно, sample_rate Гц."""
    type: Literal["speech_start"] = "speech_start"
    sample_rate: int


class SpeechEnd(BaseModel):
    type: Literal["speech_end"] = "speech_end"


ClientMessage = Annotated[
    Union[Task, Decision, Cancel, Hello, AudioStart, AudioEnd], Field(discriminator="type")
]
client_adapter = TypeAdapter(ClientMessage)
