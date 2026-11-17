#pragma once
// SCENE_EXPORTER: the one deterministic rule that turns Daz runtime paths into
// Unreal package paths (spec decision 5).

#include "CoreMinimal.h"

class FDazScenePaths
{
public:
	/**
	 * Replaces every character outside [A-Za-z0-9_-] with '_', collapses runs of
	 * '_' and trims them from both ends. A segment with no usable characters
	 * (non-Latin letters, only symbols) becomes "n" + an 8-character hash of it,
	 * so two such names stay apart; an empty segment becomes "_".
	 */
	static FString SanitizeSegment(const FString& Segment);

	/**
	 * Replaces only characters that aren't valid in an asset name (anything outside
	 * [A-Za-z0-9_-]) with '_', without collapsing or trimming, so names that are
	 * already valid ("Material__63") stay exactly as they are
	 */
	static FString ValidAssetName(const FString& Name);

	/** The character rule alone; can return an empty string */
	static FString SanitizeChars(const FString& Segment);

	/**
	 * Library location for a file at RuntimePath ("Runtime/Textures/Vendor/x.jpg"):
	 * OutPackageFolder = Root + sanitized folders, OutAssetName = sanitized file
	 * name without extension. When the package path (folder/name, what becomes the
	 * .uasset file) would be longer than MaxObjectPathLength, folders are shortened (longest first, never below 8
	 * characters) and an 8-character hash of the original path is added to the name.
	 */
	static void MakeAssetPath(const FString& Root, const FString& RuntimePath, int32 MaxObjectPathLength,
		FString& OutPackageFolder, FString& OutAssetName);

	/**
	 * Root + sanitized RelativeFolder ("Vendor/Product/Label"), with the folders
	 * shortened like MakeAssetPath so that a name of NameBudget characters still
	 * fits in MaxObjectPathLength.
	 */
	static FString MakeFolder(const FString& Root, const FString& RelativeFolder, int32 NameBudget, int32 MaxObjectPathLength);

	/** "<Folder>/<Name>.<Name>" */
	static FString ObjectPath(const FString& PackageFolder, const FString& AssetName);

	/** First 8 hex characters of the MD5 of the lower-cased path */
	static FString ShortHash(const FString& Path);

	/** Lower case, forward slashes, no leading slash: the key the ledger uses for a runtime path */
	static FString NormalizeKey(const FString& Path);
};
