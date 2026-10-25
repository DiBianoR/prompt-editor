"""Builds or refreshes a prompt's topic index with an OpenAI-compatible LLM.

Each topic is handled on its own, in one conversation:
  1. find    the model reads the whole prompt looking only for this topic and
             reports every passage in its own words (subtopics emerge freely)
  2. format  the model converts its findings into ranges (prompts/format_topic.md)
  3. resolve the ranges are matched to the document (resolve_quotes.py); any
             that fail go back to the model to fix, up to --repairs rounds
Then ranges are cleaned up (merged, de-duplicated) and written as the index the
editor reads. Every request and reply is kept in a run folder for inspection.

Commands:
  config  --base-url URL --model NAME [--api-key]   save settings (key is asked for)
  models                                            list the server's models
  topics  DOC [-o topics.txt]                       propose topics (adds missing ones
                                                    if DOC already has an index)
  build   DOC [--topics topics.txt] [--only NAME]   index every topic (or one)

Topics file: one topic per line, "Name" or "Name | what belongs under it";
lines starting with # are ignored. Without --topics, build reuses the topics of
the existing index, or asks the model to propose them.

Settings come from flags, then PE_LLM_BASE_URL / PE_LLM_MODEL / PE_LLM_API_KEY,
then tools/llm.local.json (written by `config`, not committed).
"""

import argparse
import datetime
import getpass
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_index  # noqa: E402
from resolve_quotes import Document, ResolveError, merge_spans, resolve_range  # noqa: E402

TOOLS = Path(__file__).resolve().parent
PROMPTS = TOOLS / "prompts"
CONFIG = TOOLS / "llm.local.json"


# ---- Settings and client ----------------------------------------------------

def load_settings(args):
    saved = json.loads(CONFIG.read_text(encoding="utf-8")) if CONFIG.exists() else {}
    def pick(flag, env, key):
        return getattr(args, flag, None) or os.environ.get(env) or saved.get(key)
    return {
        "base_url": pick("base_url", "PE_LLM_BASE_URL", "base_url"),
        "model": pick("model", "PE_LLM_MODEL", "model"),
        "api_key": os.environ.get("PE_LLM_API_KEY") or saved.get("api_key"),
    }


class LLMError(Exception):
    pass


class LLM:
    """Minimal chat-completions client. Tries a JSON schema response format when
    asked, and falls back to plain JSON mode or none if the server refuses it."""

    def __init__(self, base_url, model, api_key=None, timeout=1800, log_dir=None, temperature=0.2):
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.api_key = api_key
        self.timeout = timeout
        self.log_dir = log_dir
        self.temperature = temperature
        self.json_modes = ["json_schema", "json_object", None]
        self.calls = 0

    def _request(self, method, path, body=None):
        req = urllib.request.Request(self.base_url + path, method=method,
                                     data=json.dumps(body).encode() if body is not None else None)
        req.add_header("Content-Type", "application/json")
        if self.api_key:
            req.add_header("Authorization", f"Bearer {self.api_key}")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:500]
            raise LLMError(f"HTTP {e.code} from {path}: {detail}") from None
        except urllib.error.URLError as e:
            raise LLMError(f"cannot reach {self.base_url}: {e.reason}") from None

    def models(self):
        data = self._request("GET", "/models")
        return [m.get("id") for m in data.get("data", [])]

    def chat(self, messages, schema=None, max_tokens=16384):
        while True:
            mode = self.json_modes[0] if schema else None
            body = {"model": self.model, "messages": messages,
                    "temperature": self.temperature, "max_tokens": max_tokens}
            if mode == "json_schema":
                body["response_format"] = {"type": "json_schema",
                                           "json_schema": {"name": "topic", "schema": schema}}
            elif mode == "json_object":
                body["response_format"] = {"type": "json_object"}
            started = time.time()
            try:
                data = self._request("POST", "/chat/completions", body)
            except LLMError as e:
                # A server that rejects the response format gets the next simpler one.
                msg = str(e)
                if mode and ("HTTP 400" in msg or "HTTP 422" in msg or "format" in msg.lower()):
                    self.json_modes.pop(0)
                    print(f"    (server refused response_format {mode}; using {self.json_modes[0]})")
                    continue
                raise
            choice = data["choices"][0]
            text = strip_thinking(choice["message"].get("content") or "")
            self.calls += 1
            if self.log_dir:
                self.log_dir.mkdir(parents=True, exist_ok=True)
                (self.log_dir / f"{self.calls:03d}.json").write_text(json.dumps(
                    {"seconds": round(time.time() - started, 1), "response_format": mode,
                     "messages": messages, "reply": text, "finish_reason": choice.get("finish_reason"),
                     "usage": data.get("usage")}, ensure_ascii=False, indent=2), encoding="utf-8")
            if choice.get("finish_reason") == "length":
                print("    (warning: reply hit the token limit and may be cut off)")
            return text


