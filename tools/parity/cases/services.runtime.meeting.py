"""Curated inputs for services/runtime/meeting.py (M7.19 meeting-runtime): the pure helpers of
the meeting runtime.

    redact_meeting_url(url)                  the logged origin of a dashboard link
    _with_history_target(url, id, view)      a dashboard link opening one past meeting
    _ended_message(payload, status, finalizing)
    _recovered_follow_up_step(meeting, settings)
        MeetingRuntime._recovered_follow_up_step; `settings` is a settings dict keyed by the
        stored names (meeting_end_redecode, meeting_end_polish, meeting_end_report).
    _require_dashboard_address(result)
        over an engine that records whether it was aborted: {"aborted": bool} when the start
        result is accepted, else the RuntimeError it raises.
"""


class _Engine:
    def __init__(self):
        self.aborted = False

    def _abort_start(self):
        self.aborted = True


def _with_history_target(module, url, meeting_id, view=""):
    return module._with_history_target(url, meeting_id, view=view)


def _ended_message(module, payload, status, finalizing):
    return module._ended_message(payload, status, finalizing=finalizing)


def _recovered_follow_up_step(module, meeting, settings):
    return module.MeetingRuntime._recovered_follow_up_step(meeting, settings)


def _require_dashboard_address(module, result):
    engine = _Engine()
    module._require_dashboard_address(engine, result)
    return {"aborted": engine.aborted}


WRAPPERS = {
    "_with_history_target": _with_history_target,
    "_ended_message": _ended_message,
    "_recovered_follow_up_step": _recovered_follow_up_step,
    "_require_dashboard_address": _require_dashboard_address,
}

_REDECODE = {"meeting_end_redecode": True, "meeting_end_polish": True,
             "meeting_end_report": True}
_NO_REDECODE = {"meeting_end_redecode": False, "meeting_end_polish": True,
                "meeting_end_report": True}
_REPORT_ONLY = {"meeting_end_redecode": False, "meeting_end_polish": False,
                "meeting_end_report": True}
_NOTHING = {"meeting_end_redecode": False, "meeting_end_polish": False,
            "meeting_end_report": False}

CASES = {
    "redact_meeting_url": [
        [None], [""],
        ["http://127.0.0.1:8765/m/secret-host-token"],
        ["http://127.0.0.1:8765/m/secret?history=m_1&view=report"],
        ["HTTPS://Example.COM:9000/m/token#frag"],
        ["http://[::1]:8765/m/token"],
        ["http://[::1:8765/m/token"],
        ["http://[127.0.0.1]:8765/m/token"],
        ["127.0.0.1:8765/m/token"],
        ["/m/token"],
        ["m/token"],
        ["file:///tmp/x"],
        ["  http://192.168.1.20:8765/m/guest-token"],
        ["http://user:pass@host:1/m/t"],
    ],
    "_with_history_target": [
        ["http://127.0.0.1:9000/m/secret-token?source=qt", "m_review/one"],
        ["http://127.0.0.1:9000/m/secret-token?source=qt", "m_review/one", "report"],
        ["http://127.0.0.1:9000/m/host-token", "m_selected"],
        ["http://127.0.0.1:9000/m/host-token", "m_selected", "report"],
        ["http://127.0.0.1:9000/m/host-token?history=old&x=1&history=older", "m_new"],
        ["http://127.0.0.1:9000/m/host-token?view=transcript", "m_1", "report"],
        ["http://127.0.0.1:9000/m/host-token?view=transcript", "m_1"],
        ["http://127.0.0.1:9000/m/host-token?flag&empty=&a+b=c%20d", "m 1"],
        ["http://127.0.0.1:9000/m/host-token?q=caf%C3%A9&bad=%ZZ%e9", "m_ü~._-"],
        ["http://127.0.0.1:9000/m/host-token#fragment", "m_1"],
        ["http://127.0.0.1:9000/m/host-token?&&a=1&", "m_1"],
    ],
    "_ended_message": [
        [{}, "ended", True],
        [{"capture_error": "Device lost"}, "ended", True],
        [{"capture_error": "Device lost"}, "ended", False],
        [{"capture_error": ""}, "needs_recovery", False],
        [{}, "needs_recovery", False],
        [{}, "ended", False],
        [{}, "failed", False],
        [{"capture_error": 0}, "ended", False],
    ],
    "_recovered_follow_up_step": [
        [{"spool_dir": "/s", "cloud_enabled": False}, _REDECODE],
        [{"spool_dir": "", "cloud_enabled": False}, _REDECODE],
        [{"spool_dir": "/s", "cloud_enabled": True}, _NO_REDECODE],
        [{"spool_dir": "/s", "cloud_enabled": True}, _REPORT_ONLY],
        [{"spool_dir": "/s", "cloud_enabled": True}, _NOTHING],
        [{"spool_dir": None, "cloud_enabled": True}, _REDECODE],
        [{"cloud_enabled": 1}, _NO_REDECODE],
        [{}, {}],
        [{"spool_dir": "/s"}, {}],
        [{"spool_dir": "/s", "cloud_enabled": True}, {"meeting_end_redecode": "yes"}],
    ],
    "_require_dashboard_address": [
        [{"host_url": "http://127.0.0.1:8765/m/h"}],
        [{"url": "http://127.0.0.1:8765"}],
        [{"host_url": "", "url": "http://127.0.0.1:8765"}],
        [{"host_url": None, "url": None}],
        [{}],
        [{"guest_url": "http://127.0.0.1:8765/m/g"}],
    ],
}
