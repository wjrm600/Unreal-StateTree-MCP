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
			"HTTPServer",   // types surface in StateTreeMCPSubsystem.h
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",          // IPluginManager - runtime plugin detection
			"UnrealEd",          // editor lifecycle
			"EditorSubsystem",   // UEditorSubsystem
			"AssetRegistry",     // finding StateTree assets
			"AssetTools",        // creating StateTree assets
			"Json",
			"JsonUtilities",
			"GameplayTags",
			"DeveloperSettings",
		});

		// StateTree itself is delay-loaded. If the user has the plugin disabled the
		// module still links and reports a clean error at runtime instead of
		// refusing to build or crashing on load.
		AddOptionalDynamicModule(Target, "StateTree", "StateTreeModule");
		AddOptionalDynamicModule(Target, "StateTree", "StateTreeEditorModule");

		// FStateTreeCompilerLog holds FStateTreeBindableStructDesc, which derives from
		// FPropertyBindingBindableStructDescriptor - and that base class exports its
		// virtual destructor and vtable from PropertyBindingUtils. Merely declaring a
		// compiler log on the stack therefore needs this module linked.
		//
		// Linked hard rather than delay-loaded on purpose: delay-load resolves function
		// imports, not the data symbols a vtable needs. StateTree's own .uplugin enables
		// PropertyBindingUtils, so whenever StateTree is available this one is too.
		AddOptionalModule(Target, "PropertyBindingUtils", "PropertyBindingUtils");
	}

	/// <summary>Returns true when a public header exists in the engine's StateTree plugin.</summary>
	private static bool HasStateTreeHeader(ReadOnlyTargetRules Target, string HeaderName)
	{
		string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
		string Candidate = Path.Combine(EngineDir, "Plugins", "Runtime", "StateTree",
			"Source", "StateTreeEditorModule", "Public", HeaderName);
		return File.Exists(Candidate);
	}

	/// <summary>Returns true when the engine ships this module inside the named runtime plugin.</summary>
	private static bool FindOptionalModule(ReadOnlyTargetRules Target, string PluginName, string ModuleName)
	{
		string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
		string Candidate = Path.Combine(EngineDir, "Plugins", "Runtime", PluginName, "Source", ModuleName);
		return Directory.Exists(Candidate);
	}

	private static void ReportMissing(ReadOnlyTargetRules Target, string ModuleName)
	{
		Console.WriteLine(string.Format(
			"StateTreeMCP: module '{0}' not found on UE {1}.{2} - StateTree tools will be disabled.",
			ModuleName, Target.Version.MajorVersion, Target.Version.MinorVersion));
	}

	/// <summary>
	/// Links a module only when the engine actually ships it, and delay-loads its DLL
	/// on Windows so a disabled StateTree plugin degrades to a runtime message
	/// instead of a load-time failure. Only safe for modules we call functions on.
	/// </summary>
	private void AddOptionalDynamicModule(ReadOnlyTargetRules Target, string PluginName, string ModuleName)
	{
		if (!FindOptionalModule(Target, PluginName, ModuleName))
		{
			ReportMissing(Target, ModuleName);
			return;
		}

		PrivateDependencyModuleNames.Add(ModuleName);

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDelayLoadDLLs.Add(string.Format("UnrealEditor-{0}.dll", ModuleName));
		}

		Console.WriteLine(string.Format("StateTreeMCP: linked optional module '{0}' (delay-load).", ModuleName));
	}

	/// <summary>
	/// Links a module when the engine ships it, without delay-load. Use for modules
	/// whose data symbols (vtables, exported constants) we depend on.
	/// </summary>
	private void AddOptionalModule(ReadOnlyTargetRules Target, string PluginName, string ModuleName)
	{
		if (!FindOptionalModule(Target, PluginName, ModuleName))
		{
			ReportMissing(Target, ModuleName);
			return;
		}

		PrivateDependencyModuleNames.Add(ModuleName);
		Console.WriteLine(string.Format("StateTreeMCP: linked optional module '{0}'.", ModuleName));
	}
}
