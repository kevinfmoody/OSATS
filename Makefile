CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pthread
LDFLAGS ?= -pthread

# Linux provides kqueue through libkqueue. macOS has it in libc.
KQUEUE_CFLAGS := $(shell pkg-config --cflags libkqueue 2>/dev/null)
KQUEUE_LIBS := $(shell pkg-config --libs libkqueue 2>/dev/null)
CXXFLAGS += $(KQUEUE_CFLAGS)
LDFLAGS += $(KQUEUE_LIBS)

COMMON = position.cc compressible.cc aircraft.cc message.cc

.PHONY: all clean

all: osats-server osats-client

osats-server: $(COMMON) store.cc server.cc main.cc
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

osats-client: $(COMMON) client.cc clientmain.cc
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f osats-server osats-client *.o
