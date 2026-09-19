# Unreal StateTree MCP

*[한국어](README.md) · English*

An MCP server that lets an AI assistant **read and edit Unreal Engine StateTree assets** directly.

> **Status: verified on UE 5.7.4.** Creating an asset, editing states, tasks,
> conditions, transitions and bindings, compiling and saving — all exercised end
> to end against a real project's own task types. Other engine versions are
> untested; see [docs/VERSION_SUPPORT.md](docs/VERSION_SUPPORT.md).

---

## Why a C++ plugin

Unreal automation is normally done in Python. **StateTree cannot be.**

The two arrays that hold the state hierarchy are declared like this:

```cpp
// StateTreeEditorData.h — root states
UPROPERTY(Instanced)
TArray<TObjectPtr<UStateTreeState>> SubTrees;

// StateTreeState.h — child states
UPROPERTY(Instanced)
TArray<TObjectPtr<UStateTreeState>> Children;
```

Neither `EditAnywhere` nor `BlueprintReadWrite`. Unreal's property access gate
(`PropertyAccessUtil::CanGetPropertyValue`) requires **at least one** of
`CPF_Edit`, `CPF_BlueprintVisible` or `CPF_BlueprintAssignable`. These have none
of the three, so even `get_editor_property("children")` comes back
`PermissionDenied`.

**Python therefore cannot see the shape of the tree at all.** From C++ both are
plain `public` members and simply read. Half of what this plugin does is take a
structure only C++ can see and hand it out as JSON.

Compiling is the same story: `UStateTreeEditingSubsystem::CompileStateTree()` is
a plain C++ static, not a `UFUNCTION`, so no script can call it.

---

## How it fits together

```
Claude  ──stdio──▶  MCP server (Python)  ──HTTP──▶  Unreal editor plugin (C++)
                    mcp-server/                     UnrealPlugin/StateTreeMCP/
                                                    127.0.0.1:8092/rpc
```

There is **one** endpoint, `POST /rpc`. Requests are
`{"action": "...", "params": {...}}`; replies are `{"ok": true, "result": {...}}`
or `{"ok": false, "error": "..."}`. Adding a tool needs no routing change, and
the whole thing is testable with a single `curl`.

```
UnrealPlugin/StateTreeMCP/Source/StateTreeMCP/
├─ Public/
│  ├─ StateTreeMCPCompat.h      ← the only file that knows about engine versions
│  ├─ StateTreeMCPSubsystem.h   ← the HTTP bridge
│  └─ StateTreeMCPModule.h
└─ Private/
   ├─ StateTreeMCPCompat.cpp
   ├─ StateTreeMCPSubsystem.cpp ← tool handlers
   └─ StateTreeMCPModule.cpp
```

---

## Tools

| Tool | What it does |
|---|---|
| **Asset** | |
| `statetree_capabilities` | Engine version and which features it supports |
| `statetree_list_schemas` | Schemas this project offers |
| `statetree_create` | Create a StateTree asset |
| `statetree_describe` | The whole tree — states, tasks, conditions, transitions, bindings, with ids |
| `statetree_compile` | Validate and compile (saves by default, returns compiler messages) |
| `statetree_save` | Write to disk |
| **States** | |
| `statetree_add_state` · `rename_state` · `remove_state` | Add · rename · remove (with its subtree) |
| **Nodes (tasks, conditions, evaluators)** | |
| `statetree_list_node_types` | Available node types and their settable properties |
| `statetree_add_node` · `set_node_properties` · `remove_node` | Add · change settings · remove |
| **Transitions** | |
| `statetree_add_transition` · `remove_transition` | Add · remove |
| **Bindings** | |
| `statetree_list_bindable` | What this node is allowed to read |
| `statetree_add_binding` · `remove_binding` | Wire · unwire |

**Tool names are fixed across engine versions.** Version differences are absorbed
inside the plugin.

A typical run:

```
list_schemas → create → add_state → list_node_types → add_node
             → list_bindable → add_binding → add_transition → compile
```

**Not covered:** moving or duplicating states, editing tree parameters, editing
utility considerations. Do those in the editor.

