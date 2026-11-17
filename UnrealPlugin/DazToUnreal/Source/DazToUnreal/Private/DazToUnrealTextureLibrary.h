#pragma once
// A shared texture library, switched on per export by the .dtu field "Texture Library".
// Each Daz texture file is imported once, at <Root>/Runtime/... keyed by its Daz
// runtime path, and reused by later imports while its source file and hash are unchanged.

#include "CoreMinimal.h"

class FJsonObject;
class UObject;

/** A texture file one import needs, and how its materials use it */
struct FDazLibraryTextureRequest
{
	/** Absolute path as written in the .dtu */
	FString SourceFile;
	/** Attached to a colour property (wants sRGB) */
	bool bColour = false;
	/** Attached to Cutout Opacity (wants sRGB off, as the bridge has always done) */
	bool bCutout = false;
};

class FDazToUnrealTextureLibrary
{
public:
	/**
	 * The library root ("/DazAssets/Textures") from the .dtu's "Texture Library" field.
	 * Empty when the field is absent, or when the value isn't a mounted content path
	 * (logged), so the caller imports textures the old way.
	 */
	static FString GetRoot(const TSharedPtr<FJsonObject>& JsonObject);

	/**
	 * "Runtime/Textures/Vendor/x.jpg" for a file inside a Daz content directory
	 * (the path from its Runtime folder), else empty
	 */
	static FString RuntimePathOf(const FString& SourceFile);

	/**
	 * Where a file lives in the library before any name clash is resolved:
	 * <Root>/Runtime/<rest of the runtime path>/<file stem>, or
	 * <Root>/External/<hash of the full path>/<file stem> for files outside Daz content
	 */
	static void MakeLibraryPath(const FString& Root, const FString& SourceFile, FString& OutPackageFolder, FString& OutAssetName);

	/**
	 * Imports each requested file into the library, or reuses the library texture when
	 * its import source and hash match. Returns SourceFile -> texture object path for every
	 * file that has a library texture; files missing from the map failed and should be
	 * imported the old way. Textures that need sRGB turned off are added to OutDisableSRGB
	 * (the bridge's deferred list) unless running as a commandlet, where it's done at once.
	 */
	static TMap<FString, FString> Sync(const FString& Root, const TArray<FDazLibraryTextureRequest>& Requests, TArray<UObject*>& OutDisableSRGB);
};
