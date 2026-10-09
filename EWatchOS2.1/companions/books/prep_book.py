#!/usr/bin/env python3
"""Turn a Project Gutenberg / Standard Ebooks plain-text book, or a Markdown
file converted from a PDF/ebook, into a .txt the watch reader displays well.

Why: the reader treats every '\\n' as a hard line break and its fonts only
cover printable ASCII (0x20-0x7E). Gutenberg .txt files are hard-wrapped at
~70 columns, UTF-8 (curly quotes, em dashes), and carry a licence
header/footer. Uploaded raw, every paragraph comes out ragged and
punctuation shows as junk.

This script:
  - strips the "*** START/END OF THE PROJECT GUTENBERG EBOOK ***" boilerplate
  - unwraps hard-wrapped paragraphs (blank line = paragraph break)
  - folds Unicode punctuation/accents to ASCII
  - drops _italic_ underscores
  - keeps one blank line between paragraphs

For .md input (PDF -> Markdown conversions) it also removes the page
furniture a PDF leaves behind:
  - running headers/footers ("110 the fellowship of the ring") and bare page
    numbers, found by repetition rather than a fixed list
  - picture/OCR blocks, tables (contents pages), <br>/<sup> tags, heading
    markers, **bold** and _italic_ markup
  - front matter before the first Prologue/Chapter/Book heading and back matter
    from Maps/Copyright/About the Publisher/Works by onwards (--keep-front /
    --keep-back to turn off, or --start/--end REGEX to choose the cut)
  - rejoins paragraphs that a page break split in two ("ques- / tions")
  - fixes PDF accents written as separate marks ("Lo\u00b4rien" -> "Lorien")

Usage:
  python3 prep_book.py pg1342.txt                 -> pg1342.watch.txt
  python3 prep_book.py pg1342.txt -o pride.txt
  python3 prep_book.py fellowship.md -o fellowship.txt
  python3 prep_book.py book.md --start "^PROLOGUE" --end "^APPENDIX"
"""
import argparse
import os
import re
import sys
import unicodedata

FLASH_BUDGET = 896 * 1024  # whole LittleFS partition

PUNCT = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"',
    "′": "'", "″": '"',
    "–": "-", "—": "--", "―": "--", "‒": "-", "−": "-",
    "…": "...", " ": " ", " ": " ", " ": " ", " ": " ",
    "«": '"', "»": '"', "‹": "'", "›": "'",
    "æ": "ae", "Æ": "AE", "œ": "oe", "Œ": "OE", "ß": "ss",
    "·": ".", "•": "*", "£": "GBP ", "€": "EUR ",
    "﻿": "",
}

START_RE = re.compile(r"^\*\*\*\s*START OF (THE|THIS) PROJECT GUTENBERG.*$", re.I | re.M)
END_RE = re.compile(r"^\*\*\*\s*END OF (THE|THIS) PROJECT GUTENBERG.*$", re.I | re.M)


def to_ascii(s: str) -> str:
    s = "".join(PUNCT.get(ch, ch) for ch in s)
    s = unicodedata.normalize("NFKD", s)
    return "".join(ch for ch in s if 0x20 <= ord(ch) < 0x7F or ch == "\n")


def strip_boilerplate(s: str) -> str:
    m = START_RE.search(s)
    if m:
        s = s[m.end():]
    m = END_RE.search(s)
    if m:
        s = s[:m.start()]
    return s


# --- Markdown / PDF-conversion cleanup -------------------------------------

# Spacing accent marks a PDF extractor leaves next to the letter they belonged
# to ("Lo\u00b4rien", "du\u02c6 m"); dotless i shows up for i-with-diaeresis.
SPACING_MARKS = "\u00b4\u02c6\u00a8\u02dc\u02dd\u00b8\u02c7\u02d8\u02d9\u02da\u00af`"
FRONT_RE = re.compile(r"^(prologue|chapter\s+(1|i|one)\b|book\s+(1|i|one)\b|part\s+(1|i|one)\b)", re.I)
BACK_RE = re.compile(r"^(maps?|copyright|about the (publisher|author)|works by|also by|other books by|"
                     r"acknowledg(e)?ments|index)\b", re.I)
