"""Быстрые команды: короткие фразы про музыку выполняем без модели.

Зачем: маленькая модель иногда отвечает «пауза установлена», не вызвав тул,
особенно если в истории уже есть такой ход. Для частых простых команд надёжнее
и быстрее (без ~2 с «думаю») распознать фразу правилом и сразу нажать клавишу.
Всё остальное, длинное или сложное, идёт к модели как обычно.
"""
import re

MAX_WORDS = 6   # «поставь музыку на ютубе на паузу» — да, «включи песню про паузу в жизни...» — нет

# Однозначные фразы: срабатывают всегда (если фраза короткая, см. MAX_WORDS).
# Порядок важен: сначала более конкретные правила
RULES = [
    ("next", re.compile(r"(следующ\w* (трек|песн\w*|видео)|переключи (трек|песню))")),
    ("previous", re.compile(r"(предыдущ\w*|назад трек|верни прошл\w*)")),
    ("mute", re.compile(r"(выключи звук|без звука|убери звук)")),
    ("volume_up", re.compile(r"(погромче|громче|прибавь|увеличь громкость)")),
    ("volume_down", re.compile(r"(потише|тише|убавь|уменьши громкость)")),
    ("play_pause", re.compile(r"(пауз\w*|сними с паузы|выключи (музыку|видео|песню|трек)|верни музыку|включи обратно)")),
]

# Неоднозначные слова: «расскажи дальше», «продолжи рассказ», «стоп, я не то сказал»
# — это не про музыку. Срабатывают, только если слово в фразе одно («дальше», «стоп»,
# «стоп пожалуйста») или в ней есть музыка/видео/трек.
AMBIGUOUS = [
    ("next", re.compile(r"(дальше|пропусти|следующ\w*|переключи)")),
    ("play_pause", re.compile(r"(стоп|останов\w*|продолж\w*|возобнов\w*)")),
]
FILLER = {"пожалуйста", "давай", "ну", "а", "ок", "окей", "плиз"}
MEDIA_WORDS = re.compile(r"(музык\w*|видео|трек\w*|песн\w*|ютуб\w*|youtube|плеер\w*|спотифа\w*)")

REPLIES = {
    "play_pause": "Готово.",
    "next": "Следующий трек.",
    "previous": "Предыдущий трек.",
    "volume_up": "Громче.",
    "volume_down": "Тише.",
    "mute": "Звук выключен.",
}

NUMBER = re.compile(r"\b(\d{1,3})\b")


def quick_command(text: str) -> dict | None:
    """Фраза -> вызов тула media или None, если это не быстрая команда."""
    clean = re.sub(r"[^\w\s]", " ", text.lower())
    words = clean.split()
    if len(words) > MAX_WORDS:
        return None

    action = next((a for a, pat in RULES if pat.search(clean)), None)
    alone = len([w for w in words if w not in FILLER]) == 1
    if action is None and (alone or MEDIA_WORDS.search(clean)):
        action = next((a for a, pat in AMBIGUOUS if pat.search(clean)), None)
    if action is None:
        return None

    args = {"action": action}
    if action.startswith("volume"):
        m = NUMBER.search(clean)
        # «громче на 20» -> 20% -> 10 нажатий по 2%. Без числа — 10%
        args["times"] = max(1, int(m.group(1)) // 2) if m else 5
    return {"tool": "media", "args": args}