def strip_thinking(text):
    """Drops reasoning blocks some local models put in the reply."""
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.DOTALL)
    if "</think>" in text:  # an opening tag swallowed by the chat template
        text = text.split("</think>", 1)[1]
    return text.strip()


# ---- Prompts ----------------------------------------------------------------

def render(name, **values):
    text = (PROMPTS / name).read_text(encoding="utf-8")
    for key, value in values.items():
        text = text.replace("{{" + key + "}}", value)
    return text


def system_message():
    return {"role": "system", "content": render("system.md").strip()}


RANGE_SCHEMA = {
    "type": "object",
    "properties": {
        "lines": {"type": "string"},
        "quote": {"type": "string"},
        "from": {"type": "string"},
        "to": {"type": "string"},
        "line": {"type": "integer"},
    },
}
NODE_PROPS = {
    "name": {"type": "string"},
    "note": {"type": "string"},
    "ranges": {"type": "array", "items": RANGE_SCHEMA},
}
TOPIC_SCHEMA = {
    "type": "object",
    "properties": dict(NODE_PROPS, subtopics={
        "type": "array",
        "items": {"type": "object", "properties": NODE_PROPS, "required": ["name", "ranges"]},
    }),
    "required": ["name", "ranges", "subtopics"],
}


def parse_json_reply(text):
    """The first JSON object in a reply, tolerating code fences and chatter."""
    text = re.sub(r"```(?:json)?", "", text)
    start = text.find("{")
    if start == -1:
        raise ValueError("no JSON object in the reply")
    obj, _ = json.JSONDecoder().raw_decode(text[start:])
    if not isinstance(obj, dict):
        raise ValueError("the reply is not a JSON object")
    return obj


# ---- One topic ----------------------------------------------------------------

def resolve_node(doc, node, path):
    """Resolves a node's ranges. Returns (spans, failures)."""
    spans, failures = [], []
    ranges = node.get("ranges") or []
    if not isinstance(ranges, list):
        return [], [(path + ".ranges", "not a list", ranges)]
    for i, item in enumerate(ranges):
        try:
            spans.append(resolve_range(doc, item))
        except ResolveError as e:
            failures.append((f"{path}.ranges[{i}]", str(e), item))
    return spans, failures


def resolve_topic(doc, obj):
    """Resolves the model's JSON for one topic. Returns (topic, failures) where
    topic = {"name", "note", "spans", "subtopics": [{"name", "note", "spans"}]}."""
    topic = {"name": str(obj.get("name") or "").strip(), "note": str(obj.get("note") or "").strip()}
    topic["spans"], failures = resolve_node(doc, obj, "topic")
    topic["subtopics"] = []
    subs = obj.get("subtopics") or []
    for j, sub in enumerate(subs if isinstance(subs, list) else []):
        if not isinstance(sub, dict):
            failures.append((f"subtopics[{j}]", "not an object", sub))
            continue
        spans, fails = resolve_node(doc, sub, f"subtopics[{j}]")
        failures += fails
        topic["subtopics"].append({"name": str(sub.get("name") or f"Part {j + 1}").strip(),
                                   "note": str(sub.get("note") or "").strip(), "spans": spans})
    return topic, failures


def clean_topic(doc, topic):
    """Merges overlapping spans, removes top-level spans a subtopic already
    covers, and drops empty subtopics."""
    for sub in topic["subtopics"]:
        sub["spans"] = merge_spans(sub["spans"], doc.text)
    covered = [(s, e) for sub in topic["subtopics"] for s, e, _ in sub["spans"]]
    topic["spans"] = [sp for sp in merge_spans(topic["spans"], doc.text)
                      if not any(s <= sp[0] and sp[1] <= e for s, e in covered)]
    topic["subtopics"] = [s for s in topic["subtopics"] if s["spans"]]
    return topic


def format_failures(failures):
    return "\n".join(f"- {where}: {reason}\n  {json.dumps(item, ensure_ascii=False)}"
                     for where, reason, item in failures)


