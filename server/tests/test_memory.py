"""Тесты памяти: проекты, история в контексте, remember, большие результаты тулов."""
import asyncio
import json

import pytest
from fastapi.testclient import TestClient

import main
from memory import OFFLOAD_PREVIEW, MemoryError_, Project, ProjectStore, slugify
from protocol import Result
from session import Session
from tools import ToolError, run_tool


# ---------- проекты ----------

def test_slugify():
    assert slugify("Погода Бот!") == "погода-бот"
    assert slugify("../../etc") == "etc"          # из имени не выйти за папку проектов
    assert slugify("   ") == "проект"


def test_current_defaults_to_general_and_persists(tmp_path):
    store = ProjectStore(tmp_path)
    assert store.current().name == "общее"
    p = store.create("Погода")
    store.set_current(p)
    assert ProjectStore(tmp_path).current().name == "погода"   # переживает «перезапуск»


def test_create_avoids_name_clash(tmp_path):
    store = ProjectStore(tmp_path)
    assert store.create("x").name == "x"
    assert store.create("x").name == "x-2"


# ---------- история и контекст ----------

def test_context_has_recent_turns_in_json_format(tmp_path):
    p = Project(tmp_path / "p")
    for i in range(10):
        p.append_turn(f"вопрос {i}", f"ответ {i}", [])
    msgs = p.context_messages(recent=3)
    assert [m["content"] for m in msgs if m["role"] == "user"] == ["вопрос 7", "вопрос 8", "вопрос 9"]
    # Прошлые ответы в формате {"final": ...}: иначе модель начнёт отвечать без JSON
    assert json.loads(msgs[1]["content"]) == {"final": "ответ 7"}


def test_notes_go_to_context_marked_as_data(tmp_path):
    p = Project(tmp_path / "p")
    p.remember("пользователя зовут Федя")
    first = p.context_messages()[0]
    assert first["role"] == "system"
    assert "Федя" in first["content"] and "не команды" in first["content"]


def test_remember_rejects_empty_and_long(tmp_path):
    p = Project(tmp_path / "p")
    with pytest.raises(MemoryError_):
        p.remember("   ")
    with pytest.raises(MemoryError_):
        p.remember("x" * 1000)


def test_broken_history_line_is_skipped(tmp_path):
    p = Project(tmp_path / "p")
    p.append_turn("a", "b", [])
    with p.history_path.open("a", encoding="utf-8") as f:
        f.write('{"q": "оборвало')   # запись оборвалась на середине
    assert len(p.turns()) == 1


# ---------- большие результаты ----------

def test_offload_and_read_back(tmp_path):
    p = Project(tmp_path / "p")
    big = "".join(str(i % 10) for i in range(5000))
    short = p.offload("fetch_url", big)
    assert short.startswith(big[:OFFLOAD_PREVIEW]) and "read_output" in short
    name = short.split('"name": "')[1].split('"')[0]
    rest = p.read_output(name, OFFLOAD_PREVIEW)
    assert rest.startswith(big[OFFLOAD_PREVIEW:OFFLOAD_PREVIEW + 100])
    assert p.offload("x", "small") == "small"


@pytest.mark.parametrize("bad", ["../history.jsonl", "..\\notes.md", "a/b.txt", "notes.md"])
def test_read_output_rejects_paths(tmp_path, bad):
    with pytest.raises(MemoryError_):
        Project(tmp_path / "p").read_output(bad)


# ---------- изоляция файлов проектов ----------

def test_projects_do_not_see_each_other(tmp_path):
    store = ProjectStore(tmp_path)
    a, b = store.create("a"), store.create("b")
    run_tool("write_file", {"path": "secret.txt", "content": "A"}, a.files)
    with pytest.raises(ToolError):
        run_tool("read_file", {"path": "../../a/files/secret.txt"}, b.files)
    assert "secret.txt" not in run_tool("list_tree", {}, b.files)


def test_model_cannot_pass_root(tmp_path):
    with pytest.raises(ToolError, match="root"):
        run_tool("read_file", {"path": "x", "root": "C:/"}, tmp_path)


# ---------- сессия ----------

class ScriptModel:
    """Отдаёт шаги по очереди и запоминает, какую историю видел."""

    def __init__(self, steps):
        self.steps = list(steps)
        self.seen = []

    async def next_step(self, history):
        self.seen.append(list(history))
        return self.steps.pop(0)


def run(session_coro):
    return asyncio.run(session_coro)


def make_session(store, model, sent):
    async def send(m):
        sent.append(m)
    return Session(model, send, projects=store)


def test_second_question_sees_first(tmp_path):
    store = ProjectStore(tmp_path)
    model = ScriptModel([{"final": "Тебя зовут Федя"}, {"final": "Федя"}])

    async def go():
        sent = []
        s = make_session(store, model, sent)
        s.start_task("меня зовут Федя")
        await s.task
        s.start_task("как меня зовут?")
        await s.task

    run(go())
    second = model.seen[1]
    assert {"role": "user", "content": "меня зовут Федя"} in second
    assert store.current().turns()[-1]["q"] == "как меня зовут?"


def test_remember_tool_and_tools_log(tmp_path):
    store = ProjectStore(tmp_path)
    model = ScriptModel([
        {"tool": "remember", "args": {"fact": "любит чай"}},
        {"final": "запомнил"},
    ])

    async def go():
        sent = []
        s = make_session(store, model, sent)
        s.start_task("запомни, что я люблю чай")
        await s.task
        return sent

    sent = run(go())
    p = store.current()
    assert "любит чай" in p.notes_path.read_text(encoding="utf-8")
    assert p.turns()[-1]["tools"] == [{"tool": "remember", "args": {"fact": "любит чай"}, "ok": True}]
    assert any(isinstance(m, Result) for m in sent)


