.PHONY: run

main: src/main.cpp
	g++ -std=c++23 -Wall -Wextra -lxcb -lxcb-ewmh -g $< -o $@

run:
	./main --window --bypass-wm-below
