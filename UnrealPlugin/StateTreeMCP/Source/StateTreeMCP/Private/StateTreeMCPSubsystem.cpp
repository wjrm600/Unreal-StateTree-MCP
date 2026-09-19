// Copyright (c) 2026. Licensed under the MIT License.

#include "StateTreeMCPSubsystem.h"

#include "StateTreeMCPCompat.h"
#include "StateTreeMCPModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
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
#include "StateTreeState.h"
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
					Entry->SetNumberField(TEXT("taskCount"), State.Tasks.Num());
					Entry->SetNumberField(TEXT("transitionCount"), State.Transitions.Num());
					Entry->SetNumberField(TEXT("enterConditionCount"), State.EnterConditions.Num());
					Entry->SetBoolField(TEXT("enabled"), State.bEnabled);
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
#if STATETREEMCP_HAS_STATETREE
			FGuid ParentId;
			if (!ParentIdText.IsEmpty() && FGuid::Parse(ParentIdText, ParentId))
			{
				StateTreeMCPCompat::VisitAllStates(EditorData,
					[&Parent, &ParentId](UStateTreeState& State, UStateTreeState*, int32)
					{
						if (!Parent && State.ID == ParentId)
						{
							Parent = &State;
						}
					});

				if (!Parent)
				{
					OutError = FString::Printf(TEXT("No state with id '%s'."), *ParentIdText);
					return false;
				}
			}
#endif

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

			if (!StateTreeMCPCompat::CompileTree(Tree, OutError))
			{
				return false;
			}

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetBoolField(TEXT("compiled"), true);
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
