// Copyright (c) 2026. Licensed under the MIT License.
//
// ============================================================================
//  THE ONLY FILE THAT KNOWS ABOUT ENGINE VERSION DIFFERENCES.
// ============================================================================
//  Epic renames and reshuffles StateTree's editor API between releases. Every
//  such difference is absorbed here, behind a signature that never changes.
//  The rest of the plugin calls StateTreeMCPCompat:: and stays version-blind.
//
//  Porting to a new engine version should mean editing this file and nothing
//  else. If you find yourself adding an #if to any other file, move it here.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

#define STATETREEMCP_UE_AT_LEAST(Major, Minor) \
	(ENGINE_MAJOR_VERSION > (Major) || (ENGINE_MAJOR_VERSION == (Major) && ENGINE_MINOR_VERSION >= (Minor)))

class UStateTree;
class UStateTreeEditorData;
class UStateTreeState;

namespace StateTreeMCPCompat
{
	/** Which optional StateTree features this engine build actually supports. */
	struct FCapabilities
	{
		/** The StateTree plugin is present and enabled in this project. */
		bool bStateTreeEnabled = false;

		/** UStateTreeEditingSubsystem exists, so assets can be compiled and validated. */
		bool bCanCompile = false;

		/** UStateTreeState exposes utility-selection Considerations. */
		bool bHasConsiderations = false;

		/** UStateTreeState exposes per-state TasksCompletion. */
		bool bHasTasksCompletion = false;

		/** Engine version string, e.g. "5.7". */
		FString EngineVersion;
	};

	/**
	 * Probes the running editor once and caches the result.
	 * Backs the `statetree_capabilities` MCP tool so the assistant can ask what
	 * this engine supports instead of guessing from a version number.
	 */
	STATETREEMCP_API const FCapabilities& GetCapabilities();

	/** True when StateTree is usable. Every handler should check this first. */
	STATETREEMCP_API bool IsStateTreeAvailable(FString& OutReason);

	// ---- Reading the hierarchy -------------------------------------------
	// These exist because SubTrees and Children are plain UPROPERTY() members:
	// fully readable from C++, but invisible to Python and Blueprint. Wrapping
	// them here is what lets the MCP layer report a tree's real shape.

	/** Root states of the asset (the "SubTrees" array). */
	STATETREEMCP_API TArray<UStateTreeState*> GetRootStates(UStateTreeEditorData* EditorData);

	/** Direct children of a state. */
	STATETREEMCP_API TArray<UStateTreeState*> GetChildStates(UStateTreeState* State);

	/** Walks the whole hierarchy depth-first, parent before children. */
	STATETREEMCP_API void VisitAllStates(
		UStateTreeEditorData* EditorData,
		TFunctionRef<void(UStateTreeState& State, UStateTreeState* Parent, int32 Depth)> Visitor);

	/** Looks a state up by id anywhere in the tree, or null when absent. */
	STATETREEMCP_API UStateTreeState* FindState(UStateTreeEditorData* EditorData, const FGuid& StateID);

	// ---- Mutating the hierarchy ------------------------------------------

	/** Creates a child state under Parent (or a new root when Parent is null). */
	STATETREEMCP_API UStateTreeState* AddChildState(
		UStateTreeEditorData* EditorData, UStateTreeState* Parent, FName Name);

	/** Removes a state and everything under it. Returns false if not found. */
	STATETREEMCP_API bool RemoveState(UStateTreeEditorData* EditorData, const FGuid& StateID);

	/** Renames a state. Returns false if not found. */
	STATETREEMCP_API bool RenameState(UStateTreeEditorData* EditorData, const FGuid& StateID, FName NewName);

	/** Names of the state properties that may be set, for reporting and validation. */
	STATETREEMCP_API TArray<FString> GetEditableStateProperties();

	/**
	 * Moves a state under a new parent, at a given position among its siblings.
	 *
	 * Sibling order is not cosmetic: the default selection behaviour tries
	 * children in order, so position decides priority.
	 *
	 * NewParentID may be invalid to make the state a root. Index counts from 0,
	 * and anything past the end (or negative) appends.
	 */
	STATETREEMCP_API bool MoveState(
		UStateTreeEditorData* EditorData,
		const FGuid& StateID,
		const FGuid& NewParentID,
		int32 Index,
		FString& OutError);

	// ---- Creating assets --------------------------------------------------

	/**
	 * Every StateTree needs a schema, which decides what it can be attached to and
	 * which nodes it may use. Projects add their own, so the list is gathered by
	 * reflection rather than hard-coded.
	 */
	STATETREEMCP_API TArray<UClass*> GetSchemaClasses();

	/** Finds a schema class by name, accepting either "UStateTreeComponentSchema" or "StateTreeComponentSchema". */
	STATETREEMCP_API UClass* FindSchemaClass(const FString& SchemaName);

	/**
	 * Creates a StateTree asset with one root state, compiled and ready.
	 * PackagePath is a content folder, e.g. "/Game/AI".
	 */
	STATETREEMCP_API UStateTree* CreateStateTree(
		const FString& PackagePath, const FString& AssetName, UClass* SchemaClass, FString& OutError);

	// ---- Compiling --------------------------------------------------------

	/** Fixes up links and validates the asset after an edit. No-op if unsupported. */
	STATETREEMCP_API void ValidateTree(UStateTree* StateTree);

	/**
	 * One line from the compiler, as the Message Log would show it.
	 *
	 * The text already names the state and node the message concerns; the
	 * compiler keeps those as separate fields but does not expose them, and its
	 * own formatting is the one the editor shows.
	 */
	struct FCompileMessage
	{
		FString Severity;   // "Error", "Warning" or "Info"
		FString Message;
	};

