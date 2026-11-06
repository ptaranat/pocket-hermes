import contextlib
import json
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MOCK_PORT, RELAY_PORT = 18642, 18643
RELAY_URL = f"http://127.0.0.1:{RELAY_PORT}"
answered = threading.Event()
received = {}


class MockHermes(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, value):
        body = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def event(self, **fields):
        self.wfile.write(f"data: {json.dumps(fields)}\n\n".encode())
        self.wfile.flush()

    def do_GET(self):
        if self.path == "/v1/models":
            return self.send_json({"data": [{"id": "mock"}]})
        self.send_response(200)
        self.end_headers()
        self.event(event="tool.started", tool="terminal", tool_call_id="1", preview="git push origin main")
        self.event(event="approval.request", command="git push origin main", choices=["once", "session", "deny"])
        answered.wait(20)
        self.event(event="tool.completed", tool="terminal", tool_call_id="1")
        self.event(event="run.completed", output=f"Choice was {received.get('choice')}.")

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        if self.path.endswith("/approval"):
            received.update(body)
            answered.set()
            return self.send_json({"ok": True})
        self.send_json({"run_id": "run_mock", "status": "started"})


def relay(token, path, data=None):
    request = urllib.request.Request(RELAY_URL + path, data=data, headers={"Authorization": f"Bearer {token}"})
    return urllib.request.urlopen(request, timeout=20).read().decode()


def main():
    wav_path = sys.argv[1]
    token = open(os.environ["RELAY_TOKEN_FILE"]).read().strip()
    threading.Thread(target=ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), MockHermes).serve_forever, daemon=True).start()
    env = dict(os.environ, RELAY_HOST="127.0.0.1", RELAY_PORT=str(RELAY_PORT), HERMES_URL=f"http://127.0.0.1:{MOCK_PORT}")
    server = subprocess.Popen(["uv", "run", "--script", sys.argv[2]], env=env, stdout=open(os.environ.get("RELAY_TEST_LOG", os.devnull), "w"), stderr=subprocess.STDOUT, start_new_session=True)
    try:
        for _ in range(90):
            try:
                urllib.request.urlopen(RELAY_URL + "/health", timeout=1)
                break
            except OSError:
                time.sleep(1)
        turn = relay(token, "/v1/turn", open(wav_path, "rb").read()).strip()
        seen = []
        for _ in range(400):
            lines = relay(token, f"/v1/turn/{turn}").split("\n")
            if not seen or seen[-1] != lines[0]:
                seen.append(lines[0])
            if lines[0] == "approval":
                print("asked:", lines[2])
                try:
                    relay(token, f"/v1/turn/{turn}/approval", b"bad")
                except urllib.error.HTTPError as error:
                    print("bad choice rejected:", error.code)
                print("answer:", relay(token, f"/v1/turn/{turn}/approval", b"deny").strip())
            if lines[0] in ("done", "failed"):
                print("states:", " > ".join(seen))
                print("hermes received:", received)
                print("reply:", lines[2])
                try:
                    relay(token, f"/v1/turn/{turn}/speech", b"")
                except urllib.error.HTTPError as error:
                    print("read aloud without cartesia:", error.code, error.read().decode().strip())
                try:
                    relay(token, f"/v1/turn/{turn}/approval", b"once")
                except urllib.error.HTTPError as error:
                    print("late answer rejected:", error.code)
                return
            time.sleep(0.25)
        print("timed out:", seen)
    finally:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(server.pid, signal.SIGTERM)


if __name__ == "__main__":
    main()
