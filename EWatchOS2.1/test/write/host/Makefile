# Host benches for the Write recognisers (same source as the firmware).
SRC = ../../../src/apps/modules/write
all: bench cnn_bench lm_bench
bench: bench.cpp $(SRC)/write_recog.cpp $(SRC)/write_recog.h $(SRC)/write_templates.h
	xcrun clang++ -std=c++17 -O2 -Wall -I$(SRC) $(EXTRA) -o $@ bench.cpp $(SRC)/write_recog.cpp
cnn_bench: cnn_bench.cpp $(SRC)/write_cnn.cpp $(SRC)/write_cnn.h $(SRC)/write_cnn_weights.h
	xcrun clang++ -std=c++17 -O2 -Wall -I$(SRC) $(EXTRA) -o $@ cnn_bench.cpp $(SRC)/write_cnn.cpp
lm_bench: lm_bench.cpp $(SRC)/write_lm.cpp $(SRC)/write_lm.h $(SRC)/write_lm_data.h
	xcrun clang++ -std=c++17 -O2 -Wall -I$(SRC) $(EXTRA) -o $@ lm_bench.cpp $(SRC)/write_lm.cpp
