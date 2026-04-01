# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import os
import subprocess
from pathlib import Path

# -- Path setup --------------------------------------------------------------

_confdir = Path(__file__).parent.resolve()
_buildoc_path = (_confdir / '..' / 'build' / 'doc' / 'doxygen').resolve()

# -- Project information -----------------------------------------------------

project = 'UBI on Zephyr'
copyright = '2026, Kamil Kielbasa'
author = 'Kamil Kielbasa'
version = '0.19.0'

# -- General configuration ---------------------------------------------------

extensions = [
    'breathe',
    'myst_parser',
    'sphinx_rtd_theme',
    'sphinxcontrib.mermaid',
]

source_suffix = ['.rst', '.md']
master_doc = 'index'

templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store', 'environment_setup.md']

# -- Options for HTML output -------------------------------------------------

html_theme = 'sphinx_rtd_theme'

# -- MyST parser options -----------------------------------------------------

myst_heading_anchors = 3
myst_fence_as_directive = ["mermaid"]

# -- Doxygen + Breathe -------------------------------------------------------

os.makedirs(_buildoc_path, exist_ok=True)
subprocess.call('doxygen', shell=True, cwd=str(_confdir))

breathe_projects = {'ubi': str(_buildoc_path / 'xml')}
breathe_default_project = 'ubi'
