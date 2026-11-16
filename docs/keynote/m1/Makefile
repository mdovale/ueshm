# PDF lands in this directory. Aux files land in build/keynote/m1 (gitignored).

.PHONY: all clean

all:
	latexmk -lualatex -interaction=nonstopmode -halt-on-error \
		-outdir=. -auxdir=../../../build/keynote/m1 ueshm.tex

clean:
	latexmk -lualatex -outdir=. -auxdir=../../../build/keynote/m1 -C ueshm.tex
