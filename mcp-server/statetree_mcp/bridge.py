"""HTTP bridge to the StateTree MCP plugin running inside the Unreal Editor."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from typing import Any

DEFAULT_PORT = 8092
DEFAULT_TIMEOUT = 30.0


class BridgeError(RuntimeError):
    """The editor answered, but the action failed."""


class BridgeUnavailable(RuntimeError):
    """The editor could not be reached at all."""


class UnrealBridge:
    """Sends one action at a time to the editor's POST /rpc endpoint."""

    def __init__(self, port: int | None = None, timeout: float = DEFAULT_TIMEOUT) -> None:
        self.port = port or int(os.environ.get("STATETREE_MCP_PORT", DEFAULT_PORT))
        self.timeout = timeout

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}/rpc"

    def call(self, action: str, **params: Any) -> dict[str, Any]:
        payload = json.dumps({"action": action, "params": params}).encode("utf-8")
        request = urllib.request.Request(
            self.url,
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                envelope = json.loads(response.read().decode("utf-8"))
        except urllib.error.URLError as exc:
            # Much the most common failure: the editor simply is not open.
            raise BridgeUnavailable(
                f"Could not reach the Unreal Editor on port {self.port}. "
                "Open the project with the StateTree MCP plugin enabled, then try again. "
                f"({exc})"
            ) from exc
        except TimeoutError as exc:
            raise BridgeUnavailable(
                f"The editor did not answer within {self.timeout:g}s. "
                "It may be compiling, importing, or showing a modal dialog."
            ) from exc

        if not envelope.get("ok", False):
            raise BridgeError(envelope.get("error", "The editor reported an unspecified failure."))

        return envelope.get("result", {})
