CXX := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -O0 -g \
            -fsanitize=address,undefined

BUILD_DIR := build
TARGET := $(BUILD_DIR)/hydra

SRCS := main.cpp bigint.cpp
OBJS := $(SRCS:%.cpp=$(BUILD_DIR)/%.o)

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.cpp bigint.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET)

test: | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) bigint_test.cpp bigint.cpp \
		-o $(BUILD_DIR)/bigint_test
	./$(BUILD_DIR)/bigint_test

clean:
	rm -rf $(BUILD_DIR)

