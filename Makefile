.PHONY: all build test test-integration test-lifecycle run functions clean

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

run: build
	./control-plane/build/faas-control-plane

functions:
	docker build -t hello:v1 functions/hello
	docker build -t sleep:v1 functions/sleep

clean:
	$(MAKE) -C control-plane clean
	$(MAKE) -C cli clean
