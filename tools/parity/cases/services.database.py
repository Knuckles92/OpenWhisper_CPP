"""Schema and migrations of services/database.py and services/models.py (M1.4 sql-db).

Each case opens a real DatabaseManager on a copy in out/parity (never OpenWhisper's database)
and records what the file holds afterwards: sqlite_master (type, name, tbl_name, sql), PRAGMA
table_info of every table, and every row of every ordinary table. The C++ replay,
tests/db/test_golden.cpp, opens the same files with fv::Database and compares.

    fresh_schema()                    a new file
    migrated_schema(fixture)          tests/fixtures/db/<fixture>.db opened once
    remigrated_schema(fixture, v)     opened, schema_version forced back to v, opened again

The fixtures are made by OpenWhisper's Python as well, by running this file as a script:

    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/cases/services.database.py --write-fixtures

    schema_v5.db   the v5 layout of tests/test_database.py's legacy meeting tables, with a
                        v5-era transcription_history holding two rows
    schema_v16.db  tests/test_personalize_foundation_history.py's v16 database
    schema_v17.db  a full schema made by DatabaseManager, then put back to v17: the v17
                        dictation_stats of tests/test_stats_features.py, no
                        meeting_segment_delivery yet (so the backfill runs), two meeting
                        segments, a legacy meetings table
    fresh_v18.sql       sqlite_master.sql of a fresh database, in creation order
"""

import shutil
import sqlite3
import sys
from contextlib import closing
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ow_isolation  # noqa: E402

WORK = ow_isolation.SCRATCH / "database-cases"
FIXTURES = ow_isolation.REPO / "tests" / "fixtures" / "db"
FIXTURE_NAMES = ("schema_v5", "schema_v16", "schema_v17")


