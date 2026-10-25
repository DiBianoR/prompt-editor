Some of those ranges could not be matched to the document:

{{FAILURES}}

Reply with a JSON object that gives a corrected range for each of these, and nothing else:

{"fixes": [ {"id": "r1", "lines": "12-14"}, {"id": "r2", "quote": "exact text", "line": 30} ]}

Use the same three forms as before: "lines", "quote", or "from" and "to". Copy text exactly from the document, without line numbers or "...". Include only the ids listed above; every other range is already fine and must not be repeated. Leave out an id if its passage does not actually exist in the document.
