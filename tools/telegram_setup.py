"""One-time Telegram setup for the proposal notices (docs/proposals-up.md).

Run it yourself in a normal PowerShell window, after making the bot with
@BotFather and sending the bot one message:

    python tools/telegram_setup.py --wrangler-dir push-server

It asks for the bot token without showing it, finds your chat with the bot
from the message you sent, and stores the token, the chat id and a new
webhook secret as Worker secrets in one `wrangler secret bulk` call (stdin;
this makes a new deployment of the push server). Then it points the bot's
Undo buttons at the push server and sends a test message.

The token is never printed, logged or written to a file. Run it again to
change the bot or the chat.
"""
import argparse
import getpass
import json
import os
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from push_release import load_config  # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

API = "https://api.telegram.org"


def tg(token, method, body=None):
    req = urllib.request.Request(f"{API}/bot{token}/{method}",
                                 data=json.dumps(body or {}).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            out = json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        out = json.loads(e.read().decode("utf-8", "replace") or "{}")
    except urllib.error.URLError as e:
        # The address holds the token: report the reason only.
        raise SystemExit(f"Cannot reach Telegram: {e.reason}")
    if not out.get("ok"):
        raise SystemExit(f"Telegram refused {method}: {out.get('description', 'no reason given')}")
    return out["result"]


def find_chat(token, wait):
    tg(token, "deleteWebhook")  # getUpdates works only without a webhook
    deadline = time.time() + wait
    offset = 0
    while time.time() < deadline:
        for u in tg(token, "getUpdates", {"offset": offset, "timeout": 20}):
            offset = u["update_id"] + 1
            chat = (u.get("message") or {}).get("chat") or {}
            if chat.get("type") == "private":
                return chat
        print("Waiting for a message to the bot ...")
    raise SystemExit("No message to the bot arrived. Send it any message and run this again.")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wrangler-dir", required=True, help="the push-server folder")
    ap.add_argument("--wait", type=int, default=120, help="seconds to wait for your message to the bot")
    args = ap.parse_args(argv)
    cfg = load_config()

    token = getpass.getpass("Bot token from @BotFather (not shown): ").strip()
    if not token:
        raise SystemExit("No token given.")
    bot = tg(token, "getMe")
    print(f"Bot: @{bot.get('username')}")
    chat = find_chat(token, args.wait)
    print(f"Chat: {chat.get('first_name', '')} {chat.get('last_name', '')}".rstrip())

    hook_secret = secrets.token_urlsafe(32)
    values = {"TELEGRAM_BOT_TOKEN": token, "TELEGRAM_CHAT_ID": str(chat["id"]),
              "TELEGRAM_WEBHOOK_SECRET": hook_secret}
    cmd = ["npx.cmd" if os.name == "nt" else "npx", "wrangler", "secret", "bulk"]
    r = subprocess.run(cmd, input=json.dumps(values), text=True, cwd=args.wrangler_dir)
    if r.returncode:
        raise SystemExit("wrangler could not store the secrets; nothing else was changed.")

    url = cfg["server"].rstrip("/") + f"/v1/{cfg['church']}/telegram"
    tg(token, "setWebhook", {"url": url, "secret_token": hook_secret, "allowed_updates": ["callback_query"]})
    tg(token, "sendMessage", {"chat_id": chat["id"],
                              "text": "CGF Projector: song changes from the church laptops will appear here."})
    print("Done: the bot sent you a test message.")


if __name__ == "__main__":
    main()
