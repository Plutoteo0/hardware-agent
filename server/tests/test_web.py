"""Тесты web.py без сети: проверка адресов, разбор HTML, разворачивание ссылок DuckDuckGo."""
import socket

import pytest

import web


def fake_resolve(ip):
    """Подменяем DNS: любое имя резолвится в нужный IP."""
    def _getaddrinfo(host, port, *args, **kwargs):
        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (ip, port or 443))]
    return _getaddrinfo


@pytest.mark.parametrize("ip", ["127.0.0.1", "10.0.0.5", "192.168.1.1", "169.254.169.254"])
def test_internal_addresses_are_blocked(monkeypatch, ip):
    monkeypatch.setattr(socket, "getaddrinfo", fake_resolve(ip))
    with pytest.raises(ValueError, match="not allowed"):
        web.check_url("http://example.com/")


def test_public_address_is_allowed(monkeypatch):
    monkeypatch.setattr(socket, "getaddrinfo", fake_resolve("93.184.216.34"))
    web.check_url("https://example.com/")  # не поднимает исключение


@pytest.mark.parametrize("url", ["file:///etc/passwd", "ftp://example.com/x", "gopher://example.com"])
def test_non_http_schemes_are_blocked(url):
    with pytest.raises(ValueError, match="only http"):
        web.check_url(url)


def test_html_to_text_skips_scripts_and_styles():
    page = "<html><head><title>T</title></head><body><script>evil()</script><p>Привет</p><style>x{}</style><p>мир</p></body></html>"
    assert web.html_to_text(page) == "Привет мир"


def test_ddg_redirect_link_is_unwrapped():
    href = "//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fpage&rut=abc"
    assert web._unwrap_ddg_link(href) == "https://example.com/page"


def test_plain_link_is_kept():
    assert web._unwrap_ddg_link("https://example.com/") == "https://example.com/"
