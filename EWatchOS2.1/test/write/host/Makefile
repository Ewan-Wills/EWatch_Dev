# Host bench for the Write recogniser (same source as the firmware).
SRC = ../../src/apps/write
bench: bench.cpp $(SRC)/write_recog.cpp $(SRC)/write_recog.h $(SRC)/write_templates.h
	xcrun clang++ -std=c++17 -O2 -Wall -I$(SRC) $(EXTRA) -o $@ bench.cpp $(SRC)/write_recog.cpp
