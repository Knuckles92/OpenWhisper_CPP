"""Curated inputs for ui_qt/widgets/walkthrough_copy_tips.py (fv_ui_walkthrough's setCopyTip).

    set_copy_tip(copy_format, on_finish)

calls set_copy_tip on a stand-in action that records its tooltip; the result is {"tooltip": the
rich text set, "paragraphs": the plain paragraphs it was built from}. A widget's accessible
description is the paragraphs joined by a blank line. An unknown format raises KeyError.
"""


class _Target:
    def __init__(self):
        self.tooltip = None

    def setToolTip(self, text):
        self.tooltip = text


def _set_copy_tip(module, copy_format, on_finish):
    target = _Target()
    module.set_copy_tip(target, copy_format, on_finish=on_finish)
    if on_finish and copy_format == module.WalkthroughCopyFormat.AGENT_TASK:
        paragraphs = list(module._AGENT_ON_FINISH)
    else:
        paragraphs = [module._COPY_TIPS[copy_format]]
    return {"tooltip": target.tooltip, "paragraphs": paragraphs}


WRAPPERS = {"set_copy_tip": _set_copy_tip}

CASES = {
    "set_copy_tip": [[fmt, on_finish]
                     for fmt in ("pdf", "agent_task", "both", "agent", "word")
                     for on_finish in (False, True)]
                    + [["markdown", False]],
}
