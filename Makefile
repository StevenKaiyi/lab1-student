CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic

.PHONY: all clean grade

all: mini-tmux

mini-tmux: mini_tmux.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f mini-tmux

# Black-box grading via Docker (compiles inside container for Linux compatibility)
grade:
	docker run --rm \
		-v $(PWD):/src:ro \
		mini-tmux-harness grade-src
