APP := ds
SRC := src/main.cpp
OBJ_DIR := build
BIN_DIR := bin

DEBUG_BIN := $(BIN_DIR)/$(APP)_debug
RELEASE_BIN := $(BIN_DIR)/$(APP)

CXX := g++
CXXFLAGS_COMMON := -std=c++20 -Wall -Wextra -pedantic
CXXFLAGS_DEBUG := $(CXXFLAGS_COMMON) -g -O0
CXXFLAGS_RELEASE := $(CXXFLAGS_COMMON) -O3 -DNDEBUG -flto
LDFLAGS_RELEASE := -flto

.PHONY: all build run nothing release install clean

all: run

build: $(DEBUG_BIN)

run: $(DEBUG_BIN)
	./$(DEBUG_BIN)

nothing: run

release: $(RELEASE_BIN)
	@if command -v upx >/dev/null 2>&1; then \
		echo "Compressing $(RELEASE_BIN) with upx..."; \
		upx --best --lzma $(RELEASE_BIN); \
	else \
		echo "upx not found; skipping compression."; \
	fi

install: release
	sudo install -d /usr/local/share/ds
	sudo install -m 644 configs/configs.ds /usr/local/share/ds/configs.ds
	sudo install -m 644 configs/themes.ds /usr/local/share/ds/themes.ds
	sudo install -m 755 $(RELEASE_BIN) /usr/local/bin/$(APP)
	@echo "Installed /usr/local/bin/$(APP) and /usr/local/share/ds/{configs.ds,themes.ds}"

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

$(DEBUG_BIN): $(SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS_DEBUG) $< -o $@

$(RELEASE_BIN): $(SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS_RELEASE) $(LDFLAGS_RELEASE) $< -o $@
	strip -s $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)/$(APP) $(BIN_DIR)/$(APP)_debug
