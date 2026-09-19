// Copyright (c) 2026. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

STATETREEMCP_API DECLARE_LOG_CATEGORY_EXTERN(LogStateTreeMCP, Log, All);

class FStateTreeMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
