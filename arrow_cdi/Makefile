# arrow_cdi build
#   make            -> release shared + static libraries (build/)
#   make asan       -> ASan-instrumented libraries and test binaries
#   make test       -> build and run the native test suite (ASan)
#   make clean
#
# The Python ctypes wrapper loads build/libarrow_cdi.so; ASan variants are
# named libarrow_cdi_asan.so so both can coexist.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wno-sign-conversion -fPIC -O2 -g -D_POSIX_C_SOURCE=200809L
IFLAGS  := -Iinclude -Isrc
SRC     := src/ac_alloc.c src/ac_payload.c src/ac_create.c src/ac_export.c
OBJ     := $(SRC:src/%.c=build/%.o)

ASAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer -O1 -g

.PHONY: all asan test clean

all: build/libarrow_cdi.so build/libarrow_cdi.a build/libconsumer.so

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

build/libarrow_cdi.so: $(OBJ)
	$(CC) -shared -o $@ $(OBJ)

build/libarrow_cdi.a: $(OBJ)
	ar rcs $@ $(OBJ)

# Non-instrumented in-process consumer for the Python demo (memory safety
# is covered separately by the ASan build of the same source).
build/libconsumer.so: demo/consumer.c $(OBJ)
	$(CC) $(CFLAGS) $(IFLAGS) -DARROW_CDI_CONSUMER_LIB -shared \
	      demo/consumer.c $(OBJ) -o $@

# ---- ASan variants ----
ASAN_OBJ := $(SRC:src/%.c=build/asan_%.o)
build/asan_%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $(IFLAGS) -c $< -o $@

asan: build/libarrow_cdi_asan.so build/native_tests_asan build/consumer_asan \
      build/libconsumer_asan.so

build/libarrow_cdi_asan.so: $(ASAN_OBJ)
	$(CC) -shared $(ASAN_FLAGS) -o $@ $(ASAN_OBJ)

build/native_tests_asan: tests/native_tests.c $(ASAN_OBJ)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $(IFLAGS) $< $(ASAN_OBJ) -o $@

build/consumer_asan: demo/consumer.c $(ASAN_OBJ)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $(IFLAGS) $< $(ASAN_OBJ) -o $@

# In-process consumer: loaded by the Python demo through ctypes so the
# exported shell pointers stay valid (same address space as the producer).
build/libconsumer_asan.so: demo/consumer.c $(ASAN_OBJ)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $(IFLAGS) -DARROW_CDI_CONSUMER_LIB \
	      -shared $< $(ASAN_OBJ) -o $@

test: asan
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./build/native_tests_asan

clean:
	rm -rf build
