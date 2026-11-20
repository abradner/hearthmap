"""History sources: the JSON file format and Home Assistant's REST history API."""

from __future__ import annotations

import json
import threading
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest

from hearthmap_calibrate.history import (
    HistoryError,
    Sample,
    fetch_ha_history,
    history_from_ha_response,
    history_from_json,
    history_to_json,
    load_history_json,
    parse_value,
)

TOKEN = "test-token-not-a-secret-7f3a9c"


def doc(**sensors):
    return {"version": 1, "sensors": sensors}


def test_json_round_trip_keeps_values_unavailable_and_units():
    h = history_from_json(
        {
            "version": 1,
            "units": {"sensor.example_a": "%"},
            "sensors": {
                "sensor.example_a": [
                    ["2026-01-01T01:00:00Z", 41.5],
                    ["2026-01-01T00:00:00+00:00", "40"],
                    ["2026-01-01T02:00:00Z", "unavailable"],
                    ["2026-01-01T03:00:00+10:00", None],
                ]
            },
        }
    )
    samples = h.sensors["sensor.example_a"]
    assert [s.v for s in samples] == [None, 40.0, 41.5, None]  # sorted by time; +10:00 is earliest
    assert h.units == {"sensor.example_a": "%"}
    again = history_from_json(history_to_json(h))
    assert again.sensors == h.sensors and again.units == h.units


@pytest.mark.parametrize(
    "raw,expected",
    [(0, 0.0), ("0", 0.0), (" 12.5 ", 12.5), ("unknown", None), ("", None), (None, None), (True, None), ("nan", None), ("inf", None), ([1], None)],
)
def test_parse_value(raw, expected):
    assert parse_value(raw) == expected


def test_zero_is_a_reading_not_missing():
    h = history_from_json(doc(**{"sensor.example_a": [["2026-01-01T00:00:00Z", 0]]}))
    assert h.sensors["sensor.example_a"] == [Sample(1767225600.0, 0.0)]


@pytest.mark.parametrize(
    "bad,needle",
    [
        ([], "JSON object"),
        ({"version": 2, "sensors": {"a": []}}, "version"),
        ({"version": 1, "sensors": {}}, "non-empty"),
        ({"version": 1, "sensors": {"a": "x"}}, "list"),
        ({"version": 1, "sensors": {"a": [["2026-01-01T00:00:00Z"]]}}, "pair"),
        ({"version": 1, "sensors": {"a": [["2026-01-01T00:00:00", 1]]}}, "no timezone"),
        ({"version": 1, "sensors": {"a": [["yesterday", 1]]}}, "ISO 8601"),
        ({"version": 1, "sensors": {"a": [[1767225600, 1]]}}, "ISO 8601 string"),
        ({"version": 1, "units": {"a": 3}, "sensors": {"a": []}}, "units"),
    ],
)
def test_json_format_errors_are_specific(bad, needle):
    with pytest.raises(HistoryError, match=needle):
        history_from_json(bad)


def test_load_history_json_errors(tmp_path):
    with pytest.raises(HistoryError, match="cannot read"):
        load_history_json(str(tmp_path / "missing.json"))
    (tmp_path / "bad.json").write_text("{", encoding="utf-8")
    with pytest.raises(HistoryError, match="not JSON"):
        load_history_json(str(tmp_path / "bad.json"))


HA_RESPONSE = [
    [
        {"entity_id": "sensor.example_a", "state": "40", "attributes": {"unit_of_measurement": "%"}, "last_changed": "2026-01-01T00:00:00+00:00"},
        {"state": "unavailable", "last_changed": "2026-01-01T01:00:00+00:00"},
        {"state": "38.5", "last_changed": "2026-01-01T02:00:00+00:00"},
    ],
    [{"entity_id": "sensor.example_b", "state": "2900", "attributes": {}, "last_changed": "2026-01-01T00:00:00+00:00"}],
]


def test_ha_response_parsing_minimal_response_shape():
    h = history_from_ha_response(HA_RESPONSE, ["sensor.example_a", "sensor.example_b", "sensor.example_c"])
    assert [s.v for s in h.sensors["sensor.example_a"]] == [40.0, None, 38.5]
    assert h.units == {"sensor.example_a": "%"}
    assert h.sensors["sensor.example_c"] == []  # asked for, nothing returned: kept, analysed as empty


