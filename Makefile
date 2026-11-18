CC ?= gcc
CFLAGS ?= -O3 -march=native

.PHONY: build verify clean
build:
	$(CC) $(CFLAGS) -o engine/engine engine/engine.c -lm

verify: build
	@a=$$(./engine/engine matrix 3000 12345 | tr -d '\r' | md5sum | cut -d' ' -f1); \
	b=$$(ENGINE_OBSERVER=1 ENGINE_OBS_COMPAT=1 ./engine/engine matrix 3000 12345 | tr -d '\r' | md5sum | cut -d' ' -f1); \
	want=$$(tr -d '\r\n' < engine/fixtures/expected_matrix_md5.txt); \
	echo "plain: $$a"; echo "observer: $$b"; echo "expected: $$want"; \
	if [ "$$a" = "$$want" ] && [ "$$b" = "$$want" ]; then echo "VERIFY OK"; else echo "VERIFY FAILED"; exit 1; fi

clean:
	rm -f engine/engine engine/engine.exe
