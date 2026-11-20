"""Curated inputs for services/app_update.py (M3.2: update/check.h, schedule.h, release_feed.h,
trust.h).

Only functions that do no network I/O are run. Those that read OPENWHISPER_UPDATE_FEED_URL take
the feed as their last argument (None: unset) and set it for the call. Release payloads carry
OpenWhisper's repository and asset names: the C++ replay rebrands inputs and results together
(so the repository becomes "Knuckles92/OpenWhisper_CPP"), maps the native archive's "-win64.tar.xz" to
OpenWhisper_CPP's "-win64.zip", and takes the repository from pins.github_repo. Release results are
dicts of the ReleaseInfo fields the port keeps (the macOS disk image is other-platform).
"""

import email.message
import io
import os
import shutil
import ssl
import urllib.error
import urllib.parse
from datetime import datetime, timezone

import ow_isolation

REPO = "Knuckles92/OpenWhisper"
DOWNLOAD = f"https://github.com/{REPO}/releases/download"
FEED = "http://127.0.0.1:8765/releases/latest"
DIGEST = "ab" * 32


class _Feed:
    """Sets the feed variable for one call, as monkeypatch.setenv does."""

    def __init__(self, module, feed):
        self._name = module.UPDATE_FEED_ENV
        self._feed = feed

    def __enter__(self):
        self._saved = os.environ.pop(self._name, None)
        if self._feed is not None:
            os.environ[self._name] = self._feed

    def __exit__(self, *exc):
        os.environ.pop(self._name, None)
        if self._saved is not None:
            os.environ[self._name] = self._saved
        return False


def _asset_dict(asset):
    if asset is None:
        return None
    return {"url": asset.url, "name": asset.name, "size_bytes": asset.size_bytes,
            "sha256": asset.sha256, "github_asset_id": asset.github_asset_id}


def _release_dict(release):
    return {"version": release.version, "tag_name": release.tag_name,
            "html_url": release.html_url, "notes": release.notes,
            "prerelease": release.prerelease,
            "setup_asset": _asset_dict(release.setup_asset),
            "native_asset": _asset_dict(release.native_asset)}


def _seconds(module, settings, now, interval, channel):
    return module.seconds_until_auto_check(
        settings, now=datetime.fromisoformat(now), interval_s=interval, channel=channel)


def _parse_last_check_at(module, raw):
    parsed = module.parse_last_check_at(raw)
    return None if parsed is None else parsed.isoformat()


def _payload(module, payload, allow_prerelease, feed):
    with _Feed(module, feed):
        return _release_dict(module.parse_release_payload(payload,
                                                          allow_prerelease=allow_prerelease))


def _select(module, payload, feed):
    with _Feed(module, feed):
        return _release_dict(module.select_newest_release(payload))


def _named(module, assets, expected_name, tag_name, feed):
    with _Feed(module, feed):
        return _asset_dict(module._parse_named_asset(assets, expected_name=expected_name,
                                                     tag_name=tag_name))


def _allowed(module, url, tag_name, name, feed):
    with _Feed(module, feed):
        return module._asset_url_allowed(url, module._expected_asset_url(tag_name, name))


def _well_formed(module, url):
    return module._parsed_url_is_well_formed(urllib.parse.urlsplit(url))


