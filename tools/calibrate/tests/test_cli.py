"""End to end through the command line, plus the committed example outputs."""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

from hearthmap_calibrate import cli
from hearthmap_calibrate.files import ToolError, read_token

TOOL = Path(__file__).resolve().parent.parent
EXAMPLES = TOOL / "examples"
ROOT = TOOL.parent.parent
GENERATED_AT = "2026-01-29T00:00:00Z"
TOKEN = "test-token-not-a-secret-7f3a9c"


def run(*args) -> int:
    return cli.main([str(a) for a in args])


def propose(tmp_path, *extra):
    out, report = tmp_path / "calibration.json", tmp_path / "report.txt"
    code = run("propose", "--history", EXAMPLES / "history.json", "--out", out, "--report", report, "--generated-at", GENERATED_AT, *extra)
    return code, out, report


def test_example_history_matches_its_generator():
    generated = subprocess.run([sys.executable, str(EXAMPLES / "make_history.py")], check=True, capture_output=True, text=True).stdout
    assert generated == (EXAMPLES / "history.json").read_text(encoding="utf-8"), "rerun examples/make_history.py"


def test_example_outputs_match_the_tool(tmp_path):
    code, out, report = propose(tmp_path)
    assert code == 0
    assert out.read_text(encoding="utf-8") == (EXAMPLES / "calibration.json").read_text(encoding="utf-8")
    assert report.read_text(encoding="utf-8") == (EXAMPLES / "report.txt").read_text(encoding="utf-8")


def test_output_validates_and_says_what_it_should(tmp_path, calibration_validator):
    _, out, report = propose(tmp_path)
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert list(calibration_validator.iter_errors(doc)) == []
    sensors = {s["entity_id"]: s for s in doc["sensors"]}
    assert sensors["sensor.example_lavender_moisture"]["status"] == "no_variation"
    assert "calibration" not in sensors["sensor.example_lavender_moisture"]
    pothos = sensors["sensor.example_pothos_moisture_raw"]
    assert pothos["polarity"] == "inverted" and pothos["calibration"]["dry"] > pothos["calibration"]["wet"]
    assert all(s["happy_is_default"] for s in sensors.values() if s["status"] == "ok")
    text = report.read_text(encoding="utf-8")
    assert "cannot calibrate: no variation (every valid reading is 0)" in text
    assert "happy range is a DEFAULT" in text


def test_outputs_are_owner_only_and_never_overwritten(tmp_path):
    _, out, report = propose(tmp_path)
    for f in (out, report):
        assert stat.S_IMODE(os.stat(f).st_mode) == 0o600
    first = out.read_bytes()
    code, _, _ = propose(tmp_path)
    assert code == 2 and out.read_bytes() == first
    os.chmod(out, 0o644)
    code, _, _ = propose(tmp_path, "--force")
    assert code == 0 and stat.S_IMODE(os.stat(out).st_mode) == 0o600


def test_report_goes_to_stdout_by_default(tmp_path, capsys):
    assert run("propose", "--history", EXAMPLES / "history.json", "--out", tmp_path / "c.json") == 0
    assert "hearthmap calibration report" in capsys.readouterr().out


def test_entity_filter_and_unknown_entity(tmp_path):
    code, out, _ = propose(tmp_path, "--entity", "sensor.example_basil_moisture")
    assert code == 0
    assert [s["entity_id"] for s in json.loads(out.read_text())["sensors"]] == ["sensor.example_basil_moisture"]
    assert run("propose", "--history", EXAMPLES / "history.json", "--out", tmp_path / "x.json", "--entity", "sensor.example_none") == 2


def test_bad_happy_fractions_are_refused(tmp_path):
    assert propose(tmp_path, "--happy", "0.8", "0.2")[0] == 2


def test_merge_writes_a_new_valid_file_and_refuses_in_place(tmp_path, config_validator):
    _, cal, _ = propose(tmp_path)
    config = tmp_path / "config.json"
    config.write_text((ROOT / "examples/flat/config.json").read_text(encoding="utf-8"), encoding="utf-8")
    original = config.read_bytes()
    assert run("merge", "--config", config, "--calibration", cal, "--out", config) == 2
    assert run("merge", "--config", config, "--calibration", cal, "--out", config, "--force") == 2
    link = tmp_path / "link.json"
    link.symlink_to(config)
    assert run("merge", "--config", config, "--calibration", cal, "--out", link, "--force") == 2
    assert config.read_bytes() == original
    new = tmp_path / "new.json"
    assert run("merge", "--config", config, "--calibration", cal, "--out", new) == 0
    merged = json.loads(new.read_text(encoding="utf-8"))
    assert list(config_validator.iter_errors(merged)) == []
    basil = next(p for p in merged["pins"] if p["id"] == "plant_basil")
    assert basil["calibration"]["dry"] == 0 and basil["calibration"]["wet"] == 50
    assert config.read_bytes() == original


