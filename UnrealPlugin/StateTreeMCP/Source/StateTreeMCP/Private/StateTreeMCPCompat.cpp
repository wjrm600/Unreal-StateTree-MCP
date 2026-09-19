// Copyright (c) 2026. Licensed under the MIT License.

#include "StateTreeMCPCompat.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/UObjectHash.h"

#if STATETREEMCP_HAS_STATETREE
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeFactory.h"
#include "StateTreeNodeBase.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StateTreeTypes.h"
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

UStateTreeState* FindState(UStateTreeEditorData* EditorData, const FGuid& StateID)
{
#if STATETREEMCP_HAS_STATETREE
	if (!EditorData || !StateID.IsValid())
	{
		return nullptr;
	}

	UStateTreeState* Found = nullptr;
	VisitAllStates(EditorData, [&Found, &StateID](UStateTreeState& State, UStateTreeState*, int32)
	{
		if (!Found && State.ID == StateID)
		{
			Found = &State;
		}
	});
	return Found;
#else
	return nullptr;
#endif
}

bool RenameState(UStateTreeEditorData* EditorData, const FGuid& StateID, FName NewName)
{
#if STATETREEMCP_HAS_STATETREE
	if (UStateTreeState* State = FindState(EditorData, StateID))
	{
		State->Modify();
		State->Name = NewName;
		return true;
	}
#endif
	return false;
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
// Nodes: tasks, conditions, evaluators
// ---------------------------------------------------------------------------

bool ParseNodeKind(const FString& Text, ENodeKind& OutKind)
{
	if (Text.Equals(TEXT("task"), ESearchCase::IgnoreCase))            { OutKind = ENodeKind::Task;           return true; }
	if (Text.Equals(TEXT("condition"), ESearchCase::IgnoreCase))       { OutKind = ENodeKind::EnterCondition; return true; }
	if (Text.Equals(TEXT("enterCondition"), ESearchCase::IgnoreCase))  { OutKind = ENodeKind::EnterCondition; return true; }
	if (Text.Equals(TEXT("evaluator"), ESearchCase::IgnoreCase))       { OutKind = ENodeKind::Evaluator;      return true; }
	if (Text.Equals(TEXT("globalTask"), ESearchCase::IgnoreCase))      { OutKind = ENodeKind::GlobalTask;     return true; }
	return false;
}

#if STATETREEMCP_HAS_STATETREE

/** The base struct every node of this kind derives from. */
static const UScriptStruct* GetNodeBaseStruct(ENodeKind Kind)
{
	switch (Kind)
	{
	case ENodeKind::Task:
	case ENodeKind::GlobalTask:      return FStateTreeTaskBase::StaticStruct();
	case ENodeKind::EnterCondition:  return FStateTreeConditionBase::StaticStruct();
	case ENodeKind::Evaluator:       return FStateTreeEvaluatorBase::StaticStruct();
	}
	return nullptr;
}

#endif

TArray<const UScriptStruct*> GetNodeTypes(UStateTreeEditorData* EditorData, ENodeKind Kind)
{
	TArray<const UScriptStruct*> Out;
#if STATETREEMCP_HAS_STATETREE
	const UScriptStruct* Base = GetNodeBaseStruct(Kind);
	if (!Base || !EditorData)
	{
		return Out;
	}

	const UStateTreeSchema* Schema = EditorData->Schema;

	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (Struct == Base || !Struct->IsChildOf(Base))
		{
			continue;
		}

		// The schema is the authority on what this particular tree may use, so a
		// type it rejects is not offered even though it exists.
		if (Schema && !Schema->IsStructAllowed(Struct))
		{
			continue;
		}

		Out.Add(Struct);
	}

	Out.Sort([](const UScriptStruct& A, const UScriptStruct& B) { return A.GetName() < B.GetName(); });
#endif
	return Out;
}

const UScriptStruct* FindNodeType(UStateTreeEditorData* EditorData, ENodeKind Kind, const FString& TypeName)
{
	if (TypeName.IsEmpty())
	{
		return nullptr;
	}

	for (const UScriptStruct* Struct : GetNodeTypes(EditorData, Kind))
	{
		if (Struct->GetName() == TypeName
			|| FString::Printf(TEXT("F%s"), *Struct->GetName()) == TypeName
			|| Struct->GetPathName() == TypeName)
		{
			return Struct;
		}
	}
	return nullptr;
}

const UStruct* GetNodeInstanceType(const UScriptStruct* NodeType)
{
#if STATETREEMCP_HAS_STATETREE
	if (!NodeType || !NodeType->IsChildOf(FStateTreeNodeBase::StaticStruct()))
	{
		return nullptr;
	}

	// GetInstanceDataType is virtual, so it needs a real instance to ask. The
	// struct's default object serves, and costs nothing to keep around.
	FInstancedStruct Probe;
	Probe.InitializeAs(NodeType);
	return Probe.Get<FStateTreeNodeBase>().GetInstanceDataType();
#else
	return nullptr;
#endif
}

