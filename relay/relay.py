# /// script
# requires-python = ">=3.11"
# dependencies = ["faster-whisper", "numpy"]
# ///
import hashlib
import hmac
import json
import os
import re
import sqlite3
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np
from faster_whisper import WhisperModel

HOST = os.environ.get("RELAY_HOST", "127.0.0.1")
PORT = int(os.environ.get("RELAY_PORT", "8643"))
DEVICE_TOKEN = Path(os.environ["RELAY_TOKEN_FILE"]).read_text().strip()
HERMES_URL = os.environ.get("HERMES_URL", "http://127.0.0.1:8642").rstrip("/")
HERMES_KEY = os.environ.get("API_SERVER_KEY", "")
CARTESIA_KEY = os.environ.get("CARTESIA_API_KEY", "")
CARTESIA_VOICE = os.environ.get("CARTESIA_VOICE_ID", "")
CARTESIA_MODEL = os.environ.get("CARTESIA_MODEL", "sonic-3.5")
CARTESIA_VERSION = os.environ.get("CARTESIA_VERSION", "2026-03-01")
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "base.en")
HERMES_HOME = Path(os.environ.get("HERMES_HOME", Path.home() / ".hermes"))
READ_ITEMS_FILE = Path(os.environ.get("RELAY_READ_FILE", Path.home() / ".config/brick-relay/read-items.json"))

MAX_UPLOAD_BYTES = 16 * 1024 * 1024
MAX_SPOKEN_CHARS = 1500
HERMES_TIMEOUT_SECONDS = 120
HERMES_ATTEMPTS = 4
HERMES_RETRY_SECONDS = 3
SESSION_IDLE_SECONDS = 30 * 60
AUDIO_TTL_SECONDS = 10 * 60
AUDIO_PURGE_INTERVAL_SECONDS = 60
AUDIO_ID = re.compile(r"^/v1/audio/([0-9a-f]{32})\.wav$")
EMOJI = re.compile("[\U0001F000-\U0001FAFF\u2600-\u27BF\uFE0F\u200D]")
TURN_PATH = re.compile(r"^/v1/turn/([0-9a-f]{32})$")
TURN_ACTION_PATH = re.compile(r"^/v1/turn/([0-9a-f]{32})/(stop|speech|approval)$")
CHAT_PATH = re.compile(r"^/v1/chats/([A-Za-z0-9_.:-]{1,96})$")
INBOX_PATH = re.compile(r"^/v1/inbox/([0-9a-f]{16})$")
SESSION_ID = re.compile(r"^[A-Za-z0-9_.:-]{1,96}$")
CHAT_SOURCES = {"telegram": "Telegram", "desktop": "Desktop", "gui": "Desktop", "cli": "CLI", "tui": "CLI",
                "local": "Local", "acp": "Macro", "api_server": "Voice"}
APPROVAL_CHOICES = {"once", "session", "deny"}
MAX_CHATS = 24
SESSIONS_SCANNED = 200
MAX_CHAT_MESSAGES = 30
MAX_ITEM_CHARS = 1000
BACKGROUND_WINDOW_SECONDS = 7 * 24 * 3600
MAX_STEPS_SHOWN = 4
HIDDEN_TOOLS = {"tool_search", "tool_describe", "tool_call"}
TOOL_LABELS = {
    "web_search": "Searching the web",
    "web_extract": "Reading a page",
    "terminal": "Running a command",
    "search_files": "Searching files",
    "read_file": "Reading a file",
    "write_file": "Writing a file",
    "patch": "Editing a file",
    "memory": "Checking memory",
    "zeromem_recall": "Checking memory",
    "session_search": "Searching past chats",
    "skill_view": "Loading skill",
    "delegate_task": "Delegating",
    "vision_analyze": "Analyzing an image",
    "browser_navigate": "Opening a page",
    "browser_exec": "Using the browser",
    "execute_code": "Running code",
    "ha_get_state": "Checking the house",
    "ha_list_entities": "Checking the house",
    "ha_call_service": "Controlling the house",
    **json.loads(os.environ.get("RELAY_TOOL_LABELS", "{}")),
}
DEVICE_RULES = (
    "This turn comes from a handheld with a small screen and speaker. Reply in one to three short "
    "sentences. No markdown, lists, links or emoji. "
    "Finish the whole task in this turn and give the final answer: do not delegate to background "
    "agents, and never say you will get back to me, because nothing can reach this device after "
    "your reply ends."
)
DEVICE_RULES = f"{DEVICE_RULES} {os.environ.get('RELAY_EXTRA_RULES', '')}".strip()

