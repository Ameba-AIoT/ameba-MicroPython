# Standalone Sphinx config for previewing the Chinese translation locally.
# Mirrors ../conf.py -- see that file's comment for why this is independent
# of micropython/docs/conf.py.

project = "Ameba port 文档（中文，本地预览）"
copyright = "MicroPython authors and contributors"

extensions = ["sphinx_rtd_theme"]

master_doc = "index"
source_suffix = ".rst"
exclude_patterns = ["_build"]

language = "zh_CN"
default_role = "any"

html_theme = "sphinx_rtd_theme"
