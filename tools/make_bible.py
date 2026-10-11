#!/usr/bin/env python3
"""Turn the books.txt of an ASV Bible HTML package into one small text file for the Bible app.

usage: make_bible.py [ASV-FOLDER] [OUTPUT]      (defaults: asv/ and tools/bible_asv.txt)

The output has one line per verse, "chapter:verse text", and a line "=Book name" before each book.
The ASV text is public domain, but the folder itself is not part of this repository (it is in .gitignore).
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "asv")
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "bible_asv.txt")

def pretty(name):
    name = name.strip()
    if name == "Songs": return "Song of Solomon"
    m = re.fullmatch(r"([A-Za-z]+)(\d)", name)
    return "%s %s" % (m.group(2), m.group(1)) if m else name

books, cur, verses = [], None, 0
for line in open(os.path.join(src, "books.txt"), encoding="utf-8-sig"):
    line = line.rstrip("\r\n")
    if line.startswith("="):
        m = re.match(r"=(\d{3}) (.+)", line)
        if m and m.group(1) != "000":
            cur = [pretty(m.group(2)), []]; books.append(cur)
        else:
            cur = None
        continue
    if cur is None or line.strip() == "END": continue
    m = re.match(r"^\S+ (\d+):(\d+) \S+ \d+:\d+ (.*)$", line)
    if not m: raise SystemExit("cannot read this line: " + line[:80])
    cur[1].append("%s:%s %s" % (m.group(1), m.group(2), m.group(3).strip())); verses += 1

with open(out, "w", encoding="ascii", newline="\n") as f:
    for name, lines in books:
        f.write("=" + name + "\n")
        f.write("\n".join(lines) + "\n")
print("%d books, %d verses, %d bytes -> %s" % (len(books), verses, os.path.getsize(out), out))
