// Copyright (c) 2026. Licensed under the MIT License.

#include "StateTreeMCPCompat.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#if STATETREEMCP_HAS_STATETREE
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#if STATETREEMCP_HAS_EDITING_SUBSYSTEM
#include "StateTreeEditingSubsystem.h"
#include "StateTreeCompilerLog.h"
#endif
#endif

DEFINE_LOG_CATEGORY_STATIC(LogStateTreeMCPCompat, Log, All);

namespace StateTreeMCPCompat
{

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

/** True when a UCLASS declares a property of this name, at any point in its chain. */
static bool ClassHasProperty(const UClass* Class, const TCHAR* PropertyName)
{
	return Class != nullptr && Class->FindPropertyByName(FName(PropertyName)) != nullptr;
}

const FCapabilities& GetCapabilities()
{
	static FCapabilities Caps = []()
	{
		FCapabilities Out;
		Out.EngineVersion = FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);

		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("StateTree"));
		Out.bStateTreeEnabled = Plugin.IsValid() && Plugin->IsEnabled();

#if STATETREEMCP_HAS_STATETREE
		if (Out.bStateTreeEnabled)
		{
			Out.bCanCompile = (STATETREEMCP_HAS_EDITING_SUBSYSTEM != 0);

			// Ask the reflection system rather than the version number: a field can
			// appear in a hotfix, and a project can sit on a custom engine branch.
			const UClass* StateClass = UStateTreeState::StaticClass();
			Out.bHasConsiderations  = ClassHasProperty(StateClass, TEXT("Considerations"));
			Out.bHasTasksCompletion = ClassHasProperty(StateClass, TEXT("TasksCompletion"));
		}
#endif
		return Out;
	}();

	return Caps;
}

