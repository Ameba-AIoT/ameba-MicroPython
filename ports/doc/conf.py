# Standalone Sphinx config for previewing ports/doc locally.
#
# This is intentionally independent of micropython/docs/conf.py (that tree
# is read-only upstream) -- it only reuses the same theme so the rendered
# output looks close to the real MicroPython docs site. Cross-references to
# labels defined in micropython/docs (e.g. machine.Pin, soft_bricking) will
# show up as "undefined label" warnings since that project isn't built here;
# that's expected for a local preview.

project = "Ameba port docs (local preview)"
copyright = "MicroPython authors and contributors"

extensions = ["sphinx_rtd_theme"]

master_doc = "index"
source_suffix = ".rst"

# zh/ is its own standalone Sphinx project (own conf.py); without this,
# Sphinx would also pick up its .rst files as orphaned documents here.
exclude_patterns = ["_build", "zh"]

default_role = "any"

html_theme = "sphinx_rtd_theme"
