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

	// ---- Mutating the hierarchy ------------------------------------------

	/** Creates a child state under Parent (or a new root when Parent is null). */
	STATETREEMCP_API UStateTreeState* AddChildState(
		UStateTreeEditorData* EditorData, UStateTreeState* Parent, FName Name);

	/** Removes a state and everything under it. Returns false if not found. */
	STATETREEMCP_API bool RemoveState(UStateTreeEditorData* EditorData, const FGuid& StateID);

	// ---- Compiling --------------------------------------------------------

	/** Fixes up links and validates the asset after an edit. No-op if unsupported. */
	STATETREEMCP_API void ValidateTree(UStateTree* StateTree);

	/** Compiles the asset. Returns false and fills OutError when it cannot. */
	STATETREEMCP_API bool CompileTree(UStateTree* StateTree, FString& OutError);

	/** The asset's editor data, or null if the asset has none. */
	STATETREEMCP_API UStateTreeEditorData* GetEditorData(UStateTree* StateTree);
}
