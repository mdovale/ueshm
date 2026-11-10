# PDF lands in this directory. Aux files land in build/keynote (gitignored).

.PHONY: all clean

all:
	latexmk -lualatex -interaction=nonstopmode -halt-on-error \
		-outdir=. -auxdir=../../build/keynote ueshm.tex

clean:
	latexmk -lualatex -outdir=. -auxdir=../../build/keynote -C ueshm.tex
