import re
import sys

include = re.compile(r"^\s*#\s*include\s*<[^>]+>")

with open(sys.argv[1], encoding="utf-8", errors="replace") as source:
    for line in source:
        # Preserve line count so source links remain correct.
        sys.stdout.write("\n" if include.match(line) else line)