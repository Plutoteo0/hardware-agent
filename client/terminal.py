"""Терминальный клиент. Запуск: .venv/bin/python client/terminal.py

Играет роль будущего T-Embed: «экран» — печать в консоль,
«энкодер» — клавиши y/n в ответ на карточку подтверждения.
"""
import asyncio
import json
import os
import sys

import websockets

URL = os.environ.get("AGENT_URL", "ws://127.0.0.1:8000/ws")


async def read_server(ws, state: dict):
    async for raw in ws:
        m = json.loads(raw)
        t = m["type"]
        if t == "confirm_request":
            state["confirm"] = m
            print(f"\n[CONFIRM {m['id']}] {m['tool']}({json.dumps(m['args'], ensure_ascii=False)})"
                  f"  y/n? ({m['timeout_s']}s)")
        elif t == "status":
            state["confirm"] = None
            print(f"  ... {m['text']}")
        elif t == "result":
            print(f"\n[RESULT] {m['text']}\n")
        elif t == "error":
            print(f"\n[ERROR] {m['text']}\n")


# async def ask_user(ws, confirm_q: asyncio.Queue):
#     loop = asyncio.get_running_loop()
#     while True:
#         m = await confirm_q.get()
#         print(f"\n[CONFIRM {m['id']}] {m['tool']}({json.dumps(m['args'], ensure_ascii=False)})"
#               f"  y/n? ({m['timeout_s']}s)")
#         # input() блокирующий, поэтому в executor, чтобы не стопорить сеть
#         answer = await loop.run_in_executor(None, sys.stdin.readline)
#         approve = answer.strip().lower() == "y"
#         await ws.send(json.dumps({"type": "decision", "id": m["id"], "approve": approve}))


async def main():
    state = {"confirm": None}
    async with websockets.connect(URL) as ws:
        asyncio.create_task(read_server(ws, state))
        print("Команды: ls | read <path> | write <path> <text> | /cancel")
        loop = asyncio.get_running_loop()
        while True:
            line = (await loop.run_in_executor(None, sys.stdin.readline)).strip()
            if not line:
                continue
            if state["confirm"] is not None:
                approve = line.lower() == "y"
                await ws.send(json.dumps({"type": "decision", "id": state["confirm"]["id"], "approve": approve}))
                state["confirm"] = None
                continue
            msg = {"type": "cancel"} if line == "/cancel" else {"type": "task", "text": line}
            await ws.send(json.dumps(msg))
if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
