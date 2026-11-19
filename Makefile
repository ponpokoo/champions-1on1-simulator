CC ?= gcc
CFLAGS ?= -O3 -march=native
PYTHON ?= python3

.PHONY: build verify clean
build:
	$(CC) $(CFLAGS) -o engine/engine engine/engine.c -lm

verify: build
	$(PYTHON) engine/verify_matrix.py

clean:
	rm -f engine/engine engine/engine.exe
