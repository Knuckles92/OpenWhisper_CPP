"""The harness's own reference: the a33 trial's EmptyState, used only by fv_support_tests.

M0.10 owns the real empty_state reference (builders/a02.py)."""


def empty_state_harness(variant):
    from PyQt6.QtWidgets import QPushButton

    from ui_qt.widgets.empty_state import EmptyState

    empty = EmptyState("No snippets yet", "Type a trigger phrase and the text it expands to.")
    button = QPushButton("Add snippet")
    empty.add_action(button)
    # The button is the first focusable widget, so it takes focus on show; pinned explicitly.
    return empty, (420, 150), button


BUILDERS = {"empty_state_harness": empty_state_harness}
