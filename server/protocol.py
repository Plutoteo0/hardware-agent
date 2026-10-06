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


class ProjectList(BaseModel):
    """Попросить список проектов. Ответ — Projects."""
    type: Literal["project_list"] = "project_list"


class ProjectSwitch(BaseModel):
    type: Literal["project_switch"] = "project_switch"
    name: str


class ProjectNew(BaseModel):
    """Новый проект и сразу переключиться на него. Без имени — имя по дате."""
    type: Literal["project_new"] = "project_new"
    name: str | None = None


class ProjectDelete(BaseModel):
    """Удалить проект (в корзину sandbox/projects/_trash). Ответ — Projects."""
    type: Literal["project_delete"] = "project_delete"
    name: str


class ProjectClear(BaseModel):
    """Очистить историю проекта (файл уходит в _trash). Файлы и заметки остаются.
    Нужно для «общего»: его нельзя удалить, но историю можно начать заново. Ответ — Projects."""
    type: Literal["project_clear"] = "project_clear"
    name: str


class HistoryList(BaseModel):
    """Попросить последние вопросы-ответы текущего проекта. Ответ — History."""
    type: Literal["history_list"] = "history_list"


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


class ProjectItem(BaseModel):
    name: str
    turns: int


class Projects(BaseModel):
    type: Literal["projects"] = "projects"
    current: str
    items: list[ProjectItem]


class ProjectInfo(BaseModel):
    """Текущий проект. Приходит после hello и после переключения."""
    type: Literal["project"] = "project"
    name: str
    turns: int


class HistoryItem(BaseModel):
    q: str
    a: str
    error: bool = False


class History(BaseModel):
    """Новые сверху."""
    type: Literal["history"] = "history"
    items: list[HistoryItem]


class SpeechStart(BaseModel):
    """Дальше идут двоичные кадры с озвучкой ответа: PCM 16 бит, моно, sample_rate Гц."""
    type: Literal["speech_start"] = "speech_start"
    sample_rate: int


class SpeechEnd(BaseModel):
    type: Literal["speech_end"] = "speech_end"


ClientMessage = Annotated[
    Union[Task, Decision, Cancel, Hello, AudioStart, AudioEnd, ProjectList, ProjectSwitch, ProjectNew,
          ProjectDelete, ProjectClear, HistoryList],
    Field(discriminator="type"),
]
client_adapter = TypeAdapter(ClientMessage)
