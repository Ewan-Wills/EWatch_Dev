# Write recogniser: training + evaluation

Builds the CNN (`src/apps/modules/write/write_cnn_weights.h`) and the English context
tables (`write_lm_data.h`), and drives the on-watch sweeps. See the module NOTES.md.

Data is not in the repo. Put it in `data/` next to these scripts (or symlink it):
```
data/uji/ujipenchars2.txt        https://archive.ics.uci.edu/static/public/177/uji+pen+characters+version+2.zip
data/emnist/gzip/*.gz            https://biometrics.nist.gov/cs_links/EMNIST/gzip.zip  (560 MB)
data/lm/count_1w.txt             https://norvig.com/ngrams/count_1w.txt
```
(The working copy used on 2026-10-13 is `~/Documents/EWatch/ml-write/data`.)

```
python3.12 -m venv .venv && .venv/bin/pip install torch numpy pillow scipy
.venv/bin/python prep_data.py 40                       # 40 trn writers (held-out eval)
.venv/bin/python train.py 12 write_cnn_weights.h       # prints unseen-writer accuracy, int8 check
ALLWRITERS=1 .venv/bin/python prep_data.py 40          # shipping model: all 60 writers
WTRAIN=data/train_all.npz .venv/bin/python train.py 14 write_cnn_weights.h
.venv/bin/python lm.py sim                             # what context buys (unseen writers)
.venv/bin/python lm.py header ../../../src/apps/modules/write/write_lm_data.h 20000
```
Options: `WCNN=16,32,64,128` (widths), `WCASE=1` (62 classes, merged), `WPT=` (checkpoint name).
