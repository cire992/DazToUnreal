#include "DazToUnrealTextureLibrary.h"
#include "DazToUnreal.h"
#include "SceneExporter/DazScenePaths.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace DazToUnrealTextureLibraryPrivate
{
	// Same budget as the scene exporter's default MaxObjectPathLength
	const int32 LibraryMaxPathLength = 150;

	FString CleanFile(const FString& File)
	{
		FString Full = FPaths::ConvertRelativePathToFull(File);
		FPaths::NormalizeFilename(Full);
		return Full;
	}

	/** What a file is keyed on: its runtime path, or its full path outside Daz content */
	FString KeyOf(const FString& SourceFile)
	{
		const FString RuntimePath = FDazToUnrealTextureLibrary::RuntimePathOf(SourceFile);
		return FDazScenePaths::NormalizeKey(RuntimePath.IsEmpty() ? CleanFile(SourceFile) : RuntimePath);
	}

	/** The source file a library texture was imported from, or empty */
	FString ImportedFrom(const UTexture* Texture)
	{
		return Texture && Texture->AssetImportData ? Texture->AssetImportData->GetFirstFilename() : FString();
	}

	/** The asset at Folder/Name, in memory or on disk */
	UObject* FindAsset(const FString& PackageFolder, const FString& AssetName)
	{
		const FString ObjectPath = FDazScenePaths::ObjectPath(PackageFolder, AssetName);
		if (UObject* Object = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Object;
		}
		if (FPackageName::DoesPackageExist(PackageFolder / AssetName))
		{
			return LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		}
		return nullptr;
	}

	/** True when Texture was imported from SourceFile and the file hasn't changed since */
	bool IsUnchanged(const UTexture* Texture, const FString& SourceFile)
	{
		const FString Imported = ImportedFrom(Texture);
		if (Imported.IsEmpty() || !CleanFile(Imported).Equals(SourceFile, ESearchCase::IgnoreCase))
		{
			return false;
		}
		const FAssetImportInfo& Info = Texture->AssetImportData->GetSourceData();
		if (Info.SourceFiles.Num() == 0)
		{
			return false;
		}
		const FAssetImportInfo::FSourceFile& File = Info.SourceFiles[0];
		if (File.FileHash.IsValid())
		{
			return File.FileHash == FMD5Hash::HashFile(*SourceFile);
		}
		// No hash recorded: fall back to the modified time
		return File.Timestamp == IFileManager::Get().GetTimeStamp(*SourceFile);
	}

	/**
	 * Library textures waiting for sRGB off. Kept apart from the bridge's own list, which
	 * ImportTextureAssets empties, so a later import in the same tick can't drop them.
	 */
	TArray<TWeakObjectPtr<UTexture>> PendingSRGBOff;

	void ApplySRGBOff(UTexture* Texture)
	{
		Texture->PreEditChange(nullptr);
		Texture->SRGB = false;
		Texture->PostEditChange();
		Texture->MarkPackageDirty();
	}

	void DisableSRGB(UTexture* Texture)
	{
		if (IsRunningCommandlet())
		{
			ApplySRGBOff(Texture);
		}
		else
		{
			// Deferred one tick like the bridge's own imports (see FDazToUnrealModule::Tick)
			PendingSRGBOff.AddUnique(Texture);
		}
	}
}

void FDazToUnrealTextureLibrary::FlushDeferredSRGB()
{
	using namespace DazToUnrealTextureLibraryPrivate;
	for (const TWeakObjectPtr<UTexture>& Texture : PendingSRGBOff)
	{
		if (Texture.IsValid() && Texture->SRGB)
		{
			ApplySRGBOff(Texture.Get());
		}
	}
	PendingSRGBOff.Empty();
}

FString FDazToUnrealTextureLibrary::GetRoot(const TSharedPtr<FJsonObject>& JsonObject)
{
	FString Root;
	if (!JsonObject.IsValid() || !JsonObject->TryGetStringField(TEXT("Texture Library"), Root))
	{
		return FString();
	}
	Root = Root.TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
	while (Root.EndsWith(TEXT("/")))
	{
		Root.LeftChopInline(1);
	}
	if (Root.IsEmpty())
	{
		return FString();
	}

	FString Filename;
	if (!Root.StartsWith(TEXT("/")) || !FPackageName::TryConvertLongPackageNameToFilename(Root / TEXT("Probe"), Filename))
	{
		UE_LOG(LogDazToUnreal, Warning, TEXT("DazToUnreal: Texture library: \"%s\" isn't a mounted content path; importing textures the old way."), *Root);
		return FString();
	}
	UE_LOG(LogDazToUnreal, Log, TEXT("DazToUnreal: Texture library: %s"), *Root);
	return Root;
}

FString FDazToUnrealTextureLibrary::RuntimePathOf(const FString& SourceFile)
{
	const FString Path = SourceFile.Replace(TEXT("\\"), TEXT("/"));
	const FString Lower = Path.ToLower();
	const FString Marker = TEXT("/runtime/");
	int32 Index = Lower.Find(TEXT("/runtime/textures/"));
	if (Index == INDEX_NONE)
	{
		Index = Lower.Find(Marker);
	}
	if (Index == INDEX_NONE)
	{
		return FString();
	}
	const FString Rest = Path.Mid(Index + Marker.Len());
	return Rest.IsEmpty() ? FString() : TEXT("Runtime/") + Rest;
}

