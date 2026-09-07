# Spectral EXR Example Makefile

CC = gcc
CXX = g++
CFLAGS = -O2 -I../../deps/miniz
CXXFLAGS = -std=c++11 -O2 -I../../ -I../../deps/miniz

# Use clang if available
ifneq ($(shell which clang 2>/dev/null),)
	CC = clang
	CXX = clang++
endif

TARGET = spectral_example

all: $(TARGET)

miniz.o: ../../deps/miniz/miniz.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): spectral_example.cc miniz.o ../../tinyexr.h
	$(CXX) $(CXXFLAGS) -o $@ spectral_example.cc miniz.o

# Run tests
test: $(TARGET)
	./$(TARGET) roundtrip

# Create sample files
samples: $(TARGET)
	./$(TARGET) write emissive_sample.exr emissive 256 256 6500
	./$(TARGET) write reflective_sample.exr reflective 256 256
	@echo "Created emissive_sample.exr and reflective_sample.exr"

clean:
	rm -f $(TARGET) miniz.o *.exr

.PHONY: all test samples clean
