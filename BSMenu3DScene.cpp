//----------------------------------------------------------------------
// ZENIMAX MEDIA PROPRIETARY INFORMATION
//
// This software is developed and/or supplied under the terms of a license
// or non-disclosure agreement with ZeniMax Media Inc. and may not be copied
// or disclosed except in accordance with the terms of that agreement.
//
//   Copyright (c) 2020 ZeniMax Media Incorporated.
//   All Rights Reserved.
//
// ZeniMax Media Incorporated, Rockville, Maryland 20850
// http://www.zenimax.com
//
// FILE	  BSMenu3DScene.cpp
// Owner  Justin McLaine
// DATE	  03/24/2020
//----------------------------------------------------------------------

#include "BSMenuPCH.h"
#include "BSMenu3DScene.h"
#include "UI.h"

#include <BSMain/BSFadeNode.h>
#include <BSMain/BSNodeVisitation.h>
#include <BSMain/BSSceneNode.h>
#include <BSMain/ImageSpaceData.h>
#include <BSMain/StorageTypes/AppSTScene.h>
#include <BSMain/Render/BSRenderSettingsManager.h>
#include <BSSystem/BSFixedString.h>
#include <BSSystem/XINISetting.h>
#include <CreationRenderer/CreationRendererInit.h>
#include <CreationRenderer/CreationRendererRenderGraph.h>
#include <CreationRenderer/CreationRendererTexture.h>
#include <CreationRenderer/StorageTable.h>
#include <CreationRenderer/StorageTypes/STCameraView.h>
#include <CreationRenderer/StorageTypes/STImageSpace.h>
#include <CreationRenderer/StorageTypes/STScaleform.h>
#include <CreationRenderer/StorageTypes/STMaterialInstance.h>
#include <NiMain/BSGeometry.h>
#include <NiMain/NiCamera.h>
#include <NiMain/NiPick.h>

extern INISetting fDefaultFOV;
extern INISetting bTemporalAA;

INISetting fUICameraNearDistance("fUICameraNearDistance:Interface", 0.214f); // meters
INISetting fUICameraFarDistance("fUICameraFarDistance:Interface", 214.3125f); // meters

namespace BSMenu3D
{
#pragma region Scene

	Scene::Scene(const BSFixedString& aName, StorageTable::ImageSpaceDataKey aImageSpaceData, const float aFOV)
		: Name(aName)
	{
		SetMemContext(MC_UI_SYSTEM);

		spScene = new BSSceneNode(aName);

		spObjects = new NiNode();
		spScene->AttachChild(spObjects);

		SetupCamera(spScene, aImageSpaceData, aFOV);

		Update();	

		SceneManager::QInstance().Register(this);
	}

	Scene::Scene(const BSFixedString& aName, const BSSceneNodePtr& aspSceneOverride, StorageTable::ImageSpaceDataKey aImageSpaceData, const float aFOV)
		: Name(aName)
	{
		SetMemContext(MC_UI_SYSTEM);

		BSASSERTFAST(aspSceneOverride);

		SetupCamera(aspSceneOverride, aImageSpaceData, aFOV);

		Update();

		SceneManager::QInstance().Register(this);
	}

	Scene::~Scene()
	{
		SceneManager::QInstance().Unregister(this);
	}

	void Scene::SetupCamera(const BSSceneNodePtr& aspScene, StorageTable::ImageSpaceDataKey aImageSpaceData, float aFOV)
	{
		spCamera = new NiCamera();
		spCamera->SetName(Name);
		spCamera->RegisterAsRenderCamera();

		NiFrustum fr;
		fr.m_fFar = fUICameraFarDistance.Float();
		fr.m_fNear = fUICameraNearDistance.Float();
		spCamera->SetMaxFarNearRatio(fr.m_fFar / fr.m_fNear);

		const auto& targetSize = QRenderTargetSize();
		const float fscreenAspect = targetSize.x / targetSize.y;
		const float fov = (aFOV > 0.f) ? aFOV : fDefaultFOV.Float();
		const float theta = fov * DEG_TO_RAD * FOVScaleC;
		fr.m_fLeft = -tan(theta) * fscreenAspect;
		fr.m_fRight = tan(theta) * fscreenAspect;
		fr.m_fBottom = -tan(theta);
		fr.m_fTop = tan(theta);

		spCamera->SetViewFrustum(fr);
		spCamera->SetClipspaceType(NiFrustumType::Perspective);

		spCamera->SetTranslate(NiPoint3::ZERO);
		spCamera->SetRotate({ NiPoint3::UNIT_Y, NiPoint3::UNIT_Z, NiPoint3::UNIT_X });
		spCamera->SetMinNearPlaneDist(1.0f);

		// Specular was being disabled by the LOD
		spCamera->SetLODAdjust(0.0f);

		CameraViewHandle.Register();
		CameraViewHandle->WriteDiscard<StorageTable::CameraViewData>({
			aspScene->QRendererHandle().QKey(),
			spCamera->QRendererHandle().QKey(),
			true
#if defined(BETAVERSION)
			, Name
#endif // BETAVERSION
			});

		if (aImageSpaceData.IsValidForWriter())
		{
			CameraViewHandle->WriteDiscard(std::move(aImageSpaceData));
		}
		else
		{
			ImageSpaceHandle.Register();
			CameraViewHandle->WriteDiscard(ImageSpaceHandle->QStrongKey());
		}

		CreationRenderer::FeatureSetup featureSetup = BSRenderSettingsManager::QInstance().QRendererState().QFeatures();
		featureSetup.SetEnabled(CreationRenderer::Feature::TemporalAA, bTemporalAA);
		CameraViewHandle->WriteDiscard<CreationRenderer::FeatureSetup>(std::move(featureSetup));
	}