	/**
	 * Compiles the asset, collecting what the compiler said.
	 *
	 * The messages matter as much as the result: a failure usually names the one
	 * state or task at fault, and without them a caller is only told "it failed".
	 */
	STATETREEMCP_API bool CompileTree(
		UStateTree* StateTree, TArray<FCompileMessage>& OutMessages, FString& OutError);

	/** The asset's editor data, or null if the asset has none. */
	STATETREEMCP_API UStateTreeEditorData* GetEditorData(UStateTree* StateTree);

	// ---- Nodes: tasks, conditions, evaluators -----------------------------
	//
	// A node is an FStateTreeEditorNode holding two pieces: Node, the task or
	// condition type itself, and Instance, that type's settings. Building one by
	// hand means initialising both, which is what AddNode does.

	/** Which list on a state (or on the asset, for global nodes) a node belongs to. */
	enum class ENodeKind : uint8
	{
		Task,
		EnterCondition,
		Evaluator,      // asset-level
		GlobalTask,     // asset-level
	};

	/** Parses "task", "condition", "evaluator" or "globalTask". */
	STATETREEMCP_API bool ParseNodeKind(const FString& Text, ENodeKind& OutKind);

	/**
	 * Node types of this kind that the asset's schema permits.
	 * A schema restricts what may be used, so the answer depends on the asset.
	 */
	STATETREEMCP_API TArray<const UScriptStruct*> GetNodeTypes(
		UStateTreeEditorData* EditorData, ENodeKind Kind);

	/** Finds a node type by name, with or without its leading F, or by path. */
	STATETREEMCP_API const UScriptStruct* FindNodeType(
		UStateTreeEditorData* EditorData, ENodeKind Kind, const FString& TypeName);

	/** The struct holding a node type's settings, or null when it has none. */
	STATETREEMCP_API const UStruct* GetNodeInstanceType(const UScriptStruct* NodeType);

	/**
	 * Appends a node of NodeType to the relevant list and hands back the new
	 * node's id and the memory of its settings, for the caller to fill in.
	 */
	STATETREEMCP_API bool AddNode(
		UStateTreeEditorData* EditorData,
		UStateTreeState* State,
		ENodeKind Kind,
		const UScriptStruct* NodeType,
		FGuid& OutNodeID,
		const UStruct*& OutInstanceType,
		void*& OutInstanceMemory,
		FString& OutError);

	/**
	 * Finds a node's settings anywhere in the asset, for reading or editing.
	 * OutOwner is the object to call Modify() on before changing anything.
	 */
	STATETREEMCP_API bool GetNodeInstance(
		UStateTreeEditorData* EditorData,
		const FGuid& NodeID,
		const UStruct*& OutInstanceType,
		void*& OutInstanceMemory,
		UObject*& OutOwner);

	/** Removes a node, and any bindings that targeted it. Returns false if not found. */
	STATETREEMCP_API bool RemoveNode(UStateTreeEditorData* EditorData, const FGuid& NodeID);

	// ---- Bindings ---------------------------------------------------------
	//
	// A task rarely carries its inputs as literals: it reads them from context
	// data such as the AI controller, from tree parameters, or from an earlier
	// node's output. A binding is that wire.

	/** A struct a node's property can be wired to. */
	struct FBindableSource
	{
		FString ID;                  // pass back as sourceStructId
		FString Name;
		FString StructName;
		TArray<FString> Properties;  // readable properties on it
	};

	/** What the given node is allowed to bind to, per the tree's structure. */
	STATETREEMCP_API TArray<FBindableSource> GetBindableSources(
		UStateTreeEditorData* EditorData, const FGuid& NodeID);

	/** A wire already in place on a node. */
	struct FBindingInfo
	{
		FString TargetProperty;  // the setting being fed
		FString SourceName;      // what feeds it, e.g. "AIController"
		FString SourcePath;      // the property on it, empty when bound whole
	};

	/** The wires feeding this node, so a caller can see what it has set up. */
	STATETREEMCP_API TArray<FBindingInfo> GetNodeBindings(
		UStateTreeEditorData* EditorData, const FGuid& NodeID);

	/** Wires SourceStructID.SourceProperty into the node's TargetProperty. */
	STATETREEMCP_API bool AddBinding(
		UStateTreeEditorData* EditorData,
		const FGuid& NodeID,
		const FString& TargetProperty,
		const FGuid& SourceStructID,
		const FString& SourceProperty,
		FString& OutError);

	/** Drops whatever was wired into the node's TargetProperty. */
	STATETREEMCP_API bool RemoveBinding(
		UStateTreeEditorData* EditorData, const FGuid& NodeID, const FString& TargetProperty);

	// ---- Transitions ------------------------------------------------------

	/**
	 * Adds a transition to a state.
	 *
	 * TargetStateID is only consulted for a GotoState link; the other link types
	 * (NextState, Succeeded, Failed and so on) name their destination themselves.
	 */
	STATETREEMCP_API bool AddTransition(
		UStateTreeEditorData* EditorData,
		UStateTreeState* State,
		const FString& TriggerName,
		const FString& LinkTypeName,
		const FGuid& TargetStateID,
		const FString& PriorityName,
		FGuid& OutTransitionID,
		FString& OutError);

	/** Removes a transition by id, searching every state. Returns false if not found. */
	STATETREEMCP_API bool RemoveTransition(UStateTreeEditorData* EditorData, const FGuid& TransitionID);

	// ---- Saving -----------------------------------------------------------

	/**
	 * Writes the asset's package to disk.
	 *
	 * Editing only marks the package dirty. Without an explicit save the work
	 * lives in memory alone and is lost when the editor closes - a trap worth
	 * closing rather than documenting.
	 */
	STATETREEMCP_API bool SaveAsset(UObject* Asset, FString& OutError);
}
