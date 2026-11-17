#include "DazToUnrealFbx.h"



void FDazToUnrealFbx::RenameDuplicateBones(FbxNode* RootNode)
{
	TMap<FString, int> ExistingBones;
	RenameDuplicateBones(RootNode, ExistingBones);
}

void FDazToUnrealFbx::RenameDuplicateBones(FbxNode* RootNode, TMap<FString, int>& ExistingBones)
{
	if (RootNode == nullptr) return;

	FbxNodeAttribute* Attr = RootNode->GetNodeAttribute();
	if (Attr && Attr->GetAttributeType() == FbxNodeAttribute::eSkeleton)
	{
		FString BoneName = UTF8_TO_TCHAR(RootNode->GetName());
		if (ExistingBones.Contains(BoneName))
		{
			ExistingBones[BoneName] += 1;
			BoneName = FString::Printf(TEXT("%s_RENAMED_%d"), *BoneName, ExistingBones[BoneName]);
			RootNode->SetName(TCHAR_TO_UTF8(*BoneName));
		}
		else
		{
			ExistingBones.Add(BoneName, 1);
		}
	}

	for (int ChildIndex = 0; ChildIndex < RootNode->GetChildCount(); ++ChildIndex)
	{
		FbxNode * ChildNode = RootNode->GetChild(ChildIndex);
		RenameDuplicateBones(ChildNode, ExistingBones);
	}
}

void FDazToUnrealFbx::FixClusterTranformLinks(FbxScene* Scene, FbxNode* RootNode)
{
	FbxGeometry* NodeGeometry = static_cast<FbxGeometry*>(RootNode->GetMesh());

	// Create missing weights
	if (NodeGeometry)
	{

		for (int DeformerIndex = 0; DeformerIndex < NodeGeometry->GetDeformerCount(); ++DeformerIndex)
		{
			FbxSkin* Skin = static_cast<FbxSkin*>(NodeGeometry->GetDeformer(DeformerIndex));
			if (Skin)
			{
				for (int ClusterIndex = 0; ClusterIndex < Skin->GetClusterCount(); ++ClusterIndex)
				{
					// Get the current tranform
					FbxAMatrix Matrix;
					FbxCluster* Cluster = Skin->GetCluster(ClusterIndex);
					Cluster->GetTransformLinkMatrix(Matrix);

					// Update the rotation
					FbxDouble3 Rotation = Cluster->GetLink()->PostRotation.Get();
					Matrix.SetR(Rotation);
					Cluster->SetTransformLinkMatrix(Matrix);
				}
			}
		}
	}

	for (int ChildIndex = 0; ChildIndex < RootNode->GetChildCount(); ++ChildIndex)
	{
		FbxNode* ChildNode = RootNode->GetChild(ChildIndex);
		FixClusterTranformLinks(Scene, ChildNode);
	}
}
void FDazToUnrealFbx::RemoveBindPoses(FbxScene* Scene)
{
	for (int32 PoseIndex = Scene->GetPoseCount() - 1; PoseIndex >= 0; --PoseIndex)
	{
		Scene->RemovePose(PoseIndex);
	}
}

void FDazToUnrealFbx::AddWeightsToAllNodes(FbxNode* Parent)
{
	//for (int ChildIndex = Parent->GetChildCount() - 1; ChildIndex >= 0; --ChildIndex)
	{
		FbxNode* ChildNode = Parent;//Parent->GetChild(ChildIndex);
		//RootBone->AddChild(ChildNode);
		FString ChildName = UTF8_TO_TCHAR(ChildNode->GetName());
		UE_LOG(LogTemp, Warning, TEXT("ChildNode %s Checking Weights"), *ChildName);

		if (FbxGeometry* NodeGeometry = static_cast<FbxGeometry*>(ChildNode->GetMesh()))
		{
			UE_LOG(LogTemp, Warning, TEXT("  No Deformers: %s"), *ChildName);
			if (NodeGeometry->GetDeformerCount() == 0)
			{
				FbxCluster* Cluster = FbxCluster::Create(Parent->GetScene(), "");
				Cluster->SetLink(Parent);
				Cluster->SetLinkMode(FbxCluster::eTotalOne);

				FbxSkin* Skin = FbxSkin::Create(Parent->GetScene(), "");
				Skin->AddCluster(Cluster);
				NodeGeometry->AddDeformer(Skin);

				for (int PolygonIndex = 0; PolygonIndex < ChildNode->GetMesh()->GetPolygonCount(); ++PolygonIndex)
				{
					for (int VertexIndex = 0; VertexIndex < ChildNode->GetMesh()->GetPolygonSize(PolygonIndex); ++VertexIndex)
					{
						int Vertex = ChildNode->GetMesh()->GetPolygonVertex(PolygonIndex, VertexIndex);
						Cluster->AddControlPointIndex(Vertex, 1.0f);
					}
				}
			}
		}

		//AddWeightsToAllNodes(ChildNode);
	}


}

FString FDazToUnrealFbx::GetObjectNameForMaterial(FbxSurfaceMaterial* Material)
{
	FbxScene* Scene = Material->GetScene();

	for (int32 MeshIndex = Scene->GetGeometryCount() - 1; MeshIndex >= 0; --MeshIndex)
	{
		FbxGeometry* Geometry = Scene->GetGeometry(MeshIndex);
		FbxNode* GeometryNode = Geometry->GetNode();
		int32 MaterialCount = GeometryNode->GetMaterialCount();
		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; MaterialIndex++)
		{
			FbxSurfaceMaterial* NodeMaterial = GeometryNode->GetMaterial(MaterialIndex);
			if (NodeMaterial == Material)
			{
				FString ObjectName = UTF8_TO_TCHAR(Geometry->GetName());
				return ObjectName;
			}
		}
	}

	return FString();
}

