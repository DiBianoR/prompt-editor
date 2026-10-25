Now turn your findings into the exact format the editor uses. Reply with a single JSON object and nothing else:

{
  "name": "{{TOPIC}}",
  "note": "contradictions, duplicates or other problems worth flagging, or an empty string",
  "ranges": [ RANGE, ... ],
  "subtopics": [
    { "name": "subtopic name", "note": "", "ranges": [ RANGE, ... ] }
  ]
}

Each RANGE marks one passage of the document, in one of three forms:

  {"lines": "12-14"}                          whole lines 12 through 14 ("12" for one line)
  {"quote": "exact text", "line": 12}         part of a line: a sentence or a clause
  {"from": "first words", "to": "last words", "line": 12}
                                              a passage that starts or ends partway through a line,
                                              running from the start of "from" to the end of "to"

"line" is the line number where the passage starts. It is optional, but include it.

Rules:

1. Text in "quote", "from" and "to" must be copied character for character from the document: the same words, punctuation, quotation marks and capitalization. Never include the line numbers, and never shorten a quote with "...".
2. Use "lines" when a passage is one or more whole lines, such as a bullet or a paragraph. Use "quote" only when the passage is part of a line. Use "from"/"to" for a passage that spans lines but starts or ends partway through one; make each 4 to 12 words long.
3. Make each range a complete unit: a sentence, a bullet, a list item or a paragraph. Cut a sentence only when just part of it is about the topic. Leave out headings unless the heading itself says something about the topic.
4. One range per separate passage. Do not merge passages that are apart, and do not split one continuous passage into many small ranges.
5. If your findings fell naturally into distinct aspects of the topic, make each aspect a subtopic, with its passages under it, and put passages about the topic as a whole in the top-level "ranges". If they do not divide naturally, use an empty "subtopics" list. Do not put the same passage both in a subtopic and in the top-level ranges.
6. While converting, check each finding against the document once more: drop anything that is not really about the topic, add anything you missed, and correct boundaries.
7. If the topic does not appear in the document, return empty "ranges" and "subtopics".