audio_dir = Path(tempfile.mkdtemp(prefix="brick-relay-"))
turn_lock = threading.Lock()
whisper = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
session = {"id": "", "used_at": 0.0}
turns = {}
hermes_model = {"name": ""}


class RelayError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


def decode_left_channel(wav_bytes):
    decoded = subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-i", "pipe:0", "-af", "pan=mono|c0=c0",
         "-ar", "16000", "-f", "s16le", "pipe:1"],
        input=wav_bytes, capture_output=True, timeout=30,
    )
    if decoded.returncode != 0 or not decoded.stdout:
        raise RelayError(400, "That recording didn't work.")
    samples = np.frombuffer(decoded.stdout, dtype="<i2").astype(np.float32) / 32768
    samples -= samples.mean()
    peak = float(np.abs(samples).max())
    return samples * (0.5 / peak) if peak > 0 else samples


def transcribe(wav_bytes):
    segments, _ = whisper.transcribe(decode_left_channel(wav_bytes), vad_filter=False)
    text = " ".join(s.text.strip() for s in segments if s.no_speech_prob < 0.6).strip()
    if not text:
        raise RelayError(422, "heard nothing")
    return text


def hermes_request(path, payload=None, timeout=HERMES_TIMEOUT_SECONDS, idempotency_key=None):
    headers = {"Authorization": f"Bearer {HERMES_KEY}", "Content-Type": "application/json"}
    if idempotency_key:
        headers["Idempotency-Key"] = idempotency_key
    return urllib.request.urlopen(
        urllib.request.Request(
            HERMES_URL + path,
            data=None if payload is None else json.dumps(payload).encode(),
            headers=headers,
        ),
        timeout=timeout,
    )


def hermes_json(path, payload=None, idempotency_key=None):
    attempts = HERMES_ATTEMPTS if payload is None or idempotency_key else 1
    for attempt in range(attempts):
        try:
            with hermes_request(path, payload, idempotency_key=idempotency_key) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            if error.code < 500 or attempt == attempts - 1:
                detail = error.read(300).decode(errors="replace")
                print(f"hermes {path.split('?')[0]} {error.code}: {detail}", flush=True)
                message = "Hermes is offline right now." if error.code >= 500 else "Hermes refused that request."
                raise RelayError(502, message) from error
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            if attempt == attempts - 1:
                raise RelayError(502, "Hermes is offline right now.") from error
        time.sleep(HERMES_RETRY_SECONDS)


def resolve_hermes_model():
    if not hermes_model["name"]:
        hermes_model["name"] = hermes_json("/v1/models")["data"][0]["id"]
    return hermes_model["name"]


def current_session_id():
    now = time.monotonic()
    if not session["id"] or now - session["used_at"] > SESSION_IDLE_SECONDS:
        session["id"] = f"pocket-{uuid.uuid4().hex}"
    session["used_at"] = now
    return session["id"]


def step_label(tool):
    if tool in TOOL_LABELS:
        return TOOL_LABELS[tool]
    return "Using " + tool.removeprefix("mcp__").replace("__", " ").replace("_", " ")


def run_events(run_id):
    try:
        with hermes_request(f"/v1/runs/{run_id}/events") as stream:
            for raw in stream:
                line = raw.decode(errors="replace").strip()
                if line.startswith("data:"):
                    try:
                        yield json.loads(line[5:])
                    except json.JSONDecodeError:
                        continue
    except (urllib.error.URLError, TimeoutError) as error:
        raise RelayError(502, "Hermes is offline right now.") from error


