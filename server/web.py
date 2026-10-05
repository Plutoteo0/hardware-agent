"""Тулы для интернета: web_search (DuckDuckGo без ключа) и fetch_url.

Страницы из интернета недоверенные: в них может быть текст, похожий на
инструкции для модели. Поэтому результат режем по длине, а промпт
говорит модели, что это данные, а не команды.
"""
import html
import re
import socket
import ipaddress
import time
from html.parser import HTMLParser
from urllib.parse import parse_qs, unquote, urljoin, urlparse

import httpx

USER_AGENT = "Mozilla/5.0 (compatible; hardware-agent/0.1)"   # для поиска DuckDuckGo
# Для чтения страниц — честная подпись программы со ссылкой. Википедия и другие сайты
# с правилами для ботов отвечают 403 на подписи «под браузер», а на такую — 200.
BOT_USER_AGENT = "hardware-agent/0.1 (https://github.com/Plutoteo0/hardware-agent)"
TIMEOUT_S = 15
MAX_BYTES = 2_000_000   # сколько скачиваем максимум: у магазинов один <head> со скриптами бывает 1 МБ
MAX_CHARS = 4000        # сколько отдаём модели
MAX_REDIRECTS = 3
ALLOWED_TYPES = ("text/html", "text/plain", "application/json")


# ---------- поиск ----------

def web_search(query: str) -> str:
    """Поиск DuckDuckGo без ключа. Возвращает до 5 результатов.

    При частых запросах DuckDuckGo иногда отдаёт пустую страницу. Тогда пауза
    и запасной адрес (lite-версия): у неё отдельные ограничения.
    """
    results = _search_html(query)
    if not results:
        time.sleep(1.5)
        results = _search_lite(query)
    if not results:
        return "ничего не найдено (поисковик мог временно ограничить запросы — можно повторить позже)"
    return "\n".join(f"{i + 1}. {title}\n   {url}\n   {snippet}"
                     for i, (title, url, snippet) in enumerate(results[:5]))


def _post(url: str, query: str) -> str:
    resp = httpx.post(url, data={"q": query}, headers={"User-Agent": USER_AGENT},
                      timeout=TIMEOUT_S, follow_redirects=True)
    resp.raise_for_status()
    return resp.text


def _search_html(query: str) -> list[tuple[str, str, str]]:
    body = _post("https://html.duckduckgo.com/html/", query)
    links = re.findall(r'class="result__a"[^>]*href="([^"]+)"[^>]*>(.*?)</a>', body, re.S)
    snippets = re.findall(r'class="result__snippet"[^>]*>(.*?)</a>', body, re.S)
    return [(_strip_tags(t), _unwrap_ddg_link(html.unescape(h)), _strip_tags(snippets[i]) if i < len(snippets) else "")
            for i, (h, t) in enumerate(links)]


def _search_lite(query: str) -> list[tuple[str, str, str]]:
    body = _post("https://lite.duckduckgo.com/lite/", query)
    links = re.findall(r'<a[^>]+class=.result-link.[^>]*>.*?</a>', body, re.S)
    snippets = re.findall(r'<td[^>]+class=.result-snippet.[^>]*>(.*?)</td>', body, re.S)
    out = []
    for i, a in enumerate(links):
        href = re.search(r'href="([^"]+)"', a)
        if href:
            out.append((_strip_tags(a), _unwrap_ddg_link(html.unescape(href.group(1))),
                        _strip_tags(snippets[i]) if i < len(snippets) else ""))
    return out


def _unwrap_ddg_link(href: str) -> str:
    """DuckDuckGo иногда оборачивает ссылку: //duckduckgo.com/l/?uddg=<настоящий URL>."""
    if href.startswith("//"):
        href = "https:" + href
    qs = parse_qs(urlparse(href).query)
    if "uddg" in qs:
        return unquote(qs["uddg"][0])
    return href


def _strip_tags(fragment: str) -> str:
    return html.unescape(re.sub(r"<[^>]+>", "", fragment)).strip()


# ---------- открытие страницы ----------

def fetch_url(url: str) -> str:
    """Скачивает страницу и возвращает её текст. Закрыт для внутренних адресов.

    Сначала честная подпись бота; если сайт отказал (403) — ещё раз с подписью «под браузер»:
    одним сайтам нужна первая, другим вторая.
    """
    try:
        return _fetch(url, BOT_USER_AGENT)
    except httpx.HTTPStatusError as e:
        if e.response.status_code != 403:
            raise
        return _fetch(url, USER_AGENT)


