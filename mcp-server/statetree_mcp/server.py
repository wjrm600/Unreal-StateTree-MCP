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
def statetree_list_schemas() -> str:
    """List the StateTree schemas this project offers.

    A schema decides what a tree can be attached to and which tasks and
    conditions it may use, and every new tree needs one. Projects define their
    own, so call this before `statetree_create` rather than guessing a name.
    """
    return _call("list_schemas")


@mcp.tool()
def statetree_create(package_path: str, name: str, schema: str) -> str:
    """Create a new StateTree asset containing a single root state.

    Args:
        package_path: Content folder to create it in, e.g. "/Game/AI".
        name: Asset name, e.g. "ST_Grunt".
        schema: Schema class name from `statetree_list_schemas`.

    The asset is compiled and saved. Add states to it with
    `statetree_add_state`.
    """
    return _call("create", packagePath=package_path, name=name, schema=schema)


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
def statetree_list_node_types(asset_path: str, kind: str = "task") -> str:
    """List the tasks, conditions or evaluators this tree is allowed to use.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".
        kind: One of "task", "condition", "evaluator", "globalTask".

    Each entry carries its settable `properties` with names and types. Call this
    before `statetree_add_node`: the tree's schema decides what is permitted,
    and projects define their own tasks, so the list cannot be guessed.
    """
    return _call("list_node_types", assetPath=asset_path, kind=kind)


@mcp.tool()
def statetree_add_node(
    asset_path: str,
    kind: str,
    node_type: str,
    state_id: str = "",
    properties: dict | None = None,
) -> str:
    """Add a task, condition or evaluator, and set its properties.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".
        kind: "task" or "condition" for a state; "evaluator" or "globalTask" for
            the whole tree.
        node_type: Type name from `statetree_list_node_types`.
        state_id: The state to add to. Required for tasks and conditions, and
            left empty for evaluators and global tasks, which belong to the tree.
        properties: Settings for the node, keyed by the property names that
            `statetree_list_node_types` reports for this type.

    This is what makes a state actually do something; a state with no tasks runs
    and completes immediately.
    """
    return _call(
        "add_node",
        assetPath=asset_path,
        kind=kind,
        nodeType=node_type,
        stateId=state_id,
        properties=properties or {},
    )


@mcp.tool()
def statetree_add_transition(
    asset_path: str,
    state_id: str,
    trigger: str = "OnStateCompleted",
    link_type: str = "GotoState",
    target_state_id: str = "",
    priority: str = "Normal",
) -> str:
    """Add a transition telling a state where to go and when.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".
        state_id: The state the transition leaves from.
        trigger: When it fires — "OnStateCompleted", "OnStateSucceeded",
            "OnStateFailed", "OnTick", "OnEvent", "OnDelegate".
        link_type: Where it goes — "GotoState", "NextState",
            "NextSelectableState", "Succeeded", "Failed", "None".
        target_state_id: The destination, required only for "GotoState".
        priority: "Low", "Normal", "Medium", "High" or "Critical", deciding
            which transition wins when several fire at once.

    Without transitions a tree cannot move between states, so this is what turns
    a hierarchy into behaviour.
    """
    return _call(
        "add_transition",
        assetPath=asset_path,
        stateId=state_id,
        trigger=trigger,
        linkType=link_type,
        targetStateId=target_state_id,
        priority=priority,
    )


@mcp.tool()
def statetree_rename_state(asset_path: str, state_id: str, name: str) -> str:
    """Rename a state.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".
        state_id: Id of the state, from `statetree_describe`.
        name: The new name.
    """
    return _call("rename_state", assetPath=asset_path, stateId=state_id, name=name)


@mcp.tool()
def statetree_remove_state(asset_path: str, state_id: str) -> str:
    """Remove a state and everything nested under it.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".
        state_id: Id of the state, from `statetree_describe`.

    Child states go with the parent, so check the tree first if you only meant
    to remove one. The reply reports how many states were removed.
    """
    return _call("remove_state", assetPath=asset_path, stateId=state_id)


@mcp.tool()
def statetree_compile(asset_path: str, save: bool = True) -> str:
    """Validate and compile a StateTree asset so the changes take effect in game.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".
        save: Also write the asset to disk, which is almost always what you want.
            Editing alone only marks it dirty, so skipping this loses the work if
            the editor closes.

    Edits made through the other tools do nothing at runtime until the asset is
    compiled.
    """
    return _call("compile", assetPath=asset_path, save=save)


@mcp.tool()
def statetree_save(asset_path: str) -> str:
    """Write a StateTree asset to disk.

    Args:
        asset_path: Content path of the asset, e.g. "/Game/AI/ST_Grunt".

    Only needed to persist edits without compiling; `statetree_compile` saves by
    default.
    """
    return _call("save", assetPath=asset_path)


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
