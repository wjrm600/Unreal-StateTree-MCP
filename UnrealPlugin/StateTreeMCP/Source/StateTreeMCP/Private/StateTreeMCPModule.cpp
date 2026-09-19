// Copyright (c) 2026. Licensed under the MIT License.

#include "StateTreeMCPModule.h"

#include "StateTreeMCPCompat.h"

#define LOCTEXT_NAMESPACE "FStateTreeMCPModule"

DEFINE_LOG_CATEGORY(LogStateTreeMCP);

void FStateTreeMCPModule::StartupModule()
{
	const StateTreeMCPCompat::FCapabilities& Caps = StateTreeMCPCompat::GetCapabilities();

	UE_LOG(LogStateTreeMCP, Log,
		TEXT("StateTree MCP starting on UE %s (StateTree=%s, compile=%s, considerations=%s, tasks-completion=%s)"),
		*Caps.EngineVersion,
		Caps.bStateTreeEnabled   ? TEXT("yes") : TEXT("no"),
		Caps.bCanCompile         ? TEXT("yes") : TEXT("no"),
		Caps.bHasConsiderations  ? TEXT("yes") : TEXT("no"),
		Caps.bHasTasksCompletion ? TEXT("yes") : TEXT("no"));

	FString Reason;
	if (!StateTreeMCPCompat::IsStateTreeAvailable(Reason))
	{
		// Load anyway: the bridge still answers statetree_capabilities, which is how
		// the assistant learns why the other tools are unavailable.
		UE_LOG(LogStateTreeMCP, Warning, TEXT("StateTree tools disabled - %s"), *Reason);
	}
}

void FStateTreeMCPModule::ShutdownModule()
{
	UE_LOG(LogStateTreeMCP, Log, TEXT("StateTree MCP shutting down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FStateTreeMCPModule, StateTreeMCP)
