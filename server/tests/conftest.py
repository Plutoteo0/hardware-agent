"""Общие настройки тестов."""
import pytest

import main
from memory import ProjectStore


@pytest.fixture(autouse=True)
def isolated_projects(tmp_path, monkeypatch):
    """Тесты через main.app не должны писать историю в настоящую sandbox/projects."""
    store = ProjectStore(tmp_path / "projects")
    monkeypatch.setattr(main, "PROJECTS", store)
    return store
