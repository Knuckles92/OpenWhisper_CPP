"""Curated inputs for ui_qt/utils/list_reconcile.py reconcile_cards (fv_uimath reconcile_plan).

reconcile_cards works on a QLayout and card widgets; the wrapper runs it on a fake layout that
records what it does and fake cards that remember their id, so the result is the plan the C++
side computes without widgets:

    args:   [old_layout, previous, records]
            old_layout  ids of the cards in the layout, in order (null: an item that is no card)
            previous    [[id, fingerprint], ...]: what each shown card was drawn from
            records     [[id, fingerprint], ...]: the new list
    result: {"entries": [[id, reused], ...], "removed": [layout positions taken out],
             "inserted": [positions passed to insertWidget], "fingerprints": [[id, fp], ...]}

The fake layout's insertWidget first takes the card out of its old position, as
QLayout::addChildWidget does for a widget already in the layout.
"""


class _Card:
    def __init__(self, record_id, created):
        self.record_id = record_id
        self.created = created
        self.entry = None
        self.meeting = None

    def hide(self):
        pass

    def setParent(self, parent):
        pass

    def deleteLater(self):
        pass


class _Item:
    def __init__(self, widget):
        self._widget = widget

    def widget(self):
        return self._widget


class _Layout:
    def __init__(self, widgets):
        self.widgets = list(widgets)
        self.removed = []
        self.inserted = []

    def count(self):
        return len(self.widgets)

    def itemAt(self, index):
        if 0 <= index < len(self.widgets):
            return _Item(self.widgets[index])
        return None

    def takeAt(self, index):
        self.removed.append(index)
        return _Item(self.widgets.pop(index))

    def insertWidget(self, index, widget):
        self.inserted.append(index)
        if widget in self.widgets:
            self.widgets.remove(widget)
        self.widgets.insert(min(index, len(self.widgets)), widget)


def _reconcile(module, old_layout, previous, records):
    cards = {}
    widgets = []
    for record_id in old_layout:
        if record_id is None:
            widgets.append(None)
            continue
        card = cards.setdefault(record_id, _Card(record_id, created=False))
        widgets.append(card)
    previous_map = {}
    for record_id, fingerprint in previous:
        card = cards.setdefault(record_id, _Card(record_id, created=False))
        previous_map[record_id] = (fingerprint, card)
    layout = _Layout(widgets)
    new_records = [{"id": record_id, "fp": fingerprint} for record_id, fingerprint in records]
    current = module.reconcile_cards(
        layout, new_records, previous_map,
        create=lambda record: _Card(str(record["id"]), created=True),
        fingerprint=lambda record: record["fp"],
    )
    entries = []
    for record in new_records:
        record_id = str(record["id"])
        shown = current[record_id][1]
        entries.append([record_id, not shown.created])
    return {
        "entries": entries,
        "removed": layout.removed,
        "inserted": layout.inserted,
        "fingerprints": [[record_id, value[0]] for record_id, value in sorted(current.items())],
    }


WRAPPERS = {"reconcile_cards": _reconcile}

CASES = {
    "reconcile_cards": [
        [[], [], []],
        [[], [], [["a", 1], ["b", 2]]],
        [["a", "b"], [["a", 1], ["b", 2]], [["a", 1], ["b", 2]]],
        [["a", "b"], [["a", 1], ["b", 2]], [["b", 2], ["a", 1]]],
        [["a", "b"], [["a", 1], ["b", 2]], [["a", 1], ["b", 3]]],
        [["a", "b", "c"], [["a", 1], ["b", 2], ["c", 3]], [["c", 3], ["a", 1]]],
        [["a", "b", "c"], [["a", 1], ["b", 2], ["c", 3]], [["d", 4], ["b", 2], ["e", 5]]],
        [["a", None, "b"], [["a", 1], ["b", 2]], [["a", 1], ["b", 2]]],
        [[None], [], [["a", 1]]],
        [["a", "b"], [["a", {"t": "x", "n": 1}], ["b", [1, 2]]],
         [["a", {"n": 1, "t": "x"}], ["b", [2, 1]]]],
        [["a"], [["a", 1]], [["a", 1.0]]],
        [["a"], [["a", 1]], [["a", True]]],
        [["a"], [["a", "1"]], [["a", 1]]],
        [["x", "y", "z"], [["x", 0], ["y", 0], ["z", 0]], [["z", 0], ["y", 0], ["x", 0]]],
        [["a", "b"], [["a", 1], ["b", 2]], []],
        [["a"], [["a", None]], [["a", None], ["b", None]]],
    ],
}
