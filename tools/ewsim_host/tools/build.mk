# Compiles one project's host build. Driven by the top-level Makefile / ewsimrec:
#   make -f tools/build.mk B=build/<proj>
.DEFAULT_GOAL := $(B)/ewsim
include $(B)/config.mk
CC  := xcrun clang
CXX := xcrun clang++
OPT ?= -O2
CFLAGS   := -std=gnu11 $(OPT) -g -w -fno-strict-aliasing
CXXFLAGS := -std=gnu++17 $(OPT) -g -w -fno-strict-aliasing -Wno-c++11-narrowing
$(B)/ewsim: $(OBJS)
	@echo '  LD $@'
	@$(CXX) $(OBJS) -lz -o $@
-include $(OBJS:.o=.d)
