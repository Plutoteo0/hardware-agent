"""Тесты защиты песочницы: safe_path не должен пускать наружу."""
import os

import pytest

import tools
from tools import ToolError, safe_path


@pytest.fixture
def sandbox(tmp_path, monkeypatch):
    """Подменяем песочницу на временную папку, чтобы не трогать настоящую."""
    root = tmp_path / "sandbox"
    root.mkdir()
    monkeypatch.setattr(tools, "SANDBOX", root)
    return root


def test_normal_path_is_inside_sandbox(sandbox):
    (sandbox / "hello.txt").write_text("hi")
    assert safe_path("hello.txt") == (sandbox / "hello.txt").resolve()


def test_dotdot_escape_is_rejected(sandbox):
    with pytest.raises(ToolError):
        safe_path("../secret.txt")


def test_absolute_path_outside_is_rejected(sandbox, tmp_path):
    outside = tmp_path / "secret.txt"
    outside.write_text("x")
    with pytest.raises(ToolError):
        safe_path(str(outside))


def test_symlink_escape_is_rejected(sandbox, tmp_path):
    outside = tmp_path / "secret.txt"
    outside.write_text("x")
    link = sandbox / "link.txt"
    try:
        os.symlink(outside, link)
    except OSError:
        pytest.skip("на Windows симлинк требует прав администратора или режима разработчика")
    with pytest.raises(ToolError):
        safe_path("link.txt")
