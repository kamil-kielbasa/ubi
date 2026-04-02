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
version = '0.21.1'

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

_repodir = _confdir.parent.resolve()
_input_file = _repodir / 'lib' / 'include' / 'ubi.h'
_xml_dir = _buildoc_path / 'xml'

os.makedirs(_buildoc_path, exist_ok=True)

# Write a temporary Doxyfile with absolute paths so the build works
# regardless of the current working directory (fixes GitHub Actions CI).
_doxyfile_src = (_confdir / 'Doxyfile').read_text()
_doxyfile_src += f'\nINPUT          = {_input_file}\n'
_doxyfile_src += f'OUTPUT_DIRECTORY = {_buildoc_path}\n'

_doxyfile_tmp = _buildoc_path / 'Doxyfile.generated'
_doxyfile_tmp.write_text(_doxyfile_src)

print(f'[conf.py] doxygen INPUT        = {_input_file} (exists={_input_file.exists()})')
print(f'[conf.py] doxygen OUTPUT_DIR   = {_buildoc_path}')
print(f'[conf.py] generated Doxyfile   = {_doxyfile_tmp}')

result = subprocess.run(
    ['doxygen', str(_doxyfile_tmp)],
    capture_output=True,
    text=True,
)

print(f'[conf.py] doxygen exit code    = {result.returncode}')
if result.stdout.strip():
    print(f'[conf.py] doxygen stdout:\n{result.stdout}')
if result.stderr.strip():
    print(f'[conf.py] doxygen stderr:\n{result.stderr}')

if result.returncode != 0:
    raise RuntimeError(f'Doxygen failed (exit {result.returncode}):\n{result.stderr}')

_ubi_xml = _xml_dir / 'ubi_8h.xml'
if not _ubi_xml.exists():
    _xml_files = list(_xml_dir.glob('*')) if _xml_dir.exists() else []
    raise RuntimeError(
        f'Doxygen did not produce {_ubi_xml}\n'
        f'  INPUT = {_input_file} (exists: {_input_file.exists()})\n'
        f'  XML dir contents: {[f.name for f in _xml_files]}\n'
        f'  stderr: {result.stderr}'
    )

# Verify the XML actually contains function definitions.
_xml_content = _ubi_xml.read_text()
if 'ubi_device_init' not in _xml_content:
    # Dump first 2000 chars for diagnostics.
    raise RuntimeError(
        f'ubi_8h.xml exists but does not contain ubi_device_init.\n'
        f'  File size: {_ubi_xml.stat().st_size} bytes\n'
        f'  First 2000 chars:\n{_xml_content[:2000]}'
    )

print(f'[conf.py] ubi_8h.xml OK ({_ubi_xml.stat().st_size} bytes, contains ubi_device_init)')

breathe_projects = {'ubi': str(_xml_dir)}
breathe_default_project = 'ubi'
