"""Curated inputs for ui_qt/utils/font_scale.py scale_qss_fonts (fv_ui_theme scaleQssFonts).

Integers round half to even (10px at 1.15 is 11.5 -> 12, 13px at 1.15 is 14.95 -> 15); decimals
format with :.1f on the binary value and lose trailing zeros and the dot."""

_SIZES = ["font-size: 10px;", "font-size: 11px;", "font-size: 13px;", "font-size: 14px;",
          "font-size: 12.5px;", "font-size: 9pt;", "font-size: 0.5px;", "font-size:0px;"]
_SCALES = [0.9, 1.0, 1.15, 1.3, 1.0000001]

CASES = {
    "scale_qss_fonts": [[size, scale] for size in _SIZES for scale in _SCALES] + [
        ["QLabel { font-size: 14px; min-height: 14px; padding: 8px; }", 1.3],
        ["font-size: 10pt;", 1.5], ["font-size: 10.5px;", 2.0], ["", 1.3],
        ["FONT-SIZE: 12PX; Font-Size:\t7Pt;", 1.15], ["font-size:   20px", 0.9],
        ["font-size: 12em; font-size: 12; font-size: -3px;", 1.3],
        ["font-size: 1px;", 0.1], ["font-size: 1.04px;", 1.0], ["font-size: 1.04px;", 1.3],
        ["font-size: 0.04px;", 1.0000001], ["font-size: 0.05px;", 1.5], ["font-size: 3.25px;", 1.0001],
        ["font-size: 15px;", 1.1], ["font-size: 25px;", 1.1], ["font-size: 5px;", 0.9],
        ["font-size: 1.px;", 1.3], ["font-size: .5px;", 1.3], ["font-size: 007px;", 1.3],
        ["a{font-size:9px}b{font-size:11px}", 1.15], ["font-size: ١٢px;", 1.3],
        ["font-size: 14px;", 0.999999], ["font-size: 14px;", 0.9999989],
        ["font-size: 14px;", 0.0], ["font-size: 14px;", -1.0],
    ],
}
