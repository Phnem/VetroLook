.PHONY: clean all check check-v3

CC = clang
CXX = clang++
CXXFLAGS = -fsanitize=address,undefined -g -O2 -std=c++11 -I ../../deps/miniz
CXXFLAGS_V3 = -fsanitize=address,undefined -g -O2 -std=c++17 -I ../../deps/miniz
CXXFLAGS_BENCH = -g -O2 -std=c++17 -I ../../deps/miniz
CFLAGS = -fsanitize=address,undefined -g -O2 -I ../../deps/miniz
CFLAGS_BENCH = -g -O2 -I ../../deps/miniz

all: tester tester-v3

miniz.o:
	$(CC) -c $(CFLAGS) ../../deps/miniz/miniz.c

miniz-v3.o:
	$(CC) -c $(CFLAGS) ../../deps/miniz/miniz.c -o miniz-v3.o

tinyexr_c_impl.o: ../../tinyexr_c_impl.c ../../tinyexr_c.h ../../tinyexr_huffman.hh ../../tinyexr_piz.hh
	$(CXX) -c $(CXXFLAGS_V3) -x c++ ../../tinyexr_c_impl.c -I ../.. -o tinyexr_c_impl.o

tester: tester.cc ../../tinyexr.h miniz.o
	$(CXX) $(CXXFLAGS) -o tester tester.cc miniz.o

tester-v3: tester-v3.cc ../../tinyexr_v3.hh ../../tinyexr_c.h tinyexr_c_impl.o miniz-v3.o
	$(CXX) $(CXXFLAGS_V3) -o tester-v3 tester-v3.cc tinyexr_c_impl.o miniz-v3.o -I ../..

tinyexr_c_impl_bench.o: ../../tinyexr_c_impl.c ../../tinyexr_c.h ../../tinyexr_huffman.hh ../../tinyexr_piz.hh
	$(CXX) -c $(CXXFLAGS_BENCH) -x c++ ../../tinyexr_c_impl.c -I ../.. -o tinyexr_c_impl_bench.o

miniz-bench.o:
	$(CC) -c $(CFLAGS_BENCH) ../../deps/miniz/miniz.c -o miniz-bench.o

bench-v3: bench_v3.cc ../../tinyexr_v3.hh ../../tinyexr_c.h tinyexr_c_impl_bench.o miniz-bench.o
	$(CXX) $(CXXFLAGS_BENCH) -o bench-v3 bench_v3.cc tinyexr_c_impl_bench.o miniz-bench.o -I ../..

check: tester
	./tester

check-v3: tester-v3
	./tester-v3

clean:
	rm -rf tester tester-v3 bench-v3 miniz.o miniz-v3.o miniz-bench.o tinyexr_c_impl.o tinyexr_c_impl_bench.o
