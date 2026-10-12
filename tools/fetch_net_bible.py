#!/usr/bin/env python3
"""Downloads the NET Bible chapters from https://www.divinerevelations.info/documents/bible/html/?dir=NET_Bible and writes one text file
for the Bible app: tools/bible_net.txt, in the same format as tools/bible_asv.txt ("=Book name", then one "chapter:verse text" line each).
Section headings and footnotes are dropped. A psalm title becomes verse 0 of its chapter (shown without a number).
The NET Bible is copyrighted (Biblical Studies Press). The text file is for your own phone and stays out of the repository.
usage: fetch_net_bible.py [OUTPUT]"""
import concurrent.futures, html, os, re, sys, time, unicodedata, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = "https://www.divinerevelations.info/documents/bible/html/NET_Bible/"
CODES = ("GEN EXO LEV NUM DEU JOS JDG RUT 1SA 2SA 1KI 2KI 1CH 2CH EZR NEH EST JOB PSA PRO ECC SNG ISA JER LAM EZK DAN HOS JOL AMO OBA JON MIC NAM HAB ZEP "
         "HAG ZEC MAL MAT MRK LUK JHN ACT ROM 1CO 2CO GAL EPH PHP COL 1TH 2TH 1TI 2TI TIT PHM HEB JAS 1PE 2PE 1JN 2JN 3JN JUD REV").split()
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "bible_net.txt")

def names():
    n = []
    for line in open(os.path.join(HERE, "bible_asv.txt"), encoding="ascii"):
        if line.startswith("="): n.append(line[1:].strip())
    if len(n) != 66: sys.exit("run tools/make_bible.py first (it makes bible_asv.txt, which has the book names)")
    return n

def get(url, tries=4):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "FBTools-bible-fetch"}), timeout=40) as r: return r.read().decode("utf-8-sig", "replace")
        except Exception as e:
            if i == tries - 1: raise
            time.sleep(1 + i)

def ascii_text(t):
    t = t.replace("‘", "'").replace("’", "'").replace("“", '"').replace("”", '"').replace("–", "-").replace("—", "-").replace("…", "...")
    t = unicodedata.normalize("NFKD", t).encode("ascii", "replace").decode("ascii")
    return re.sub(r"\s+", " ", t.replace("\xa0", " ")).strip()

def chapter(code, n):
    page = get("%s%s%03d.htm" % (BASE, code, n)) if code == "PSA" else get("%s%s%02d.htm" % (BASE, code, n))       # the Psalms use three digits
    body = page[page.find('class="main"'):]
    body = re.sub(r"<div class='(chapterlabel|s|mt|footnote|copyright)'.*?</div>", "", body, flags=re.S)
    body = re.sub(r"<div class=\"(footnote|copyright)\".*?</div>", "", body, flags=re.S)
    body = re.sub(r"<ul class='tnav'>.*?</ul>", "", body, flags=re.S)
    body = re.sub(r"<span class=\"verse\" id=\"V(\d+)\">[^<]*</span>", lambda m: "\x00%s\x01" % m.group(1), body)
    body = re.sub(r"<div class='d'>(.*?)</div>", lambda m: "\x000\x01" + m.group(1), body, flags=re.S)
    text = html.unescape(re.sub(r"<[^>]+>", " ", body))
    verses = {}
    for m in re.finditer(r"\x00(\d+)\x01([^\x00]*)", text):
        v = int(m.group(1)); t = ascii_text(m.group(2))
        if t: verses[v] = (verses.get(v, "") + " " + t).strip()
    return verses

def book(code):
    chapters, n, fails = [], 1, 0
    while True:
        try: v = chapter(code, n)
        except Exception as e:
            if "404" in str(e) or n > 1: break
            raise
        if not v: break
        chapters.append(v); n += 1
    return chapters

def main():
    nm = names(); res = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        futs = {ex.submit(book, c): c for c in CODES}
        for f in concurrent.futures.as_completed(futs):
            c = futs[f]; res[c] = f.result(); print("%s: %d chapters" % (c, len(res[c])), flush=True)
    total = 0
    with open(out, "w", encoding="ascii", newline="\n") as f:
        for code, name in zip(CODES, nm):
            f.write("=" + name + "\n")
            for ci, vs in enumerate(res[code], 1):
                for v in sorted(vs): f.write("%d:%d %s\n" % (ci, v, vs[v])); total += 1
    print("%d verses -> %s" % (total, out))

if __name__ == "__main__":
    main()