def _dump(path: Path) -> dict:
    with closing(sqlite3.connect(path)) as conn:
        master = [list(row) for row in conn.execute(
            "SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY type, name")]
        tables = [row[0] for row in conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
        info = {table: [list(row) for row in conn.execute(f'PRAGMA table_info("{table}")')]
                for table in tables}
        # Ordinary tables only: the FTS5 shadow tables hold binary index pages.
        plain = [table for table in tables if not table.startswith("meeting_segments_fts")]
        rows = {table: [list(row) for row in conn.execute(
            f'SELECT * FROM "{table}" ORDER BY rowid')] for table in plain}
        version = [list(row) for row in conn.execute("SELECT version FROM schema_version")]
    return {"sqlite_master": master, "table_info": info, "rows": rows,
            "schema_version": version}


def _open_and_close(path: Path) -> None:
    from services.database import DatabaseManager

    manager = DatabaseManager(db_path=str(path))
    manager.close()


def _fresh_path(name: str) -> Path:
    WORK.mkdir(parents=True, exist_ok=True)
    path = WORK / f"{name}.db"
    for suffix in ("", "-wal", "-shm", "-journal"):
        Path(str(path) + suffix).unlink(missing_ok=True)
    return path


def _fresh_schema(module):
    path = _fresh_path("fresh")
    _open_and_close(path)
    return _dump(path)


def _migrated_schema(module, fixture):
    path = _fresh_path(fixture)
    shutil.copyfile(FIXTURES / f"{fixture}.db", path)
    _open_and_close(path)
    return _dump(path)


def _remigrated_schema(module, fixture, version):
    path = _fresh_path(fixture)
    shutil.copyfile(FIXTURES / f"{fixture}.db", path)
    _open_and_close(path)
    with closing(sqlite3.connect(path)) as conn, conn:
        conn.execute("UPDATE schema_version SET version = ?", (version,))
    _open_and_close(path)
    return _dump(path)


CASES = {
    "fresh_schema": [[]],
    "migrated_schema": [[name] for name in FIXTURE_NAMES],
    "remigrated_schema": [["schema_v16", 16], ["schema_v17", 17]],
}

WRAPPERS = {
    "fresh_schema": _fresh_schema,
    "migrated_schema": _migrated_schema,
    "remigrated_schema": _remigrated_schema,
}

PINS = {
    "sqlite": sqlite3.sqlite_version,
    "note": "each case runs DatabaseManager on a copy in out/parity and dumps sqlite_master "
            "(ORDER BY type, name), PRAGMA table_info per table, every row of every ordinary "
            "table ORDER BY rowid, and schema_version",
}


# ---- fixtures ----

def _legacy(path: Path, version: int, *statements: str) -> None:
    """A database file left at ``version`` by an older build (tests/test_database.py)."""
    with closing(sqlite3.connect(path)) as conn:
        conn.execute("CREATE TABLE schema_version (version INTEGER PRIMARY KEY)")
        conn.execute(f"INSERT INTO schema_version(version) VALUES ({version})")
        for statement in statements:
            conn.execute(statement)
        conn.commit()


def _write_v5(path: Path) -> None:
    _legacy(
        path, 5,
        """
        CREATE TABLE transcription_history (
            id TEXT PRIMARY KEY,
            text TEXT NOT NULL,
            timestamp TEXT NOT NULL,
            model TEXT NOT NULL,
            audio_file TEXT,
            transcription_time REAL,
            audio_duration REAL,
            file_size INTEGER
        )
        """,
        "CREATE INDEX idx_history_timestamp ON transcription_history (timestamp)",
        "INSERT INTO transcription_history VALUES ('v5-a', 'first entry', "
        "'2025-03-01T09:15:00', 'local_whisper', 'recording_1.wav', 1.5, 3.25, 48000)",
        "INSERT INTO transcription_history VALUES ('v5-b', 'second entry', "
        "'2025-03-02T10:00:00+00:00', 'api_whisper', NULL, NULL, NULL, NULL)",
        """
        CREATE TABLE meetings (
            id TEXT PRIMARY KEY,
            title TEXT,
            start_time TEXT
        )
        """,
        """
        CREATE TABLE meeting_chunks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            meeting_id TEXT NOT NULL,
            chunk_index INTEGER NOT NULL,
            text TEXT NOT NULL,
            timestamp TEXT NOT NULL
        )
        """,
        """
        CREATE TABLE meeting_insights (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            meeting_id TEXT NOT NULL,
            insight_type TEXT NOT NULL,
            content TEXT NOT NULL,
            custom_prompt TEXT,
            generated_at TEXT NOT NULL
        )
        """,
        "CREATE INDEX idx_insights_meeting_id ON meeting_insights(meeting_id)",
        "CREATE UNIQUE INDEX idx_insights_unique "
        "ON meeting_insights(meeting_id, insight_type, COALESCE(custom_prompt, ''))",
        "CREATE INDEX idx_meetings_start_time ON meetings(start_time)",
        "CREATE INDEX idx_chunks_meeting_id ON meeting_chunks(meeting_id)",
        "INSERT INTO meetings VALUES ('m1', 'Old meeting', '2025-02-01T10:00:00')",
        "INSERT INTO meeting_chunks (meeting_id, chunk_index, text, timestamp) "
        "VALUES ('m1', 0, 'hello', '2025-02-01T10:00:05')",
    )


def _write_v16(path: Path) -> None:
    """tests/test_personalize_foundation_history.py::_v16_database."""
    _legacy(
        path, 16,
        """
        CREATE TABLE transcription_history (
            id TEXT PRIMARY KEY,
            text TEXT NOT NULL,
            raw_text TEXT,
            timestamp TEXT NOT NULL,
            model TEXT NOT NULL,
            audio_file TEXT,
            transcription_time REAL,
            audio_duration REAL,
            file_size INTEGER,
            cleanup_provider TEXT,
            cleanup_model TEXT,
            source_name TEXT,
            title TEXT,
            origin_device_id TEXT,
            origin_device_name TEXT
        )
        """,
        "INSERT INTO transcription_history (id, text, timestamp, model, source_name) "
        "VALUES ('old', 'kept text', '2026-10-01T09:00:00+00:00', 'base', 'Quick Record')",
    )


def _write_v17(path: Path) -> None:
    _open_and_close(path)
    with closing(sqlite3.connect(path)) as conn:
        conn.execute("PRAGMA foreign_keys=ON")
        conn.execute(
            "INSERT INTO transcription_history (id, text, timestamp, model, source_name, "
            "app_name, entry_kind, language) VALUES ('v17-a', 'hello there', "
            "'2026-10-05T10:00:00+00:00', 'parakeet', 'Quick Record', 'Notepad', "
            "'dictation', 'en')")
        conn.execute(
            "INSERT INTO meeting_sessions (id, title, status, started_at, ended_at, "
            "paused_total_s, host_token, guest_token, cloud_enabled, spool_dir, state_seq) "
            "VALUES ('meet-1', 'Standup', 'ended', '2026-10-05T09:00:00+00:00', "
            "'2026-10-05T09:30:00+00:00', 0.0, 'h', 'g', 0, '', 0)")
        # Inserted in the opposite order of their start times: the backfill follows rowid.
        for segment_id, start, text in (("meet-1-1", 1.0, "three more words"),
                                        ("meet-1-0", 0.0, "hello there")):
            conn.execute(
                "INSERT INTO meeting_segments (id, meeting_id, channel, start_s, end_s, text, "
                "speaker_source, speaker_pinned, created_at) VALUES (?, 'meet-1', 'mic', ?, "
                "?, ?, 'channel', 0, '2026-10-05T09:00:00+00:00')",
                (segment_id, start, start + 1.0, text))
        # Before meeting_segment_delivery existed.
        conn.execute("DROP TRIGGER meeting_segments_delivery_ai")
        conn.execute("DROP TABLE meeting_segment_delivery")
        # tests/test_stats_features.py::test_a_v17_stats_table_gains_the_feature_columns.
        conn.execute("DROP TABLE dictation_stats")
        conn.execute(
            "CREATE TABLE dictation_stats (id INTEGER PRIMARY KEY AUTOINCREMENT, entry_id TEXT, "
            "timestamp TEXT NOT NULL, entry_kind TEXT NOT NULL, app_name TEXT, app_category TEXT, "
            "cleanup_level TEXT, spoken_words INTEGER NOT NULL, final_words INTEGER NOT NULL, "
            "words_edited INTEGER NOT NULL, audio_seconds REAL, language TEXT)")
        conn.execute(
            "INSERT INTO dictation_stats (timestamp, entry_kind, spoken_words, final_words, "
            "words_edited) VALUES ('2026-10-05T10:00:00', 'dictation', 4, 4, 0)")
        conn.execute("CREATE TABLE meetings (id TEXT PRIMARY KEY, title TEXT, start_time TEXT)")
        conn.execute("UPDATE schema_version SET version = 17")
        conn.commit()
        # One self-contained file: no -wal beside it.
        conn.execute("PRAGMA journal_mode=DELETE")
        conn.execute("VACUUM")


def write_fixtures() -> None:
    FIXTURES.mkdir(parents=True, exist_ok=True)
    for name, writer in (("schema_v5", _write_v5), ("schema_v16", _write_v16),
                         ("schema_v17", _write_v17)):
        path = _fresh_path(name)
        writer(path)
        shutil.copyfile(path, FIXTURES / f"{name}.db")
        print(f"{name}.db")
    path = _fresh_path("fresh-sql")
    _open_and_close(path)
    with closing(sqlite3.connect(path)) as conn:
        statements = [row[0] for row in conn.execute(
            "SELECT sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY rowid")]
    (FIXTURES / "fresh_v18.sql").write_bytes(
        ("".join(f"{statement};\n\n" for statement in statements)).encode("utf-8"))
    print("fresh_v18.sql")


if __name__ == "__main__":
    if sys.argv[1:] != ["--write-fixtures"]:
        raise SystemExit("usage: services.database.py --write-fixtures")
    ow_isolation.check_reference_commit(ow_isolation.checkout())
    data_root = ow_isolation.isolate()
    ow_isolation.after_import_guards(data_root)
    write_fixtures()