def index_topic(llm, doc, name, hint, repairs, run_dir):
    """Runs find, format and repair for one topic. Returns (topic, dropped failures)."""
    run_dir.mkdir(parents=True, exist_ok=True)
    messages = [system_message(), {"role": "user", "content": render(
        "find_topic.md", DOCUMENT=doc.numbered(), TOPIC=name,
        TOPIC_HINT=f"({hint})\n" if hint else "")}]
    t = time.time()
    findings = llm.chat(messages)
    (run_dir / "1-findings.md").write_text(findings, encoding="utf-8")
    print(f"    found in {time.time() - t:.0f}s ({len(findings)} chars of findings)")

    messages += [{"role": "assistant", "content": findings},
                 {"role": "user", "content": render("format_topic.md", TOPIC=name)}]
    topic, last_failures = None, []
    for attempt in range(repairs + 1):
        t = time.time()
        reply = llm.chat(messages, schema=TOPIC_SCHEMA)
        (run_dir / f"2-format-{attempt}.json").write_text(reply, encoding="utf-8")
        try:
            obj = parse_json_reply(reply)
        except ValueError as e:
            print(f"    reply {attempt + 1} was not valid JSON ({e})")
            messages += [{"role": "assistant", "content": reply}, {"role": "user", "content":
                          f"That was not a valid JSON object ({e}). Reply with only the JSON object."}]
            continue
        candidate, failures = resolve_topic(doc, obj)
        if topic is None or len(failures) <= len(last_failures):
            topic, last_failures = candidate, failures
        n = len(candidate["spans"]) + sum(len(s["spans"]) for s in candidate["subtopics"])
        print(f"    formatted in {time.time() - t:.0f}s: {n} ranges resolved, {len(failures)} failed")
        if not failures:
            break
        if attempt < repairs:
            messages += [{"role": "assistant", "content": reply},
                         {"role": "user", "content": render("repair.md", FAILURES=format_failures(failures))}]
    if topic is None:
        raise LLMError(f"no usable JSON for topic {name!r}")
    topic["name"] = name  # the topic list's name wins over the model's rewording
    topic = clean_topic(doc, topic)
    (run_dir / "3-result.json").write_text(json.dumps(
        {"topic": topic, "dropped": [list(f) for f in last_failures]},
        ensure_ascii=False, indent=2), encoding="utf-8")
    return topic, last_failures


# ---- Topics -------------------------------------------------------------------

def read_topics_file(path):
    topics = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, hint = line.partition("|")
        topics.append((name.strip(), hint.strip()))
    return topics


def parse_topic_lines(reply):
    topics = []
    for line in reply.splitlines():
        line = re.sub(r"^\s*(?:[-*+]|\d+[.)])\s*", "", line).strip().strip("*").strip()
        if not line or line.upper() == "NONE":
            continue
        name, _, hint = line.partition("|")
        name = name.strip().strip("*:").strip()
        if name and len(name) <= 80:
            topics.append((name, hint.strip()))
    return topics


def discover_topics(llm, doc, existing_index=None):
    existing = ""
    if existing_index:
        names = "\n".join(f"- {t['name']}" for t in existing_index["topics"])
        gaps = "\n".join(f"- {g['exact'][:300]}" for g in existing_index.get("unindexed", [])) or "(none)"
        existing = render("discover_existing.md", TOPICS=names, GAPS=gaps)
    reply = llm.chat([system_message(), {"role": "user", "content": render(
        "discover.md", DOCUMENT=doc.numbered(), EXISTING=existing)}], max_tokens=4096)
    return parse_topic_lines(reply)


# ---- Index assembly -------------------------------------------------------------

