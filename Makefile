CXX = clang++
INC_DIR ?= /opt/include
LIB_DIR ?= /opt/lib/native
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -I$(INC_DIR)
LDLIBS ?= -L$(LIB_DIR) -lkoopa -pthread

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