---

## Installing

### 1. The plugin

Copy `UnrealPlugin/StateTreeMCP/` into your project's `Plugins/`.

```powershell
Copy-Item -Recurse "<this repo>\UnrealPlugin\StateTreeMCP" "<project>\Plugins\StateTreeMCP"
```

Add to your `.uproject`:

```json
{ "Name": "StateTree",    "Enabled": true },
{ "Name": "StateTreeMCP", "Enabled": true }
```

Right-click the `.uproject` → **Generate Visual Studio project files** → build.

### 2. The MCP server

The only dependency is `mcp`, which you probably already have if you run any
other MCP server.

```bash
python -c "import mcp" || pip install mcp
```

**Installing the package (`pip install -e .`) is not recommended.** Pointing
`PYTHONPATH` at the repository instead, as below, keeps one copy of the code
rather than one installed and one checked out.

### 3. Registering with Claude

```json
"unreal-statetree": {
  "command": "python",
  "args": ["-m", "statetree_mcp"],
  "env": {
    "PYTHONPATH": "<this repo>\\mcp-server",
    "STATETREE_MCP_PORT": "8092"
  }
}
```

Windows paths need **doubled backslashes** inside JSON (`E:\\Unreal Project\\...`).
A single one is a parse error.

**Which file you put it in matters** — clients read different ones.

| Client | Config file |
|---|---|
| Claude desktop app (Windows) | `%LOCALAPPDATA%\Packages\Claude_*\LocalCache\Roaming\Claude\claude_desktop_config.json` |
| Claude desktop app (macOS) | `~/Library/Application Support/Claude/claude_desktop_config.json` |
| Claude Code (per project) | `.mcp.json` in the project root |

The Code tab inside the desktop app reads the **desktop** config. If you use
both, put the same entry in each. **Restart the app fully after editing.**

> The server needs **the Unreal editor to be running**. With it closed, tools
> answer "Could not reach the Unreal Editor on port 8092...".

---

## Building

Right-click the `.uproject` → **Generate Visual Studio project files** → build.
It is a new module, so **a full rebuild is required; hot reload will not do**.

Open the editor and check the output log:

```
LogStateTreeMCP: StateTree MCP starting on UE 5.7 (StateTree=yes, compile=yes, ...)
LogStateTreeMCP: StateTree MCP bridge listening on http://127.0.0.1:8092/rpc
```

Testing the bridge on its own, without the MCP server:

```bash
curl -X POST http://127.0.0.1:8092/rpc -H "Content-Type: application/json" -d "{\"action\":\"capabilities\"}"
```

### Notes for porting to another engine version

These are what the first 5.7 build tripped over. Another version will likely
trip in the same places.

| Symptom | Cause | Fix |
|---|---|---|
| `C2371` / `C2064` | `FHttpResultCallback` is a typedef for a `TFunction`, not a class, so it cannot be forward declared | include `HttpResultCallback.h` |
| `C2679` / `C2664` | `BindRoute` returns `FHttpRouteHandle`, not `FDelegateHandle` | change the member's type |
| `LNK2019` on a destructor | `FStateTreeCompilerLog` → `FStateTreeBindableStructDesc` → `FPropertyBindingBindableStructDescriptor`, whose vtable belongs to another plugin | link `PropertyBindingUtils` (**not delay-loaded** — a vtable is a data symbol) |
| Only short requests are "invalid JSON" | `Request.Body` is a byte array with no terminator | copy it, append a `0`, then convert |

---

## Port conflicts

The default is **8092**, chosen because other Unreal MCP bridges commonly take
8091. To change it, pass `-StateTreeMCPPort=9000` to the editor and set
`STATETREE_MCP_PORT=9000` for the MCP server.

---

## Supported engine versions

See [docs/VERSION_SUPPORT.md](docs/VERSION_SUPPORT.md). In short: **developed on
UE 5.7, other versions untested.** Supporting a new one is meant to mean editing
`StateTreeMCPCompat.h/.cpp` and nothing else.

---

## License

MIT — [LICENSE](LICENSE)