bool IsStateTreeAvailable(FString& OutReason)
{
#if !STATETREEMCP_HAS_STATETREE
	OutReason = FString::Printf(
		TEXT("This build of UE %d.%d does not ship the StateTree plugin source, so StateTree tools are unavailable."),
		ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	return false;
#else
	if (!GetCapabilities().bStateTreeEnabled)
	{
		OutReason = TEXT("The StateTree plugin is not enabled for this project. ")
			TEXT("Enable it in Edit > Plugins (search for StateTree), then restart the editor.");
		return false;
	}
	OutReason.Reset();
	return true;
#endif
}

// ---------------------------------------------------------------------------
// Reading the hierarchy
//
// UStateTreeEditorData::SubTrees and UStateTreeState::Children are declared as
// plain UPROPERTY(Instanced): public C++ members carrying neither Edit nor
// BlueprintVisible. PropertyAccessUtil::CanGetPropertyValue() requires one of
// those flags, so Python cannot read them at all - which is exactly why this
// plugin has to be C++ rather than an editor script.
// ---------------------------------------------------------------------------

TArray<UStateTreeState*> GetRootStates(UStateTreeEditorData* EditorData)
{
	TArray<UStateTreeState*> Out;
#if STATETREEMCP_HAS_STATETREE
	if (EditorData)
	{
		Out.Reserve(EditorData->SubTrees.Num());
		for (const TObjectPtr<UStateTreeState>& State : EditorData->SubTrees)
		{
			if (State)
			{
				Out.Add(State);
			}
		}
	}
#endif
	return Out;
}

TArray<UStateTreeState*> GetChildStates(UStateTreeState* State)
{
	TArray<UStateTreeState*> Out;
#if STATETREEMCP_HAS_STATETREE
	if (State)
	{
		Out.Reserve(State->Children.Num());
		for (const TObjectPtr<UStateTreeState>& Child : State->Children)
		{
			if (Child)
			{
				Out.Add(Child);
			}
		}
	}
#endif
	return Out;
}

void VisitAllStates(
	UStateTreeEditorData* EditorData,
	TFunctionRef<void(UStateTreeState& State, UStateTreeState* Parent, int32 Depth)> Visitor)
{
#if STATETREEMCP_HAS_STATETREE
	if (!EditorData)
	{
		return;
	}

	// Deliberately hand-rolled rather than using UStateTreeEditorData::VisitHierarchy():
	// that helper reports no depth, and its signature has shifted between releases.
	// Walking Children ourselves is stable across every version that has StateTree.
	TArray<TTuple<UStateTreeState*, UStateTreeState*, int32>> Stack;
	for (int32 Index = EditorData->SubTrees.Num() - 1; Index >= 0; --Index)
	{
		if (UStateTreeState* Root = EditorData->SubTrees[Index])
		{
			Stack.Emplace(Root, nullptr, 0);
		}
	}

	while (Stack.Num() > 0)
	{
		const TTuple<UStateTreeState*, UStateTreeState*, int32> Entry = Stack.Pop();
		UStateTreeState* State = Entry.Get<0>();

		Visitor(*State, Entry.Get<1>(), Entry.Get<2>());

		for (int32 Index = State->Children.Num() - 1; Index >= 0; --Index)
		{
			if (UStateTreeState* Child = State->Children[Index])
			{
				Stack.Emplace(Child, State, Entry.Get<2>() + 1);
			}
		}
	}
#endif
}

// ---------------------------------------------------------------------------
// Mutating the hierarchy
// ---------------------------------------------------------------------------

UStateTreeState* AddChildState(UStateTreeEditorData* EditorData, UStateTreeState* Parent, FName Name)
{
#if STATETREEMCP_HAS_STATETREE
	if (!EditorData)
	{
		return nullptr;
	}

	// FStateTreeViewModel::AddChildState() would also do this, but it works off the
	// editor's current selection and needs the asset window open. Creating the
	// object directly keeps the MCP usable while the asset is closed.
	UStateTreeState* NewState = NewObject<UStateTreeState>(
		EditorData, UStateTreeState::StaticClass(), NAME_None, RF_Transactional);
	if (!NewState)
	{
		return nullptr;
	}

	NewState->Name = Name;

	if (Parent)
	{
		Parent->Modify();
		NewState->Parent = Parent;
		Parent->Children.Add(NewState);
	}
	else
	{
		EditorData->Modify();
		NewState->Parent = nullptr;
		EditorData->SubTrees.Add(NewState);
	}

	return NewState;
#else
	return nullptr;
#endif
}

bool RemoveState(UStateTreeEditorData* EditorData, const FGuid& StateID)
{
#if STATETREEMCP_HAS_STATETREE
	if (!EditorData || !StateID.IsValid())
	{
		return false;
	}

	UStateTreeState* Target = nullptr;
	UStateTreeState* TargetParent = nullptr;
	VisitAllStates(EditorData, [&](UStateTreeState& State, UStateTreeState* Parent, int32)
	{
		if (!Target && State.ID == StateID)
		{
			Target = &State;
			TargetParent = Parent;
		}
	});

	if (!Target)
	{
		return false;
	}

	if (TargetParent)
	{
		TargetParent->Modify();
		TargetParent->Children.Remove(Target);
	}
	else
	{
		EditorData->Modify();
		EditorData->SubTrees.Remove(Target);
	}

	return true;
#else
	return false;
#endif
}

// ---------------------------------------------------------------------------
// Compiling
// ---------------------------------------------------------------------------

UStateTreeEditorData* GetEditorData(UStateTree* StateTree)
{
#if STATETREEMCP_HAS_STATETREE && WITH_EDITORONLY_DATA
	return StateTree ? Cast<UStateTreeEditorData>(StateTree->EditorData) : nullptr;
#else
	return nullptr;
#endif
}

void ValidateTree(UStateTree* StateTree)
{
#if STATETREEMCP_HAS_STATETREE && STATETREEMCP_HAS_EDITING_SUBSYSTEM
	if (StateTree)
	{
		UStateTreeEditingSubsystem::ValidateStateTree(StateTree);
	}
#endif
}

bool CompileTree(UStateTree* StateTree, FString& OutError)
{
	if (!StateTree)
	{
		OutError = TEXT("No StateTree asset was given.");
		return false;
	}

#if STATETREEMCP_HAS_STATETREE && STATETREEMCP_HAS_EDITING_SUBSYSTEM
	UStateTreeEditingSubsystem::ValidateStateTree(StateTree);

	FStateTreeCompilerLog Log;
	if (UStateTreeEditingSubsystem::CompileStateTree(StateTree, Log))
	{
		OutError.Reset();
		return true;
	}

	OutError = TEXT("Compilation failed. See the Message Log (StateTree) for details.");
	return false;
#else
	OutError = FString::Printf(
		TEXT("UE %d.%d does not expose UStateTreeEditingSubsystem, so this plugin cannot compile StateTree assets. ")
		TEXT("Open the asset in the editor and press Compile instead."),
		ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	return false;
#endif
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

bool SaveAsset(UObject* Asset, FString& OutError)
{
	UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
	if (!Package)
	{
		OutError = TEXT("No package to save.");
		return false;
	}

	const FString FileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;

	if (!UPackage::SavePackage(Package, Asset, *FileName, Args))
	{
		OutError = FString::Printf(
			TEXT("Failed to write '%s'. The file may be read-only or checked out by someone else."),
			*FileName);
		return false;
	}

	UE_LOG(LogStateTreeMCPCompat, Log, TEXT("Saved %s"), *Package->GetName());
	OutError.Reset();
	return true;
}

} // namespace StateTreeMCPCompat