bool AddNode(
	UStateTreeEditorData* EditorData,
	UStateTreeState* State,
	ENodeKind Kind,
	const UScriptStruct* NodeType,
	FGuid& OutNodeID,
	const UStruct*& OutInstanceType,
	void*& OutInstanceMemory,
	FString& OutError)
{
#if STATETREEMCP_HAS_STATETREE
	if (!EditorData || !NodeType)
	{
		OutError = TEXT("Missing editor data or node type.");
		return false;
	}

	const bool bAssetLevel = (Kind == ENodeKind::Evaluator || Kind == ENodeKind::GlobalTask);
	if (!bAssetLevel && !State)
	{
		OutError = TEXT("This node kind belongs to a state, so a state id is required.");
		return false;
	}

	TArray<FStateTreeEditorNode>* List = nullptr;
	UObject* Outer = EditorData;
	switch (Kind)
	{
	case ENodeKind::Task:           List = &State->Tasks;                Outer = State; break;
	case ENodeKind::EnterCondition: List = &State->EnterConditions;      Outer = State; break;
	case ENodeKind::Evaluator:      List = &EditorData->Evaluators;      break;
	case ENodeKind::GlobalTask:     List = &EditorData->GlobalTasks;     break;
	}

	Outer->Modify();

	FStateTreeEditorNode& NewNode = List->AddDefaulted_GetRef();
	NewNode.ID = FGuid::NewGuid();
	NewNode.Node.InitializeAs(NodeType);

	// A node carries its settings either as a struct or as an instanced object,
	// depending on the type. Both paths mirror what the editor itself does.
	const FStateTreeNodeBase& NodeBase = NewNode.Node.Get<FStateTreeNodeBase>();
	const UStruct* InstanceType = NodeBase.GetInstanceDataType();

	OutInstanceType = InstanceType;
	OutInstanceMemory = nullptr;

	if (const UScriptStruct* InstanceStruct = Cast<const UScriptStruct>(InstanceType))
	{
		NewNode.Instance.InitializeAs(InstanceStruct);
		OutInstanceMemory = NewNode.Instance.GetMutableMemory();
	}
	else if (const UClass* InstanceClass = Cast<const UClass>(InstanceType))
	{
		NewNode.InstanceObject = NewObject<UObject>(Outer, InstanceClass);
		OutInstanceMemory = NewNode.InstanceObject;
	}

	if (const UScriptStruct* RuntimeStruct = Cast<const UScriptStruct>(NodeBase.GetExecutionRuntimeDataType()))
	{
		NewNode.ExecutionRuntimeData.InitializeAs(RuntimeStruct);
	}

	OutNodeID = NewNode.ID;
	OutError.Reset();
	return true;
#else
	OutError = TEXT("StateTree is not available in this engine build.");
	return false;
#endif
}

// ---------------------------------------------------------------------------
// Transitions
// ---------------------------------------------------------------------------

#if STATETREEMCP_HAS_STATETREE

/**
 * Resolves an enum entry by name through reflection rather than a hand-written
 * table, so entries added in a later engine version work without a code change.
 */
template <typename TEnum>
static bool ParseEnum(const FString& Name, TEnum& OutValue, FString& OutError, const TCHAR* Label)
{
	const UEnum* Enum = StaticEnum<TEnum>();
	const int64 Value = Enum->GetValueByNameString(Name);
	if (Value == INDEX_NONE)
	{
		TArray<FString> Valid;
		for (int32 i = 0; i < Enum->NumEnums() - 1; ++i)
		{
			if (!Enum->HasMetaData(TEXT("Hidden"), i))
			{
				Valid.Add(Enum->GetNameStringByIndex(i));
			}
		}
		OutError = FString::Printf(TEXT("'%s' is not a %s. Try one of: %s"),
			*Name, Label, *FString::Join(Valid, TEXT(", ")));
		return false;
	}

	OutValue = static_cast<TEnum>(Value);
	return true;
}

#endif

bool AddTransition(
	UStateTreeEditorData* EditorData,
	UStateTreeState* State,
	const FString& TriggerName,
	const FString& LinkTypeName,
	const FGuid& TargetStateID,
	const FString& PriorityName,
	FGuid& OutTransitionID,
	FString& OutError)
{
#if STATETREEMCP_HAS_STATETREE
	if (!State)
	{
		OutError = TEXT("A state is required.");
		return false;
	}

	EStateTreeTransitionTrigger Trigger = EStateTreeTransitionTrigger::OnStateCompleted;
	if (!TriggerName.IsEmpty() && !ParseEnum(TriggerName, Trigger, OutError, TEXT("transition trigger")))
	{
		return false;
	}

	EStateTreeTransitionType LinkType = EStateTreeTransitionType::GotoState;
	if (!LinkTypeName.IsEmpty() && !ParseEnum(LinkTypeName, LinkType, OutError, TEXT("transition type")))
	{
		return false;
	}

	EStateTreeTransitionPriority Priority = EStateTreeTransitionPriority::Normal;
	if (!PriorityName.IsEmpty() && !ParseEnum(PriorityName, Priority, OutError, TEXT("transition priority")))
	{
		return false;
	}

	UStateTreeState* Target = nullptr;
	if (LinkType == EStateTreeTransitionType::GotoState)
	{
		Target = FindState(EditorData, TargetStateID);
		if (!Target)
		{
			OutError = TEXT("A GotoState transition needs targetStateId to name an existing state.");
			return false;
		}
	}

	State->Modify();

	FStateTreeTransition& Transition = State->Transitions.AddDefaulted_GetRef();
	Transition.ID = FGuid::NewGuid();
	Transition.Trigger = Trigger;
	Transition.Priority = Priority;
	Transition.State.LinkType = LinkType;
	if (Target)
	{
		// The name is carried alongside the id purely so the editor can report a
		// broken link by name after the target is gone.
		Transition.State.ID = Target->ID;
		Transition.State.Name = Target->Name;
	}

	OutTransitionID = Transition.ID;
	OutError.Reset();
	return true;
#else
	OutError = TEXT("StateTree is not available in this engine build.");
	return false;
#endif
}

