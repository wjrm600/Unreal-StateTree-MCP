"""MCP server exposing Unreal Engine StateTree authoring to AI assistants.

Tool names are part of this project's public contract and stay fixed across
engine versions; only the plugin behind them changes. Call `statetree_capabilities`
first when you need to know what the connected engine actually supports.
"""

from __future__ import annotations

import json
from typing import Any

from mcp.server.fastmcp import FastMCP

from .bridge import BridgeError, BridgeUnavailable, UnrealBridge

mcp = FastMCP("unreal-statetree")
bridge = UnrealBridge()


def _call(action: str, **params: Any) -> str:
    """Runs one bridge action and renders the outcome as text for the model."""
    try:
        return json.dumps(bridge.call(action, **params), indent=2, ensure_ascii=False)
    except (BridgeError, BridgeUnavailable) as exc:
        return f"ERROR: {exc}"


@mcp.tool()
def statetree_capabilities() -> str:
    """Report the connected engine version and which StateTree features it supports.

    Worth calling before anything else: StateTree's editor API differs between
    engine releases, and this says what is actually available rather than what a
    version number implies.
    """
    return _call("capabilities")


@mcp.tool()
def statetree_describe(asset_path: str) -> str:
    """List every state in a StateTree asset, with its hierarchy depth and contents.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".

    Returns a flat list of states, each carrying `id`, `name`, `depth` and
    `parentId`, so the full tree shape can be reconstructed.
    """
    return _call("describe_tree", assetPath=asset_path)


@mcp.tool()
def statetree_add_state(asset_path: str, name: str, parent_id: str = "") -> str:
    """Add a state to a StateTree asset.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".
        name: Name for the new state.
        parent_id: Id of the parent state, from `statetree_describe`. Leave empty
            to add a new root state.

    The asset is marked dirty but not saved, and not compiled. Call
    `statetree_compile` when the edits are complete.
    """
    return _call("add_state", assetPath=asset_path, name=name, parentId=parent_id)


@mcp.tool()
def statetree_compile(asset_path: str) -> str:
    """Validate and compile a StateTree asset so the changes take effect in game.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".

    Edits made through the other tools do nothing at runtime until the asset is
    compiled.
    """
    return _call("compile", assetPath=asset_path)


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
