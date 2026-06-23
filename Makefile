CXX = clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

SRC := $(wildcard src/*.cpp)
BUILD_DIR ?= .
OUT := $(BUILD_DIR)/compiler

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(OUT)
