"""A fake OpenAI-compatible server for testing the editor's indexing end to end.

It plays the model's part in the pipeline with simple rules: findings name the
lines that contain the topic's first word, the format step returns those lines
plus one quote that does not exist (so a repair round happens), and the repair
step returns the lines alone. It requires an API key, can reject the
json_schema response format once, and can be slowed down to test cancelling.

Usage: py tests/fake_llm_server.py [--port 9099] [--key test-key] [--delay 0] [--reject-schema]
"""

import argparse
import json
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import time

ARGS = None
STATE = {"schema_rejected": False, "calls": 0}


def numbered_lines(messages):
    m = re.search(r"<document>\n(.*?)\n</document>", messages[1]["content"], re.S)
    lines = []
    for line in (m.group(1) if m else "").split("\n"):
        mm = re.match(r"\s*(\d+)\| ?(.*)", line)
        if mm:
            lines.append((int(mm.group(1)), mm.group(2)))
    return lines


def topic_of(messages):
    m = re.search(r"^Topic: (.+)$", messages[1]["content"], re.M)
    return m.group(1).strip() if m else "?"


def matching_lines(messages):
    word = topic_of(messages).split()[0].lower()
    return [n for n, text in numbered_lines(messages) if word in text.lower()]


def reply_for(messages):
    last = messages[-1]["content"]
    if "List the topics" in last or "List only the topics" in last:
        return "Jealousy | possessiveness and jealous outbursts\nMusic | music the persona likes or plays\n"
    if "Find every part" in last:
        lines = matching_lines(messages)
        return f"<think>looking for {topic_of(messages)}</think>Found the topic on lines {lines}."
    lines = matching_lines(messages)
    ranges = [{"lines": str(n), "line": n} for n in lines]
    if "exact format the editor uses" in last:
        ranges.append({"quote": "This sentence is not in the document anywhere at all.", "line": 1})
    doc = {"name": topic_of(messages), "note": "", "ranges": ranges[:1],
           "subtopics": [{"name": "Mentions", "note": "", "ranges": ranges[1:]}] if len(ranges) > 1 else []}
    return "```json\n" + json.dumps(doc) + "\n```"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(fmt % args, flush=True)

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        if self.headers.get("Authorization") != f"Bearer {ARGS.key}":
            self.send_json(401, {"error": {"message": "invalid api key"}})
            return False
        return True

    def do_GET(self):
        if not self.authorized():
            return
        if self.path == "/v1/models":
            self.send_json(200, {"object": "list", "data": [{"id": "fake-small"}, {"id": "fake-large"}]})
        else:
            self.send_json(404, {"error": {"message": "not found"}})

    def do_POST(self):
        if not self.authorized():
            return
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        fmt = (body.get("response_format") or {}).get("type")
        if ARGS.reject_schema and fmt == "json_schema" and not STATE["schema_rejected"]:
            STATE["schema_rejected"] = True
            self.send_json(400, {"error": {"message": "response_format json_schema is not supported"}})
            return
        STATE["calls"] += 1
        time.sleep(ARGS.delay)
        content = reply_for(body["messages"])
        self.send_json(200, {"choices": [{"index": 0, "finish_reason": "stop",
                                          "message": {"role": "assistant", "content": content}}],
                             "usage": {"prompt_tokens": 1, "completion_tokens": 1}})


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=9099)
    p.add_argument("--key", default="test-key")
    p.add_argument("--delay", type=float, default=0)
    p.add_argument("--reject-schema", action="store_true")
    ARGS = p.parse_args()
    print(f"fake LLM on http://127.0.0.1:{ARGS.port}/v1", flush=True)
    ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler).serve_forever()
