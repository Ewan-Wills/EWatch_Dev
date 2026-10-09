# ewsim_host: offline EWatch app renderer (host build of an EWatchOS2.1-family firmware).
#   make                       build EWatchOS2.1  -> build/EWatchOS2.1/ewsim
#   EW_APPS="doom books" make  with app modules -> build/EWatchOS2.1+books+doom/ewsim
#   make PROJ=<path>           another EWatchOS2.1-family project folder
#   make clean                 (removes every build of the project, all module sets)
PROJ ?= EWatchOS2.1
ENV  ?= ewatch
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || echo 8)

all:
	@B=$$(python3 tools/genconfig.py $(PROJ) $(ENV)) && $(MAKE) --no-print-directory -j$(JOBS) -f tools/build.mk B=$$B

clean:
	rm -rf build/$(notdir $(PROJ)) build/$(notdir $(PROJ))+*

.PHONY: all clean