// Some accesories attached in ways like using the DzRigidFollowNode become additional meshes.
// This function attached them to the skeleton of the primary mesh so the don't break the skeleton.
bool FDazToUnrealFbx::ReadBoneParents(const FString& FbxFilePath, TMap<FString, FString>& OutBoneParents)
{
	OutBoneParents.Reset();
	FbxManager* SdkManager = FbxManager::Create();
	FbxIOSettings* IOSettings = FbxIOSettings::Create(SdkManager, IOSROOT);
	SdkManager->SetIOSettings(IOSettings);

	FbxImporter* Importer = FbxImporter::Create(SdkManager, "");
	bool bRead = Importer->Initialize(TCHAR_TO_UTF8(*FbxFilePath), -1, SdkManager->GetIOSettings());
	FbxScene* Scene = nullptr;
	if (bRead)
	{
		Scene = FbxScene::Create(SdkManager, "");
		bRead = Importer->Import(Scene);
	}
	Importer->Destroy();

	if (bRead)
	{
		CollectBoneParents(Scene->GetRootNode(), FString(), OutBoneParents);
	}
	SdkManager->Destroy();
	return bRead;
}

void FDazToUnrealFbx::CollectBoneParents(FbxNode* Node, const FString& ParentBone, TMap<FString, FString>& OutBoneParents)
{
	if (Node == nullptr) return;

	FString ParentForChildren = ParentBone;
	FbxNodeAttribute* Attr = Node->GetNodeAttribute();
	if (Attr && Attr->GetAttributeType() == FbxNodeAttribute::eSkeleton)
	{
		const FString BoneName = UTF8_TO_TCHAR(Node->GetName());
		OutBoneParents.Add(BoneName, ParentBone);
		ParentForChildren = BoneName;
	}
	for (int ChildIndex = 0; ChildIndex < Node->GetChildCount(); ++ChildIndex)
	{
		CollectBoneParents(Node->GetChild(ChildIndex), ParentForChildren, OutBoneParents);
	}
}

void FDazToUnrealFbx::ParentAdditionalSkeletalMeshes(FbxScene* Scene)
{
	FbxNode* RootNode = Scene->GetRootNode();

	// Find the root bone.  There should only be one bone off the scene root
	FbxNode* RootBone = nullptr;
	FString RootBoneName = TEXT("");
	for (int RootNodeIndex = Scene->GetNodeCount() -1; RootNodeIndex >= 0; --RootNodeIndex)
	{
		FbxNode* OtherRootNode = Scene->GetNode(RootNodeIndex);

		if (OtherRootNode != RootNode)
		{
			if (FbxSkeleton* OtherRootNodeSkeleton = OtherRootNode->GetSkeleton())
			{
				OtherRootNodeSkeleton->SetSkeletonType(FbxSkeleton::eLimbNode);
			}
			else if(OtherRootNode->GetMesh() == nullptr)
			{
				FbxSkeleton* SkeletonAttribute = FbxSkeleton::Create(Scene, OtherRootNode->GetName());
				SkeletonAttribute->SetSkeletonType(FbxSkeleton::eLimbNode);
				OtherRootNode->SetNodeAttribute(SkeletonAttribute);
			}
		}
	}
}


// Takes twist bones "out of line".  G3 and G8 have twist bones between some joints like thigh and knee.
void FDazToUnrealFbx::FixTwistBones(FbxNode* Node)
{
	if (Node == nullptr) return;

	// Process Children first since they'll get reparented
	for (int ChildIndex = Node->GetChildCount() - 1; ChildIndex >= 0; --ChildIndex)
	{
		FbxNode* ChildNode = Node->GetChild(ChildIndex);
		FixTwistBones(ChildNode);
	}

	FbxNodeAttribute* Attr = Node->GetNodeAttribute();
	if (Attr && Attr->GetAttributeType() == FbxNodeAttribute::eSkeleton)
	{
		FString BoneName = UTF8_TO_TCHAR(Node->GetName());
		if (BoneName.Contains(TEXT("twist")))
		{
			for (int ChildIndex = Node->GetChildCount() - 1; ChildIndex >= 0; --ChildIndex)
			{
				FbxNode* ChildNode = Node->GetChild(ChildIndex);
				if (Node->GetParent())
				{
					Node->RemoveChild(ChildNode);
					Node->GetParent()->AddChild(ChildNode);
				}
			}
		}
	}
}

// Removes a node and re-parents it's children to it's current parent
void FDazToUnrealFbx::RemoveNodeAndReparent(FbxNode* NodeToRemove)
{
	if (FbxNode* ParentNode = NodeToRemove->GetParent())
	{
		for (int ChildIndex = NodeToRemove->GetChildCount() - 1; ChildIndex >= 0; --ChildIndex)
		{
			FbxNode* ChildNode = NodeToRemove->GetChild(ChildIndex);
			NodeToRemove->RemoveChild(ChildNode);
			ParentNode->AddChild(ChildNode);
		}
		ParentNode->RemoveChild(NodeToRemove);
	}
}

void FDazToUnrealFbx::ConvertToEpicSkeleton(FbxScene* Scene)
{
	TArray<FString> NodesToRemove;
	TMap<FString, FString> NodesToAdd;
	TMap<FString, FString> NodesToRename;

	NodesToRemove.Add(TEXT("pelvis"));

	// Add this under that
	NodesToAdd.Add(TEXT("spine_04"), TEXT("spine3"));

	NodesToRename.Add(TEXT("hip"), TEXT("pelvis"));




}