// Copyright (c) 2026. Licensed under the MIT License.

using System;
using System.IO;
using UnrealBuildTool;

public class StateTreeMCP : ModuleRules
{
	public StateTreeMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// ------------------------------------------------------------------
		// Engine version this module is being compiled against.
		// Every version-specific decision in this file goes through these two
		// numbers so the branching stays in one readable place.
		// ------------------------------------------------------------------
		int Major = Target.Version.MajorVersion;
		int Minor = Target.Version.MinorVersion;

		PublicDefinitions.Add(string.Format("STATETREEMCP_UE_MAJOR={0}", Major));
		PublicDefinitions.Add(string.Format("STATETREEMCP_UE_MINOR={0}", Minor));

		// UStateTreeEditingSubsystem (CompileStateTree / ValidateStateTree) was not
		// always available. Probe the header instead of hard-coding a version, so a
		// mid-version Epic change does not silently break the build.
		// Does this engine ship StateTree at all? Everything else hangs off this.
		bool bHasStateTree = HasStateTreeHeader(Target, "StateTreeEditorData.h");
		PublicDefinitions.Add(string.Format("STATETREEMCP_HAS_STATETREE={0}", bHasStateTree ? 1 : 0));

		bool bHasEditingSubsystem = HasStateTreeHeader(Target, "StateTreeEditingSubsystem.h");
		PublicDefinitions.Add(string.Format("STATETREEMCP_HAS_EDITING_SUBSYSTEM={0}", bHasEditingSubsystem ? 1 : 0));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",          // IPluginManager - runtime plugin detection
			"UnrealEd",          // editor lifecycle
			"EditorSubsystem",   // UEditorSubsystem
			"AssetRegistry",     // finding StateTree assets
			"AssetTools",        // creating StateTree assets
			"HTTPServer",        // the local bridge Claude talks to
			"Json",
			"JsonUtilities",
			"GameplayTags",
			"DeveloperSettings",
		});

		// StateTree itself is delay-loaded. If the user has the plugin disabled the
		// module still links and reports a clean error at runtime instead of
		// refusing to build or crashing on load.
		AddOptionalDynamicModule(Target, "StateTreeModule");
		AddOptionalDynamicModule(Target, "StateTreeEditorModule");
	}

	/// <summary>Returns true when a public header exists in the engine's StateTree plugin.</summary>
	private static bool HasStateTreeHeader(ReadOnlyTargetRules Target, string HeaderName)
	{
		string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
		string Candidate = Path.Combine(EngineDir, "Plugins", "Runtime", "StateTree",
			"Source", "StateTreeEditorModule", "Public", HeaderName);
		return File.Exists(Candidate);
	}

	/// <summary>Returns true when the engine ships a StateTree module source folder by this name.</summary>
	private static bool FindOptionalModule(ReadOnlyTargetRules Target, string ModuleName)
	{
		string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
		string Candidate = Path.Combine(EngineDir, "Plugins", "Runtime", "StateTree", "Source", ModuleName);
		return Directory.Exists(Candidate);
	}

	/// <summary>
	/// Links a module only when the engine actually ships it, and delay-loads its DLL
	/// on Windows so a disabled StateTree plugin degrades to a runtime message
	/// instead of a load-time failure.
	/// </summary>
	private void AddOptionalDynamicModule(ReadOnlyTargetRules Target, string ModuleName)
	{
		if (!FindOptionalModule(Target, ModuleName))
		{
			Console.WriteLine(string.Format(
				"StateTreeMCP: module '{0}' not found on UE {1}.{2} - StateTree tools will be disabled.",
				ModuleName, Target.Version.MajorVersion, Target.Version.MinorVersion));
			return;
		}

		PrivateDependencyModuleNames.Add(ModuleName);

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDelayLoadDLLs.Add(string.Format("UnrealEditor-{0}.dll", ModuleName));
		}

		Console.WriteLine(string.Format("StateTreeMCP: linked optional module '{0}' (delay-load).", ModuleName));
	}
}