	NiPoint2 Scene::QRenderTargetSize() const
	{
		const auto& windowSettings = gBSRenderSettingsManager.QWindowSettings();

		return NiPoint2 { 
			static_cast<float>(windowSettings.BackbufferWidth),
			static_cast<float>(windowSettings.BackbufferHeight)
		};
	}

	/// <summary> Attach an object to the scene </summary>
	/// <param name="apObj"> Object to attach </param>
	void Scene::AttachObject(NiAVObject* apObj)
	{
		BSASSERTFAST(spObjects);

		BSVisit::TraverseScenegraphNodes(apObj, [&](NiNode &arNode)
		{
			arNode.SetAlwaysDraw(true);

			auto* pfadenode = arNode.IsTopFadeNode();
			if (pfadenode)
			{
				pfadenode->SetCurrentFade(1.f);
			}

			return BSVisit::BSVisitControl::Continue;
		});

		spObjects->AttachChild(apObj);
	}

	/// <summary> Detach an object from the scene </summary>
	/// <param name="apObj"> Object to detach </param>
	void Scene::DetachObject(NiAVObject* apObj)
	{
		BSASSERTFAST(spObjects);

		spObjects->DetachChild(apObj);
	}

	/// <summary> Detach all objects from the scene </summary>
	void Scene::DetachAllObjects()
	{
		BSASSERTFAST(spObjects);

		spObjects->DetachAllChildren();
	}

	/// <summary> Prepare a menu to render to an offscreen target so it can be used as a texture on an object </summary>
	/// <param name="aWidth"> Width of render target </param>
	/// <param name="aHeight"> Height of render target </param>
	void Scene::SetupMenuToTexture(const uint32_t aWidth, const uint32_t aHeight)
	{
		BSASSERTFAST(!MenuToTexture.QAvailable());
		BSASSERTFAST(!MenuToTexture.QBusy());

		TextureDB::RequestRenderTarget(Name, aWidth, aHeight, 1, TinyImageFormat::Format::R8G8B8A8_UNORM, MenuToTexture);
	}

	/// <summary> Prepare an object to use the offscreen menu target </summary>
	/// <param name="aObjectName"> Name of the object to switch out the albedo material for the offscreen menu target </param>
	void Scene::SetupMenuToTextureOnObject(const BSFixedString& aObjectName)
	{
		BSASSERTFAST(!aObjectName.QEmpty());
		BSASSERTFAST(spScene);

		BSVisit::TraverseScenegraphGeometries(spScene, [&](BSGeometry& arGeometry)
		{
			const BSFixedString& rgeomName = arGeometry.GetName();

			if (!rgeomName.QEmpty() && strncmp(rgeomName.QString(), aObjectName.QString(), aObjectName.QLength()) == 0)
			{
#pragma message( __FILE__ ": TODO: GEN-325209: Material Refactor: a texture override need to be a material instance trait, shouldn't have to create a new material.")
				auto htarget = QMenuToTextureRenderTarget().Replicate();
				CreationRenderer::MaterialInstance::TEMP_RequestModifyLayer(
					arGeometry.QMaterialInstanceKey(),
					0,
					{{TextureUtils::Base, htarget}});					
			}

			// There could be multiple target geometries, so keep iterating even if we've found one.
			return BSVisit::BSVisitControl::Continue;
		});
	}

	/// <summary> Set this scene as active or not </summary>
	/// <param name="aActive"> true to set active </param>
	void Scene::SetActive(const bool aActive)
	{
		Active = aActive;
	}

	bool Scene::QActive() const
	{
		return Active;
	}

