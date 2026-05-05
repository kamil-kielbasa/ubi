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
version = '0.51.0'

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

_xml_dir = _buildoc_path / 'xml'

os.makedirs(_buildoc_path, exist_ok=True)

# Run Doxygen from the doc/ directory so that the relative paths in Doxyfile
# (INPUT, OUTPUT_DIRECTORY) resolve correctly regardless of the shell cwd.
result = subprocess.run(
    ['doxygen', 'Doxyfile'],
    capture_output=True,
    text=True,
    cwd=str(_confdir),
)

if result.returncode != 0:
    raise RuntimeError(f'Doxygen failed (exit {result.returncode}):\n{result.stderr}')

# Diagnostics: list XML contents so CI failures are easy to debug.
_xml_files = sorted(f.name for f in _xml_dir.iterdir()) if _xml_dir.exists() else []
print(f'[conf.py] doxygen xml files: {_xml_files}')

# Verify the XML contains the expected group compound for Breathe.
_group_xml = _xml_dir / 'group__ubi__device.xml'
if not _group_xml.exists():
    raise RuntimeError(
        f'Doxygen did not produce {_group_xml.name}\\n'
        f'  xml dir contents: {_xml_files}'
    )

breathe_projects = {'ubi': str(_xml_dir)}
breathe_default_project = 'ubi'
