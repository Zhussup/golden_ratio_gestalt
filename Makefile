# Builds every standalone .c/.cpp animation in this directory.
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall
LDLIBS    = $(shell pkg-config --libs glfw3) -lGL

BINS := $(patsubst %.cpp,%,$(wildcard *.cpp))

all: $(BINS)

%: %.cpp
	$(CXX) $(CXXFLAGS) $< $(LDLIBS) -o $@

clean:
	rm -f $(BINS)

.PHONY: all clean