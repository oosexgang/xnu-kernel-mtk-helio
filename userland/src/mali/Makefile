# hakios-mali on this machine: the library, its tests and the probe against the fake GPU (fake_gpu.hpp).
#
#   make test     build and run tests/host_test and `mali-probe --fake` (macOS or Linux, C11 and C++17)
#   make clean
#
# The descriptors are packed by Mesa's generated genxml/v7_pack.h, made here from MESA's v7.xml by Mesa's own
# gen_pack.py: MESA is the Mesa source tree kernel/deps/setup.sh unpacks (Mesa 25.0.7). The phone's build is
# system27/userland/mali27.sh.

MESA     ?= ../../../kernel/deps/build/mesa/mesa-25.0.7
OUT      ?= out/host
CC       ?= cc
CXX      ?= c++
PYTHON   ?= python3

GEN      = $(OUT)/gen/genxml/v7_pack.h
# v7.c only: Mesa's generated code and util headers (they warn about parameters they do not use)
V7FLAGS  = -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-shift-count-overflow -Werror \
           -I$(OUT)/gen -I$(MESA)/src -I$(MESA)/include -I$(MESA)/src/panfrost/lib
CXXFLAGS_MALI = -std=c++17 -O1 -g -Wall -Wextra -Werror -I.
LIBSRC   = gpu.cpp pass.cpp draw.cpp blit.cpp metal.cpp fake_gpu.cpp iokit_transport.cpp
LIBOBJ   = $(LIBSRC:%.cpp=$(OUT)/%.o) $(OUT)/v7.o
ifeq ($(shell uname),Darwin)
LDLIBS   = -framework IOKit -framework CoreFoundation -framework IOSurface
endif

.PHONY: test clean
test: $(OUT)/host_test $(OUT)/mali-probe
	$(OUT)/host_test
	$(OUT)/mali-probe --fake

$(GEN): $(MESA)/src/panfrost/lib/genxml/v7.xml $(MESA)/src/panfrost/lib/genxml/gen_pack.py
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/panfrost/lib/genxml/gen_pack.py $< > $@.tmp && mv $@.tmp $@

$(OUT)/v7.o: v7.c v7.h $(GEN)
	@mkdir -p $(OUT)
	$(CC) $(V7FLAGS) -c v7.c -o $@

$(OUT)/%.o: %.cpp mali.hpp pass.hpp draw.hpp metal.hpp program.h shaders_v7.h fake_gpu.hpp v7.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS_MALI) -c $< -o $@

$(OUT)/host_test.o: tests/host_test.cpp mali.hpp pass.hpp fake_gpu.hpp v7.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS_MALI) -c $< -o $@

$(OUT)/host_test: $(OUT)/host_test.o $(LIBOBJ)
	$(CXX) -o $@ $^ $(LDLIBS)

$(OUT)/mali-probe: $(OUT)/probe.o $(LIBOBJ)
	$(CXX) -o $@ $^ $(LDLIBS)

clean:
	rm -rf out