def spoken_messages(session_id):
    quoted = urllib.parse.quote(session_id, safe="")
    messages = hermes_json(f"/api/sessions/{quoted}/messages?limit=200").get("data", [])
    return [m for m in messages if m.get("role") in ("user", "assistant") and isinstance(m.get("content"), str)
            and m["content"].strip() and not m["content"].startswith("[ASYNC")]


def ask_hermes(turn, text, chat):
    run = hermes_json("/v1/runs", {"model": resolve_hermes_model(), "input": text, "instructions": DEVICE_RULES,
                                   "session_id": chat or current_session_id()}, idempotency_key=turn.id)
    turn.update(run_id=run["run_id"])
    for event in run_events(run["run_id"]):
        name = event.get("event")
        if name == "message.delta":
            turn.add_text(event.get("delta") or "")
            continue
        if name != "approval.request" and turn.approval:
            turn.update(state="working", approval="")
        if event.get("tool") in HIDDEN_TOOLS:
            continue
        if name == "approval.request":
            turn.update(state="approval", approval=event.get("command") or event.get("description") or "Allow this action?")
        elif name == "tool.started":
            turn.update(partial="")
            turn.start_step(event.get("tool_call_id"), step_label(event.get("tool", "tool")), event.get("preview", ""))
        elif name in ("tool.completed", "tool.failed"):
            turn.finish_step(event.get("tool_call_id"))
        elif name == "run.completed":
            reply = (event.get("output") or "").strip()
            if reply:
                return reply
            raise RelayError(502, "Hermes didn't answer.")
        elif name in ("run.failed", "run.cancelled"):
            raise RelayError(502, "Hermes stopped before finishing.")
    raise RelayError(502, "Hermes didn't answer.")


