"""Soil sensor history: the in-memory model and its two sources.

A history is, per entity id, a time-ordered list of samples. A sample's value is a float, or
None for "unavailable" (any state that is not a number: Home Assistant's `unavailable` and
`unknown`, an empty string, a JSON null). Timestamps must carry a timezone; a naive timestamp is
ambiguous by hours and is refused rather than guessed.

Sources:
  * load_history_json(): the file format documented in tools/calibrate/README.md.
  * fetch_ha_history(): Home Assistant's REST history API (/api/history/period). The token is
    passed in by the caller and only ever placed in the Authorization header: it is never
    logged, never put in a URL and never included in an error message.
"""

from __future__ import annotations

import json
import math
import re
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Callable

FORMAT_VERSION = 1


class HistoryError(ValueError):
    """A history source is malformed or unreachable. The message never contains a secret."""


@dataclass(frozen=True)
class Sample:
    t: float  # seconds since the Unix epoch, UTC
    v: float | None  # None: unavailable / unknown / not a number


@dataclass
class History:
    sensors: dict[str, list[Sample]] = field(default_factory=dict)
    units: dict[str, str] = field(default_factory=dict)


def parse_timestamp(text: Any) -> float:
    if not isinstance(text, str):
        raise HistoryError(f"timestamp must be an ISO 8601 string, got {type(text).__name__}")
    try:
        dt = datetime.fromisoformat(text)
    except ValueError as e:
        raise HistoryError(f"timestamp is not ISO 8601: {text!r}") from e
    if dt.tzinfo is None:
        raise HistoryError(f"timestamp has no timezone (ambiguous): {text!r}")
    return dt.timestamp()


def parse_value(raw: Any) -> float | None:
    """A number, or None for anything that is not a finite number (unavailable)."""
    if isinstance(raw, bool):  # bool is an int subclass; true/false is not a reading
        return None
    if isinstance(raw, (int, float)):
        value = float(raw)
    elif isinstance(raw, str):
        try:
            value = float(raw.strip())
        except ValueError:
            return None
    else:
        return None
    return value if math.isfinite(value) else None


def format_timestamp(t: float) -> str:
    return datetime.fromtimestamp(t, tz=timezone.utc).isoformat().replace("+00:00", "Z")


def _sorted(samples: list[Sample]) -> list[Sample]:
    return sorted(samples, key=lambda s: s.t)


def history_from_json(doc: Any) -> History:
    if not isinstance(doc, dict):
        raise HistoryError("history file must be a JSON object")
    if doc.get("version") != FORMAT_VERSION:
        raise HistoryError(f"history file version must be {FORMAT_VERSION}, got {doc.get('version')!r}")
    sensors = doc.get("sensors")
    if not isinstance(sensors, dict) or not sensors:
        raise HistoryError("history file needs a non-empty 'sensors' object")
    units = doc.get("units", {})
    if not isinstance(units, dict) or not all(isinstance(u, str) for u in units.values()):
        raise HistoryError("'units' must map entity ids to unit strings")
    history = History(units=dict(units))
    for entity_id, rows in sensors.items():
        if not isinstance(rows, list):
            raise HistoryError(f"{entity_id}: samples must be a list of [timestamp, value] pairs")
        samples = []
        for n, row in enumerate(rows):
            if not isinstance(row, list) or len(row) != 2:
                raise HistoryError(f"{entity_id}[{n}]: each sample is a [timestamp, value] pair")
            try:
                samples.append(Sample(parse_timestamp(row[0]), parse_value(row[1])))
            except HistoryError as e:
                raise HistoryError(f"{entity_id}[{n}]: {e}") from None
        history.sensors[entity_id] = _sorted(samples)
    return history


def load_history_json(path: str) -> History:
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except OSError as e:
        raise HistoryError(f"cannot read history file {path}: {e.strerror}") from None
    except json.JSONDecodeError as e:
        raise HistoryError(f"history file {path} is not JSON: {e}") from None
    return history_from_json(doc)


def history_to_json(history: History) -> dict:
    doc: dict[str, Any] = {"version": FORMAT_VERSION}
    if history.units:
        doc["units"] = dict(sorted(history.units.items()))
    doc["sensors"] = {
        entity_id: [[format_timestamp(s.t), "unavailable" if s.v is None else s.v] for s in samples]
        for entity_id, samples in history.sensors.items()
    }
    return doc


# --- Home Assistant REST history -------------------------------------------------------------

Fetch = Callable[[urllib.request.Request, float], bytes]

# A bearer token is printable ASCII with no whitespace. Anything else (a newline from two pasted
# credentials, a stray space) would make http.client raise an error quoting the whole header.
_TOKEN_RE = re.compile(r"[\x21-\x7e]+")


