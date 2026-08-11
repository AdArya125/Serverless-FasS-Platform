.PHONY: all build test run functions clean

all: build

build:
	$(MAKE) -C control-plane
	$(MAKE) -C cli

test:
	$(MAKE) -C control-plane test

run: build
	./control-plane/build/faas-control-plane

functions:
	docker build -t hello:v1 functions/hello

clean:
	$(MAKE) -C control-plane clean
	$(MAKE) -C cli clean