void FDazToUnrealTextureLibrary::MakeLibraryPath(const FString& Root, const FString& SourceFile, FString& OutPackageFolder, FString& OutAssetName)
{
	using namespace DazToUnrealTextureLibraryPrivate;
	const FString RuntimePath = RuntimePathOf(SourceFile);
	if (!RuntimePath.IsEmpty())
	{
		FDazScenePaths::MakeAssetPath(Root, RuntimePath, LibraryMaxPathLength, OutPackageFolder, OutAssetName);
	}
	else
	{
		const FString External = TEXT("External") / FDazScenePaths::ShortHash(CleanFile(SourceFile)) / FPaths::GetCleanFilename(SourceFile);
		FDazScenePaths::MakeAssetPath(Root, External, LibraryMaxPathLength, OutPackageFolder, OutAssetName);
	}
}

TMap<FString, FString> FDazToUnrealTextureLibrary::Sync(const FString& Root, const TArray<FDazLibraryTextureRequest>& Requests)
{
	using namespace DazToUnrealTextureLibraryPrivate;
	TMap<FString, FString> Result;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Object paths given out in this import -> key, so two files never share one asset
	TMap<FString, FString> Claims;

	for (const FDazLibraryTextureRequest& Request : Requests)
	{
		const FString Source = CleanFile(Request.SourceFile);
		if (!FPaths::FileExists(Source))
		{
			UE_LOG(LogDazToUnreal, Warning, TEXT("DazToUnreal: Texture library: %s is missing; importing it the old way."), *Request.SourceFile);
			continue;
		}
		const FString Key = KeyOf(Source);

		// The path rule, then a name that no other file holds (foo.jpg and foo.png in one folder)
		FString PackageFolder;
		FString BaseName;
		MakeLibraryPath(Root, Source, PackageFolder, BaseName);
		const FString Extension = FDazScenePaths::SanitizeSegment(FPaths::GetExtension(Source));
		const FString Candidates[] = { BaseName, BaseName + TEXT("_") + Extension, BaseName + TEXT("_") + Extension + TEXT("_") + FDazScenePaths::ShortHash(Key) };

		FString AssetName;
		UTexture* Existing = nullptr;
		for (const FString& Candidate : Candidates)
		{
			const FString* Claim = Claims.Find(FDazScenePaths::ObjectPath(PackageFolder, Candidate).ToLower());
			if (Claim && *Claim != Key)
			{
				continue;
			}
			UObject* Object = FindAsset(PackageFolder, Candidate);
			UTexture* Texture = Cast<UTexture>(Object);
			if (Object && (!Texture || KeyOf(ImportedFrom(Texture)) != Key))
			{
				continue;
			}
			AssetName = Candidate;
			Existing = Texture;
			break;
		}
		if (AssetName.IsEmpty())
		{
			UE_LOG(LogDazToUnreal, Warning, TEXT("DazToUnreal: Texture library: no free name for %s in %s; importing it the old way."), *Source, *PackageFolder);
			continue;
		}
		const FString ObjectPath = FDazScenePaths::ObjectPath(PackageFolder, AssetName);
		Claims.Add(ObjectPath.ToLower(), Key);

		// Reuse when unchanged, else import (or replace in place) straight from the Daz file
		UTexture* Texture = nullptr;
		if (Existing && IsUnchanged(Existing, Source))
		{
			Texture = Existing;
			UE_LOG(LogDazToUnreal, Log, TEXT("DazToUnreal: Texture library: reused %s (%s)"), *ObjectPath, *Source);
		}
		else
		{
			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = Source;
			Task->DestinationPath = PackageFolder;
			Task->DestinationName = AssetName;
			Task->bReplaceExisting = true;
			Task->bReplaceExistingSettings = false;
			Task->bAutomated = true;
			Task->bSave = false;
			AssetTools.ImportAssetTasks({ Task });
			for (UObject* Object : Task->GetObjects())
			{
				Texture = Cast<UTexture>(Object);
				if (Texture)
				{
					break;
				}
			}
			if (Texture == nullptr)
			{
				UE_LOG(LogDazToUnreal, Warning, TEXT("DazToUnreal: Texture library: import failed for %s -> %s; importing it the old way."), *Source, *ObjectPath);
				continue;
			}
			UE_LOG(LogDazToUnreal, Log, TEXT("DazToUnreal: Texture library: %s %s (%s)"), Existing ? TEXT("updated") : TEXT("imported"), *ObjectPath, *Source);
		}

		// sRGB: cutout maps are linear, as the bridge has always made them. One library
		// texture has one setting, so the cutout wins wherever the uses disagree.
		if (Request.bCutout && Request.bColour)
		{
			UE_LOG(LogDazToUnreal, Warning, TEXT("DazToUnreal: Texture library: sRGB conflict: %s is both a colour map and a cutout in this import; the cutout wins (sRGB off)."), *ObjectPath);
		}
		// A texture already queued for sRGB off (an earlier import this tick) counts as linear
		const bool bPendingOff = PendingSRGBOff.Contains(Texture);
		const bool bLinear = !Texture->SRGB || bPendingOff;
		if (Request.bCutout && !bLinear)
		{
			if (Texture == Existing)
			{
				UE_LOG(LogDazToUnreal, Warning, TEXT("DazToUnreal: Texture library: sRGB change: %s was sRGB and this import uses it as a cutout; turning sRGB off for every material that uses it."), *ObjectPath);
			}
			DisableSRGB(Texture);
		}
		else if (Request.bColour && !Request.bCutout && bLinear && Texture->CompressionSettings != TC_Normalmap)
		{
			UE_LOG(LogDazToUnreal, Warning, TEXT("DazToUnreal: Texture library: sRGB conflict: %s is linear (a cutout elsewhere) and this import uses it as colour; leaving it linear (the cutout wins)."), *ObjectPath);
		}

		Result.Add(Request.SourceFile, ObjectPath);
	}
	return Result;
}
