CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic

.PHONY: all clean

all: mini-tmux

mini-tmux: mini_tmux.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f mini-tmux