ROMAN_OR_NUM = r"(?:\d{1,4}|[ivxlc]{1,7})"
PAGE_NUM_RE = re.compile(rf"^{ROMAN_OR_NUM}$", re.I)
END_PUNCT = tuple(".!?\"'\u2019\u201d)]:;*")


def md_to_paras(s: str) -> list:
    """Markdown text -> list of (text, is_heading) paragraphs, markup removed."""
    s = re.sub(r"<!--\s*Start of picture text\s*-->.*?(<!--\s*End of picture text\s*-->|\n\s*\n)", "\n\n", s, flags=re.S | re.I)
    s = re.sub(r"<!--.*?-->", "", s, flags=re.S)
    s = re.sub(r"<br\s*/?>", " ", s, flags=re.I)
    s = re.sub(r"<sup>\s*(.*?)\s*</sup>\s*", r"\1", s, flags=re.I | re.S)  # "LO<sup>\u00b4</sup> RIEN"
    s = re.sub(r"</?[a-z][^>]*>", "", s, flags=re.I)
    s = re.sub(rf"(?<=[A-Za-z])\s?[{SPACING_MARKS}]\s?(?=[A-Za-z])", "", s)
    s = re.sub(rf"[{SPACING_MARKS}]", "", s)
    s = s.replace("\u0131", "i")
    s = re.sub(r"\\([\\`*_{}\[\]()#+\-.!<>|])", r"\1", s)  # markdown escapes
    paras = []
    for block in re.split(r"\n\s*\n", s):
        lines = [ln.strip() for ln in block.split("\n") if ln.strip()]
        lines = [ln for ln in lines if not ln.startswith("|")]  # tables (contents)
        if not lines:
            continue
        heading = lines[0].startswith("#")
        t = " ".join(re.sub(r"^#+\s*", "", ln) for ln in lines)
        t = re.sub(r"\*\*(.+?)\*\*", r"\1", t)
        t = re.sub(r"(?<!\w)_(.+?)_(?!\w)", r"\1", t)
        t = re.sub(r"\s+([,.;:!?])", r"\1", t)            # "Ring_ ," -> "Ring,"
        t = re.sub(r"([\u2018\u201c])\s+(?=\w)", r"\1", t)  # "\u2018 Go" -> "\u2018Go"
        t = re.sub(r"(?<=\S)\s+([\u2019\u201d])(?=\s|$|[,.;:!?])", r"\1", t)
        t = re.sub(r"\s{2,}", " ", t).strip()
        if t:
            paras.append([t, heading])
    return paras


def furniture_key(t: str) -> str:
    """Running header text with its page number removed, for counting repeats."""
    k = t.lower()
    k = re.sub(rf"^{ROMAN_OR_NUM}\s+|\s+{ROMAN_OR_NUM}$", "", k)
    return re.sub(r"[^a-z]", "", k)


def strip_page_furniture(paras: list) -> tuple:
    counts = {}
    for t, _ in paras:
        if len(t) <= 70:
            k = furniture_key(t)
            counts[k] = counts.get(k, 0) + 1
    out, dropped = [], 0
    for t, h in paras:
        if PAGE_NUM_RE.match(t):
            dropped += 1
            continue
        k = furniture_key(t)
        # Repeated short line with no sentence punctuation = running header.
        # Real headings ("## PROLOGUE", "### Chapter 1") share text with the
        # running headers but aren't all lowercase like the furniture is.
        if (h and t != t.lower()) or re.match(r"^(chapter|book|part)\s+\w+$", t, re.I):
            out.append([t, h])
            continue
        if len(t) <= 70 and k and counts.get(k, 0) >= 3 and not t.endswith(END_PUNCT):
            dropped += 1
            continue
        out.append([t, h])
    return out, dropped