def _fetch(url: str, user_agent: str) -> str:
    current = url
    for _ in range(MAX_REDIRECTS + 1):
        check_url(current)
        with httpx.stream(
            "GET", current,
            headers={"User-Agent": user_agent},
            timeout=TIMEOUT_S,
            follow_redirects=False,
        ) as resp:
            if resp.is_redirect:
                # Редирект проверяем сами, иначе можно уйти во внутреннюю сеть через 302
                current = urljoin(current, resp.headers["location"])
                continue
            resp.raise_for_status()
            ctype = resp.headers.get("content-type", "").split(";")[0].strip()
            if ctype not in ALLOWED_TYPES:
                raise ValueError(f"unsupported content type: {ctype or 'unknown'}")
            raw = _read_capped(resp)
            encoding = resp.encoding or "utf-8"
        text = raw.decode(encoding, errors="replace")
        if ctype == "text/html":
            text = page_text(text)
        return text[:MAX_CHARS]
    raise ValueError("too many redirects")


def _read_capped(resp: httpx.Response) -> bytes:
    chunks, total = [], 0
    for chunk in resp.iter_bytes():
        total += len(chunk)
        chunks.append(chunk)
        if total >= MAX_BYTES:
            break
    return b"".join(chunks)[:MAX_BYTES]


def check_url(url: str) -> None:
    """Пускаем только http(s) на публичные адреса.

    Имя хоста превращаем в IP и проверяем сам адрес: так не обойти проверку
    записью вида 'localhost' или 'my.router.lan'.
    Ограничение: DNS может вернуть другой адрес при самом запросе (DNS rebinding).
    Для домашнего агента этого достаточно, но это стоит помнить.
    """
    parsed = urlparse(url)
    if parsed.scheme not in ("http", "https"):
        raise ValueError(f"only http/https allowed: {url}")
    if not parsed.hostname:
        raise ValueError(f"no host in url: {url}")
    try:
        infos = socket.getaddrinfo(parsed.hostname, parsed.port or 443)
    except socket.gaierror as e:
        raise ValueError(f"cannot resolve host {parsed.hostname}: {e}")
    for info in infos:
        ip = ipaddress.ip_address(info[4][0])
        if ip.is_private or ip.is_loopback or ip.is_link_local or ip.is_reserved or ip.is_multicast or ip.is_unspecified:
            raise ValueError(f"address not allowed: {parsed.hostname} -> {ip}")


def page_text(page: str) -> str:
    """Текст страницы для модели.

    Основной путь — trafilatura: готовая библиотека, которая вытаскивает саму статью
    из произвольного сайта (без меню, рекламы, подвала). Под отдельные сайты ничего не подгоняем.
    На страницах без «статьи» (оглавления, списки ссылок) она возвращает почти пусто —
    тогда простой запасной вариант html_to_text.
    """
    import trafilatura  # тяжёлый импорт, только когда нужен
    try:
        text = trafilatura.extract(page, include_comments=False, include_tables=True) or ""
    except Exception:
        text = ""
    return text if len(text) >= 500 else html_to_text(page)


class _TextExtractor(HTMLParser):
    """Запасной простой парсер (см. page_text). Дорабатывать под сайты не нужно."""

    ALWAYS_SKIP = {"script", "style", "noscript", "head", "template", "svg"}
    LAYOUT = {"nav", "header", "footer", "aside", "form", "button"}   # меню, шапка, подвал, панели
    CONTENT = {"main", "article"}

    def __init__(self, skip_layout: bool = True):
        super().__init__()
        self.skip = self.ALWAYS_SKIP | (self.LAYOUT if skip_layout else set())
        self.parts: list[str] = []
        self.main_parts: list[str] = []
        self.skip_depth = 0
        self.main_depth = 0

    def handle_starttag(self, tag, attrs):
        if tag in self.skip:
            self.skip_depth += 1
        if tag in self.CONTENT:
            self.main_depth += 1

    def handle_endtag(self, tag):
        if tag in self.skip and self.skip_depth:
            self.skip_depth -= 1
        if tag in self.CONTENT and self.main_depth:
            self.main_depth -= 1

    def handle_data(self, data):
        if self.skip_depth:
            return
        self.parts.append(data)
        if self.main_depth:
            self.main_parts.append(data)


def _join(parts: list[str]) -> str:
    return re.sub(r"\s+", " ", " ".join(parts)).strip()


def html_to_text(page: str) -> str:
    parser = _TextExtractor()
    parser.feed(page)
    main, whole = _join(parser.main_parts), _join(parser.parts)
    if len(main) >= 500:          # есть нормальный <main>/<article> — берём только статью
        return main
    if len(whole) >= 200:
        return whole
    # Кривая разметка (незакрытый <nav> «съел» всю страницу) — без пропуска меню
    lenient = _TextExtractor(skip_layout=False)
    lenient.feed(page)
    return _join(lenient.parts)
