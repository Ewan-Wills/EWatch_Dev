# bookreader PC uploader

Send a plain-text `.txt` book to the EWatch over USB. The watch saves it to
LittleFS and it appears in the on-watch **Books** library.

## Setup (once)

```sh
cd EWatchOS2.1/companions/books
python3 -m venv .venv && source .venv/bin/activate   # optional but tidy
pip install -r requirements.txt
```

## Upload a book

1. **On the watch:** swipe up → open **Books**. Opening the app starts the
   serial listener (you'll see `>>BOOK READY` on the port). Keep it on the
   Library screen.
2. **Close any serial monitor** (the PlatformIO monitor, `screen`, etc.) — only
   one program can own the USB port at a time.
3. Run:

```sh
python3 upload.py mybook.txt
# or name the port explicitly:
python3 upload.py --port /dev/cu.usbmodem101 mybook.txt
```

On success you'll see `>>BOOK OK mybook.txt <bytes>` and the book shows up in
the Library (tap it to read).

## Getting books

Free, public-domain plain text: [Project Gutenberg](https://www.gutenberg.org)
(pick "Plain Text UTF-8" on a book's page, e.g.
`https://www.gutenberg.org/ebooks/1342.txt.utf-8` for Pride and Prejudice), or
[Standard Ebooks](https://standardebooks.org) (cleaner editions, but EPUB
only: convert with `pandoc book.epub -t plain -o book.txt` first). Run the
text through `prep_book.py` before uploading:

```sh
python3 prep_book.py pg1342.txt -o pride.txt
python3 upload.py pride.txt
```

It strips the Gutenberg licence header/footer, unwraps the ~70-column hard
line breaks (the reader breaks on every newline, so raw files come out
ragged), and folds curly quotes, dashes and accents to ASCII (the fonts only
have printable ASCII). It warns if the result is over the 896 KB book storage.

### Markdown from a PDF

A `.md` made from a PDF (or EPUB) gets extra cleanup: running page headers
("110 the fellowship of the ring"), page numbers, picture/OCR junk, contents
tables, markup, the front matter before the first Prologue/Chapter/Book
heading and the back matter from Maps/Copyright/About the Publisher on. It also
rejoins paragraphs split by a page break and fixes accents the PDF wrote
as separate marks. It prints what it cut and the first/last lines so you can
check.

```sh
python3 prep_book.py fellowship.md -o fellowship.txt
python3 prep_book.py book.md --keep-front               # keep forewords etc.
python3 prep_book.py book.md --start "^BOOK ONE" --end "^APPENDIX"
```

### One file per chapter

`--chapters` writes `name.00.txt` (prologue / front), `name.01.txt`, … one per
chapter, so each upload is small (~30-80 KB) and you can keep just the
chapters you're reading on the watch:

```sh
python3 prep_book.py fellowship.md -o fellowship.txt --chapters
python3 upload.py fellowship.01.txt
```

### Books bigger than the storage

`--split N` writes `name.part1.txt`, `name.part2.txt`, … cut at chapter
headings. Upload part 1, delete it when you're done and upload part 2:

```sh
python3 prep_book.py fellowship.md -o fellowship.txt --split 2
python3 upload.py fellowship.part1.txt
```

## List / delete

```sh
python3 upload.py --list
python3 upload.py --delete mybook.txt
```

## Notes

- Port auto-detection picks the first `cu.usbmodem*` (macOS) / `ttyACM*`
  (Linux). Use `--port` if you have several devices.
- Native USB-CDC ignores the baud rate, so transfer runs at full USB speed.
- Filenames are sanitised on the watch (safe characters only, forced `.txt`
  suffix, basename only). A book over ~2 MB is rejected (flash is ~896 KB
  anyway).
- The upload is CRC32-checked end to end; a corrupted transfer is rejected and
  the partial file is discarded.
