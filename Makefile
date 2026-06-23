CXX = clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -I/opt/include
LDLIBS ?= -L/opt/lib/native -lkoopa -pthread

SRC := $(wildcard src/*.cpp)
BUILD_DIR ?= .
OUT := $(BUILD_DIR)/compiler

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(OUT)
