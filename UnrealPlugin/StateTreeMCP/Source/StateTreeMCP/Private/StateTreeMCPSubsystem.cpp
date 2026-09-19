// Copyright (c) 2026. Licensed under the MIT License.

#include "StateTreeMCPSubsystem.h"

#include "StateTreeMCPCompat.h"
#include "StateTreeMCPModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "JsonObjectConverter.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if STATETREEMCP_HAS_STATETREE
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#endif

namespace
{
	/** Loads a StateTree by content path, e.g. "/Game/AI/ST_Grunt". */
	UStateTree* LoadStateTree(const FString& AssetPath, FString& OutError)
	{
#if STATETREEMCP_HAS_STATETREE
		UStateTree* Tree = LoadObject<UStateTree>(nullptr, *AssetPath);
		if (!Tree)
		{
			OutError = FString::Printf(TEXT("No StateTree asset at '%s'."), *AssetPath);
		}
		return Tree;
#else
		OutError = TEXT("StateTree is not available in this engine build.");
		return nullptr;
#endif
	}

	/** Loads an asset and its editor data together, since nearly every handler needs both. */
	UStateTreeEditorData* LoadEditorData(const FString& AssetPath, UStateTree*& OutTree, FString& OutError)
	{
		OutTree = LoadStateTree(AssetPath, OutError);
		if (!OutTree)
		{
			return nullptr;
		}

		UStateTreeEditorData* EditorData = StateTreeMCPCompat::GetEditorData(OutTree);
		if (!EditorData)
		{
			OutError = FString::Printf(TEXT("'%s' has no editor data."), *AssetPath);
		}
		return EditorData;
	}

	/** Parses a state id, reporting the malformed and the missing differently. */
	bool ParseStateId(const FString& Text, FGuid& OutId, FString& OutError)
	{
		if (!FGuid::Parse(Text, OutId))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a state id. Call describe_tree to get one."), *Text);
			return false;
		}
		return true;
	}
}

void UStateTreeMCPSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Allow an override so several bridges can run side by side.
	FParse::Value(FCommandLine::Get(), TEXT("StateTreeMCPPort="), Port);

	RegisterHandlers();
	StartServer();
}

void UStateTreeMCPSubsystem::Deinitialize()
{
	StopServer();
	Super::Deinitialize();
}

