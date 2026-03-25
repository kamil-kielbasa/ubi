# Makefile for UBI Sphinx documentation
#
# Usage:
#   make html     Build HTML documentation (Doxygen runs automatically via conf.py)
#   make clean    Remove all build artifacts

SPHINXBUILD   = sphinx-build
SOURCEDIR     = .
BUILDDIR      = ../_build
DOXYDIR       = ../build/doc/doxygen

.PHONY: html clean

html:
	$(SPHINXBUILD) $(SOURCEDIR) $(BUILDDIR) -W

clean:
	rm -rf $(BUILDDIR) $(DOXYDIR)
