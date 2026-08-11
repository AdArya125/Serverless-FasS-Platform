CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread -I ../common/include -MMD -MP
LDFLAGS := -pthread

BUILD := build
BIN := $(BUILD)/cloudfn

SRC := src/main.cpp ../common/src/http_client.cpp
OBJ := $(BUILD)/main.o $(BUILD)/http_client.o
DEP := $(OBJ:.o=.d)

vpath %.cpp src ../common/src

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(DEP)
