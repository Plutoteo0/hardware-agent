"""Тулы для интернета: web_search (DuckDuckGo без ключа) и fetch_url.

Страницы из интернета недоверенные: в них может быть текст, похожий на
инструкции для модели. Поэтому результат режем по длине, а промпт
говорит модели, что это данные, а не команды.
"""
import html
import re
import socket
import ipaddress
from html.parser import HTMLParser
from urllib.parse import parse_qs, unquote, urljoin, urlparse

import httpx

USER_AGENT = "Mozilla/5.0 (compatible; hardware-agent/0.1)"
TIMEOUT_S = 15
MAX_BYTES = 300_000     # сколько скачиваем максимум
MAX_CHARS = 4000        # сколько отдаём модели
MAX_REDIRECTS = 3
ALLOWED_TYPES = ("text/html", "text/plain", "application/json")


# ---------- поиск ----------

def web_search(query: str) -> str:
    """Поиск через HTML-версию DuckDuckGo. Возвращает до 5 результатов."""
    resp = httpx.post(
        "https://html.duckduckgo.com/html/",
        data={"q": query},
        headers={"User-Agent": USER_AGENT},
        timeout=TIMEOUT_S,
        follow_redirects=True,
    )
    resp.raise_for_status()
    body = resp.text

    links = re.findall(r'class="result__a"[^>]*href="([^"]+)"[^>]*>(.*?)</a>', body, re.S)
    snippets = re.findall(r'class="result__snippet"[^>]*>(.*?)</a>', body, re.S)

    lines = []
    for i, (href, title_html) in enumerate(links[:5]):
        url = _unwrap_ddg_link(html.unescape(href))
        title = _strip_tags(title_html)
        snippet = _strip_tags(snippets[i]) if i < len(snippets) else ""
        lines.append(f"{i + 1}. {title}\n   {url}\n   {snippet}")

    return "\n".join(lines) or "ничего не найдено"


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
    """Скачивает страницу и возвращает её текст. Закрыт для внутренних адресов."""
    current = url
    for _ in range(MAX_REDIRECTS + 1):
        check_url(current)
        with httpx.stream(
            "GET", current,
            headers={"User-Agent": USER_AGENT},
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
            text = html_to_text(text)
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


class _TextExtractor(HTMLParser):
    SKIP = {"script", "style", "noscript", "head"}

    def __init__(self):
        super().__init__()
        self.parts: list[str] = []
        self.skip_depth = 0

    def handle_starttag(self, tag, attrs):
        if tag in self.SKIP:
            self.skip_depth += 1

    def handle_endtag(self, tag):
        if tag in self.SKIP and self.skip_depth:
            self.skip_depth -= 1

    def handle_data(self, data):
        if not self.skip_depth:
            self.parts.append(data)


def html_to_text(page: str) -> str:
    parser = _TextExtractor()
    parser.feed(page)
    text = " ".join(parser.parts)
    return re.sub(r"\s+", " ", text).strip()
