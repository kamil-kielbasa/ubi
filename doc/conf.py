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
version = '0.102.0'

# -- General configuration ---------------------------------------------------

extensions = [
    'breathe',
    'myst_parser',
    'sphinxcontrib.mermaid',
    'sphinx_reredirects',
]

# -- Redirects ---------------------------------------------------------------
# Old URLs from the v1.0.0 documentation reorganization. Each entry generates
# a small HTML page with a meta-refresh redirect so external bookmarks and
# search-engine results survive the reshuffle.
redirects = {
    # Positioning piece kept out of the published sidebar.
    'why_ubi_for_zephyr': 'https://github.com/kamil-kielbasa/ubi/blob/main/doc/positioning/why_ubi_for_zephyr.md',
    # Pages merged into "What is UBI?".
    'overview': 'getting_started/what_is_ubi.html',
    'introduction': 'getting_started/what_is_ubi.html',
    # Pages split into Quick Start + Test Strategy.
    'getting_started': 'getting_started/quick_start.html',
    # Secure Architecture split into developer Overview + normative spec; the
    # three small secure satellites folded into the spec.
    'secure_architecture': 'reference/onflash_format_spec.html',
    'secure_volume_lifecycle': 'reference/onflash_format_spec.html',
    'secure_recovery_notes': 'reference/onflash_format_spec.html',
    'secure_runtime_policy': 'reference/onflash_format_spec.html',
    # Pages relocated into topical subfolders (flat -> sectioned layout).
    'what_is_ubi': 'getting_started/what_is_ubi.html',
    'quick_start': 'getting_started/quick_start.html',
    'concepts': 'getting_started/concepts.html',
    'plain_workflow': 'guide/plain_workflow.html',
    'secure_workflow': 'guide/secure_workflow.html',
    'configuration': 'guide/configuration.html',
    'cookbook': 'guide/cookbook.html',
    'plain_architecture': 'architecture/plain_architecture.html',
    'secure_overview': 'architecture/secure_overview.html',
    'api': 'reference/api.html',
    'kconfig_reference': 'reference/kconfig_reference.html',
    'error_codes': 'reference/error_codes.html',
    'onflash_format_spec': 'reference/onflash_format_spec.html',
    'roadmap': 'project/roadmap.html',
    'contributing': 'project/contributing.html',
    'test_strategy': 'project/test_strategy.html',
    'changelog': 'project/changelog.html',
}

source_suffix = ['.rst', '.md']
master_doc = 'index'

templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store', 'positioning']

# -- Options for HTML output -------------------------------------------------

html_theme = 'furo'
html_static_path = ['_static']
html_css_files = ['custom.css']
html_title = 'UBI on Zephyr'
html_theme_options = {
    'source_repository': 'https://github.com/kamil-kielbasa/ubi/',
    'source_branch': 'main',
    'source_directory': 'doc/',
    'sidebar_hide_name': False,
    'footer_icons': [
        {
            'name': 'GitHub',
            'url': 'https://github.com/kamil-kielbasa/ubi',
            'class': '',
            # Inline SVG so the icon renders at any URL depth (an `<img
            # src="_static/...">` would be resolved relative to the
            # current page and 404 on nested routes). `currentColor`
            # makes it follow Furo's footer text colour in both
            # light and dark mode.
            'html': (
                '<svg xmlns="http://www.w3.org/2000/svg" '
                'viewBox="0 0 16 16" width="16" height="16" '
                'fill="currentColor" aria-hidden="true">'
                '<path fill-rule="evenodd" d="M8 0C3.58 0 0 3.58 0 8c0 '
                '3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01'
                '-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-'
                '.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 '
                '1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07'
                '-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08'
                '-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-'
                '.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-'
                '.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 '
                '3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-'
                '.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0 0 '
                '16 8c0-4.42-3.58-8-8-8z"/>'
                '</svg>'
            ),
        },
    ],
}

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
