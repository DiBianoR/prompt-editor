# prompt editor
traverse and edit system prompts by topic

A Windows editor for long system prompts. The left pane lists topics from an
index file; selecting a topic highlights every range of the prompt that covers
it, and Next/Previous (F3 / Shift+F3, or Alt+Down / Alt+Up) steps through them.

## Index

The index lives next to the prompt as `<name>.index.json`. It is produced
outside the editor (by an LLM or by hand) and maps topics and one level of
subtopics to ranges of the prompt. Each range stores byte offsets plus its
exact text and surrounding context, so it can be found again after the prompt
changes.

- Build it with a model from the app: **Index > LLM settings...** takes the
  endpoint of any OpenAI-compatible server (e.g. a local llama.cpp router at
  `http://host:port/v1`), the API key and the model, and lets you edit each
  prompt or reset it to its default. **Index > Build index with the model**
  (Ctrl+R) asks for the topic list (or has the model propose one, or propose
  additional topics for an existing index). **Rebuild full index** redoes every
  listed topic and drops the rest; **Build new topics only** builds just the
  listed topics the index lacks, keeps the listed ones it has, and (after
  asking) deletes the ones taken off the list. Either way the index ends up
  in the list's order, so editing the list and choosing **Build new topics
  only** with nothing new just reorders or prunes the index. The model then
  indexes each topic in its own conversation: the model reports every passage
  on the topic, then converts its findings into ranges. Only ranges that do not
  match the document go back to it to fix, one by one; anything still unmatched
  is listed under **Lost ranges** below the topics, where you select the
  passage in the document and click **Place at selection**, or **Discard** it.
  **Show the model's analysis** (Ctrl+I) shows what the model wrote about the
  selected topic before converting it. Progress shows at the bottom; Cancel stops it.
  The model rates every passage's tier, how load-bearing it is for the topic
  (1 core, 2 high precedence, 3 standard, 4 supporting, 5 incidental); lower
  tiers are highlighted more lightly.
- Edit the index by hand from the **Passage** menu: add the selection to the
  selected topic (Ctrl+Shift+A), remove the current passage (Delete in the topic
  list), fit it to the selection (Ctrl+Shift+F), or set its tier (Alt+1..5,
  Alt+0 for none). In the topic list, Left/Right step through the passages like
  Previous/Next; the current passage's lines are shaded in the line-number
  margin. Ctrl+Shift+Delete removes the selected topic or subtopic from the
  index (both deletes ask first; the document is not changed). Ctrl+F finds
  text in the document. The list shows each topic's share of the document;
  **Index > Topic coverage chart** (Ctrl+G) draws them as bars, biggest first.
- Each topic is saved as soon as it is finished, so stopping a run (Cancel,
  closing the app, an error, even a crash) loses at most the topic in progress:
  finished topics go into the index, and the next **Rebuild index** offers to
  resume with the rest.
  **Rebuild selected topic** redoes one topic. Settings live in
  `%APPDATA%\PromptEditor` (the key encrypted for your Windows account) and
  transcripts of each run in `%LOCALAPPDATA%\PromptEditor\runs`.
- The default prompts are `tools/prompts/*.md`, built into the app. The same
  pipeline is available from a terminal as `tools/llm_index.py`.
- Or write a spec that only quotes text, and build the index with
  `py tools/build_index.py prompt.md spec.json prompt.index.json`.
- Edits made in the editor move ranges with the text; saving writes the
  updated index. Ranges edited since the last LLM refresh are marked
  `touched`, and text no topic covers is listed under `unindexed`.
- Edits made elsewhere are handled when the file is opened: each range is
  found by its offsets, then its exact text, then normalized text (quotes,
  dashes, whitespace), then approximately. Approximate matches are flagged
  for review; ranges that cannot be found are kept as lost, never dropped.

## Build

Requires Visual Studio 2022 (Desktop C++). Scintilla and nlohmann/json are vendored.

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    build\Release\PromptEditor.exe prompt.md

Tests: `build\Release\core_tests.exe [fixture_dir]`.