// ---------------------------------------------------------------------------
// Creating assets
// ---------------------------------------------------------------------------

TArray<UClass*> GetSchemaClasses()
{
	TArray<UClass*> Out;
#if STATETREEMCP_HAS_STATETREE
	TArray<UClass*> Derived;
	GetDerivedClasses(UStateTreeSchema::StaticClass(), Derived, /*bRecursive*/ true);

	for (UClass* Class : Derived)
	{
		// Abstract bases and the reinstanced leftovers of a hot reload are not
		// things anyone can pick, so keep them out of the list.
		if (Class && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			Out.Add(Class);
		}
	}

	Out.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
#endif
	return Out;
}

UClass* FindSchemaClass(const FString& SchemaName)
{
	if (SchemaName.IsEmpty())
	{
		return nullptr;
	}

	for (UClass* Class : GetSchemaClasses())
	{
		// Accept the name with or without the leading U, and the full path too,
		// because callers reasonably write it either way.
		if (Class->GetName() == SchemaName
			|| FString::Printf(TEXT("U%s"), *Class->GetName()) == SchemaName
			|| Class->GetPathName() == SchemaName)
		{
			return Class;
		}
	}
	return nullptr;
}

UStateTree* CreateStateTree(
	const FString& PackagePath, const FString& AssetName, UClass* SchemaClass, FString& OutError)
{
#if STATETREEMCP_HAS_STATETREE
	if (!SchemaClass)
	{
		OutError = TEXT("A schema class is required. Call list_schemas to see the options.");
		return nullptr;
	}

	UStateTreeFactory* Factory = NewObject<UStateTreeFactory>();
	Factory->SetSchemaClass(SchemaClass);

	// The factory opens the asset editor by default, which is unhelpful when the
	// caller is a script creating several trees in a row.
	Factory->bEditAfterNew = false;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* Created = AssetTools.CreateAsset(AssetName, PackagePath, UStateTree::StaticClass(), Factory);

	UStateTree* NewTree = Cast<UStateTree>(Created);
	if (!NewTree)
	{
		OutError = FString::Printf(
			TEXT("Could not create '%s/%s'. The name may already be taken, or the path may not exist."),
			*PackagePath, *AssetName);
		return nullptr;
	}

	UE_LOG(LogStateTreeMCPCompat, Log, TEXT("Created %s with schema %s"),
		*NewTree->GetPathName(), *SchemaClass->GetName());
	OutError.Reset();
	return NewTree;
#else
	OutError = TEXT("StateTree is not available in this engine build.");
	return nullptr;
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

bool CompileTree(UStateTree* StateTree, TArray<FCompileMessage>& OutMessages, FString& OutError)
{
	OutMessages.Reset();

	if (!StateTree)
	{
		OutError = TEXT("No StateTree asset was given.");
		return false;
	}

#if STATETREEMCP_HAS_STATETREE && STATETREEMCP_HAS_EDITING_SUBSYSTEM
	UStateTreeEditingSubsystem::ValidateStateTree(StateTree);

	FStateTreeCompilerLog Log;
	const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(StateTree, Log);

	for (const FStateTreeCompilerLogMessage& Entry : Log.Messages)
	{
		FCompileMessage& Out = OutMessages.AddDefaulted_GetRef();
		switch (Entry.Severity)
		{
		case EMessageSeverity::Error:          Out.Severity = TEXT("Error");   break;
		case EMessageSeverity::Warning:        Out.Severity = TEXT("Warning"); break;
		default:                               Out.Severity = TEXT("Info");    break;
		}
		Out.StateName = Entry.State ? Entry.State->Name.ToString() : FString();
		Out.NodeName = Entry.Item.Name.ToString();
		Out.Message = Entry.Message;
	}

	if (bCompiled)
	{
		OutError.Reset();
		return true;
	}

	// Lead with the first error rather than a generic failure: it is nearly
	// always the one that needs fixing, and the rest follow from it.
	const FCompileMessage* FirstError = OutMessages.FindByPredicate(
		[](const FCompileMessage& M) { return M.Severity == TEXT("Error"); });

	OutError = FirstError
		? FString::Printf(TEXT("Compilation failed: %s"), *FirstError->Message)
		: TEXT("Compilation failed, and the compiler gave no reason.");
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