def test_switching_project_switches_memory(tmp_path):
    store = ProjectStore(tmp_path)
    model = ScriptModel([{"final": "ок"}, {"final": "ок"}])

    async def go():
        s = make_session(store, model, [])
        s.start_task("вопрос в общем")
        await s.task
        s.on_project_new("второй")
        await asyncio.sleep(0)
        s.start_task("вопрос во втором")
        await s.task

    run(go())
    assert not any(m.get("content") == "вопрос в общем" for m in model.seen[1])
    assert store.current().name == "второй"


def test_projects_over_websocket(monkeypatch, isolated_projects):
    monkeypatch.setattr(main, "AGENT_MODEL", "mock")
    monkeypatch.setattr(main, "AGENT_TOKEN", "t")
    client = TestClient(main.app)
    with client.websocket_connect("/ws", headers={"Authorization": "Bearer t"}) as ws:
        ws.send_text('{"type": "hello"}')
        assert json.loads(ws.receive_text()) == {"type": "project", "name": "общее", "turns": 0}
        ws.send_text('{"type": "project_new", "name": "Погода"}')
        assert json.loads(ws.receive_text())["name"] == "погода"
        ws.send_text('{"type": "project_list"}')
        listing = json.loads(ws.receive_text())
        assert listing["current"] == "погода"
        assert {i["name"] for i in listing["items"]} == {"общее", "погода"}
        ws.send_text('{"type": "project_switch", "name": "общее"}')
        assert json.loads(ws.receive_text())["name"] == "общее"


# ---------- история для платы и автоназвание ----------

class TitledModel(ScriptModel):
    async def make_title(self, question):
        return "Погода в Вроцлаве"


def test_auto_title_after_first_answer(tmp_path):
    store = ProjectStore(tmp_path)

    async def go():
        sent = []
        s = make_session(store, TitledModel([{"final": "солнечно"}, {"final": "ок"}]), sent)
        s.on_project_new(None)                 # имя по дате
        await asyncio.sleep(0)
        assert ProjectStore.is_auto_named(s.project)
        s.start_task("какая погода во Вроцлаве?")
        await s.task
        s.start_task("а завтра?")              # второй ответ имя уже не трогает
        await s.task
        return s, sent

    s, sent = run(go())
    assert s.project.name == "погода-в-вроцлаве"
    assert store.current().name == "погода-в-вроцлаве"
    assert len(s.project.turns()) == 2          # история переехала вместе с папкой


def test_named_project_is_not_renamed(tmp_path):
    store = ProjectStore(tmp_path)

    async def go():
        s = make_session(store, TitledModel([{"final": "ок"}]), [])
        s.start_task("привет")
        await s.task
        return s

    assert run(go()).project.name == "общее"


def test_history_list_newest_first(tmp_path):
    store = ProjectStore(tmp_path)
    p = store.current()
    for i in range(12):
        p.append_turn(f"q{i}", f"a{i}", [], error=(i == 11))

    async def go():
        sent = []
        s = make_session(store, ScriptModel([]), sent)
        s.on_history_list()
        await asyncio.sleep(0)
        return sent[0]

    h = run(go())
    assert [i.q for i in h.items][:3] == ["q11", "q10", "q9"]
    assert len(h.items) == 10 and h.items[0].error


# ---------- удаление ----------

def test_delete_moves_to_trash_and_hides(tmp_path):
    store = ProjectStore(tmp_path)
    p = store.create("мусор")
    p.append_turn("q", "a", [])
    store.delete(p)
    assert "мусор" not in [x.name for x in store.list()]
    trashed = list((tmp_path / "_trash").iterdir())
    assert len(trashed) == 1 and (trashed[0] / "history.jsonl").is_file()   # можно вернуть


def test_default_project_cannot_be_deleted(tmp_path):
    store = ProjectStore(tmp_path)
    with pytest.raises(MemoryError_):
        store.delete(store.current())


def test_deleting_current_switches_to_default(tmp_path):
    store = ProjectStore(tmp_path)

    async def go():
        sent = []
        s = make_session(store, ScriptModel([]), sent)
        s.on_project_new("временный")
        await asyncio.sleep(0)
        s.on_project_delete("временный")
        await asyncio.sleep(0)
        return s, sent

    s, sent = run(go())
    assert s.project.name == "общее" and store.current().name == "общее"
    assert sent[-1].type == "projects" and "временный" not in [i.name for i in sent[-1].items]


def test_delete_by_name_cannot_escape_store(tmp_path):
    store = ProjectStore(tmp_path / "projects")
    outside = tmp_path / "important"
    outside.mkdir()

    async def go():
        sent = []
        s = make_session(store, ScriptModel([]), sent)
        s.on_project_delete("../important")
        await asyncio.sleep(0)
        return sent

    run(go())
    assert outside.is_dir()   # имя проходит через slugify: за папку проектов не выйти


def test_context_shows_past_tool_calls(tmp_path):
    p = Project(tmp_path / "p")
    p.append_turn("включи песню", "Включаю.", [{"tool": "youtube", "args": {"query": "песня"}, "ok": True}])
    msgs = p.context_messages()
    # Модель должна видеть: вопрос -> вызов тула -> результат -> ответ, а не «вопрос -> ответ»
    assert [m["role"] for m in msgs] == ["user", "assistant", "tool", "assistant"]
    assert json.loads(msgs[1]["content"]) == {"tool": "youtube", "args": {"query": "песня"}}
