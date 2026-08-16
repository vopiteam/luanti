// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2020 Jean-Patrick Guerrero <jeanpatrick.guerrero@gmail.com>

#pragma once

#include "ICameraSceneNode.h"
#include "StyleSpec.h"
#include <AnimatedMeshSceneNode.h>
#include <IGUIElement.h>
#include <IGUIEnvironment.h>
#include <string>
#include <vector>


class GUIScene : public gui::IGUIElement
{
public:
	GUIScene(gui::IGUIEnvironment *env, scene::ISceneManager *smgr,
		 gui::IGUIElement *parent, core::recti rect, s32 id = -1);

	~GUIScene();

	/// @param mesh does not get consumed, mesh->drop() must still be called afterward
	scene::AnimatedMeshSceneNode *setMesh(scene::IAnimatedMesh *mesh = nullptr);

	void setTexture(u32 idx, video::ITexture *texture);
	void setBackgroundColor(const video::SColor &color) noexcept { m_bgcolor = color; };
	void setFrameLoop(f32 begin, f32 end);
	void setAnimationSpeed(f32 speed);
	void enableMouseControl(bool enable) noexcept { m_mouse_ctrl = enable; };
	void setRotation(v2f rot) noexcept { m_custom_rot = rot; };
	void enableContinuousRotation(bool enable) noexcept { m_inf_rot = enable; };
	void setStyles(const std::array<StyleSpec, StyleSpec::NUM_STATES> &styles);

#if IS_VOPI_ENGINE
	// VOPI extension — attach a secondary mesh to a named bone of the
	// primary mesh. Mirrors entity:set_attach behavior in-world so
	// wearable previews can use the same .obj files as world-side
	// attachments. The bone must exist on the primary mesh set via
	// setMesh(); if it doesn't, the call logs an error and returns
	// nullptr without spawning the node.
	//
	// Textures are applied per attachment-mesh material slot, identical
	// settings to setTexture() (alpha-blended, nearest-neighbour, no
	// backface culling). The `textures` vector is indexed by
	// mesh->getTextureSlot(material_index); under-supplied entries log a
	// warning, missing slots fall back to the mesh's default material.
	//
	// Returned pointer is owned by the scene manager — DO NOT drop() it.
	// We also retain it in m_attachments for cleanup tracking via
	// clearAttachments() (also called from setMesh()). The whole subtree
	// is also implicitly destroyed when the scene manager is dropped in
	// ~GUIScene; clearAttachments() is the explicit path used between
	// setMesh swaps.
	scene::AnimatedMeshSceneNode *addAttachment(
		scene::IAnimatedMesh *mesh,
		const std::vector<video::ITexture *> &textures,
		const std::string &bone_name,
		const v3f &position, const v3f &rotation, const v3f &scale);

	void clearAttachments();

	// VOPI extension — how calcOptimalDistance() decides the camera
	// pull-back. See setFitMode().
	enum class FitMode
	{
		//! Fit the raw axis-aligned bounding box: the horizontal extent is
		//! max(X, Z) regardless of where the camera actually is. This is
		//! the default, so every formspec that doesn't ask for something
		//! else keeps its existing framing.
		AABB,
		//! Fit the silhouette the camera actually sees. The bounding box
		//! corners are projected onto the view plane at the element's
		//! rotation and the projected extents are what gets fitted, so
		//! meshes with very different proportions come out at a consistent
		//! apparent size instead of long ones shrinking away. The box is
		//! also swept over the animation loop, so a pose that only occurs
		//! mid-loop cannot grow outside the framed area. Framed for the
		//! initial rotation only: continuous or mouse-driven rotation
		//! changes the silhouette afterwards.
		SILHOUETTE,
	};

	//! @param fill fraction of the element the fitted silhouette spans
	//!        (1.0 = edge to edge, below 1 leaves a margin, above 1
	//!        deliberately crops). Ignored by FitMode::AABB.
	void setFitMode(FitMode mode, f32 fill = 1.0f) noexcept
	{
		m_fit_mode = mode;
		m_fit_fill = fill;
	}
#endif

	virtual void draw();
	virtual bool OnEvent(const SEvent &event);

private:
	void calcOptimalDistance();
#if IS_VOPI_ENGINE
	//! World-space aabb of the primary mesh unioned with every attachment,
	//! at whatever pose they currently hold.
	core::aabbox3df getSceneBox() const;
	//! getSceneBox() unioned over the whole animation loop, so the framing
	//! covers every pose the idle loop reaches instead of only the one
	//! instant calcOptimalDistance() happens to sample. Leaves the mesh on
	//! the frame it was called with.
	core::aabbox3df getAnimatedSceneBox();
	//! Orthonormal camera basis for the current camera position, matching
	//! the look-at matrix Irrlicht builds from it.
	void getCameraBasis(v3f &right, v3f &up, v3f &fwd) const;
	//! calcOptimalDistance() body for FitMode::SILHOUETTE.
	//! @return the camera distance, or 0 when the camera set-up is
	//!         degenerate and the caller should keep the distance it
	//!         already has.
	f32 calcSilhouetteDistance(f32 h_slope, f32 v_slope);
#endif
	void updateTargetPos();
	void updateCamera(scene::ISceneNode *target);
	void setCameraRotation(v3f rot);
	/// @return true indicates that the rotation was corrected
	bool correctBounds(v3f &rot);
	void cameraLoop();

	void updateCameraPos() { m_cam_pos = m_cam->getPosition(); };
	v3f getCameraRotation() const { return (m_cam_pos - m_target_pos).getHorizontalAngle(); };
	void rotateCamera(const v3f &delta) { setCameraRotation(getCameraRotation() + delta); };

	scene::ISceneManager *m_smgr;
	video::IVideoDriver *m_driver;
	scene::ICameraSceneNode *m_cam;
	scene::ISceneNode *m_target = nullptr;
	scene::AnimatedMeshSceneNode *m_mesh = nullptr;

#if IS_VOPI_ENGINE
	// Secondary meshes attached to bones of m_mesh. Owned by m_smgr; we
	// hold raw observer pointers only so clearAttachments() can selectively
	// remove our nodes (without disturbing other smgr children) and so
	// calcOptimalDistance() can union their aabb with the primary's.
	// Validity: each pointer remains valid until clearAttachments() is
	// called (which runs from setMesh swap and from ~GUIScene via the
	// IGUIElement base destructor path).
	std::vector<scene::AnimatedMeshSceneNode *> m_attachments;

	// Framing strategy; AABB keeps the pre-existing behaviour and is what
	// every element gets unless the formspec opts into something else.
	FitMode m_fit_mode = FitMode::AABB;
	f32 m_fit_fill = 1.0f;
#endif

	f32 m_cam_distance = 50.f;

	u64 m_last_time = 0;

	v3f m_cam_pos;
	v3f m_target_pos;
	v3f m_last_target_pos;
	// Cursor positions
	v2f m_curr_pos;
	v2f m_last_pos;
	// Initial rotation
	v2f m_custom_rot;

	bool m_mouse_ctrl = true;
	bool m_update_cam = false;
	bool m_inf_rot    = false;
	bool m_initial_rotation = true;

	video::SColor m_bgcolor;
};