void UStateTreeMCPSubsystem::StartServer()
{
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	Router = HttpServerModule.GetHttpRouter(Port);
	if (!Router.IsValid())
	{
		UE_LOG(LogStateTreeMCP, Error, TEXT("Could not bind the StateTree MCP bridge to port %d."), Port);
		return;
	}

	RouteHandle = Router->BindRoute(
		FHttpPath(TEXT("/rpc")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateUObject(this, &UStateTreeMCPSubsystem::HandleRpc));

	HttpServerModule.StartAllListeners();

	UE_LOG(LogStateTreeMCP, Log, TEXT("StateTree MCP bridge listening on http://127.0.0.1:%d/rpc"), Port);
}

void UStateTreeMCPSubsystem::StopServer()
{
	if (Router.IsValid() && RouteHandle.IsValid())
	{
		Router->UnbindRoute(RouteHandle);
	}
	Router.Reset();
	RouteHandle.Reset();
}

// ---------------------------------------------------------------------------
// Handlers - one entry per MCP tool
// ---------------------------------------------------------------------------

void UStateTreeMCPSubsystem::RegisterHandlers()
{
	// What this engine supports. Always answers, even when StateTree is missing,
	// so the assistant can explain the situation rather than failing opaquely.
	Handlers.Add(TEXT("capabilities"),
		[](const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>& OutResult, FString&)
		{
			const StateTreeMCPCompat::FCapabilities& Caps = StateTreeMCPCompat::GetCapabilities();
			OutResult = MakeShared<FJsonObject>();
			OutResult->SetStringField(TEXT("engineVersion"), Caps.EngineVersion);
			OutResult->SetBoolField(TEXT("stateTreeEnabled"), Caps.bStateTreeEnabled);
			OutResult->SetBoolField(TEXT("canCompile"), Caps.bCanCompile);
			OutResult->SetBoolField(TEXT("hasConsiderations"), Caps.bHasConsiderations);
			OutResult->SetBoolField(TEXT("hasTasksCompletion"), Caps.bHasTasksCompletion);
			return true;
		});

	// The whole hierarchy of one asset. This is the tool that only C++ can provide:
	// SubTrees and Children are invisible to Python.
	Handlers.Add(TEXT("describe_tree"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			const FString AssetPath = Params->GetStringField(TEXT("assetPath"));
			UStateTree* Tree = LoadStateTree(AssetPath, OutError);
			if (!Tree)
			{
				return false;
			}

			UStateTreeEditorData* EditorData = StateTreeMCPCompat::GetEditorData(Tree);
			if (!EditorData)
			{
				OutError = FString::Printf(TEXT("'%s' has no editor data."), *AssetPath);
				return false;
			}

			// Flat list with a depth column. Easier for a model to read than nested
			// JSON, and it round-trips back to a tree without ambiguity.
			TArray<TSharedPtr<FJsonValue>> States;
#if STATETREEMCP_HAS_STATETREE
			StateTreeMCPCompat::VisitAllStates(EditorData,
				[&States](UStateTreeState& State, UStateTreeState* Parent, int32 Depth)
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("id"), State.ID.ToString());
					Entry->SetStringField(TEXT("name"), State.Name.ToString());
					Entry->SetNumberField(TEXT("depth"), Depth);
					Entry->SetStringField(TEXT("parentId"), Parent ? Parent->ID.ToString() : FString());
					Entry->SetBoolField(TEXT("enabled"), State.bEnabled);

					// Name what is on the state, not just how much: after adding a
					// task the caller needs to see that the right one landed.
					auto NodeNames = [](const TArray<FStateTreeEditorNode>& Nodes)
					{
						TArray<TSharedPtr<FJsonValue>> Out;
						for (const FStateTreeEditorNode& Node : Nodes)
						{
							const UScriptStruct* Type = Node.Node.GetScriptStruct();
							TSharedRef<FJsonObject> N = MakeShared<FJsonObject>();
							N->SetStringField(TEXT("id"), Node.ID.ToString());
							N->SetStringField(TEXT("type"), Type ? Type->GetName() : TEXT("(empty)"));
							Out.Add(MakeShared<FJsonValueObject>(N));
						}
						return Out;
					};

					Entry->SetArrayField(TEXT("tasks"), NodeNames(State.Tasks));
					Entry->SetArrayField(TEXT("enterConditions"), NodeNames(State.EnterConditions));

					TArray<TSharedPtr<FJsonValue>> Transitions;
					for (const FStateTreeTransition& Transition : State.Transitions)
					{
						TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
						T->SetStringField(TEXT("id"), Transition.ID.ToString());
						T->SetStringField(TEXT("trigger"),
							StaticEnum<EStateTreeTransitionTrigger>()->GetNameStringByValue((int64)Transition.Trigger));
						T->SetStringField(TEXT("linkType"),
							StaticEnum<EStateTreeTransitionType>()->GetNameStringByValue((int64)Transition.State.LinkType));
						T->SetStringField(TEXT("targetStateId"), Transition.State.ID.ToString());
						T->SetStringField(TEXT("targetStateName"), Transition.State.Name.ToString());
						Transitions.Add(MakeShared<FJsonValueObject>(T));
					}
					Entry->SetArrayField(TEXT("transitions"), Transitions);
					States.Add(MakeShared<FJsonValueObject>(Entry));
				});
#endif

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetStringField(TEXT("assetPath"), AssetPath);
			OutResult->SetArrayField(TEXT("states"), States);
			return true;
		});

	Handlers.Add(TEXT("add_state"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			const FString AssetPath = Params->GetStringField(TEXT("assetPath"));
			UStateTree* Tree = LoadStateTree(AssetPath, OutError);
			if (!Tree)
			{
				return false;
			}

			UStateTreeEditorData* EditorData = StateTreeMCPCompat::GetEditorData(Tree);
			if (!EditorData)
			{
				OutError = FString::Printf(TEXT("'%s' has no editor data."), *AssetPath);
				return false;
			}

			const FString ParentIdText = Params->GetStringField(TEXT("parentId"));
			UStateTreeState* Parent = nullptr;
			if (!ParentIdText.IsEmpty())
			{
				FGuid ParentId;
				if (!ParseStateId(ParentIdText, ParentId, OutError))
				{
					return false;
				}

				Parent = StateTreeMCPCompat::FindState(EditorData, ParentId);
				if (!Parent)
				{
					OutError = FString::Printf(TEXT("No state with id '%s'."), *ParentIdText);
					return false;
				}
			}

			const FName NewName(*Params->GetStringField(TEXT("name")));
			UStateTreeState* NewState = StateTreeMCPCompat::AddChildState(EditorData, Parent, NewName);
			if (!NewState)
			{
				OutError = TEXT("Could not create the state.");
				return false;
			}

			Tree->MarkPackageDirty();

			OutResult = MakeShared<FJsonObject>();
#if STATETREEMCP_HAS_STATETREE
			OutResult->SetStringField(TEXT("id"), NewState->ID.ToString());
			OutResult->SetStringField(TEXT("name"), NewState->Name.ToString());
#endif
			return true;
		});

	Handlers.Add(TEXT("compile"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			const FString AssetPath = Params->GetStringField(TEXT("assetPath"));
			UStateTree* Tree = LoadStateTree(AssetPath, OutError);
			if (!Tree)
			{
				return false;
			}

			TArray<StateTreeMCPCompat::FCompileMessage> Messages;
			const bool bCompiled = StateTreeMCPCompat::CompileTree(Tree, Messages, OutError);

			// Build the message list either way: a failure needs it to be fixable,
			// and a success may still carry warnings worth seeing.
			TArray<TSharedPtr<FJsonValue>> MessagesJson;
			for (const StateTreeMCPCompat::FCompileMessage& Message : Messages)
			{
				TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
				M->SetStringField(TEXT("severity"), Message.Severity);
				M->SetStringField(TEXT("state"), Message.StateName);
				M->SetStringField(TEXT("node"), Message.NodeName);
				M->SetStringField(TEXT("message"), Message.Message);
				MessagesJson.Add(MakeShared<FJsonValueObject>(M));
			}

			if (!bCompiled)
			{
				// The envelope only carries a string on failure, so the detail would
				// be lost. Report it as a success whose result says it did not
				// compile, and hand back every message.
				OutResult = MakeShared<FJsonObject>();
				OutResult->SetBoolField(TEXT("compiled"), false);
				OutResult->SetBoolField(TEXT("saved"), false);
				OutResult->SetStringField(TEXT("error"), OutError);
				OutResult->SetArrayField(TEXT("messages"), MessagesJson);
				OutError.Reset();
				return true;
			}

			// Compiling means the edits are finished, so persist them here rather
			// than leaving a compiled-but-unsaved asset that a restart would lose.
			// Pass "save": false to opt out.
			bool bSave = true;
			Params->TryGetBoolField(TEXT("save"), bSave);

			FString SaveError;
			const bool bSaved = bSave && StateTreeMCPCompat::SaveAsset(Tree, SaveError);

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetBoolField(TEXT("compiled"), true);
			OutResult->SetBoolField(TEXT("saved"), bSaved);
			OutResult->SetArrayField(TEXT("messages"), MessagesJson);
			if (bSave && !bSaved)
			{
				OutResult->SetStringField(TEXT("saveError"), SaveError);
			}
			return true;
		});

	Handlers.Add(TEXT("save"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			const FString AssetPath = Params->GetStringField(TEXT("assetPath"));
			UStateTree* Tree = LoadStateTree(AssetPath, OutError);
			if (!Tree)
			{
				return false;
			}

			if (!StateTreeMCPCompat::SaveAsset(Tree, OutError))
			{
				return false;
			}

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetBoolField(TEXT("saved"), true);
			return true;
		});

	// Which schemas this project offers. A new tree cannot be made without one,
	// and projects add their own, so the answer has to come from the editor.
	Handlers.Add(TEXT("list_schemas"),
		[](const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Schemas;
			for (const UClass* Class : StateTreeMCPCompat::GetSchemaClasses())
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Class->GetName());
				Entry->SetStringField(TEXT("path"), Class->GetPathName());
				Entry->SetStringField(TEXT("description"), Class->GetToolTipText().ToString());
				Schemas.Add(MakeShared<FJsonValueObject>(Entry));
			}

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetArrayField(TEXT("schemas"), Schemas);
			return true;
		});

	Handlers.Add(TEXT("create"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			const FString PackagePath = Params->GetStringField(TEXT("packagePath"));
			const FString AssetName = Params->GetStringField(TEXT("name"));
			const FString SchemaName = Params->GetStringField(TEXT("schema"));

			if (PackagePath.IsEmpty() || AssetName.IsEmpty())
			{
				OutError = TEXT("Both packagePath (e.g. \"/Game/AI\") and name are required.");
				return false;
			}

			UClass* SchemaClass = StateTreeMCPCompat::FindSchemaClass(SchemaName);
			if (!SchemaClass)
			{
				OutError = FString::Printf(
					TEXT("Unknown schema '%s'. Call list_schemas to see what this project offers."),
					*SchemaName);
				return false;
			}

			UStateTree* NewTree = StateTreeMCPCompat::CreateStateTree(
				PackagePath, AssetName, SchemaClass, OutError);
			if (!NewTree)
			{
				return false;
			}

			// New assets exist only in memory until saved, and an unsaved brand new
			// asset is easier to lose than an edit to an existing one.
			FString SaveError;
			const bool bSaved = StateTreeMCPCompat::SaveAsset(NewTree, SaveError);

			OutResult = MakeShared<FJsonObject>();
			// The package name ("/Game/AI/ST_Grunt"), not GetPathName's object form
			// ("/Game/AI/ST_Grunt.ST_Grunt"): the caller feeds this straight back
			// into the other tools, so it should match what they take.
			OutResult->SetStringField(TEXT("assetPath"), NewTree->GetOutermost()->GetName());
			OutResult->SetStringField(TEXT("schema"), SchemaClass->GetName());
			OutResult->SetBoolField(TEXT("saved"), bSaved);
			if (!bSaved)
			{
				OutResult->SetStringField(TEXT("saveError"), SaveError);
			}
			return true;
		});

	Handlers.Add(TEXT("remove_state"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			UStateTree* Tree = nullptr;
			UStateTreeEditorData* EditorData =
				LoadEditorData(Params->GetStringField(TEXT("assetPath")), Tree, OutError);
			if (!EditorData)
			{
				return false;
			}

			const FString StateIdText = Params->GetStringField(TEXT("stateId"));
			FGuid StateId;
			if (!ParseStateId(StateIdText, StateId, OutError))
			{
				return false;
			}

			// Report how much is going away: removing a state takes its whole
			// subtree with it, which is easy to do by accident.
			int32 Removed = 0;
			if (UStateTreeState* Target = StateTreeMCPCompat::FindState(EditorData, StateId))
			{
				TArray<UStateTreeState*> Pending{ Target };
				while (Pending.Num() > 0)
				{
					UStateTreeState* Current = Pending.Pop();
					++Removed;
					Pending.Append(StateTreeMCPCompat::GetChildStates(Current));
				}
			}

			if (!StateTreeMCPCompat::RemoveState(EditorData, StateId))
			{
				OutError = FString::Printf(TEXT("No state with id '%s'."), *StateIdText);
				return false;
			}

			Tree->MarkPackageDirty();

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetNumberField(TEXT("removedCount"), Removed);
			return true;
		});

	// What can go in a state. The schema decides, and projects add their own
	// tasks, so this has to be asked of the asset rather than assumed.
	Handlers.Add(TEXT("list_node_types"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			UStateTree* Tree = nullptr;
			UStateTreeEditorData* EditorData =
				LoadEditorData(Params->GetStringField(TEXT("assetPath")), Tree, OutError);
			if (!EditorData)
			{
				return false;
			}

			const FString KindText = Params->GetStringField(TEXT("kind"));
			StateTreeMCPCompat::ENodeKind Kind;
			if (!StateTreeMCPCompat::ParseNodeKind(KindText, Kind))
			{
				OutError = FString::Printf(
					TEXT("'%s' is not a node kind. Use task, condition, evaluator or globalTask."),
					*KindText);
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Types;
			for (const UScriptStruct* Struct : StateTreeMCPCompat::GetNodeTypes(EditorData, Kind))
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Struct->GetName());
				Entry->SetStringField(TEXT("description"), Struct->GetToolTipText().ToString());

				// Listing each type's settable properties is the point of this tool:
				// without it the caller would have to guess what add_task accepts.
				TArray<TSharedPtr<FJsonValue>> Props;
				if (const UStruct* InstanceType = StateTreeMCPCompat::GetNodeInstanceType(Struct))
				{
					Entry->SetStringField(TEXT("instanceType"), InstanceType->GetName());
					for (TFieldIterator<const FProperty> It(InstanceType); It; ++It)
					{
						const FProperty* Prop = *It;
						if (!Prop->HasAnyPropertyFlags(CPF_Edit))
						{
							continue;
						}
						TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
						P->SetStringField(TEXT("name"), Prop->GetName());
						P->SetStringField(TEXT("type"), Prop->GetCPPType());
						P->SetStringField(TEXT("description"), Prop->GetToolTipText().ToString());
						Props.Add(MakeShared<FJsonValueObject>(P));
					}
				}
				Entry->SetArrayField(TEXT("properties"), Props);
				Types.Add(MakeShared<FJsonValueObject>(Entry));
			}

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetStringField(TEXT("kind"), KindText);
			OutResult->SetArrayField(TEXT("nodeTypes"), Types);
			return true;
		});

	Handlers.Add(TEXT("add_node"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			UStateTree* Tree = nullptr;
			UStateTreeEditorData* EditorData =
				LoadEditorData(Params->GetStringField(TEXT("assetPath")), Tree, OutError);
			if (!EditorData)
			{
				return false;
			}

			const FString KindText = Params->GetStringField(TEXT("kind"));
			StateTreeMCPCompat::ENodeKind Kind;
			if (!StateTreeMCPCompat::ParseNodeKind(KindText, Kind))
			{
				OutError = FString::Printf(
					TEXT("'%s' is not a node kind. Use task, condition, evaluator or globalTask."),
					*KindText);
				return false;
			}

			// Evaluators and global tasks live on the asset, everything else on a state.
			UStateTreeState* State = nullptr;
			const FString StateIdText = Params->GetStringField(TEXT("stateId"));
			if (!StateIdText.IsEmpty())
			{
				FGuid StateId;
				if (!ParseStateId(StateIdText, StateId, OutError))
				{
					return false;
				}
				State = StateTreeMCPCompat::FindState(EditorData, StateId);
				if (!State)
				{
					OutError = FString::Printf(TEXT("No state with id '%s'."), *StateIdText);
					return false;
				}
			}

			const FString TypeName = Params->GetStringField(TEXT("nodeType"));
			const UScriptStruct* NodeType = StateTreeMCPCompat::FindNodeType(EditorData, Kind, TypeName);
			if (!NodeType)
			{
				OutError = FString::Printf(
					TEXT("'%s' is not an available %s for this tree's schema. Call list_node_types."),
					*TypeName, *KindText);
				return false;
			}

			FGuid NodeID;
			const UStruct* InstanceType = nullptr;
			void* InstanceMemory = nullptr;
			if (!StateTreeMCPCompat::AddNode(
					EditorData, State, Kind, NodeType, NodeID, InstanceType, InstanceMemory, OutError))
			{
				return false;
			}

			// Settings arrive as plain JSON and are applied by reflection, so this
			// works for any task type without knowing its fields in advance.
			TArray<FString> Applied;
			if (Params->HasTypedField<EJson::Object>(TEXT("properties")) && InstanceType && InstanceMemory)
			{
				const TSharedPtr<FJsonObject> Props = Params->GetObjectField(TEXT("properties"));
				if (!FJsonObjectConverter::JsonObjectToUStruct(
						Props.ToSharedRef(), InstanceType, InstanceMemory, 0, 0))
				{
					OutError = FString::Printf(
						TEXT("The node was added, but some properties did not apply. ")
						TEXT("Check names and types against list_node_types (instance type '%s')."),
						*InstanceType->GetName());
					return false;
				}
				Props->Values.GetKeys(Applied);
			}

			Tree->MarkPackageDirty();

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetStringField(TEXT("nodeId"), NodeID.ToString());
			OutResult->SetStringField(TEXT("nodeType"), NodeType->GetName());
			TArray<TSharedPtr<FJsonValue>> AppliedJson;
			for (const FString& Name : Applied)
			{
				AppliedJson.Add(MakeShared<FJsonValueString>(Name));
			}
			OutResult->SetArrayField(TEXT("appliedProperties"), AppliedJson);
			return true;
		});

	Handlers.Add(TEXT("add_transition"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			UStateTree* Tree = nullptr;
			UStateTreeEditorData* EditorData =
				LoadEditorData(Params->GetStringField(TEXT("assetPath")), Tree, OutError);
			if (!EditorData)
			{
				return false;
			}

			FGuid StateId;
			const FString StateIdText = Params->GetStringField(TEXT("stateId"));
			if (!ParseStateId(StateIdText, StateId, OutError))
			{
				return false;
			}

			UStateTreeState* State = StateTreeMCPCompat::FindState(EditorData, StateId);
			if (!State)
			{
				OutError = FString::Printf(TEXT("No state with id '%s'."), *StateIdText);
				return false;
			}

			FGuid TargetId;
			const FString TargetIdText = Params->GetStringField(TEXT("targetStateId"));
			if (!TargetIdText.IsEmpty() && !ParseStateId(TargetIdText, TargetId, OutError))
			{
				return false;
			}

			FGuid TransitionID;
			if (!StateTreeMCPCompat::AddTransition(
					EditorData, State,
					Params->GetStringField(TEXT("trigger")),
					Params->GetStringField(TEXT("linkType")),
					TargetId,
					Params->GetStringField(TEXT("priority")),
					TransitionID, OutError))
			{
				return false;
			}

			Tree->MarkPackageDirty();

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetStringField(TEXT("transitionId"), TransitionID.ToString());
			return true;
		});

	Handlers.Add(TEXT("rename_state"),
		[](const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
		{
			if (!StateTreeMCPCompat::IsStateTreeAvailable(OutError))
			{
				return false;
			}

			UStateTree* Tree = nullptr;
			UStateTreeEditorData* EditorData =
				LoadEditorData(Params->GetStringField(TEXT("assetPath")), Tree, OutError);
			if (!EditorData)
			{
				return false;
			}

			const FString StateIdText = Params->GetStringField(TEXT("stateId"));
			FGuid StateId;
			if (!ParseStateId(StateIdText, StateId, OutError))
			{
				return false;
			}

			const FString NewName = Params->GetStringField(TEXT("name"));
			if (NewName.IsEmpty())
			{
				OutError = TEXT("A new name is required.");
				return false;
			}

			if (!StateTreeMCPCompat::RenameState(EditorData, StateId, FName(*NewName)))
			{
				OutError = FString::Printf(TEXT("No state with id '%s'."), *StateIdText);
				return false;
			}

			Tree->MarkPackageDirty();

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetStringField(TEXT("name"), NewName);
			return true;
		});
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

bool UStateTreeMCPSubsystem::HandleRpc(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Request.Body is raw bytes with no terminator, so it cannot be handed to
	// UTF8_TO_TCHAR directly: the conversion would read past the end until it
	// happened to meet a zero. Longer bodies survived that by luck; a short one
	// like {"action":"capabilities"} came out as garbage.
	TArray<uint8> BodyBytes(Request.Body);
	BodyBytes.Add(0);
	const FString Body = UTF8_TO_TCHAR(reinterpret_cast<const char*>(BodyBytes.GetData()));

	TSharedPtr<FJsonObject> Payload;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);

	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	FString Error;
	TSharedPtr<FJsonObject> Result;

	if (!FJsonSerializer::Deserialize(Reader, Payload) || !Payload.IsValid())
	{
		Error = TEXT("Request body was not valid JSON.");
	}
	else
	{
		const FString Action = Payload->GetStringField(TEXT("action"));
		const TSharedPtr<FJsonObject> ParamsField = Payload->HasField(TEXT("params"))
			? Payload->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();

		if (const FActionHandler* Handler = Handlers.Find(Action))
		{
			// The editor is not thread-safe; HTTP callbacks already arrive on the
			// game thread via the HttpServer tick, so no marshalling is needed here.
			if (!(*Handler)(ParamsField, Result, Error) && Error.IsEmpty())
			{
				Error = FString::Printf(TEXT("Action '%s' failed."), *Action);
			}
		}
		else
		{
			Error = FString::Printf(TEXT("Unknown action '%s'."), *Action);
		}
	}

	const bool bOk = Error.IsEmpty();
	Envelope->SetBoolField(TEXT("ok"), bOk);
	if (bOk)
	{
		Envelope->SetObjectField(TEXT("result"), Result.IsValid() ? Result : MakeShared<FJsonObject>());
	}
	else
	{
		Envelope->SetStringField(TEXT("error"), Error);
	}

	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);

	OnComplete(FHttpServerResponse::Create(Serialized, TEXT("application/json")));
	return true;
}
