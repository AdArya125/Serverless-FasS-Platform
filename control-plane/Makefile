CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread -I include -I ../common/include -MMD -MP
LDFLAGS := -pthread

BUILD := build
BIN := $(BUILD)/faas-control-plane

CP_SRC := $(wildcard src/*.cpp)
COMMON_SRC := ../common/src/http_server.cpp ../common/src/http_client.cpp
SRC := $(CP_SRC) $(COMMON_SRC)
OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SRC)))
DEP := $(OBJ:.o=.d)

vpath %.cpp src ../common/src

LIB_OBJ := $(filter-out $(BUILD)/main.o,$(OBJ))
TEST_SRC := $(wildcard ../tests/control_plane/*.cpp) ../tests/test_main.cpp

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

test: $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -I ../tests -o $(BUILD)/run_tests $(LIB_OBJ) $(TEST_SRC) $(LDFLAGS)
	./$(BUILD)/run_tests

clean:
	rm -rf $(BUILD)

-include $(DEP)
