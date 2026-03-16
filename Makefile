CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic

DOCKER_IMAGE ?= ubuntu:24.04
DOCKER_DEPS  ?= build-essential tmux procps

.PHONY: all clean run shell

all: mini-tmux

mini-tmux: mini_tmux.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f mini-tmux

# Run mini-tmux interactively inside Docker (Linux environment)
run:
	docker run --rm -it \
		-v $(PWD):/src:ro \
		$(DOCKER_IMAGE) bash -c \
		'apt-get update -qq && apt-get install -y -qq $(DOCKER_DEPS) >/dev/null 2>&1 && \
		 cp -r /src /build && cd /build && make clean >/dev/null 2>&1; make && \
		 SHELL=/bin/bash exec /build/mini-tmux'

# Drop into a Docker shell with mini-tmux built (for manual testing, attach, etc.)
shell:
	docker run --rm -it \
		-v $(PWD):/src:ro \
		$(DOCKER_IMAGE) bash -c \
		'apt-get update -qq && apt-get install -y -qq $(DOCKER_DEPS) >/dev/null 2>&1 && \
		 cp -r /src /build && cd /build && make clean >/dev/null 2>&1; make && \
		 echo "=== mini-tmux built at /build/mini-tmux ===" && cd /build && exec bash'
