# TinyEXR V3 WASM Benchmark Makefile

EMCC ?= emcc
EMCXX ?= em++
BUILD_DIR = dist
SRC_DIR = src
TINYEXR_ROOT = ../..
MINIZ_DIR = $(TINYEXR_ROOT)/deps/miniz

COMMON_FLAGS = -O2 \
    -I$(TINYEXR_ROOT) \
    -I$(MINIZ_DIR) \
    -DMINIZ_NO_STDIO \
    -DTINYEXR_USE_MINIZ=1 \
    -DTINYEXR_USE_THREAD=0

LINK_FLAGS = --bind \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=134217728 \
    -s MAXIMUM_MEMORY=1073741824 \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s EXPORT_NAME="createTinyExrV3Module" \
    -s ENVIRONMENT='web,node' \
    -s WASM=1 \
    -s SINGLE_FILE=0 \
    -s NO_EXIT_RUNTIME=1 \
    -s FILESYSTEM=0 \
    -s DISABLE_EXCEPTION_CATCHING=1

OBJECTS = $(BUILD_DIR)/miniz.o \
          $(BUILD_DIR)/tinyexr_c_impl.o \
          $(BUILD_DIR)/v3_benchmark_bindings.o

.PHONY: all clean run-node run-browser

all: $(BUILD_DIR)/tinyexr_v3_wasm.js

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/miniz.o: $(MINIZ_DIR)/miniz.c | $(BUILD_DIR)
	$(EMCC) $(COMMON_FLAGS) -c $< -o $@

$(BUILD_DIR)/tinyexr_c_impl.o: $(TINYEXR_ROOT)/tinyexr_c_impl.c | $(BUILD_DIR)
	$(EMCC) $(COMMON_FLAGS) -c $< -o $@

$(BUILD_DIR)/v3_benchmark_bindings.o: $(SRC_DIR)/v3_benchmark_bindings.cc | $(BUILD_DIR)
	$(EMCXX) $(COMMON_FLAGS) -std=c++17 -c $< -o $@

$(BUILD_DIR)/tinyexr_v3_wasm.js: $(OBJECTS)
	$(EMCXX) $(LINK_FLAGS) $^ -o $@
	@echo "Build complete: $(BUILD_DIR)/tinyexr_v3_wasm.js"

run-node: all
	node js/benchmark.mjs

run-browser: all
	@echo "Starting HTTP server on port 8080..."
	@echo "Open http://localhost:8080/html/ in your browser"
	python3 -m http.server 8080

clean:
	rm -rf $(BUILD_DIR)
