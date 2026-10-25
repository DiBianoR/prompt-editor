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
  prompt or reset it to its default. **Index > Rebuild index with the model**
  (Ctrl+R) asks for the topic list (or has the model propose one) and then
  indexes each topic in its own conversation: the model reports every passage
  on the topic, converts its findings into ranges, and fixes any range that
  does not match the document. Progress shows at the bottom; Cancel stops it.
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