def trim_matter(paras: list, start_re, end_re, auto_front: bool, auto_back: bool) -> tuple:
    n = len(paras)
    first, last = 0, n
    if start_re:
        first = next((i for i, (t, _) in enumerate(paras) if start_re.search(t)), 0)
    elif auto_front:
        first = next((i for i, (t, h) in enumerate(paras[: max(1, n // 4)]) if h and FRONT_RE.match(t)), 0)
    if end_re:
        last = next((i for i in range(first + 1, n) if end_re.search(paras[i][0])), n)
    elif auto_back:
        last = next((i for i in range(max(first + 1, n * 4 // 5), n)
                     if len(paras[i][0]) <= 40 and BACK_RE.match(paras[i][0])), n)
    return paras[first:last], first, n - last


def rejoin_split_paras(paras: list) -> tuple:
    """Rejoin a paragraph a page break cut in two: it ends mid-sentence and the
    next one starts with a lowercase letter."""
    out, joined = [], 0
    for t, h in paras:
        if out and not h and not out[-1][1] and t[:1].islower():
            prev = out[-1][0]
            if re.search(r"[a-z]-$", prev):
                out[-1][0] = prev[:-1] + t           # "ques-" + "tions"
                joined += 1
                continue
            if re.search(r"[a-z,;\u2013\u2014-]$", prev):
                out[-1][0] = prev + " " + t
                joined += 1
                continue
        out.append([t, h])
    return out, joined


def clean_markdown(s: str, a) -> str:
    paras = md_to_paras(s)
    paras, furn = strip_page_furniture(paras)
    start_re = re.compile(a.start, re.I) if a.start else None
    end_re = re.compile(a.end, re.I) if a.end else None
    paras, front, back = trim_matter(paras, start_re, end_re, not a.keep_front, not a.keep_back)
    paras, joined = rejoin_split_paras(paras)
    print(f"removed {furn} header/page-number lines, {front} front-matter and {back} "
          f"back-matter paragraphs; rejoined {joined} page-split paragraphs")
    if paras:
        print(f"  starts: {paras[0][0][:60]!r}\n  ends:   {paras[-1][0][-60:]!r}")
    return "\n\n".join(t for t, _ in paras) + "\n"


def unwrap(s: str) -> str:
    paras = re.split(r"\n\s*\n", s)
    out = []
    for p in paras:
        lines = [ln.strip() for ln in p.split("\n") if ln.strip()]
        if not lines:
            continue
        out.append(" ".join(lines))
    return "\n\n".join(out) + "\n"


def split_parts(s: str, n: int) -> list:
    """Cut into n parts of roughly equal size, each starting at a chapter/book
    heading when there is one near the cut."""
    cuts = [m.start() + 2 for m in re.finditer(r"\n\n(?=(chapter|book|part)\s+\w+\n)", s, re.I)]
    bounds, prev = [0], 0
    for i in range(1, n):
        target = len(s) * i // n
        near = [c for c in cuts if c > prev]
        cut = min(near, key=lambda c: abs(c - target)) if near else target
        if abs(cut - target) > len(s) // (2 * n):  # no heading close by: cut at a paragraph
            cut = s.find("\n\n", target) + 2 or target
        # "BOOK TWO" directly above "Chapter 1": start the part at the book heading.
        before = s.rfind("\n\n", 0, cut - 2) + 2
        if re.match(r"(book|part)\s+\w+\n", s[before:cut], re.I):
            cut = before
        bounds.append(cut)
        prev = cut
    bounds.append(len(s))
    return [s[a:b].strip("\n") + "\n" for a, b in zip(bounds, bounds[1:])]


def chapter_cuts(s: str) -> list:
    """Offsets where a chapter starts ("Chapter 3", "CHAPTER III", "Book Two" ...).
    A Book/Part heading directly above a chapter heading starts that chapter."""
    cuts = []
    for m in re.finditer(r"(?:^|\n\n)(?=(chapter|book|part)\s+\w+\.?\n)", s, re.I):
        c = m.end()
        if cuts and re.match(r"(book|part)\s+\w+\.?\n\n$", s[cuts[-1]:c], re.I):
            continue  # "BOOK TWO" + "Chapter 1": one cut, at the book heading
        cuts.append(c)
    return cuts


def split_chapters(s: str) -> list:
    """One part per chapter; text before the first chapter (prologue, foreword)
    is its own part."""
    bounds = [0] + [c for c in chapter_cuts(s) if c > 0] + [len(s)]
    parts = [s[a:b].strip("\n") + "\n" for a, b in zip(bounds, bounds[1:])]
    return [p for p in parts if p.strip()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("-o", "--out")
    ap.add_argument("--start", help="regex: drop everything before the first paragraph matching it")
    ap.add_argument("--end", help="regex: drop the first paragraph matching it and everything after")
    ap.add_argument("--keep-front", action="store_true", help=".md: don't auto-drop front matter")
    ap.add_argument("--keep-back", action="store_true", help=".md: don't auto-drop back matter")
    ap.add_argument("--md", action="store_true", help="treat input as Markdown even without a .md suffix")
    ap.add_argument("--chapters", action="store_true",
                    help="write one file per chapter (name.00.txt, name.01.txt ...), "
                         "so the watch only ever holds a small file")
    ap.add_argument("--split", type=int, default=0, metavar="N",
                    help="write N part files (name.part1.txt ...), cut at chapter headings; "
                         "use when the book is bigger than the watch's storage")
    a = ap.parse_args()
    is_md = a.md or a.src.lower().endswith((".md", ".markdown"))

    raw = open(a.src, "rb").read()
    try:
        s = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        s = raw.decode("latin-1")
    s = s.replace("\r\n", "\n").replace("\r", "\n")
    s = strip_boilerplate(s)
    if is_md:
        s = to_ascii(clean_markdown(s, a))
    else:
        s = re.sub(r"(?<!\w)_(.+?)_(?!\w)", r"\1", s, flags=re.S)  # _italics_
        s = to_ascii(s)
        if a.start or a.end:
            paras, _, _ = trim_matter([[p, True] for p in unwrap(s).strip().split("\n\n")],
                                      re.compile(a.start, re.I) if a.start else None,
                                      re.compile(a.end, re.I) if a.end else None, False, False)
            s = "\n\n".join(p for p, _ in paras) + "\n"
        else:
            s = unwrap(s)

    out = a.out or os.path.splitext(a.src)[0] + ".watch.txt"
    if a.chapters:
        stem, ext = os.path.splitext(out)
        parts = split_chapters(s)
        width = max(2, len(str(len(parts) - 1)))
        outs = [(f"{stem}.{i:0{width}d}{ext or '.txt'}", part) for i, part in enumerate(parts)]
    elif a.split > 1:
        stem, ext = os.path.splitext(out)
        outs = [(f"{stem}.part{i}{ext or '.txt'}", part) for i, part in enumerate(split_parts(s, a.split), 1)]
    else:
        outs = [(out, s)]
    print(f"{len(raw)/1024:.0f} KB in")
    for path, text in outs:
        with open(path, "w", encoding="ascii", newline="\n") as f:
            f.write(text)
        print(f"{path}: {len(text)/1024:.0f} KB, starts {text[:40].splitlines()[0]!r}")
        if len(text) > FLASH_BUDGET:
            print(f"WARNING: bigger than the watch's {FLASH_BUDGET//1024} KB book storage "
                  f"(try --split {len(s) // (FLASH_BUDGET * 3 // 4) + 1})", file=sys.stderr)


if __name__ == "__main__":
    main()