def synthesize(text):
    request = urllib.request.Request(
        "https://api.cartesia.ai/tts/bytes",
        data=json.dumps({
            "model_id": CARTESIA_MODEL,
            "transcript": text[:MAX_SPOKEN_CHARS],
            "voice": {"mode": "id", "id": CARTESIA_VOICE},
            "language": "en",
            "output_format": {"container": "wav", "encoding": "pcm_s16le", "sample_rate": 48000},
        }).encode(),
        headers={"Authorization": f"Bearer {CARTESIA_KEY}", "Cartesia-Version": CARTESIA_VERSION,
                 "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read()
    except (urllib.error.URLError, TimeoutError) as error:
        raise RelayError(502, "Read aloud failed.") from error


def purge_old_audio():
    cutoff = time.time() - AUDIO_TTL_SECONDS
    for clip in audio_dir.glob("*.wav"):
        if clip.stat().st_mtime < cutoff:
            clip.unlink(missing_ok=True)


def purge_forever():
    while True:
        purge_old_audio()
        purge_old_turns()
        time.sleep(AUDIO_PURGE_INTERVAL_SECONDS)


class Turn:
    def __init__(self):
        self.id = uuid.uuid4().hex
        self.lock = threading.Lock()
        self.state = "transcribing"
        self.heard = self.reply = self.partial = self.error = self.audio = self.run_id = self.approval = ""
        self.steps = []
        self.created = time.time()

    def update(self, **fields):
        with self.lock:
            self.__dict__.update(fields)

    def add_text(self, delta):
        with self.lock:
            self.partial += delta

    def start_step(self, call_id, label, detail):
        with self.lock:
            self.steps.append({"id": call_id, "label": label, "detail": detail, "done": False})

    def finish_step(self, call_id):
        with self.lock:
            for step in self.steps:
                if step["id"] == call_id:
                    step["done"] = True

    def snapshot(self):
        with self.lock:
            lines = [self.state, one_line(self.heard), one_line(self.reply or self.error or self.approval or self.partial), self.audio]
            lines += [f"{'done' if step['done'] else 'active'}\t{one_line(step['label'])}\t{one_line(step['detail'])}"
                      for step in self.steps[-MAX_STEPS_SHOWN:]]
        return "\n".join(lines) + "\n"


def speak_turn(turn):
    if not (CARTESIA_KEY and CARTESIA_VOICE):
        raise RelayError(501, "Read aloud isn't set up.")
    if not turn.audio:
        clip_id = uuid.uuid4().hex
        (audio_dir / f"{clip_id}.wav").write_bytes(synthesize(turn.reply))
        turn.update(audio=f"/v1/audio/{clip_id}.wav")
    return turn.audio


def run_turn(turn, wav_bytes, speak, chat):
    started = time.monotonic()
    try:
        heard = transcribe(wav_bytes)
        heard_at = time.monotonic()
        turn.update(heard=heard, state="working")
        reply = ask_hermes(turn, heard, chat)
        answered_at = time.monotonic()
        turn.update(reply=reply, state="speaking" if speak else "done")
        if speak:
            speak_turn(turn)
            turn.update(state="done")
        print(f"turn: transcribe {heard_at - started:.1f}s, agent {answered_at - heard_at:.1f}s, "
              f"speech {time.monotonic() - answered_at:.1f}s", flush=True)
    except RelayError as error:
        turn.update(error=error.message, state="failed")
    except Exception as error:
        print(f"turn failed: {type(error).__name__}", flush=True)
        turn.update(error="something went wrong", state="failed")
    finally:
        turn_lock.release()


def purge_old_turns():
    cutoff = time.time() - AUDIO_TTL_SECONDS
    for turn_id, turn in list(turns.items()):
        if turn.created < cutoff:
            turns.pop(turn_id, None)


def field(text):
    return " ".join(str(text).replace("\t", " ").split())


def when_text(timestamp):
    moment = time.localtime(timestamp)
    today = time.localtime()
    if moment.tm_yday == today.tm_yday and moment.tm_year == today.tm_year:
        return time.strftime("%I:%M %p", moment).lstrip("0")
    if time.time() - timestamp < 2 * 86400:
        return "Yesterday"
    return time.strftime("%a" if time.time() - timestamp < 7 * 86400 else "%b %d", moment)


def list_chats():
    sessions = hermes_json(f"/api/sessions?limit={SESSIONS_SCANNED}").get("data", [])
    lines = []
    for item in sessions:
        identifier = item.get("id")
        source = CHAT_SOURCES.get(item.get("source"))
        if not isinstance(identifier, str) or not SESSION_ID.match(identifier):
            continue
        if not source or item.get("hidden") or not item.get("message_count"):
            continue
        if item.get("source") == "api_server" and not identifier.startswith("pocket-"):
            continue
        active = item.get("last_active") or item.get("started_at") or time.time()
        lines.append("\t".join((identifier, field(item.get("title") or "Untitled chat"), source, when_text(active))))
        if len(lines) == MAX_CHATS:
            break
    return "\n".join(lines) + "\n"


def chat_messages(session_id):
    spoken = spoken_messages(session_id)
    return "".join(f"{m['role']}\t{field(m['content'])[:MAX_ITEM_CHARS]}\n" for m in spoken[-MAX_CHAT_MESSAGES:])


def configured_model():
    try:
        config = (HERMES_HOME / "config.yaml").read_text()
    except OSError:
        return "Unknown", ""
    section = re.search(r"^model:\n((?:[ \t]+.*\n)+)", config, re.M)
    block = section.group(1) if section else ""
    name = re.search(r"^\s+default:\s*(\S+)", block, re.M)
    provider = re.search(r"^\s+provider:\s*(\S+)", block, re.M)
    pretty = name.group(1).replace("-", " ").upper().replace(" FLASH", " Flash") if name else "Unknown"
    return pretty, provider.group(1).title() if provider else ""


def routine_summary():
    try:
        jobs = json.loads((HERMES_HOME / "cron" / "jobs.json").read_text()).get("jobs", [])
    except (OSError, json.JSONDecodeError):
        return "Unknown", ""
    active = sum(bool(job.get("enabled")) for job in jobs)
    failing = sum(bool(job.get("enabled")) and job.get("last_status") == "error" for job in jobs)
    paused = len(jobs) - active
    note = f"{failing} failing" if failing else f"{paused} paused" if paused else "All healthy"
    return f"{active} active", note


def achievement_summary():
    try:
        snapshot = json.loads((HERMES_HOME / "plugin-data" / "hermes-achievements" / "scan_snapshot.json").read_text())
        return f"{snapshot['unlocked_count']} of {snapshot['total_count']}"
    except (OSError, json.JSONDecodeError, KeyError):
        return "Unknown"


def agent_text():
    health = hermes_json("/health/detailed")
    platforms = health.get("platforms", {})
    connected = sum(item.get("state") == "connected" for item in platforms.values())
    turns = health.get("metrics_today", {}).get("messages", 0)
    online = health.get("gateway_state") == "running"
    tiles = [
        ("Model", *configured_model()),
        ("Gateway", "Online" if online else "Offline", f"{connected} of {len(platforms)} platforms"),
        ("Routines", *routine_summary()),
        ("Achievements", achievement_summary(), f"{turns} turns today"),
    ]
    return "".join("\t".join(field(part) for part in tile) + "\n" for tile in tiles)


def item_id(*parts):
    return hashlib.sha1("|".join(str(part) for part in parts).encode()).hexdigest()[:16]


def routine_items():
    try:
        jobs = json.loads((HERMES_HOME / "cron" / "jobs.json").read_text()).get("jobs", [])
    except (OSError, json.JSONDecodeError):
        return []
    items = []
    for job in jobs:
        if job.get("enabled") and job.get("last_status") == "error":
            items.append({"id": item_id("routine", job.get("id"), job.get("last_run_at")),
                          "title": f"{job.get('name', 'Routine')} failed", "subtitle": "Routine",
                          "body": job.get("last_error") or "No details.", "time": job.get("last_run_at") or ""})
    return items


def background_items():
    try:
        database = sqlite3.connect(f"file:{HERMES_HOME / 'state.db'}?mode=ro", uri=True, timeout=2)
        rows = database.execute(
            "select delegation_id, completed_at, result_json from async_delegations "
            "where origin_session like '%:api_server:dm:pocket-%' and state = 'completed' and completed_at > ?",
            (time.time() - BACKGROUND_WINDOW_SECONDS,)).fetchall()
        database.close()
    except sqlite3.Error:
        return []
    items = []
    for delegation_id, completed_at, result_json in rows:
        try:
            results = json.loads(result_json or "{}").get("results", [])
        except json.JSONDecodeError:
            results = []
        body = " ".join(result.get("summary", "") for result in results)
        items.append({"id": item_id("background", delegation_id), "title": "Background result",
                      "subtitle": "From a voice chat", "body": body or "No details.", "time": completed_at})
    return items


def item_timestamp(item):
    value = item["time"]
    if isinstance(value, (int, float)):
        return float(value)
    try:
        return datetime.fromisoformat(value).timestamp()
    except (TypeError, ValueError):
        return 0.0


def read_items():
    try:
        return set(json.loads(READ_ITEMS_FILE.read_text()))
    except (OSError, json.JSONDecodeError):
        return set()


def mark_read(identifier):
    seen = read_items() | {identifier}
    READ_ITEMS_FILE.parent.mkdir(parents=True, exist_ok=True)
    READ_ITEMS_FILE.write_text(json.dumps(sorted(seen)))


def inbox_items():
    return sorted(routine_items() + background_items(), key=item_timestamp, reverse=True)


def inbox_text():
    seen = read_items()
    items = inbox_items()
    unread = sum(item["id"] not in seen for item in items)
    lines = [f"unread {unread}"]
    for item in items[:MAX_CHATS]:
        stamp = item_timestamp(item)
        lines.append("\t".join((item["id"], field(item["title"]), field(item["subtitle"]),
                                when_text(stamp) if stamp else "", "0" if item["id"] in seen else "1")))
    return "\n".join(lines) + "\n"


def inbox_body(identifier):
    for item in inbox_items():
        if item["id"] == identifier:
            mark_read(identifier)
            return f"{field(item['title'])}\n{field(item['body'])[:MAX_ITEM_CHARS * 3]}\n"
    return None


def one_line(text):
    return " ".join(EMOJI.sub("", text).split())


class Handler(BaseHTTPRequestHandler):
    server_version = "brick-relay"
    sys_version = ""

    def log_message(self, format, *args):
        print(f"{self.command} {self.path.split('?')[0]} {args[1] if len(args) > 1 else ''}", flush=True)

    def respond(self, status, body, content_type="text/plain; charset=utf-8"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        supplied = self.headers.get("Authorization", "").removeprefix("Bearer ").strip()
        if hmac.compare_digest(supplied.encode(), DEVICE_TOKEN.encode()):
            return True
        self.respond(401, b"unauthorized\n")
        return False

    def relay_error(self, error):
        return self.respond(error.status, f"{error.message}\n".encode())

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        if url.path == "/health":
            return self.respond(200, b"ok\n")
        if not self.authorized():
            return None
        try:
            if url.path == "/v1/chats":
                return self.respond(200, list_chats().encode())
            if url.path == "/v1/inbox":
                return self.respond(200, inbox_text().encode())
            if url.path == "/v1/agent":
                return self.respond(200, agent_text().encode())
            chat = CHAT_PATH.match(url.path)
            if chat:
                return self.respond(200, chat_messages(chat.group(1)).encode())
        except RelayError as error:
            return self.relay_error(error)
        item = INBOX_PATH.match(url.path)
        if item:
            body = inbox_body(item.group(1))
            return self.respond(200, body.encode()) if body else self.respond(404, b"not found\n")
        turn_match = TURN_PATH.match(url.path)
        if turn_match:
            turn = turns.get(turn_match.group(1))
            return self.respond(200, turn.snapshot().encode()) if turn else self.respond(404, b"not found\n")
        match = AUDIO_ID.match(url.path)
        clip = audio_dir / f"{match.group(1)}.wav" if match else None
        if clip is None or not clip.exists():
            return self.respond(404, b"not found\n")
        return self.respond(200, clip.read_bytes(), "audio/wav")

    def turn_action(self, turn, action, body):
        if action == "speech":
            return self.respond(200, f"{speak_turn(turn)}\n".encode())
        if not turn.run_id:
            return self.respond(409, b"no run\n")
        if action == "stop":
            hermes_json(f"/v1/runs/{turn.run_id}/stop", {})
            return self.respond(200, b"stopped\n")
        if turn.state != "approval":
            return self.respond(409, b"nothing to approve\n")
        choice = body.decode(errors="replace").strip()
        if choice not in APPROVAL_CHOICES:
            return self.respond(400, b"bad choice\n")
        hermes_json(f"/v1/runs/{turn.run_id}/approval", {"choice": choice})
        turn.update(state="working", approval="")
        return self.respond(200, b"answered\n")

    def do_POST(self):
        if not self.authorized():
            return None
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        length = int(self.headers.get("Content-Length") or 0)
        if not 0 <= length <= MAX_UPLOAD_BYTES:
            return self.respond(413, b"bad upload size\n")
        body = self.rfile.read(length)
        if url.path == "/v1/reset":
            session.update(id="", used_at=0.0)
            return self.respond(200, b"reset\n")
        action = TURN_ACTION_PATH.match(url.path)
        if action:
            turn = turns.get(action.group(1))
            if not turn:
                return self.respond(404, b"not found\n")
            try:
                return self.turn_action(turn, action.group(2), body)
            except RelayError as error:
                return self.relay_error(error)
        if url.path != "/v1/turn":
            return self.respond(404, b"not found\n")
        if not body:
            return self.respond(413, b"bad upload size\n")
        chat = query.get("session", [""])[0]
        if chat and not (SESSION_ID.match(chat) and chat.startswith("pocket-")):
            return self.respond(400, b"only brick chats can continue here\n")
        if not turn_lock.acquire(blocking=False):
            return self.respond(409, b"busy\n")
        turn = Turn()
        turns[turn.id] = turn
        speak = query.get("speak", ["0"])[0] == "1" and bool(CARTESIA_KEY and CARTESIA_VOICE)
        threading.Thread(target=run_turn, args=(turn, body, speak, chat), daemon=True).start()
        return self.respond(200, f"{turn.id}\n".encode())


if __name__ == "__main__":
    threading.Thread(target=purge_forever, daemon=True).start()
    print(f"listening on {HOST}:{PORT}", flush=True)
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