def test_merge_refuses_a_non_calibration_file(tmp_path):
    bad = tmp_path / "bad.json"
    bad.write_text('{"version": 2}', encoding="utf-8")
    assert run("merge", "--config", ROOT / "examples/flat/config.json", "--calibration", bad, "--out", tmp_path / "o.json") == 2


# --- the token ------------------------------------------------------------------------------


def test_token_from_env_and_file(tmp_path):
    assert read_token(None, None, {"HASS_TOKEN": f" {TOKEN}\n"}) == TOKEN
    assert read_token("MY_TOKEN", None, {"MY_TOKEN": TOKEN}) == TOKEN
    f = tmp_path / "token"
    f.write_text(TOKEN + "\n")
    os.chmod(f, 0o600)
    assert read_token(None, str(f), {}) == TOKEN


def test_empty_token_stops_before_any_request(tmp_path):
    # A locked secrets manager can print nothing and exit 0: that must be an error, here.
    with pytest.raises(ToolError, match="HASS_TOKEN is empty or unset"):
        read_token(None, None, {"HASS_TOKEN": ""})
    f = tmp_path / "token"
    f.write_text("\n")
    with pytest.raises(ToolError, match="empty"):
        read_token(None, str(f), {})


def test_group_readable_token_file_warns(tmp_path, capsys):
    f = tmp_path / "token"
    f.write_text(TOKEN)
    os.chmod(f, 0o644)
    read_token(None, str(f), {})
    err = capsys.readouterr().err
    assert "readable by other users" in err and TOKEN not in err


def test_there_is_no_token_argument():
    for sub in ("fetch", "propose"):
        help_text = cli.build_parser()._subparsers._group_actions[0].choices[sub].format_help()
        assert "--token " not in help_text and "--token=" not in help_text
        assert "--token-env" in help_text and "--token-file" in help_text


def test_fetch_and_propose_from_ha_never_print_the_token(tmp_path, monkeypatch, capsys):
    from hearthmap_calibrate import history

    seen = []

    def fake(request, timeout):
        seen.append(request.get_header("Authorization"))
        rows = [{"entity_id": "sensor.example_basil_moisture", "state": "40", "attributes": {"unit_of_measurement": "%"}, "last_changed": "2026-01-01T00:00:00+00:00"}]
        return json.dumps([rows]).encode()

    monkeypatch.setattr(history, "_urlopen", fake)
    monkeypatch.setattr(history.fetch_ha_history, "__kwdefaults__", {"fetch": fake, "timeout": 60.0})
    monkeypatch.setenv("HASS_TOKEN", TOKEN)
    out = tmp_path / "h.json"
    assert run("fetch", "--ha-url", "https://ha.example.test", "--config", ROOT / "examples/flat/config.json", "--out", out) == 0
    saved = json.loads(out.read_text())
    config = json.loads((ROOT / "examples/flat/config.json").read_text())
    expected = {p["entities"]["moisture"] for p in config["pins"] if p["kind"] in ("soil", "plant")}
    assert len(expected) == 9 and set(saved["sensors"]) == expected  # only soil/plant moisture
    assert run("propose", "--ha-url", "https://ha.example.test", "--entity", "sensor.example_basil_moisture", "--out", tmp_path / "c.json") == 0
    captured = capsys.readouterr()
    assert seen == [f"Bearer {TOKEN}"] * 2
    for blob in (captured.out, captured.err, out.read_text(), (tmp_path / "c.json").read_text()):
        assert TOKEN not in blob


def test_entry_script_runs():
    r = subprocess.run([sys.executable, str(TOOL / "calibrate.py"), "--version"], capture_output=True, text=True)
    assert r.returncode == 0 and r.stdout.startswith("hearthmap-calibrate ")


def test_a_newline_in_the_token_never_reaches_any_output(monkeypatch, capsys, tmp_path):
    # Review finding: http.client quotes an invalid header value, token included, in its
    # ValueError. The run must fail cleanly and the token must appear nowhere.
    import traceback

    secret = "first-half-7f3a9c\nsecond-half-SECRETPART"
    monkeypatch.setenv("HASS_TOKEN", secret)
    out = tmp_path / "c.json"
    try:
        code = run("propose", "--ha-url", "http://127.0.0.1:9", "--entity", "sensor.example_a", "--out", out)
        trace = ""
    except BaseException:  # noqa: BLE001 - the point is to see any escaping exception's text
        code, trace = None, traceback.format_exc()
    captured = capsys.readouterr()
    for blob in (trace, captured.out, captured.err):
        assert "first-half" not in blob and "SECRETPART" not in blob
    assert code == 2 and not out.exists()
