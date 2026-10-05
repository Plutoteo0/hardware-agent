"""Тулы агента, уровни риска и проверка путей.

Агент видит только одну папку: папку файлов текущего проекта (root).
Любой путь от модели проходит через safe_path — это главная защита от ../ и симлинков.
Без root работает вся SANDBOX (так было до проектов, так работают старые тесты).
"""
from pathlib import Path

from pc import media, open_app, open_search, open_url, weather, youtube
from web import fetch_url, web_search

SANDBOX = Path(__file__).resolve().parent.parent / "sandbox"

# Уровни риска — данные, не код: легко менять и показывать на экране.
#   auto      — выполняется сразу
#   ask       — нужно подтверждение энкодером
#   forbidden — отказываем всегда
RISK = {
    "list_tree": "auto",
    "read_file": "auto",
    "write_file": "ask",
    "web_search": "auto",  # только чтение: отправляет запрос в поиск
    "fetch_url": "auto",   # только чтение: скачивает страницу, внутренние адреса закрыты в web.py
    "remember": "auto",    # дописывает факт в notes.md проекта (см. memory.py)
    "read_output": "auto", # читает сохранённый большой результат тула
    # ПК (см. pc.py): безвредное — сразу, открыть ссылку и запустить программу — с подтверждением
    "open_search": "auto",
    "youtube": "auto",
    "media": "auto",
    "weather": "auto",
    "open_url": "ask",
    "open_app": "ask",
    "rm": "forbidden",
}


class ToolError(Exception):
    pass


def safe_path(rel: str, root: Path | None = None) -> Path:
    """Превращает путь от модели в абсолютный и проверяет, что он внутри root.

    resolve() раскрывает и '..', и симлинки, поэтому сравниваем уже
    настоящий путь. Проверка по строке ('..' in rel) была бы обходимой.
    """
    root = (root or SANDBOX).resolve()
    full = (root / rel).resolve()
    if full != root and root not in full.parents:
        raise ToolError(f"path outside sandbox: {rel}")
    return full


def list_tree(path: str = ".", *, root: Path | None = None) -> str:
    base = safe_path(path, root)
    if not base.is_dir():
        raise ToolError(f"not a directory: {path}")
    root = (root or SANDBOX).resolve()
    lines = sorted(str(p.relative_to(root)) for p in base.rglob("*"))
    return "\n".join(lines) or "(empty)"


def read_file(path: str, *, root: Path | None = None) -> str:
    f = safe_path(path, root)
    if not f.is_file():
        raise ToolError(f"not a file: {path}")
    # encoding явно: на Windows по умолчанию cp1251, и русский текст превращается в кашу
    return f.read_text(encoding="utf-8", errors="replace")[:20000]  # обрезка, чтобы не забить контекст


def write_file(path: str, content: str, *, root: Path | None = None) -> str:
    f = safe_path(path, root)
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(content, encoding="utf-8")
    return f"wrote {len(content)} chars to {path}"


TOOLS = {
    "list_tree": list_tree,
    "read_file": read_file,
    "write_file": write_file,
    "web_search": web_search,
    "fetch_url": fetch_url,
    "open_search": open_search,
    "open_url": open_url,
    "youtube": youtube,
    "open_app": open_app,
    "media": media,
    "weather": weather,
}


FILE_TOOLS = {"list_tree", "read_file", "write_file", "open_app"}  # им нужна папка проекта


def run_tool(name: str, args: dict, root: Path | None = None) -> str:
    fn = TOOLS.get(name)
    if fn is None:
        raise ToolError(f"unknown tool: {name}")
    # root задаёт сервер, а не модель: иначе модель могла бы выбрать себе другую папку
    if "root" in args:
        raise ToolError("argument 'root' is not allowed")
    try:
        if name in FILE_TOOLS:
            return fn(**args, root=root)
        return fn(**args)
    except TypeError as e:  # неверные аргументы от модели
        raise ToolError(f"bad args for {name}: {e}")
