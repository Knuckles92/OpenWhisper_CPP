"""Curated inputs for the date helpers of ui_qt/dialogs/settings_backup.py (M3.9).

Values are what the Backup page reads from a backup's manifest or the coordinator's snapshot:
ISO timestamps with "Z", an offset or none, empty and unreadable text, and numbers (a stored
value that is not text is shown as str(value)). The aware ones convert with astimezone(), so
they hold the machine's local zone (pins.local_timezone: Pacific, America/Los_Angeles rules,
across both daylight-saving changes of 2026); the replay passes that zone in explicitly.
Moments before 1970 are left out: Windows' localtime refuses them (OSError), which the page
does not reproduce.

    _local_date(value)    the naive isoformat() of the local wall time, or None
    _date_label(value)    the label text
"""

CASES = {
    "_local_date": [
        ["2026-10-04T12:30:00Z"],
        ["2026-10-04T12:30:00+00:00"],
        ["2026-10-04T12:30:00-07:00"],
        ["2026-10-04T12:30:00+05:30"],
        ["2026-10-04T12:30:00"],
        ["2026-10-04T00:00:00"],
        ["2026-10-04"],
        ["2026-10-04 12:30:00Z"],
        ["2026-10-04T12:30:00.123456Z"],
        ["20261004T123000Z"],
        ["2026-01-15T00:05:00Z"],
        ["2026-03-08T09:59:59Z"],
        ["2026-03-08T10:00:00Z"],
        ["2026-03-08T10:30:00Z"],
        ["2026-11-01T08:30:00Z"],
        ["2026-11-01T09:30:00Z"],
        ["2026-10-04T07:00:00Z"],
        ["2026-10-04T19:00:00Z"],
        ["9999-12-31T23:59:59"],
        ["Zulu"],
        ["not a date"],
        ["2026-13-45T00:00:00Z"],
        [" 2026-10-04T12:30:00Z"],
        [""],
        [None],
        [0],
        [12345],
    ],
    "_date_label": [
        ["2026-10-04T12:30:00Z"],
        ["2026-10-04T12:30:00+00:00"],
        ["2026-10-04T12:30:00-07:00"],
        ["2026-10-04T12:30:00+05:30"],
        ["2026-10-04T12:30:00"],
        ["2026-10-04T00:00:00"],
        ["2026-10-04T00:05:00"],
        ["2026-10-04T09:05:00"],
        ["2026-10-04T12:05:00"],
        ["2026-10-04T23:59:00"],
        ["2026-10-04"],
        ["2026-10-04T12:30:00.123456Z"],
        ["2026-01-15T00:05:00Z"],
        ["2026-03-08T09:59:59Z"],
        ["2026-03-08T10:30:00Z"],
        ["2026-11-01T08:30:00Z"],
        ["2026-11-01T09:30:00Z"],
        ["2026-12-31T23:30:00Z"],
        ["2027-01-01T07:59:59Z"],
        ["2027-07-04T03:00:00Z"],
        ["9999-12-31T23:59:59"],
        ["Zulu"],
        ["not a date"],
        ["2026-13-45T00:00:00Z"],
        [""],
        [None],
        [0],
        [12345],
        [False],
    ],
}


def _local_date(module, value):
    local = module._local_date(value)
    return None if local is None else local.replace(tzinfo=None).isoformat()


WRAPPERS = {"_local_date": _local_date}

PINS = {
    "timezone_note": "aware values convert to the machine's zone, Pacific (America/Los_Angeles "
                     "rules); replay them with that zone passed in explicitly",
}
