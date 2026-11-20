"""File and secret handling at the edge: reading the token, writing outputs."""

from __future__ import annotations

import json
import os
import stat
import sys

# Outputs can hold private data (entity ids, a house's config), so they are written owner-only,
# never with the process umask (AGENTS.md §8, Configuration).
OUTPUT_MODE = 0o600


class ToolError(Exception):
    """A user-facing error. The message never contains a secret."""


def read_token(env_name: str | None, token_file: str | None, environ=os.environ) -> str:
    """The Home Assistant token, from an environment variable or a file. Never from argv."""
    if token_file:
        try:
            st = os.stat(token_file)
            with open(token_file, encoding="utf-8") as f:
                token = f.read().strip()
        except OSError as e:
            raise ToolError(f"cannot read token file {token_file}: {e.strerror}") from None
        if st.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
            print(f"warning: token file {token_file} is readable by other users; chmod 600 it", file=sys.stderr)
        source = f"token file {token_file}"
    else:
        name = env_name or "HASS_TOKEN"
        token = environ.get(name, "").strip()
        source = f"environment variable {name}"
    # A secrets manager whose session is locked can print nothing and still succeed, so an empty
    # token is the likely failure, and it must stop here, not become an HTTP 401 later.
    if not token:
        raise ToolError(f"no Home Assistant token: {source} is empty or unset")
    return token


def write_new(path: str, text: str, *, force: bool = False) -> None:
    """Write a new file, owner-only. Refuses to overwrite unless force."""
    flags = os.O_WRONLY | os.O_CREAT | (os.O_TRUNC if force else os.O_EXCL)
    try:
        fd = os.open(path, flags, OUTPUT_MODE)
    except FileExistsError:
        raise ToolError(f"{path} exists; pass --force to overwrite it") from None
    except OSError as e:
        raise ToolError(f"cannot write {path}: {e.strerror}") from None
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(text)
    os.chmod(path, OUTPUT_MODE)  # O_CREAT's mode does not apply to an existing file under --force


def dumps(doc) -> str:
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def load_json(path: str, what: str):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except OSError as e:
        raise ToolError(f"cannot read {what} {path}: {e.strerror}") from None
    except json.JSONDecodeError as e:
        raise ToolError(f"{what} {path} is not JSON: {e}") from None