def slug(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "topic"


def byte_to_char(text):
    """Maps UTF-8 byte offsets back to character offsets."""
    table, b = {}, 0
    for i, ch in enumerate(text):
        table[b] = i
        b += len(ch.encode("utf-8"))
    table[b] = len(text)
    return table


def spans_spec(spans):
    out = []
    for s, e, status in spans:
        r = {"span": [s, e]}
        if status in ("fuzzy", "ambiguous"):
            r["status"] = status
        out.append(r)
    return out


def existing_topic_spec(text, topic):
    """An existing index topic as a spec with spans (offsets must be current)."""
    b2c = byte_to_char(text)
    def node(t):
        n = {"id": t["id"], "name": t["name"], "color": t.get("color", "")}
        if t.get("note"):
            n["note"] = t["note"]
        n["ranges"] = [dict({"span": [b2c[r["start"]], b2c[r["end"]]]},
                            **({"status": r["status"]} if r.get("status") else {}))
                       for r in t["ranges"] if r.get("status") != "orphaned"]
        return n
    spec = node(topic)
    spec["subtopics"] = [node(s) for s in topic.get("subtopics", [])]
    return spec


def new_topic_spec(topic, old_by_name, used_ids):
    old = old_by_name.get(topic["name"].lower())
    tid = old["id"] if old else slug(topic["name"])
    while tid in used_ids:
        tid += "-2"
    used_ids.add(tid)
    spec = {"id": tid, "name": topic["name"], "ranges": spans_spec(topic["spans"])}
    if old and old.get("color"):
        spec["color"] = old["color"]
    if topic["note"]:
        spec["note"] = topic["note"]
    spec["subtopics"] = []
    for sub in topic["subtopics"]:
        sid = f"{tid}.{slug(sub['name'])}"
        while sid in used_ids:
            sid += "-2"
        used_ids.add(sid)
        s = {"id": sid, "name": sub["name"], "ranges": spans_spec(sub["spans"])}
        if sub["note"]:
            s["note"] = sub["note"]
        spec["subtopics"].append(s)
    return spec


def write_index(doc_path, out_path, topic_specs):
    for i, t in enumerate(topic_specs):  # palette for new topics, inherited by subtopics
        if not t.get("color"):
            t["color"] = build_index.PALETTE[i % len(build_index.PALETTE)]
        for s in t["subtopics"]:
            s["color"] = s.get("color") or t["color"]
    index = build_index.build_index(str(doc_path), {"document": doc_path.name, "topics": topic_specs})
    if out_path.exists():
        out_path.replace(out_path.with_suffix(out_path.suffix + ".bak"))
    out_path.write_text(json.dumps(index, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    return index


# ---- Commands -----------------------------------------------------------------

def make_llm(args, log_dir=None):
    s = load_settings(args)
    if not s["base_url"] or not s["model"]:
        sys.exit("Set the server first: py tools/llm_index.py config --base-url URL --model NAME")
    return LLM(s["base_url"], s["model"], s["api_key"], log_dir=log_dir,
               temperature=getattr(args, "temperature", 0.2))


def default_index_path(doc_path):
    return doc_path.with_name(doc_path.stem + ".index.json")


def cmd_config(args):
    saved = json.loads(CONFIG.read_text(encoding="utf-8")) if CONFIG.exists() else {}
    if args.base_url:
        saved["base_url"] = args.base_url
    if args.model:
        saved["model"] = args.model
    if args.api_key:
        saved["api_key"] = getpass.getpass("API key (input hidden): ").strip()
    CONFIG.write_text(json.dumps(saved, indent=2) + "\n", encoding="utf-8")
    shown = dict(saved, api_key="(set)" if saved.get("api_key") else "(none)")
    print(f"saved {CONFIG}: {json.dumps(shown)}")


def cmd_models(args):
    for m in make_llm(args).models():
        print(m)


def cmd_topics(args):
    doc_path = Path(args.document)
    doc = Document(doc_path.read_bytes().decode("utf-8"))
    index_path = default_index_path(doc_path)
    existing = json.loads(index_path.read_text(encoding="utf-8")) if index_path.exists() else None
    topics = discover_topics(make_llm(args), doc, existing)
    out = Path(args.output or doc_path.with_name(doc_path.stem + ".topics.txt"))
    lines = [f"{n} | {h}" if h else n for n, h in topics]
    if existing:
        lines = [t["name"] for t in existing["topics"]] + lines
    out.write_text("# One topic per line: Name | what belongs under it\n" + "\n".join(lines) + "\n",
                   encoding="utf-8")
    print(f"{len(topics)} {'new ' if existing else ''}topics -> {out}")
    for n, h in topics:
        print(f"  {n}" + (f"  ({h})" if h else ""))


def cmd_build(args):
    doc_path = Path(args.document)
    raw = doc_path.read_bytes()
    text = raw.decode("utf-8")
    doc = Document(text)
    out_path = Path(args.output) if args.output else default_index_path(doc_path)
    old = json.loads(out_path.read_text(encoding="utf-8")) if out_path.exists() else None
    old_by_name = {t["name"].lower(): t for t in (old or {}).get("topics", [])}

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = Path(args.run_dir or doc_path.with_name(doc_path.stem + ".llm-runs")) / stamp
    llm = make_llm(args, log_dir=run_dir / "calls")
    print(f"model {llm.model} at {llm.base_url}; run folder {run_dir}")

    if args.only:
        if not old:
            sys.exit(f"--only needs an existing index at {out_path}")
        if old.get("documentHash") != "sha256:" + hashlib.sha256(raw).hexdigest():
            sys.exit("The index does not match the document's current text. Open and save the "
                     "document in the editor first, or rebuild every topic.")
        hint = next((h for n, h in read_topics_file(args.topics) if n.lower() == args.only.lower()), "") \
            if args.topics else ""
        topics = [(args.only, hint)]
    elif args.topics:
        topics = read_topics_file(args.topics)
    elif old:
        topics = [(t["name"], "") for t in old["topics"]]
        print(f"reusing the {len(topics)} topics of {out_path.name}")
    else:
        print("no topics given: asking the model to propose them")
        topics = discover_topics(llm, doc)
        (run_dir / "topics.txt").write_text("\n".join(f"{n} | {h}" if h else n for n, h in topics) + "\n",
                                           encoding="utf-8")
        for n, _ in topics:
            print(f"  {n}")
    if not topics:
        sys.exit("no topics to index")

    results, dropped = [], []
    for i, (name, hint) in enumerate(topics):
        print(f"[{i + 1}/{len(topics)}] {name}")
        try:
            topic, fails = index_topic(llm, doc, name, hint, args.repairs,
                                       run_dir / f"{i + 1:02d}-{slug(name)}")
        except LLMError as e:
            print(f"    FAILED: {e}")
            dropped.append((name, "topic", str(e), None))
            continue
        results.append(topic)
        dropped += [(name, *f) for f in fails]

    used_ids = set()
    if args.only:
        specs = []
        for t in old["topics"]:
            if t["name"].lower() == args.only.lower():
                continue
            spec = existing_topic_spec(text, t)
            used_ids.update([spec["id"]] + [s["id"] for s in spec["subtopics"]])
            specs.append(spec)
        replaced = [i for i, t in enumerate(old["topics"]) if t["name"].lower() == args.only.lower()]
        for r in results:
            specs.insert(replaced[0] if replaced else len(specs), new_topic_spec(r, old_by_name, used_ids))
    else:
        specs = [new_topic_spec(r, old_by_name, used_ids) for r in results]
    index = write_index(doc_path, out_path, specs)

    statuses = {}
    for t in index["topics"]:
        for n in [t] + t["subtopics"]:
            for r in n["ranges"]:
                statuses[r.get("status", "exact")] = statuses.get(r.get("status", "exact"), 0) + 1
    print(f"\n{out_path}: {len(index['topics'])} topics, "
          + ", ".join(f"{v} {k}" for k, v in sorted(statuses.items()))
          + f", {len(index['unindexed'])} unindexed passages; {llm.calls} LLM calls")
    if dropped:
        print(f"{len(dropped)} ranges could not be matched and were left out:")
        for name, where, reason, _ in dropped:
            print(f"  {name} {where}: {reason}")
    for g in index["unindexed"][:10]:
        print(f"  unindexed: {g['exact'][:100]!r}")
    print(f"details: {run_dir}")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    server = argparse.ArgumentParser(add_help=False)
    server.add_argument("--base-url", help="e.g. http://localhost:8080/v1")
    server.add_argument("--model", help="model name, as listed by `models`")
    sub = p.add_subparsers(dest="command", required=True)

    c = sub.add_parser("config", parents=[server], help="save server settings")
    c.add_argument("--api-key", action="store_true", help="ask for the API key and save it")
    sub.add_parser("models", parents=[server], help="list the server's models")
    t = sub.add_parser("topics", parents=[server], help="propose topics")
    t.add_argument("document")
    t.add_argument("-o", "--output")
    b = sub.add_parser("build", parents=[server], help="index every topic, or one with --only")
    b.add_argument("document")
    b.add_argument("--topics", help="topics file")
    b.add_argument("--only", help="re-index just this topic, keeping the others")
    b.add_argument("-o", "--output", help="index path (default: <document stem>.index.json)")
    b.add_argument("--repairs", type=int, default=2, help="rounds of fixing unmatched ranges")
    b.add_argument("--temperature", type=float, default=0.2)
    b.add_argument("--run-dir", help="where to keep transcripts (default: <stem>.llm-runs)")

    args = p.parse_args()
    try:
        {"config": cmd_config, "models": cmd_models, "topics": cmd_topics, "build": cmd_build}[args.command](args)
    except LLMError as e:
        sys.exit(f"error: {e}")


if __name__ == "__main__":
    main()