def test_ha_response_errors():
    with pytest.raises(HistoryError, match="not a list"):
        history_from_ha_response({}, [])
    with pytest.raises(HistoryError, match="no entity_id"):
        history_from_ha_response([[{"state": "1", "last_changed": "2026-01-01T00:00:00Z"}]], [])
    with pytest.raises(HistoryError, match="last_changed"):
        history_from_ha_response([[{"entity_id": "sensor.example_a", "state": "1"}]], [])


START = datetime(2026, 1, 1, tzinfo=timezone.utc)
END = START + timedelta(days=28)


def test_fetch_builds_the_request_and_keeps_the_token_in_the_header_only():
    seen = {}

    def fake(request, timeout):
        seen["url"], seen["auth"] = request.full_url, request.get_header("Authorization")
        return json.dumps(HA_RESPONSE).encode()

    h = fetch_ha_history("https://ha.example.test:8123/", TOKEN, ["sensor.example_a", "sensor.example_b"], START, END, fetch=fake)
    assert seen["auth"] == f"Bearer {TOKEN}"
    assert TOKEN not in seen["url"]
    assert seen["url"].startswith("https://ha.example.test:8123/api/history/period/2026-01-01T00%3A00%3A00%2B00%3A00?")
    assert "filter_entity_id=sensor.example_a%2Csensor.example_b" in seen["url"]
    assert "minimal_response=" in seen["url"] and "end_time=2026-01-29" in seen["url"]
    assert set(h.sensors) == {"sensor.example_a", "sensor.example_b"}


@pytest.mark.parametrize(
    "url,needle",
    [("ftp://ha.example.test", "http"), ("ha.example.test", "http"), ("https://user:pw@ha.example.test", "credentials")],
)
def test_fetch_refuses_bad_urls(url, needle):
    with pytest.raises(HistoryError, match=needle):
        fetch_ha_history(url, TOKEN, ["sensor.example_a"], START, END, fetch=lambda r, t: b"[]")


def test_fetch_refuses_an_empty_token_and_no_entities():
    with pytest.raises(HistoryError, match="token is empty"):
        fetch_ha_history("https://ha.example.test", "", ["sensor.example_a"], START, END, fetch=lambda r, t: b"[]")
    with pytest.raises(HistoryError, match="no entity ids"):
        fetch_ha_history("https://ha.example.test", TOKEN, [], START, END, fetch=lambda r, t: b"[]")


def test_fetch_non_json_body():
    with pytest.raises(HistoryError, match="not JSON"):
        fetch_ha_history("https://ha.example.test", TOKEN, ["sensor.example_a"], START, END, fetch=lambda r, t: b"<html>")


class _Handler(BaseHTTPRequestHandler):
    status = 200
    seen: list = []

    def do_GET(self):  # noqa: N802
        _Handler.seen.append((self.path, self.headers.get("Authorization")))
        body = json.dumps(HA_RESPONSE).encode() if _Handler.status == 200 else b'{"message": "Unauthorized"}'
        self.send_response(_Handler.status)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


@pytest.fixture
def server():
    srv = HTTPServer(("127.0.0.1", 0), _Handler)
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    _Handler.seen = []
    yield f"http://127.0.0.1:{srv.server_port}"
    srv.shutdown()
    srv.server_close()
    _Handler.status = 200


def test_fetch_against_a_real_http_server(server):
    h = fetch_ha_history(server, TOKEN, ["sensor.example_a"], START, END)
    assert [s.v for s in h.sensors["sensor.example_a"]] == [40.0, None, 38.5]
    path, auth = _Handler.seen[0]
    assert path.startswith("/api/history/period/") and auth == f"Bearer {TOKEN}"


def test_http_401_names_the_token_but_never_prints_it(server):
    _Handler.status = 401
    with pytest.raises(HistoryError) as err:
        fetch_ha_history(server, TOKEN, ["sensor.example_a"], START, END)
    assert "HTTP 401" in str(err.value) and "check the token" in str(err.value)
    assert TOKEN not in str(err.value) and TOKEN not in repr(err.value.__cause__) and err.value.__suppress_context__


def test_unreachable_host_is_a_history_error():
    with pytest.raises(HistoryError, match="cannot reach"):
        fetch_ha_history("http://127.0.0.1:9", TOKEN, ["sensor.example_a"], START, END, timeout=2)
