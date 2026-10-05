"""Быстрые команды и защита от выдуманных действий."""
import asyncio

import pytest

import pc
from protocol import Result
from quick import quick_command
from session import Session


@pytest.mark.parametrize("text,action", [
    ("Поставь на паузу.", "play_pause"),
    ("поставь YouTube на паузу", "play_pause"),
    ("Поставь музыку на ютубе на паузу", "play_pause"),
    ("продолжи", "play_pause"),
    ("Пауза", "play_pause"),
    ("останови", "play_pause"),
    ("выключи музыку", "play_pause"),
    ("включи обратно", "play_pause"),
    ("Следующий трек", "next"),
    ("предыдущая песня", "previous"),
    ("Сделай погромче", "volume_up"),
    ("потише пожалуйста", "volume_down"),
    ("выключи звук", "mute"),
])
def test_quick_commands(text, action):
    assert quick_command(text)["args"]["action"] == action


def test_volume_amount():
    assert quick_command("громче")["args"]["times"] == 5          # 10% по умолчанию
    assert quick_command("громче на 30")["args"]["times"] == 15   # 30% = 15 нажатий по 2%


@pytest.mark.parametrize("text,action", [
    ("дальше", "next"),
    ("стоп", "play_pause"),
    ("стоп пожалуйста", "play_pause"),
    ("продолжи музыку", "play_pause"),
    ("останови видео", "play_pause"),
    ("пропусти трек", "next"),
])
def test_ambiguous_words_in_media_context(text, action):
    assert quick_command(text)["args"]["action"] == action


@pytest.mark.parametrize("text", [
    "Включи на ютубе какую-нибудь песню фьючер",                   # это к модели: youtube
    "расскажи дальше",                                             # не следующий трек
    "продолжи рассказ",                                            # не пауза
    "стоп я не то сказал",
    "останови таймер",
    "Расскажи, как сделать паузу в программе на ESP32 правильно",  # длинное — к модели
    "Какая погода во Вроцлаве?",
])
def test_not_quick(text):
    assert quick_command(text) is None


class ScriptModel:
    def __init__(self, steps):
        self.steps = list(steps)
        self.calls = 0

    async def next_step(self, history):
        self.calls += 1
        return self.steps.pop(0)


def run_session(model, text, monkeypatch):
    pressed = []
    monkeypatch.setattr(pc, "_press", lambda vk: pressed.append(vk))

    async def go():
        sent = []

        async def send(m):
            sent.append(m)

        s = Session(model, send)
        s.start_task(text)
        await s.task
        return sent

    return asyncio.run(go()), pressed


def test_quick_command_skips_model(monkeypatch):
    model = ScriptModel([])
    sent, pressed = run_session(model, "поставь на паузу", monkeypatch)
    assert model.calls == 0 and pressed == [pc.MEDIA_KEYS["play_pause"]]
    assert [m.text for m in sent if isinstance(m, Result)] == ["Готово."]


def test_fake_action_is_challenged(monkeypatch):
    # Сначала врёт, после замечания — вызывает тул
    model = ScriptModel([
        {"final": "Steam запущен."},
        {"tool": "media", "args": {"action": "mute"}},
        {"final": "Звук выключен."},
    ])
    sent, pressed = run_session(model, "сделай так чтобы было тихо совсем пожалуйста сейчас", monkeypatch)
    assert pressed == [pc.MEDIA_KEYS["mute"]]
    assert [m.text for m in sent if isinstance(m, Result)] == ["Звук выключен."]


def test_honest_answer_without_tools_is_fine():
    model = ScriptModel([{"final": "Привет! Всё хорошо."}])

    async def go():
        sent = []

        async def send(m):
            sent.append(m)

        s = Session(model, send)
        s.start_task("как дела")
        await s.task
        return sent

    assert [m.text for m in asyncio.run(go()) if isinstance(m, Result)] == ["Привет! Всё хорошо."]
    assert model.calls == 1


@pytest.mark.parametrize("text,claim", [
    ("Python — открытый язык программирования.", False),
    ("Включение питания делается кнопкой.", False),
    ("Steam запущен.", True),
    ("Я открыл YouTube.", True),
    ("Пауза установлена.", True),
])
def test_action_claim_whole_words(text, claim):
    from session import ACTION_CLAIM
    assert bool(ACTION_CLAIM.search(text)) is claim



def test_answer_from_snippets_is_sent_to_read_page(monkeypatch):
    import session as sess
    calls = []
    monkeypatch.setattr(sess, "run_tool", lambda name, args, root=None: calls.append(name) or f"{name} ok")
    model = ScriptModel([
        {"tool": "web_search", "args": {"query": "версия"}},
        {"final": "Версия 1.2.3"},                                   # по сниппету — не принимаем
        {"tool": "fetch_url", "args": {"url": "https://example.com"}},
        {"final": "Версия 1.2.4 (example.com)"},
    ])

    async def go():
        sent = []

        async def send(m):
            sent.append(m)

        s = Session(model, send)
        s.start_task("поищи последнюю версию")
        await s.task
        return sent

    sent = asyncio.run(go())
    assert calls == ["web_search", "fetch_url"]
    assert [m.text for m in sent if isinstance(m, Result)] == ["Версия 1.2.4 (example.com)"]
