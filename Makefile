CC ?= gcc
CFLAGS ?= -O3 -march=native
PYTHON ?= python3

.PHONY: build verify verify-obsh4 clean
build:
	$(CC) $(CFLAGS) -o engine/engine engine/engine.c -lm

verify: build
	$(PYTHON) engine/verify_matrix.py

verify-obsh4:
	$(CC) $(CFLAGS) -o engine/test_obsh4_diagnostics engine/test_obsh4_diagnostics.c -lm
	./engine/test_obsh4_diagnostics

clean:
	rm -f engine/engine engine/engine.exe
	rm -f engine/test_obsh4_diagnostics engine/test_obsh4_diagnostics.exe