	/// <summary> Updates the scene </summary>
	void Scene::Update()
	{
		if (spScene)
		{
			NiUpdateData updateScene(0.f);
			spScene->Update(updateScene);
		}

		if (spCamera)
		{
			NiUpdateData updateCamera(0.f);
			spCamera->Update(updateCamera);
		}
	}

	/// <summary> Set the viewport for a Scaleform movie </summary>
	/// <param name="arUI"> Scaleform movie to set the viewport </param>
	/// <param name="arRenderArea"> Viewport rect to use </param>
	void Scene::SetViewport(GFxMovie& arUI, const NiRect<float>& arRenderArea)
	{
		const auto screenSize = QRenderTargetSize();
		const auto uiPageTargetWidthC = static_cast<uint32_t>(screenSize.x);
		const auto uiPageTargetHeightC = static_cast<uint32_t>(screenSize.y);

		// Match movie viewport to menu rendering area
		arUI.SetViewport(
			uiPageTargetWidthC,
			uiPageTargetHeightC,
			int32_t(static_cast<float>(uiPageTargetWidthC) * arRenderArea.m_left),
			int32_t(static_cast<float>(uiPageTargetHeightC) * arRenderArea.m_top),
			int32_t(static_cast<float>(uiPageTargetWidthC) * (arRenderArea.m_right - arRenderArea.m_left)),
			int32_t(static_cast<float>(uiPageTargetHeightC) * (arRenderArea.m_bottom - arRenderArea.m_top)));
	}

	/// <summary> Use window point to ray to get location for 3d for current camera </summary>
	/// <param name="arWorldPoint"> OUT: world point result </param>
	/// <param name="aScreenPoint"> Screen point to pass into WindowPointToRay </param>
	/// <param name="aDistance"> Distance the item should be from the camera </param>
	/// <param name="aAdjustForAspectRatio"> Should the point adjust for the aspect ratio of this menu </param>
	void Scene::GetWorldPointFromScreenPoint(NiPoint3& arWorldPoint, const NiPoint2& aScreenPoint, const float aDistance, const bool aAdjustForAspectRatio)
	{
		NiPoint3 arDir, arOrig;
		const auto screenSize = QRenderTargetSize();

		spCamera->WindowPointToRay(
			static_cast<long>(aScreenPoint.x),
			static_cast<long>(aScreenPoint.y),
			arOrig,
			arDir,
			screenSize.x,
			screenSize.y);

		arWorldPoint = arDir * aDistance * (aAdjustForAspectRatio ? (screenSize.x / screenSize.y) : 1.0f);
	}

	void Scene::SetClearColor(const XMFLOAT4A& aClearColor)
	{
		if (CameraViewHandle.QRegistered())
		{
			CameraViewHandle->WriteDiscard<StorageTable::ClearColor>(StorageTable::ClearColor{ aClearColor });
		}
	}

#pragma endregion

#pragma region SceneManager

	void SceneManager::InitSDM()
	{
		BSTSingletonSDM<SceneManager>::InitSDM();
	}

	void SceneManager::KillSDM()
	{
		BSTSingletonSDM<SceneManager>::KillSDM();
	}

	/// <summary> Register a scene </summary>
	/// <param name="apScene"> Scene to register </param>
	void SceneManager::Register(Scene* apScene)
	{
		BSASSERTFAST(apScene);

		BSAutoWriteLock lock(ScenesRWLock);
		MenuScenes.SetAt(apScene->QName(), apScene);
	}

	/// <summary> Unregister a scene </summary>
	/// <param name="apScene"> Scene to unregister </param>
	void SceneManager::Unregister(const Scene* apScene)
	{
		BSASSERTFAST(apScene);

		BSAutoWriteLock lock(ScenesRWLock);
		MenuScenes.Remove(apScene->QName());
	}

	/// <summary> Find a scene by its name </summary>
	/// <param name="aName"> Name of the scene to retrieve </param>
	/// <returns> Scene containing the given name or nullptr if it has not been registered </returns>
	Scene* SceneManager::GetByName(const BSFixedString& aName)
	{
		Scene* pr = nullptr;
		{
			BSAutoReadLock lock(ScenesRWLock);
			MenuScenes.GetAt(aName, pr);
		}
		return pr;
	}

	void SceneManager::ForEachScene(CallbackScenes aSceneFunc)
	{
		MenuScenes.ForEach([&aSceneFunc](const BSFixedString&, const Scene* aScene)
		{
			aSceneFunc(aScene);
			return BSContainer::Continue;
		});
	}

	/// <summary> Disable or enable all registered scenes. </summary>
	void SceneManager::SetActiveAll(bool aActive)
	{
		ForEachScene([aActive](const Scene* aScene)
		{
			const_cast<Scene*>(aScene)->SetActive(aActive);
		});
	}

#pragma endregion
}
