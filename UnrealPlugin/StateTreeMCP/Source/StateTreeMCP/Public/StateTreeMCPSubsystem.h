// Copyright (c) 2026. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "StateTreeMCPSubsystem.generated.h"

class FJsonObject;
class IHttpRouter;
struct FHttpServerRequest;
class FHttpResultCallback;

/**
 * Hosts the local HTTP bridge that the Python MCP server talks to.
 *
 * One endpoint, POST /rpc, taking {"action": "...", "params": {...}} and
 * answering {"ok": bool, "result"|"error": ...}. Keeping it to a single
 * endpoint means adding a tool is a handler registration, not a routing change,
 * and it stays testable with a plain curl command.
 */
UCLASS()
class STATETREEMCP_API UStateTreeMCPSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	/** Signature every action handler implements. Returns false to signal an error. */
	using FActionHandler = TFunction<bool(const TSharedPtr<FJsonObject>& Params,
	                                      TSharedPtr<FJsonObject>& OutResult,
	                                      FString& OutError)>;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Port the bridge listens on. Configurable so it can coexist with other MCP bridges. */
	int32 GetPort() const { return Port; }

private:
	void RegisterHandlers();
	void StartServer();
	void StopServer();

	bool HandleRpc(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	TMap<FString, FActionHandler> Handlers;
	TSharedPtr<IHttpRouter> Router;
	FDelegateHandle RouteHandle;

	/** 8092 by default: 8091 is commonly taken by other Unreal MCP bridges. */
	int32 Port = 8092;
};
