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
