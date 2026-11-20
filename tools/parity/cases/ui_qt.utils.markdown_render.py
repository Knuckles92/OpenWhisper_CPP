"""Curated inputs for ui_qt/utils/markdown_render.py MarkdownStyle (fv_ui_theme MarkdownStyle).

The style argument is "preview", "reader", or [body_pt, paragraph_gap, indent_px,
heading_scale] for a MarkdownStyle of its own. scaled() results are [body_pt, line_height,
paragraph_gap, indent_px, code_background, quote_background, heading_scale]."""


def _style(module, which):
    if which == "preview":
        return module.PREVIEW_STYLE
    if which == "reader":
        return module.READER_STYLE
    body_pt, gap, indent, scale = which
    return module.MarkdownStyle(
        body_pt=body_pt, line_height=100, paragraph_gap=gap, indent_px=indent,
        code_background="#000000", quote_background="#000000", heading_scale=tuple(scale),
    )


def _heading_pt(module, which, level):
    return _style(module, which).heading_pt(level)


def _code_pt(module, which):
    return _style(module, which).code_pt


def _scaled(module, which, factor):
    style = _style(module, which).scaled(factor)
    return [style.body_pt, style.line_height, style.paragraph_gap, style.indent_px,
            style.code_background, style.quote_background, list(style.heading_scale)]


WRAPPERS = {"heading_pt": _heading_pt, "code_pt": _code_pt, "scaled": _scaled}

_CUSTOM = [10, 4, 10, [2.0, 1.5]]
_ODD = [12.25, 3, 11, [1.55, 1.3, 1.15, 1.05, 1.0, 1.0]]

CASES = {
    "heading_pt": [[style, level] for style in ("preview", "reader")
                   for level in (-1, 0, 1, 2, 3, 4, 5, 6, 7)]
    + [[_CUSTOM, 1], [_CUSTOM, 2], [_CUSTOM, 6], [_ODD, 1], [_ODD, 3], [_ODD, 4]],
    "code_pt": [["preview"], ["reader"], [_CUSTOM], [_ODD]],
    "scaled": [[style, factor] for style in ("preview", "reader", _CUSTOM, _ODD)
               for factor in (0.9, 1.0, 1.15, 1.3, 1.5, 2.0, 0.25, 0.1)],
}
