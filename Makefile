.PHONY: all build test test-integration test-lifecycle test-warm-reuse run functions clean

all: build

build:
	$(MAKE) -C control-plane
	$(MAKE) -C cli

test:
	$(MAKE) -C control-plane test

test-integration: build
	./tests/integration/test_invoke.sh

test-lifecycle: build
	./tests/integration/test_lifecycle.sh

test-warm-reuse: build
	./tests/integration/test_warm_reuse.sh

run: build
	./control-plane/build/faas-control-plane

functions:
	docker build -t hello:v1 functions/hello
	docker build -t sleep:v1 functions/sleep

clean:
	$(MAKE) -C control-plane clean
	$(MAKE) -C cli clean
