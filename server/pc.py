"""Тулы, которые что-то делают на этом ПК: браузер, программы, музыка, погода.

Произвольную командную строку агенту НЕ даём. Он читает страницы из интернета,
а там может быть написано «выполни команду ...», и маленькая модель может
послушаться. Поэтому только конкретные действия и программы из списка APPS.
Пути к программам найдены на этом ПК; для другого ПК их нужно поправить.
"""
import html
import os
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import quote_plus, urlparse

import httpx

from web import USER_AGENT

CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
LOCAL = os.environ.get("LOCALAPPDATA", "")

# Программы, которые можно запустить. "{project}" — папка файлов текущего проекта.
APPS = {
    "vscode": [rf"{LOCAL}\Programs\Microsoft VS Code\Code.exe", "{project}"],
    "chrome": [CHROME],
    "steam": [r"C:\Program Files (x86)\Steam\steam.exe"],
    "discord": [rf"{LOCAL}\Discord\Update.exe", "--processStart", "Discord.exe"],
    "explorer": ["explorer.exe", "{project}"],
    "calculator": ["calc.exe"],
}

# Медиаклавиши Windows (виртуальные коды клавиш, как на мультимедийной клавиатуре)
MEDIA_KEYS = {
    "play_pause": 0xB3,
    "next": 0xB0,
    "previous": 0xB1,
    "volume_up": 0xAF,
    "volume_down": 0xAE,
    "mute": 0xAD,
}


class PcError(Exception):
    pass


def _windows_only() -> None:
    if sys.platform != "win32":
        raise PcError("this tool works only on Windows")


