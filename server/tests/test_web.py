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



def test_search_falls_back_to_lite(monkeypatch):
    monkeypatch.setattr(web, "_search_html", lambda q: [])          # основной отдал пустую страницу
    monkeypatch.setattr(web, "_search_lite", lambda q: [("Заголовок", "https://example.com", "сниппет")])
    monkeypatch.setattr(web.time, "sleep", lambda s: None)
    out = web.web_search("что-то")
    assert out.startswith("1. Заголовок\n   https://example.com")


def test_search_both_empty_says_so(monkeypatch):
    monkeypatch.setattr(web, "_search_html", lambda q: [])
    monkeypatch.setattr(web, "_search_lite", lambda q: [])
    monkeypatch.setattr(web.time, "sleep", lambda s: None)
    assert web.web_search("x").startswith("ничего не найдено")



def test_fetch_retries_with_browser_ua_on_403(monkeypatch):
    import httpx
    tried = []

    def fake_fetch(url, ua):
        tried.append(ua)
        if ua == web.BOT_USER_AGENT:
            req = httpx.Request("GET", url)
            raise httpx.HTTPStatusError("403", request=req, response=httpx.Response(403, request=req))
        return "текст страницы"

    monkeypatch.setattr(web, "_fetch", fake_fetch)
    assert web.fetch_url("https://example.com") == "текст страницы"
    assert tried == [web.BOT_USER_AGENT, web.USER_AGENT]



def test_html_to_text_prefers_article_and_skips_menus():
    body = "Факт. " * 120
    page = f"<html><body><nav>Главное меню Войти</nav><header>Шапка</header><main><h1>Фьючер</h1><p>{body}</p></main><footer>Подвал</footer></body></html>"
    text = web.html_to_text(page)
    assert text.startswith("Фьючер Факт.")
    assert "Главное меню" not in text and "Подвал" not in text


def test_html_to_text_survives_unclosed_nav():
    page = "<html><body><nav>меню <p>" + "Важный текст. " * 30 + "</p></body></html>"   # <nav> не закрыт
    assert "Важный текст" in web.html_to_text(page)



def test_page_text_extracts_article():
    para = "<p>" + "Фьючер — американский рэпер из Атланты. " * 20 + "</p>"
    page = f"<html><body><nav><a href='/'>Главная</a> <a href='/x'>Войти</a></nav><article><h1>Фьючер</h1>{para}</article><footer>© сайт</footer></body></html>"
    text = web.page_text(page)
    assert "американский рэпер" in text and "Войти" not in text


def test_page_text_falls_back_on_link_lists():
    page = "<html><body><ul>" + "".join(f"<li><a href='/{i}'>Раздел {i}</a></li>" for i in range(80)) + "</ul></body></html>"
    assert "Раздел 79" in web.page_text(page)   # «статьи» нет — запасной парсер отдаёт список
