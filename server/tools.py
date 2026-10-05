"""Тулы агента, уровни риска и проверка путей.

Агент видит только папку SANDBOX. Любой путь от модели проходит через
safe_path — это главная защита от ../ и симлинков.
"""
from pathlib import Path

SANDBOX = Path(__file__).resolve().parent.parent / "sandbox"

# Уровни риска — данные, не код: легко менять и показывать на экране.
#   auto      — выполняется сразу
#   ask       — нужно подтверждение энкодером
#   forbidden — отказываем всегда
RISK = {
    "list_tree": "auto",
    "read_file": "auto",
    "write_file": "ask",
    "rm": "forbidden",
}


class ToolError(Exception):
    pass


def safe_path(rel: str) -> Path:
    """Превращает путь от модели в абсолютный и проверяет, что он внутри SANDBOX.

    resolve() раскрывает и '..', и симлинки, поэтому сравниваем уже
    настоящий путь. Проверка по строке ('..' in rel) была бы обходимой.
    """
    root = SANDBOX.resolve()
    full = (root / rel).resolve()
    if full != root and root not in full.parents:
        raise ToolError(f"path outside sandbox: {rel}")
    return full


def list_tree(path: str = ".") -> str:
    base = safe_path(path)
    if not base.is_dir():
        raise ToolError(f"not a directory: {path}")
    root = SANDBOX.resolve()
    lines = sorted(str(p.relative_to(root)) for p in base.rglob("*"))
    return "\n".join(lines) or "(empty)"


def read_file(path: str) -> str:
    f = safe_path(path)
    if not f.is_file():
        raise ToolError(f"not a file: {path}")
    # encoding явно: на Windows по умолчанию cp1251, и русский текст превращается в кашу
    return f.read_text(encoding="utf-8", errors="replace")[:20000]  # обрезка, чтобы не забить контекст


def write_file(path: str, content: str) -> str:
    f = safe_path(path)
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(content, encoding="utf-8")
    return f"wrote {len(content)} chars to {path}"


TOOLS = {"list_tree": list_tree, "read_file": read_file, "write_file": write_file}


def run_tool(name: str, args: dict) -> str:
    fn = TOOLS.get(name)
    if fn is None:
        raise ToolError(f"unknown tool: {name}")
    try:
        return fn(**args)
    except TypeError as e:  # неверные аргументы от модели
        raise ToolError(f"bad args for {name}: {e}")