def _launch(argv: list[str]) -> None:
    """Запуск без командной оболочки: аргументы не интерпретируются как команды."""
    _windows_only()
    flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    subprocess.Popen(argv, creationflags=flags, close_fds=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _open_in_chrome(url: str) -> None:
    _launch([CHROME, url])


# ---------- браузер ----------

def open_search(query: str) -> str:
    query = str(query).strip()
    if not query:
        raise PcError("empty query")
    _open_in_chrome("https://www.google.com/search?q=" + quote_plus(query))
    return f"открыл в Chrome поиск: {query}"


def open_url(url: str) -> str:
    if urlparse(str(url)).scheme not in ("http", "https"):
        raise PcError(f"only http/https links: {url}")
    _open_in_chrome(url)
    return f"открыл в Chrome: {url}"


def find_youtube_video(query: str) -> tuple[str, str] | None:
    """Первое видео из поиска YouTube: (id, название). Без ключа API: из HTML страницы."""
    resp = httpx.get(
        "https://www.youtube.com/results",
        params={"search_query": query},
        headers={"User-Agent": USER_AGENT, "Accept-Language": "ru,en;q=0.8"},
        timeout=15,
    )
    resp.raise_for_status()
    m = re.search(r'"videoId":"([\w-]{11})"', resp.text)
    if not m:
        return None
    video_id = m.group(1)
    title = video_id
    try:  # название — через oEmbed, официальный адрес YouTube для превью ссылок
        o = httpx.get("https://www.youtube.com/oembed",
                      params={"url": f"https://www.youtube.com/watch?v={video_id}", "format": "json"},
                      timeout=10)
        if o.status_code == 200:
            title = html.unescape(o.json().get("title", video_id))
    except httpx.HTTPError:
        pass
    return video_id, title


def youtube(query: str) -> str:
    query = str(query).strip()
    if not query:
        raise PcError("empty query")
    found = find_youtube_video(query)
    if found is None:
        _open_in_chrome("https://www.youtube.com/results?search_query=" + quote_plus(query))
        return f"видео не нашёл, открыл поиск YouTube: {query}"
    video_id, title = found
    _open_in_chrome(f"https://www.youtube.com/watch?v={video_id}")
    return f"открыл на YouTube: {title}"


# ---------- программы ----------

def open_app(name: str, *, root: Path | None = None) -> str:
    key = str(name).strip().lower()
    if key not in APPS:
        raise PcError(f"unknown app '{name}'. Allowed: {', '.join(APPS)}")
    project = str(root) if root else os.path.expanduser("~")
    argv = [project if a == "{project}" else a for a in APPS[key]]
    exe = argv[0]
    if os.path.isabs(exe) and not Path(exe).is_file():
        raise PcError(f"{key} is not installed at {exe}")
    _launch(argv)
    return f"запустил {key}"


# ---------- музыка и громкость ----------

def _press(vk: int) -> None:
    _windows_only()
    import ctypes
    user32 = ctypes.windll.user32
    user32.keybd_event(vk, 0, 0, 0)      # нажать
    user32.keybd_event(vk, 0, 2, 0)      # отпустить (KEYEVENTF_KEYUP)


def media(action: str, times: int = 1) -> str:
    action = str(action).strip().lower()
    if action not in MEDIA_KEYS:
        raise PcError(f"unknown action '{action}'. Allowed: {', '.join(MEDIA_KEYS)}")
    # Одно нажатие громкости = 2%. Ограничиваем, чтобы не оглушить
    times = max(1, min(int(times), 25))
    for _ in range(times if action.startswith("volume") else 1):
        _press(MEDIA_KEYS[action])
    return f"нажал {action}" + (f" x{times}" if action.startswith("volume") else "")


# ---------- погода ----------

WEATHER_CODES = {
    0: "ясно", 1: "в основном ясно", 2: "переменная облачность", 3: "пасмурно",
    45: "туман", 48: "изморозь", 51: "лёгкая морось", 53: "морось", 55: "сильная морось",
    61: "небольшой дождь", 63: "дождь", 65: "сильный дождь", 66: "ледяной дождь", 67: "сильный ледяной дождь",
    71: "небольшой снег", 73: "снег", 75: "сильный снег", 77: "снежная крупа",
    80: "ливень", 81: "сильный ливень", 82: "очень сильный ливень",
    85: "снегопад", 86: "сильный снегопад", 95: "гроза", 96: "гроза с градом", 99: "сильная гроза с градом",
}


def describe_weather(place: str, data: dict) -> str:
    cur = data["current"]
    daily = data["daily"]
    lines = [
        f"{place}: сейчас {cur['temperature_2m']:.0f}°C (ощущается как {cur['apparent_temperature']:.0f}°C), "
        f"{WEATHER_CODES.get(cur['weather_code'], 'погода неизвестна')}, ветер {cur['wind_speed_10m']:.0f} км/ч."
    ]
    names = ["сегодня", "завтра", "послезавтра"]
    for i, day in enumerate(daily["time"][:3]):
        lines.append(
            f"{names[i]} ({day}): {daily['temperature_2m_min'][i]:.0f}..{daily['temperature_2m_max'][i]:.0f}°C, "
            f"{WEATHER_CODES.get(daily['weather_code'][i], '?')}, "
            f"осадки {daily['precipitation_probability_max'][i]}%"
        )
    return "\n".join(lines)


def weather(city: str) -> str:
    """Open-Meteo: бесплатно и без ключа. Сначала город -> координаты, потом прогноз."""
    city = str(city).strip()
    if not city:
        raise PcError("empty city")
    geo = httpx.get("https://geocoding-api.open-meteo.com/v1/search",
                    params={"name": city, "count": 1, "language": "ru"}, timeout=15)
    geo.raise_for_status()
    results = geo.json().get("results") or []
    if not results:
        raise PcError(f"city not found: {city}")
    g = results[0]
    fc = httpx.get("https://api.open-meteo.com/v1/forecast", params={
        "latitude": g["latitude"], "longitude": g["longitude"],
        "current": "temperature_2m,apparent_temperature,weather_code,wind_speed_10m",
        "daily": "temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code",
        "timezone": "auto", "forecast_days": 3,
    }, timeout=15)
    fc.raise_for_status()
    place = g["name"] + (f", {g['country']}" if g.get("country") else "")
    return describe_weather(place, fc.json())