class _Body(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def _bytes(spec):
    """A str is its latin-1 bytes; ["repeat", text, n] is text * n in latin-1."""
    if isinstance(spec, list):
        return (spec[1] * spec[2]).encode("latin-1")
    return spec.encode("latin-1")


def _checksums(module, raw):
    saved = module._open
    module._open = lambda url, extra_headers=None, **kwargs: _Body(_bytes(raw))
    try:
        return module._fetch_release_checksums("v2.7.0")
    finally:
        module._open = saved


def _headers(pairs):
    message = email.message.Message()
    for name, value in pairs:
        message[name] = value
    return message


def _rate_limit(module, code, headers, now):
    """[_is_rate_limited(error), what _remember_api_rate_limit stores] at a fixed now."""
    from services.settings import SettingsManager

    fixed = datetime.fromisoformat(now)

    class FixedDatetime(datetime):
        @classmethod
        def now(cls, tz=None):
            return fixed

    folder = ow_isolation.SCRATCH / "app-update-cases"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    manager = SettingsManager(str(folder / "settings.json"))
    error = urllib.error.HTTPError("https://api.github.com/x", code, "Forbidden",
                                   _headers(headers), None)
    saved = (module.datetime, module.settings_manager)
    module.datetime, module.settings_manager = FixedDatetime, manager
    try:
        limited = module._is_rate_limited(error)
        module._remember_api_rate_limit(error)
        stored = manager.load_all_settings().get("update_api_retry_at")
    finally:
        module.datetime, module.settings_manager = saved
        shutil.rmtree(folder, ignore_errors=True)
    return [limited, stored]


def _describe(module, kind, detail, headers):
    if kind == "certificate":
        exc = urllib.error.URLError(ssl.SSLCertVerificationError(1, detail))
    elif kind == "http":
        exc = urllib.error.HTTPError("https://api.github.com/x", detail[0], detail[1],
                                     _headers(headers), None)
    elif kind == "url":
        exc = urllib.error.URLError(detail)
    else:
        exc = OSError(detail)
    return module._describe_update_error(exc)


def _github_asset(name, tag="v2.1.1", **extra):
    item = {"name": name, "size": 100, "state": "uploaded", "digest": "sha256:" + DIGEST,
            "browser_download_url": f"{DOWNLOAD}/{tag}/{name}"}
    item.update(extra)
    return item


LATEST = {
    "tag_name": "v2.1.1",
    "html_url": f"https://github.com/{REPO}/releases/tag/v2.1.1",
    "body": "### Fixed\n\n- GPU Acceleration component now activates immediately\n",
    "prerelease": False,
    "draft": False,
    "assets": [{
        "name": "OpenWhisper-Setup-2.1.1.exe",
        "size": 91717696,
        "digest": "sha256:43d623f9f0a35d3ab7c8366d30bd721fe20c51bd94f3bc37b453c63fd5f8bd13",
        "browser_download_url": f"{DOWNLOAD}/v2.1.1/OpenWhisper-Setup-2.1.1.exe",
    }],
}


def _latest(**overrides):
    payload = dict(LATEST)
    payload.update(overrides)
    return payload


def _release(version, prerelease=False, draft=False, digest="cd" * 32, url=None, asset_id=1000):
    tag = f"v{version}"
    name = f"OpenWhisper-Setup-{version}.exe"
    return {"tag_name": tag, "html_url": f"https://github.com/{REPO}/releases/tag/{tag}",
            "body": f"Notes for {version}", "draft": draft, "prerelease": prerelease,
            "assets": [{"id": asset_id, "name": name, "size": 4096, "state": "uploaded",
                        "digest": f"sha256:{digest}" if digest else None,
                        "browser_download_url": url or f"{DOWNLOAD}/{tag}/{name}"}]}


NOW = "2026-08-21T12:00:00+00:00"
NOW_MICROS = "2026-08-21T12:00:00.250000+00:00"
SETUP = "OpenWhisper-Setup-2.1.1.exe"
ARCHIVE = "OpenWhisper-2.1.1-win64.tar.xz"
API = "https://api.github.com"
ASSET_API = f"{API}/repos/{REPO}/releases/assets/123456"
CDN = "https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sig=t"
PAGE = f"https://github.com/{REPO}/releases/tag/v2.7.0"

URLS = [
    FEED, "https://127.0.0.1:8765/releases/latest", "http://127.0.0.1/releases/latest",
    "http://127.0.0.1:0/releases/latest", "http://127.0.0.1:80/releases/latest",
    "http://127.9.8.7:8765/releases/latest", "http://[::1]:8765/releases/latest",
    "http://[::ffff:127.0.0.1]:8765/releases/latest", "http://[::1%25eth0]:8765/releases/latest",
    "http://[::2]:8765/releases/latest", "http://localhost:8765/releases/latest",
    "http://10.0.0.1:8765/releases/latest", "http://0127.0.0.1:8765/releases/latest",
    "http://127.0.0.1:8765/releases/latest?", "http://127.0.0.1:8765/releases/latest?x=1",
    "http://127.0.0.1:8765/releases/latest#top", "http://user@127.0.0.1:8765/releases/latest",
    "http://u:p@127.0.0.1:8765/releases/latest", "http://127.0.0.1:8765/releases/latest/",
    "http://127.0.0.1:8765/Releases/latest", "ftp://127.0.0.1:8765/releases/latest",
    "HTTP://127.0.0.1:8765/releases/latest", "http://127.0.0.1:99999/releases/latest",
    "http://127.0.0.1:87a5/releases/latest", "http://127.0.0.1:/releases/latest",
    "http://[::1:8765/releases/latest", "http://[127.0.0.1]:8765/releases/latest",
    " http://127.0.0.1:8765/releases/latest", "http://127.0.0.1:8765/rel\teases/latest",
    "http://127.0.0.1:08765/releases/latest", "", "not a url",
    f"{API}/repos/{REPO}/releases/latest", "https://github.com:443/x", "https://github.com:444/x",
    "https://github.com/x#frag", "https://user@github.com/x", "https://github.com:abc/x",
    "//github.com/x", "https:///x", "https://[v1.x]/x", "https://[V1.x]/x",
]

CASES = {
    "parse_version": [[v] for v in (
        "2.1.1", "v2.1.1", "2.2.0-rc.1", "2.1.1+build.5", "2.1.1 beta", "2.6.01", "2.5",
        "2", "", "abc", "2.x.1", "2..1", "2.1\t.0", "1.2.3.4", " V2.1.1 ", "1_0.2.3",
        "+1.2.3", "٢.٩.٠", "2.1.1-rc.1+b", "v-1", "-", "2.1.1.",
    )],
    "compare_versions": [
        ["2.1.1", "v2.1.1"], ["2.1.1", "v2.2.0"], ["2.2.0", "v2.1.1"],
        ["2.2.0-rc.1", "v2.2.0"], ["2.6.0", "v2.6.01"], ["2.6.01", "v2.6.1"],
        ["0.1.0", "0.1.1"], ["0.1.0", ""], ["", "0.0.0"], ["2.9.0", "v2.8.9"],
    ],
    "seconds_until_auto_check": [
        [settings, now, interval, channel]
        for settings, now, interval, channel in (
            [{"update_check_enabled": False}, NOW, 14400, "installer"],
            [{}, NOW, 14400, "git"],
            [{}, NOW, 14400, "source"],
            [{"update_check_enabled": True}, NOW, 14400, "installer"],
            [{"update_check_enabled": "yes"}, NOW, 14400, "installer"],
            [{"update_check_enabled": 0}, NOW, 14400, "installer"],
            [{"update_last_check_at": "2026-08-21T11:00:00+00:00"}, NOW, 14400, "installer"],
            [{"update_last_check_at": "2026-08-21T07:00:00+00:00"}, NOW, 14400, "installer"],
            [{"update_last_check_at": "2026-08-21T07:00:00+00:00",
              "update_last_failed_check_at": "2026-08-21T11:40:00+00:00"}, NOW, 14400,
             "installer"],
            [{"update_last_check_at": "2026-08-21T11:55:00+00:00"}, NOW, 0, "installer"],
            [{"update_last_failed_check_at": "2026-08-21T11:40:00+00:00"}, NOW, 0, "installer"],
            [{"update_last_check_at": "2026-08-23T12:00:00+00:00"}, NOW, 14400, "installer"],
            [{"update_last_check_at": "not-a-date"}, NOW, 14400, "installer"],
            [{"update_last_check_at": "2026-08-21T11:00:00"}, NOW, 14400, "installer"],
            [{"update_last_check_at": "2026-08-21T13:30:00+02:00"}, NOW, 14400, "installer"],
            [{"update_last_check_at": " 2026-08-21T11:00:00.123456+00:00 "}, NOW_MICROS, 14400,
             "installer"],
            [{"update_last_check_at": "2026-08-21T12:00:00+00:00"}, NOW, 14400, "installer"],
            [{"update_last_check_at": 1}, NOW, 14400, "installer"],
            [{"update_last_failed_check_at": "2026-08-21T11:00:00Z"}, NOW, 14400, "source"],
            [{"update_last_failed_check_at": "2026-08-21T11:59:59.999999+00:00"}, NOW_MICROS,
             14400, "installer"],
        )
    ],
    "parse_last_check_at": [[v] for v in (
        "2026-08-21T12:00:00+00:00", "2026-08-21T12:00:00", " 2026-08-21T12:00:00Z ",
        "2026-08-21T12:00:00.5+05:30", "2026-08-21", "20260821T120000", "not-a-date", "",
        "   ", None, 5, ["2026-08-21"], "2026-13-01T00:00:00",
    )],
    "shows_update_notice": [
        ["update_available", {}], ["up_to_date", {}], ["development", {}],
        ["update_available", {"update_notify_enabled": False}],
        ["update_available", {"update_check_enabled": False}],
        ["update_available", {"update_notify_enabled": "no"}],
        ["update_available", {"update_check_enabled": True, "update_notify_enabled": True}],
    ],
    "parse_release_payload": [
        [LATEST, False, None],
        [_latest(assets=[_github_asset("meeting-agent-win_amd64-node22-pi1.zip"),
                         *LATEST["assets"]]), False, None],
        [_latest(assets=[{k: v for k, v in LATEST["assets"][0].items() if k != "digest"}]),
         False, None],
        [{"assets": []}, False, None],
        [{"tag_name": "  "}, False, None],
        [{"tag_name": 5}, False, None],
        [{"tag_name": "v2.1.1"}, False, None],
        [_latest(assets=[dict(LATEST["assets"][0], browser_download_url=(
            "http://127.0.0.1:8765/download/OpenWhisper-Setup-2.1.1.exe"))]), False, None],
        [_latest(assets=[dict(LATEST["assets"][0], browser_download_url=(
            "http://127.0.0.1:8765/download/OpenWhisper-Setup-2.1.1.exe"))]), False, FEED],
        [_latest(assets=[dict(LATEST["assets"][0], browser_download_url=(
            "http://127.0.0.1:9999/download/OpenWhisper-Setup-2.1.1.exe"))]), False, FEED],
        [_latest(assets=[_github_asset(SETUP), _github_asset(ARCHIVE),
                         _github_asset("meeting-agent.zip")]), False, None],
        [_latest(assets=[_github_asset(ARCHIVE), _github_asset(ARCHIVE)]), False, None],
        [_latest(assets=[_github_asset(SETUP, browser_download_url=(
            "https://evil.example/OpenWhisper-Setup-2.1.1.exe"))]), False, None],
        [_latest(assets=[_github_asset(SETUP)], draft=True), False, None],
        [_latest(assets=[_github_asset(SETUP)], draft=True), True, None],
        [_latest(assets=[_github_asset(SETUP)], prerelease=True), False, None],
        [_latest(assets=[_github_asset(SETUP)], prerelease=True), True, None],
        [_latest(assets=[_github_asset(SETUP)], prerelease=1, draft=0), True, None],
        [_latest(assets=[_github_asset(SETUP)], prerelease="", draft=[]), False, None],
        [_latest(html_url="", body=None), False, None],
        [_latest(html_url=7, body="  notes  \n"), False, None],
        [_latest(assets={"name": SETUP}), False, None],
        [_latest(assets=None), False, None],
        [_latest(assets=[_github_asset(SETUP, state="starter")]), False, None],
        [_latest(assets=[_github_asset(SETUP, state=None)]), False, None],
        [_latest(assets=[_github_asset(SETUP, size="12")]), False, None],
        [_latest(assets=[_github_asset(SETUP, size="1e3")]), False, None],
        [_latest(assets=[_github_asset(SETUP, size=True)]), False, None],
        [_latest(assets=[_github_asset(SETUP, size=12.9)]), False, None],
        [_latest(assets=[_github_asset(SETUP, size=0)]), False, None],
        [_latest(assets=[_github_asset(SETUP, size=-5)]), False, None],
        [_latest(assets=[_github_asset(SETUP, size=2147483648)]), False, None],
        [_latest(assets=[_github_asset(SETUP, size=2147483649)]), False, None],
        [_latest(assets=[_github_asset(SETUP, size=[1])]), False, None],
        [_latest(assets=[dict(_github_asset(SETUP), name=None)]), False, None],
        [_latest(assets=[_github_asset(SETUP, id=7)]), False, None],
        [_latest(assets=[_github_asset(SETUP, id=7, browser_download_url=(
            "http://127.0.0.1:8765/OpenWhisper-Setup-2.1.1.exe"))]), False, FEED],
    ] + [
        [{"tag_name": "v2.8.6", "assets": [{
            "id": 123456, "url": "https://evil.example/download", "name": n, "size": 26,
            "digest": "sha256:" + DIGEST,
            "browser_download_url": f"{DOWNLOAD}/v2.8.6/{n}"}]}, False, None]
        for n in ("OpenWhisper-Setup-2.8.6.exe", "OpenWhisper-2.8.6-win64.tar.xz")
    ] + [
        [{"tag_name": "v2.6.0", "assets": [_github_asset("OpenWhisper-2.6.0-macos-arm64.dmg",
                                                          tag="v2.6.0")]}, False, None],
        [_latest(tag_name="v2.6.01", assets=[_github_asset("OpenWhisper-Setup-2.6.01.exe",
                                                            tag="v2.6.01")]), False, None],
    ],
    "select_newest_release": [
        [[_release("2.8.9"), _release("2.9.0", prerelease=True)], None],
        [[_release("2.9.0", prerelease=True), _release("2.9.1")], None],
        [[_release("2.9.1"), _release("2.9.0", prerelease=True)], None],
        [[_release("3.0.0", draft=True), dict(_release("3.0.0", prerelease=True),
                                              tag_name="v3.0.0-rc.1"),
          _release("2.9.0", prerelease=True)], None],
        [[_release("2.6.01", prerelease=True), _release("2.6.1")], None],
        [[_release("2.6.1"), _release("2.6.01", prerelease=True)], None],
        [[_release("2.6.1", prerelease=True), _release("2.6.01", prerelease=True)], None],
        [[], None],
        [{"message": "x"}, None],
        [[{"draft": True}], None],
        [[5, "x", None, {"tag_name": 7}], None],
        [[_release("2.9.0", prerelease=True,
                   url="https://example.com/OpenWhisper-Setup-2.9.0.exe")], None],
        [[_release("2.9.0", prerelease=True, digest=None)], None],
        [[dict(_release("2.9.0"), tag_name=" v2.9.0 ")], None],
        [[dict(_release("2.9.0"), tag_name="V2.9.0")], None],
        [[dict(_release("2.9.0"), tag_name="2.9.0")], None],
        [[dict(_release("2.9.0"), tag_name="v2.9")], None],
        [[dict(_release("2.9.0"), tag_name="٢.٩.٠")], None],
        [[_release("2.9.0", prerelease=True, url="http://127.0.0.1:8765/download/"
                   "OpenWhisper-Setup-2.9.0.exe")], FEED],
    ],
    "_parse_named_asset": [
        [[{"id": asset_id, "name": "OpenWhisper-2.8.6-macos-arm64.dmg", "size": 26,
           "browser_download_url": f"{DOWNLOAD}/v2.8.6/OpenWhisper-2.8.6-macos-arm64.dmg"}],
         "OpenWhisper-2.8.6-macos-arm64.dmg", "v2.8.6", None]
        for asset_id in (None, True, False, 0, -1, "123", "../latest", 1.5, 123456)
    ] + [
        [[{"id": 123456, "name": "OpenWhisper-Setup-2.8.6.exe", "size": 26,
           "browser_download_url": "http://127.0.0.1:8765/OpenWhisper-Setup-2.8.6.exe"}],
         "OpenWhisper-Setup-2.8.6.exe", "v2.8.6", FEED],
        [[{"name": "OpenWhisper-Setup-2.8.6.exe", "size": 26,
           "browser_download_url": f"{DOWNLOAD}/v2.8.6/OpenWhisper-Setup-2.8.6.exe"},
          "junk", None, 3], "OpenWhisper-Setup-2.8.6.exe", "v2.8.6", None],
        [{"not": "a list"}, "OpenWhisper-Setup-2.8.6.exe", "v2.8.6", None],
        [[{"name": "OpenWhisper-Setup-2.8.6.exe", "size": 26, "browser_download_url": 5}],
         "OpenWhisper-Setup-2.8.6.exe", "v2.8.6", None],
        [[{"name": "OpenWhisper-Setup-2.8.6.exe", "size": 26,
           "browser_download_url": f"{DOWNLOAD}/v2.8.6/OpenWhisper-Setup-2.8.6.exe",
           "digest": "SHA256:" + DIGEST.upper()}], "OpenWhisper-Setup-2.8.6.exe", "v2.8.6",
         None],
    ],
    "_digest_to_sha256": [[v] for v in (
        "sha256:" + DIGEST, "SHA256:" + DIGEST.upper(), "  sha256:  " + DIGEST + "  ",
        "sha256:" + DIGEST[:-1], "sha256:" + DIGEST + "0", "sha512:" + DIGEST, DIGEST,
        "sha256:" + "g" * 64, "sha256:0x" + "a" * 62, "sha256:0x_" + "a" * 61,
        "sha256:-" + "a" * 63, "sha256:+" + "a" * 63, "sha256:" + "a_" * 32,
        "sha256:" + "a" * 31 + "__" + "a" * 31, "sha256:_" + "a" * 63, "sha256:" + "a" * 63 + "_",
        "sha256:" + "٣" * 64, "sha256:" + "a" * 32 + " " + "a" * 31, None, 5, "",
        "sha256:", "sha256:0x" + "_" + "_" + "a" * 60,
    )],
    "_redirect_url_allowed": [
        [f"{API}/repos/{REPO}/releases/latest", f"{API}/repos/{REPO}/releases/latest"],
        [f"{API}/repos/{REPO}/releases/latest", CDN],
        [ASSET_API, CDN],
        [ASSET_API.replace(REPO, "other/repo"), CDN],
        [ASSET_API + "?redirect=1", CDN],
        [ASSET_API, "https://evil.example/file"],
        [ASSET_API, CDN.replace("https:", "http:")],
        [ASSET_API, "https://release-assets.githubusercontent.com/other/file"],
        [ASSET_API, CDN.replace(".com/", ".com:444/")],
        [ASSET_API, CDN.replace(".com/", ".com:443/")],
        [ASSET_API, CDN.replace("https://", "https://user@")],
        [f"https://github.com/{REPO}/releases/latest", PAGE],
        [f"https://github.com/{REPO}/releases/latest", "https://example.com/download"],
        [f"https://github.com/{REPO}/releases/latest", CDN],
        [f"https://github.com/{REPO}/releases/latest", "https://GitHub.com/x"],
        [f"https://github.com/{REPO}/releases/latest", "https://github.com:80/x"],
        [f"https://github.com/{REPO}/releases/latest", "https://github.com/x#f"],
        [f"http://github.com/{REPO}/releases/latest", PAGE],
        ["https://example.com/x", "https://example.com/x"],
        [FEED, FEED],
        [FEED, "http://127.0.0.1:8765/download/OpenWhisper-Setup-2.9.0.exe"],
        [FEED, "http://127.0.0.1:8766/download/x"],
        [FEED, "https://127.0.0.1:8765/download/x"],
        [FEED, "http://localhost:8765/download/x"],
        ["http://127.0.0.1:8765/other", "http://127.0.0.1:8765/other"],
        ["http://[::1]:8765/releases/latest", "http://[::1]:8765/x"],
        ["http://[::1]:8765/releases/latest", "http://[::1:8765/x"],
        [f"{API}/x", "https://api.github.com:443/y"],
        [f"{API}/x", "https://API.github.com/y"],
        [f"{API}/x", "https://api.github.com/y?q#"],
        ["https://api.github.com:443/x", "https://api.github.com/y"],
        ["https://[::1/x", "https://api.github.com/y"],
    ],
    "_parsed_url_is_well_formed": [[u] for u in URLS],
    "_is_loopback_feed_url": [[u] for u in URLS],
    "_asset_url_allowed": [
        [f"{DOWNLOAD}/v2.1.1/{SETUP}", "v2.1.1", SETUP, None],
        [f"{DOWNLOAD}/v2.1.1/{SETUP}", "v2.1.1", SETUP, FEED],
        [f"http://127.0.0.1:8765/download/{SETUP}", "v2.1.1", SETUP, None],
        [f"http://127.0.0.1:8765/download/{SETUP}", "v2.1.1", SETUP, FEED],
        [f"HTTP://127.0.0.1:8765/x/{SETUP}", "v2.1.1", SETUP, FEED],
        [f"http://127.0.0.1:8765/download/{SETUP}?x", "v2.1.1", SETUP, FEED],
        [f"http://127.0.0.1:9999/download/{SETUP}", "v2.1.1", SETUP, FEED],
        [f"http://127.0.0.1:8765/download/{ARCHIVE}", "v2.1.1", SETUP, FEED],
        [SETUP, "v2.1.1", SETUP, FEED],
        ["", "v2.1.1", SETUP, FEED],
        [f"{DOWNLOAD}/v2.1.2/{SETUP}", "v2.1.1", SETUP, None],
    ],
    "_fetch_release_checksums": [
        [f"{DIGEST}  {SETUP}\n{DIGEST}  {ARCHIVE}\n"],
        [f"{DIGEST} *{SETUP}\r\n{DIGEST.upper()}  {ARCHIVE}"],
        # U+0085 (NEL) in UTF-8, which splitlines() also breaks at.
        [f"{DIGEST}  {SETUP}Â\u0085{DIGEST}  {ARCHIVE}"],
        [f"{DIGEST}  {SETUP}\u0085"],
        [f"{DIGEST}  {SETUP}\n\n"],
        [f"{DIGEST}  {SETUP}\n{DIGEST}  {SETUP}\n"],
        [""],
        ["garbage"],
        ["ÿ"],
        [f"{DIGEST}  ../../update.exe\n"],
        [f"{DIGEST}  OpenWhisper-Setup-2.6.0.exe\n"],
        [f"{DIGEST}\t{SETUP}\n"],
        [f"{DIGEST}   {SETUP}\n"],
        [f"{DIGEST[:-1]}  {SETUP}\n"],
        [f"{DIGEST}  {SETUP} \n"],
        [f"{DIGEST}  \n"],
        [["repeat", "x", 65537]],
        [["repeat", "y", 65536]],
    ],
    "_rate_limit": [
        [403, [["X-RateLimit-Remaining", "0"], ["X-RateLimit-Reset", "1787314800"]], NOW_MICROS],
        [403, [["x-ratelimit-remaining", "0"]], NOW_MICROS],
        [403, [["X-RateLimit-Remaining", "1"]], NOW_MICROS],
        [403, [], NOW_MICROS],
        [403, [["Retry-After", ""]], NOW_MICROS],
        [403, [["Retry-After", "120"]], NOW_MICROS],
        [429, [], NOW_MICROS],
        [429, [["Retry-After", "120"]], NOW_MICROS],
        [429, [["Retry-After", "invalid"], ["X-RateLimit-Reset", "invalid"]], NOW_MICROS],
        [429, [["Retry-After", " 30 "]], NOW],
        [429, [["Retry-After", "-600"]], NOW],
        [429, [["Retry-After", "1_000"]], NOW],
        [429, [["Retry-After", "99999999999999"]], NOW],
        [429, [["Retry-After", "315537897599"]], NOW],
        [429, [["X-RateLimit-Reset", "0"]], NOW],
        [429, [["X-RateLimit-Reset", "-43200"]], NOW],
        [429, [["X-RateLimit-Reset", "-43201"]], NOW],
        [429, [["X-RateLimit-Reset", "32536850399"]], NOW],
        [429, [["X-RateLimit-Reset", "32536850400"]], NOW],
        [429, [["X-RateLimit-Reset", "1787314800"], ["Retry-After", "7200"]], NOW],
        [429, [["X-RateLimit-Reset", "1787320800"], ["Retry-After", "60"]], NOW],
        [500, [["Retry-After", "120"]], NOW],
        [404, [["X-RateLimit-Remaining", "0"]], NOW],
    ],
    "_describe_update_error": [
        ["http", [403, "Forbidden"], [["X-RateLimit-Remaining", "0"]]],
        ["http", [403, "Forbidden"], []],
        ["http", [429, "Too Many Requests"], []],
        ["http", [500, "Server Error"], []],
        ["http", [404, "Not Found"], [["Retry-After", "5"]]],
        ["certificate", "[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed", []],
        ["url", "The update URL is not trusted.", []],
        ["url", "The update server redirected to an unsafe URL.", []],
        ["other", "timed out", []],
        ["other", "", []],
    ],
    "update_check_failure_status": [[v] for v in (
        "GitHub rate-limited this update check.", "Could not reach the update server.",
        "RATE LIMIT hit", "rate-limit", "", "The update server returned an error (429 Too Many "
        "Requests).",
    )],
}

WRAPPERS = {
    "seconds_until_auto_check": _seconds,
    "parse_last_check_at": _parse_last_check_at,
    "parse_release_payload": _payload,
    "select_newest_release": _select,
    "_parse_named_asset": _named,
    "_asset_url_allowed": _allowed,
    "_parsed_url_is_well_formed": _well_formed,
    "_fetch_release_checksums": _checksums,
    "_rate_limit": _rate_limit,
    "_describe_update_error": _describe,
}

PINS = {
    "github_repo": REPO,
    "feed": FEED,
    "seconds_until_auto_check_note": "seconds_until_auto_check(settings, now=fromisoformat(now), "
                                      "interval_s=interval, channel=channel)",
    "release_note": "parse_release_payload, select_newest_release and _parse_named_asset return "
                    "ReleaseInfo / ReleaseAsset as dicts without the macOS disk image; the last "
                    "argument is OPENWHISPER_UPDATE_FEED_URL for the call (null: unset)",
    "checksums_note": "_fetch_release_checksums('v2.7.0') over a response whose bytes are the "
                      "argument in latin-1 (['repeat', text, n]: text * n)",
    "rate_limit_note": "[_is_rate_limited(HTTPError(code, headers)), the update_api_retry_at "
                       "_remember_api_rate_limit stores] with datetime.now pinned to the third "
                       "argument",
    "describe_note": "_describe_update_error of certificate: URLError(SSLCertVerificationError), "
                     "http: HTTPError(code, reason, headers), url: URLError(reason), other: "
                     "OSError(text)",
}
