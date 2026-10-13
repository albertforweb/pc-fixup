CMAKE ?= cmake
BUILD_DIR ?= build
DIST_DIR ?= dist

.PHONY: all configure build release clean

all: build

configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)/debug" -DCMAKE_BUILD_TYPE=Debug

build: configure
	$(CMAKE) --build "$(BUILD_DIR)/debug" --config Debug --target WinStartReview

release:
	$(CMAKE) -S . -B "$(BUILD_DIR)/release" -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build "$(BUILD_DIR)/release" --config Release --target WinStartReview
	$(CMAKE) --install "$(BUILD_DIR)/release" --config Release --prefix "$(DIST_DIR)"

clean:
	$(CMAKE) -E rm -rf "$(BUILD_DIR)" "$(DIST_DIR)"
