CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic

.PHONY: all clean grade

all: mini-tmux

mini-tmux: mini_tmux.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f mini-tmux

# Black-box grading via Docker
grade: mini-tmux
	docker run --rm \
		-v $(PWD)/mini-tmux:/submission/mini-tmux:ro \
		mini-tmux-harness grade