def check_token(token: str) -> None:
    """Refuse an unusable token before it reaches a header. The message never echoes it."""
    if not token:
        raise HistoryError("Home Assistant token is empty")
    if not _TOKEN_RE.fullmatch(token):
        raise HistoryError("Home Assistant token contains whitespace, control or non-ASCII characters (not shown); check its source")


def _origin(url: str) -> str:
    parts = urllib.parse.urlsplit(url)
    return f"{parts.scheme}://{parts.netloc}"


class _RefuseRedirects(urllib.request.HTTPRedirectHandler):
    """urllib's default handler re-sends the Authorization header to wherever a redirect points,
    another origin or plain http included. The API needs no redirect, so every one is refused."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = _origin(urllib.parse.urljoin(req.full_url, newurl))
        raise HistoryError(
            f"Home Assistant answered HTTP {code} with a redirect to {target}: refused a redirect, so the token"
            " goes nowhere but --ha-url. Pass the final address as --ha-url."
        )


_OPENER = urllib.request.build_opener(_RefuseRedirects)


def _urlopen(request: urllib.request.Request, timeout: float) -> bytes:
    with _OPENER.open(request, timeout=timeout) as response:
        return response.read()


def ha_history_request(base_url: str, token: str, entity_ids: list[str], start: datetime, end: datetime) -> urllib.request.Request:
    parsed = urllib.parse.urlsplit(base_url)
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        raise HistoryError("Home Assistant URL must be http:// or https:// with a host")
    if parsed.username or parsed.password:
        raise HistoryError("Home Assistant URL must not carry credentials; pass the token separately")
    if not entity_ids:
        raise HistoryError("no entity ids to fetch")
    query = urllib.parse.urlencode(
        {
            "filter_entity_id": ",".join(entity_ids),
            "end_time": end.astimezone(timezone.utc).isoformat(),
            "minimal_response": "",
        }
    )
    path = parsed.path.rstrip("/") + "/api/history/period/" + urllib.parse.quote(start.astimezone(timezone.utc).isoformat())
    url = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, path, query, ""))
    return urllib.request.Request(url, headers={"Authorization": f"Bearer {token}", "Accept": "application/json"})


def history_from_ha_response(doc: Any, entity_ids: list[str]) -> History:
    """Parse /api/history/period output: a list of per-entity lists of states.

    With minimal_response only the first state of each list carries entity_id and attributes;
    the rest carry state and last_changed.
    """
    if not isinstance(doc, list):
        raise HistoryError("Home Assistant history response is not a list")
    history = History()
    for series in doc:
        if not isinstance(series, list) or not series or not isinstance(series[0], dict):
            continue
        entity_id = series[0].get("entity_id")
        if not isinstance(entity_id, str):
            raise HistoryError("Home Assistant history series has no entity_id")
        unit = (series[0].get("attributes") or {}).get("unit_of_measurement")
        if isinstance(unit, str):
            history.units[entity_id] = unit
        samples = []
        for state in series:
            if not isinstance(state, dict) or "last_changed" not in state:
                raise HistoryError(f"{entity_id}: history state without last_changed")
            samples.append(Sample(parse_timestamp(state["last_changed"]), parse_value(state.get("state"))))
        history.sensors[entity_id] = _sorted(samples)
    for entity_id in entity_ids:
        history.sensors.setdefault(entity_id, [])
    return history


def fetch_ha_history(
    base_url: str,
    token: str,
    entity_ids: list[str],
    start: datetime,
    end: datetime,
    *,
    fetch: Fetch = _urlopen,
    timeout: float = 60.0,
) -> History:
    check_token(token)
    request = ha_history_request(base_url, token, entity_ids, start, end)
    # Every failure below becomes a fixed message built from non-secret parts only (a status
    # code, an exception type, an OS error string). The original exception is never chained or
    # printed: its text can quote the request's headers, token included.
    error: str | None = None
    try:
        body = fetch(request, timeout)
    except HistoryError:
        raise  # our own redirect refusal: already secret-free
    except urllib.error.HTTPError as e:
        hint = " (check the token)" if e.code in (401, 403) else ""
        error = f"Home Assistant answered HTTP {e.code}{hint}"
    except urllib.error.URLError as e:
        reason = e.reason
        detail = reason.strerror if isinstance(reason, OSError) and reason.strerror else type(reason).__name__
        error = f"cannot reach Home Assistant: {detail}"
    except OSError as e:
        error = f"cannot reach Home Assistant: {e.strerror or type(e).__name__}"
    except Exception as e:  # noqa: BLE001 - anything else may quote the header
        error = f"request to Home Assistant failed ({type(e).__name__})"
    if error is not None:
        raise HistoryError(error) from None
    try:
        doc = json.loads(body)
    except (json.JSONDecodeError, UnicodeDecodeError):
        raise HistoryError("Home Assistant history response is not JSON") from None
    return history_from_ha_response(doc, entity_ids)
