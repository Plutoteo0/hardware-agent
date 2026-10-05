"""Тесты тулов ПК. Запуск программ и нажатия клавиш подменены: тесты ничего не открывают."""
import pytest

import pc
from tools import RISK, run_tool


@pytest.fixture
def launched(monkeypatch):
    calls = []
    monkeypatch.setattr(pc, "_launch", lambda argv: calls.append(argv))
    return calls


@pytest.fixture
def pressed(monkeypatch):
    keys = []
    monkeypatch.setattr(pc, "_press", lambda vk: keys.append(vk))
    return keys


def test_risky_pc_tools_need_confirmation():
    assert RISK["open_url"] == "ask" and RISK["open_app"] == "ask"
    assert RISK["open_search"] == "auto" and RISK["media"] == "auto"


def test_open_search_opens_chrome_with_query(launched):
    pc.open_search("обзор ESP32")
    assert launched == [[pc.CHROME, "https://www.google.com/search?q=%D0%BE%D0%B1%D0%B7%D0%BE%D1%80+ESP32"]]


@pytest.mark.parametrize("bad", ["file:///C:/Windows", "javascript:alert(1)", "C:\\Windows\\notepad.exe"])
def test_open_url_only_http(launched, bad):
    with pytest.raises(pc.PcError):
        pc.open_url(bad)
    assert launched == []


def test_open_app_only_from_list(launched, tmp_path):
    with pytest.raises(pc.PcError, match="Allowed"):
        pc.open_app("cmd")
    with pytest.raises(pc.PcError):
        pc.open_app("powershell -c rm")
    assert launched == []


def test_explorer_opens_project_folder(launched, tmp_path):
    run_tool("open_app", {"name": "explorer"}, tmp_path)   # root — папка проекта, её задаёт сервер
    assert launched == [["explorer.exe", str(tmp_path)]]


def test_media_volume_is_capped(pressed):
    pc.media("volume_up", times=100)
    assert pressed == [pc.MEDIA_KEYS["volume_up"]] * 25
    pressed.clear()
    pc.media("play_pause", times=5)                      # не громкость — одно нажатие
    assert pressed == [pc.MEDIA_KEYS["play_pause"]]
    with pytest.raises(pc.PcError):
        pc.media("shutdown")


def test_youtube_opens_found_video(launched, monkeypatch):
    monkeypatch.setattr(pc, "find_youtube_video", lambda q: ("dQw4w9WgXcQ", "Видео"))
    assert pc.youtube("что-нибудь") == "открыл на YouTube: Видео"
    assert launched == [[pc.CHROME, "https://www.youtube.com/watch?v=dQw4w9WgXcQ"]]


def test_youtube_falls_back_to_search(launched, monkeypatch):
    monkeypatch.setattr(pc, "find_youtube_video", lambda q: None)
    pc.youtube("редкое видео")
    assert "results?search_query=" in launched[0][1]


def test_describe_weather():
    data = {
        "current": {"temperature_2m": 12.4, "apparent_temperature": 10.1, "weather_code": 3, "wind_speed_10m": 14.6},
        "daily": {"time": ["2026-10-05", "2026-10-06", "2026-10-07"],
                  "temperature_2m_min": [7, 8, 6], "temperature_2m_max": [14, 16, 12],
                  "weather_code": [3, 61, 0], "precipitation_probability_max": [10, 80, 0]},
    }
    text = pc.describe_weather("Вроцлав, Польша", data)
    assert text.startswith("Вроцлав, Польша: сейчас 12°C (ощущается как 10°C), пасмурно, ветер 15 км/ч.")
    assert "завтра (2026-10-06): 8..16°C, небольшой дождь, осадки 80%" in text
