.PHONY: all build test test-integration run functions clean

all: build

build:
	$(MAKE) -C control-plane
	$(MAKE) -C cli

test:
	$(MAKE) -C control-plane test

test-integration: build
	./tests/integration/test_invoke.sh

run: build
	./control-plane/build/faas-control-plane

functions:
	docker build -t hello:v1 functions/hello

clean:
	$(MAKE) -C control-plane clean
	$(MAKE) -C cli clean
