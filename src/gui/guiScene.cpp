// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2020 Jean-Patrick Guerrero <jeanpatrick.guerrero@gmail.com>

#include "guiScene.h"

#include <SViewFrustum.h>
#include <AnimatedMeshSceneNode.h>
#include <BoneSceneNode.h>
#include <IVideoDriver.h>
#include <ISceneManager.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include "log.h"
#include "porting.h"
#include "client/mesh.h"

GUIScene::GUIScene(gui::IGUIEnvironment *env, scene::ISceneManager *smgr,
		   gui::IGUIElement *parent, core::recti rect, s32 id)
	: IGUIElement(gui::EGUIET_ELEMENT, env, parent, id, rect)
{
	m_driver = env->getVideoDriver();
	m_smgr = smgr->createNewSceneManager(false);

	m_cam = m_smgr->addCameraSceneNode(0, v3f(0.f, 0.f, -100.f), v3f(0.f));
	m_cam->setFOV(30.f * core::DEGTORAD);
}

GUIScene::~GUIScene()
{
	setMesh(nullptr);

	m_smgr->drop();
}

scene::AnimatedMeshSceneNode *GUIScene::setMesh(scene::IAnimatedMesh *mesh)
{
#if IS_VOPI_ENGINE
	// Attachments are parented to bones of the previous mesh — drop them
	// first so we don't leave dangling references when the primary mesh
	// goes away. Scene manager would clean them up anyway when m_mesh
	// is removed (children of a removed node get pruned), but explicit
	// cleanup keeps m_attachments in sync.
	clearAttachments();
#endif

	if (m_mesh) {
		m_mesh->remove();
		m_mesh = nullptr;
	}

	if (!mesh)
		return nullptr;

	m_mesh = m_smgr->addAnimatedMeshSceneNode(mesh);
	m_mesh->setPosition(-m_mesh->getBoundingBox().getCenter());
	m_mesh->animateJoints();

#if IS_VOPI_ENGINE
	// Fix vertex colors: ensure all vertices are white so textures
	// display at full brightness instead of being darkened. Applied to
	// the scene node's mesh copy (not the shared source mesh) via
	// setMeshColor(), which rewrites every buffer's vertex colors and
	// flags them dirty for re-upload.
	if (scene::IMesh *node_mesh = m_mesh->getMesh())
		setMeshColor(node_mesh, video::SColor(255, 255, 255, 255));
#endif

	return m_mesh;
}

namespace {
// Configures one material slot the way GUIScene wants for all of its meshes:
// alpha-blended with a 0.5 clip threshold, nearest-neighbour filtering (so
// pixel textures stay sharp), backface-culling off (kawaii models often
// rely on visible inside faces). Used by both setTexture() and
// addAttachment() so primary mesh and attachments always render with
// identical settings — divergence would have been a bug magnet.
static void apply_material_settings(scene::AnimatedMeshSceneNode *node, u32 idx,
		video::ITexture *texture)
{
	video::SMaterial &material = node->getMaterial(idx);
	material.MaterialType = video::EMT_TRANSPARENT_ALPHA_CHANNEL;
	material.MaterialTypeParam = 0.5f;
	material.TextureLayers[0].Texture = texture;
	material.FogEnable = true;
	material.TextureLayers[0].MinFilter = video::ETMINF_NEAREST_MIPMAP_NEAREST;
	material.TextureLayers[0].MagFilter = video::ETMAGF_NEAREST;
	material.BackfaceCulling = false;
	material.ZWriteEnable = video::EZW_AUTO;
}
} // namespace

void GUIScene::setTexture(u32 idx, video::ITexture *texture)
{
	apply_material_settings(m_mesh, idx, texture);
}

#if IS_VOPI_ENGINE

scene::AnimatedMeshSceneNode *GUIScene::addAttachment(
	scene::IAnimatedMesh *mesh,
	const std::vector<video::ITexture *> &textures,
	const std::string &bone_name,
	const v3f &position, const v3f &rotation, const v3f &scale)
{
	// `mesh` is contract-validated by the caller (parseModelOverlay
	// already errors on null before we get here); no null check needed.
	if (!m_mesh) {
		errorstream << "GUIScene::addAttachment: no primary mesh, refusing"
			<< std::endl;
		return nullptr;
	}

	// Resolve the bone on the primary mesh. We need joint nodes to exist,
	// which requires animateJoints() to have been called — setMesh() does
	// that already, so by the time addAttachment runs the joint hierarchy
	// is built and getJointNode() can find named bones.
	scene::BoneSceneNode *bone = m_mesh->getJointNode(bone_name.c_str());
	if (!bone) {
		errorstream << "GUIScene::addAttachment: bone '" << bone_name
			<< "' not found on primary mesh" << std::endl;
		return nullptr;
	}

	// Spawn the attachment as a child of the bone, mirroring how
	// content_cao does world-side bone attachments. Position / rotation
	// passed here are in bone-local coordinates, identical semantics to
	// entity:set_attach() — that way the same _appearance numbers work
	// for both world view and inventory preview.
	scene::AnimatedMeshSceneNode *node =
		m_smgr->addAnimatedMeshSceneNode(mesh, bone);
	if (!node) {
		errorstream << "GUIScene::addAttachment: addAnimatedMeshSceneNode failed"
			<< std::endl;
		return nullptr;
	}

	node->setPosition(position);
	node->setRotation(rotation);
	node->setScale(scale);
	node->animateJoints();

	// Fix vertex colors the same way setMesh() does for the primary,
	// otherwise dark mesh vertex colors would darken the texture.
	setMeshColor(node->getMesh(), video::SColor(255, 255, 255, 255));

	// Apply textures to each material slot. Warn on under-supplied
	// textures so a missing slot doesn't silently render as the default
	// pink placeholder — matches the warning style of parseModel.
	const u32 mat_count = node->getMaterialCount();
	for (u32 i = 0; i < mat_count; ++i) {
		const u32 texture_idx = mesh->getTextureSlot(i);
		if (texture_idx >= textures.size()) {
			warningstream << "GUIScene::addAttachment: not enough textures "
				"for material slot " << i << " (needs index "
				<< texture_idx << ", got " << textures.size()
				<< " entries)" << std::endl;
			continue;
		}
		if (textures[texture_idx])
			apply_material_settings(node, i, textures[texture_idx]);
	}

	// Returned pointer is OWNED by m_smgr — caller must NOT drop() it.
	// We also keep it in m_attachments for cleanup tracking. Returned
	// purely for future API convenience (debug, runtime adjustments).
	m_attachments.push_back(node);
	return node;
}

void GUIScene::clearAttachments()
{
	// Pointers in m_attachments are guaranteed non-null by addAttachment
	// (early-returns on every failure path before push_back), so no null
	// check needed here.
	for (auto *node : m_attachments)
		node->remove();
	m_attachments.clear();
}
#endif

void GUIScene::draw()
{
	m_driver->clearBuffers(video::ECBF_DEPTH);

	// Control rotation speed based on time
	u64 new_time = porting::getTimeMs();
	u64 dtime_ms = 0;
	if (m_last_time != 0)
		dtime_ms = porting::getDeltaMs(m_last_time, new_time);
	m_last_time = new_time;

	core::rect<s32> oldViewPort = m_driver->getViewPort();
	m_driver->setViewPort(getAbsoluteClippingRect());

	if (m_bgcolor != 0) {
		core::recti borderRect =
				Environment->getRootGUIElement()->getAbsoluteClippingRect();
		Environment->getSkin()->draw3DSunkenPane(
			this, m_bgcolor, false, true, borderRect, 0);
	}

	core::dimension2d<s32> size = getAbsoluteClippingRect().getSize();
	m_smgr->getActiveCamera()->setAspectRatio((f32)size.Width / (f32)size.Height);

	if (!m_target) {
		updateCamera(m_smgr->addEmptySceneNode());
		rotateCamera(v3f(0.f));
		m_cam->bindTargetAndRotation(true);
	}

#if IS_VOPI_ENGINE
	// Apply initial rotation and distance before drawing so the first frame is correct.
	// VOPI calcOptimalDistance() uses FOV math instead of view frustum,
	// so it can be called before drawAll().
	if (m_initial_rotation && m_mesh) {
		rotateCamera(v3f(m_custom_rot.X, m_custom_rot.Y, 0.f));
		calcOptimalDistance();
		m_initial_rotation = false;
	}
#endif

	cameraLoop();

	// Continuous rotation
	if (m_inf_rot)
		rotateCamera(v3f(0.f, -0.03f * (float)dtime_ms, 0.f));

	m_smgr->drawAll();

#if !IS_VOPI_ENGINE
	if (m_initial_rotation && m_mesh) {
		rotateCamera(v3f(m_custom_rot.X, m_custom_rot.Y, 0.f));
		calcOptimalDistance();
		m_initial_rotation = false;
	}
#endif

	m_driver->setViewPort(oldViewPort);
}

bool GUIScene::OnEvent(const SEvent &event)
{
	if (m_mouse_ctrl && event.EventType == EET_MOUSE_INPUT_EVENT) {
		if (event.MouseInput.Event == EMIE_LMOUSE_PRESSED_DOWN) {
			m_last_pos = v2f((f32)event.MouseInput.X, (f32)event.MouseInput.Y);
			return true;
		} else if (event.MouseInput.Event == EMIE_MOUSE_MOVED) {
			if (event.MouseInput.isLeftPressed()) {
				m_curr_pos = v2f((f32)event.MouseInput.X, (f32)event.MouseInput.Y);

				rotateCamera(v3f(
					m_last_pos.Y - m_curr_pos.Y,
					m_curr_pos.X - m_last_pos.X, 0.f));

				m_last_pos = m_curr_pos;
				return true;
			}
		}
	}

	return gui::IGUIElement::OnEvent(event);
}

void GUIScene::setStyles(const std::array<StyleSpec, StyleSpec::NUM_STATES> &styles)
{
	StyleSpec::State state = StyleSpec::STATE_DEFAULT;
	StyleSpec style = StyleSpec::getStyleFromStatePropagation(styles, state);

	setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
	setBackgroundColor(style.getColor(StyleSpec::BGCOLOR, m_bgcolor));
}

/**
 * Sets the frame loop range for the mesh
 */
void GUIScene::setFrameLoop(f32 begin, f32 end)
{
	if (m_mesh->getStartFrame() != begin || m_mesh->getEndFrame() != end)
		m_mesh->setFrameLoop(begin, end);
}

/**
 * Sets the animation speed (FPS) for the mesh
 */
void GUIScene::setAnimationSpeed(f32 speed)
{
	m_mesh->setAnimationSpeed(speed);
}

/* Camera control functions */

#if IS_VOPI_ENGINE

core::aabbox3df GUIScene::getSceneBox() const
{
	// Start from the primary mesh aabb. Use transformed (world-space) box
	// so attachments — which always live in world coords relative to the
	// primary's joints — can be unioned in directly. For the primary, the
	// size dimensions match getBoundingBox() because setMesh() centered
	// it on origin (translation doesn't change extent).
	core::aabbox3df box = m_mesh->getTransformedBoundingBox();

	// Union attachment boxes so the camera frames everything visible, not
	// just the primary. Without this a tall hat on the Head bone would
	// clip outside the preview rect because the primary's aabb stops at
	// the head crown.
	for (auto *att : m_attachments) {
		box.addInternalBox(att->getTransformedBoundingBox());
	}

	return box;
}

core::aabbox3df GUIScene::getAnimatedSceneBox()
{
	core::aabbox3df box = getSceneBox();

	const f32 start = m_mesh->getStartFrame();
	const f32 end = m_mesh->getEndFrame();
	if (!(end > start))
		return box; // static mesh or single-frame loop, nothing to sweep

	// The framing is computed once, but the element then plays its loop for
	// as long as the formspec is open. Sampling a single instant would let
	// any pose that only occurs mid-loop (a head lifting, a tail swinging)
	// grow outside the area we framed. Sweep the loop and frame the union
	// instead — the cost is bounded and paid once: SkinnedMesh's per-frame
	// box is built from joint boxes, not from vertices, so a sample is a
	// walk over the skeleton rather than over the mesh (tens of
	// microseconds for a ~20 joint skeleton).
	//
	// Sample every frame, since a frame is the unit animations are keyed
	// in, but cap the count so a very long loop stays a bounded one-time
	// cost; the cap still puts a sample within a couple of frames of every
	// pose for the loop lengths idle animations actually use.
	static const f32 MAX_SAMPLES = 64.f;
	// Clamp while still in float: an absurdly long loop must not overflow
	// the integer conversion.
	const u32 samples = (u32)core::clamp(std::ceil(end - start), 1.f, MAX_SAMPLES);
	const f32 restore = m_mesh->getFrameNr();

	for (u32 i = 0; i <= samples; ++i) {
		m_mesh->setCurrentFrame(start + (end - start) * ((f32)i / samples));
		// OnAnimate() derives its delta as timeMs - LastTimeMs, and
		// LastTimeMs stays 0 until the first drawAll(); calcOptimalDistance
		// runs before that (see draw()), so passing 0 gives a zero delta and
		// buildFrameNr() leaves the frame we just set alone. OnAnimate then
		// only re-poses the joints and recomputes the node aabb, which is
		// exactly what we want. Passing anything else here would advance the
		// animation instead of sampling it.
		m_mesh->OnAnimate(0);
		box.addInternalBox(getSceneBox());
	}

	// Put the mesh back where we found it: draw() proceeds straight to
	// drawAll() after this, and the first visible frame should be the one
	// the frame loop asked for, not wherever the sweep happened to stop.
	m_mesh->setCurrentFrame(restore);
	m_mesh->OnAnimate(0);

	return box;
}

void GUIScene::getCameraBasis(v3f &right, v3f &up, v3f &fwd) const
{
	// Take the view direction from the camera offset itself rather than
	// rebuilding it from Euler angles: cameraLoop() only rescales
	// (m_cam_pos - m_target_pos) to the new distance, so this direction is
	// exactly the one the mesh will be seen from.
	fwd = m_target_pos - m_cam_pos;
	fwd.normalize();

	// Match buildCameraLookAtMatrixLH(), which cameraLoop() drives through
	// setTarget(): x = normalize(up_world x forward), y = forward x x.
	// correctBounds() keeps the pitch within +-60 degrees, so forward is
	// never parallel to the world up and neither cross product degenerates.
	right = v3f(0.f, 1.f, 0.f).crossProduct(fwd);
	right.normalize();
	up = fwd.crossProduct(right);
	up.normalize();
}

f32 GUIScene::calcSilhouetteDistance(f32 h_slope, f32 v_slope)
{
	// Degenerate camera offset: nothing sane to project along.
	if (m_cam_pos.getDistanceFromSQ(m_target_pos) <= 0.f)
		return 0.f;

	const core::aabbox3df box = getAnimatedSceneBox();

	v3f right, up, fwd;
	getCameraBasis(right, up, fwd);

	const f32 fill = core::clamp(m_fit_fill, 0.01f, 4.0f);

	// A corner at lateral offset x and depth offset z from the target (both
	// in camera axes) projects to |x| / ((D + z) * h_slope/2) of the half
	// width at camera distance D, so keeping it inside the `fill` fraction
	// of the frame needs D >= 2|x| / (h_slope * fill) - z; likewise for the
	// vertical axis. The distance is the largest of those over all 8
	// corners and both axes, which puts the most demanding corner exactly
	// on the edge and every other one inside. Fitting per corner rather
	// than the maximum lateral extent at the minimum depth matters: at an
	// oblique yaw the widest corner sits mid-depth, and using the near
	// depth for it would leave a margin that grows with box depth, i.e.
	// exactly the proportion dependence this mode is meant to remove.
	//
	// Offsets are measured from the camera target, not the box centre: the
	// camera always aims at m_target_pos, so an off-centre box (a tall
	// attachment on one side) has to fit around the target or half of it
	// falls outside the rect.
	v3f corners[8];
	box.getEdges(corners);

	f32 dist = 0.f;
	f32 near_depth = std::numeric_limits<f32>::max();
	for (const v3f &corner : corners) {
		const v3f p = corner - m_target_pos;
		const f32 z = p.dotProduct(fwd);
		dist = std::max(dist, 2.f * std::fabs(p.dotProduct(right)) / (h_slope * fill) - z);
		dist = std::max(dist, 2.f * std::fabs(p.dotProduct(up)) / (v_slope * fill) - z);
		near_depth = std::min(near_depth, z);
	}

	// Keep the nearest corner in front of the near clip plane. Only meshes
	// well under a node in size (or an aggressive fill) get this close, but
	// a preview shown a little smaller than asked beats one with its front
	// sliced off; the axis-aligned path has no such guard and can clip.
	dist = std::max(dist, m_cam->getNearValue() * 1.05f - near_depth);

	return std::isfinite(dist) && dist > 0.f ? dist : 0.f;
}

void GUIScene::calcOptimalDistance()
{
	// Update absolute transforms across the whole subtree before reading
	// transformed bboxes. OnAnimate(0) is the recursive variant —
	// updateAbsolutePosition() is NOT recursive (see ISceneNode.h docs),
	// so calling it only on m_mesh or only on the attachments would leave
	// the intermediate bone joints stale and make attachment world
	// positions wrong on first frame. This matters because
	// calcOptimalDistance runs once (gated by m_initial_rotation) before
	// the first drawAll(), so without this the camera distance would be
	// computed against incorrect aabb and stuck wrong for the formspec's
	// lifetime.
	m_mesh->OnAnimate(0);

	core::recti rect = getAbsolutePosition();

	// Guard a zero-sized element rect: every path below divides by the rect
	// dimensions, and inf/NaN there would push the camera to infinity and
	// blank the preview for good. Keep the existing m_cam_distance instead.
	if (rect.getWidth() <= 0 || rect.getHeight() <= 0) {
		m_update_cam = true;
		return;
	}

	// Calculate frustum slopes from FOV and aspect ratio directly,
	// avoiding dependency on view frustum which requires a prior drawAll().
	f32 fov = m_cam->getFOV();
	f32 v_slope = 2.0f * tanf(fov * 0.5f);
	f32 aspect = (f32)rect.getWidth() / (f32)rect.getHeight();
	f32 h_slope = v_slope * aspect;

	if (m_fit_mode == FitMode::SILHOUETTE) {
		// A zero result means the geometry was degenerate; keep whatever
		// distance we already have rather than teleporting the camera.
		f32 dist = calcSilhouetteDistance(h_slope, v_slope);
		if (dist > 0.f)
			m_cam_distance = dist;
		m_update_cam = true;
		return;
	}

	core::aabbox3df box = getSceneBox();

	f32 width  = box.MaxEdge.X - box.MinEdge.X;
	f32 height = box.MaxEdge.Y - box.MinEdge.Y;
	f32 depth  = box.MaxEdge.Z - box.MinEdge.Z;
	f32 max_width = width > depth ? width : depth;

	// Guard a flat/empty mesh aabb (max_width or height <= 0), which would
	// make the divisions below produce inf/NaN the same way a zero-sized
	// rect would. (parseModelOverlay already rejects non-finite attachment
	// transforms; this also covers genuinely empty primary geometry.)
	if (max_width <= 0.f || height <= 0.f) {
		m_update_cam = true;
		return;
	}

	f32 zoomX = rect.getWidth() / max_width;
	f32 zoomY = rect.getHeight() / height;
	f32 dist;

	if (zoomX < zoomY)
		dist = (max_width / h_slope) + (0.5f * max_width);
	else
		dist = (height / v_slope) + (0.5f * max_width);

	m_cam_distance = dist;
	m_update_cam = true;
#else
void GUIScene::calcOptimalDistance()
{
	core::aabbox3df box = m_mesh->getBoundingBox();
	f32 width  = box.MaxEdge.X - box.MinEdge.X;
	f32 height = box.MaxEdge.Y - box.MinEdge.Y;
	f32 depth  = box.MaxEdge.Z - box.MinEdge.Z;
	f32 max_width = width > depth ? width : depth;

	const scene::SViewFrustum *f = m_cam->getViewFrustum();
	f32 cam_far = m_cam->getFarValue();
	f32 far_width = core::line3df(f->getFarLeftUp(), f->getFarRightUp()).getLength();
	f32 far_height = core::line3df(f->getFarLeftUp(), f->getFarLeftDown()).getLength();

	core::recti rect = getAbsolutePosition();
	f32 zoomX = rect.getWidth() / max_width;
	f32 zoomY = rect.getHeight() / height;
	f32 dist;

	if (zoomX < zoomY)
		dist = (max_width / (far_width / cam_far)) + (0.5f * max_width);
	else
		dist = (height / (far_height / cam_far)) + (0.5f * max_width);

	m_cam_distance = dist;
	m_update_cam = true;
#endif
}

void GUIScene::updateCamera(scene::ISceneNode *target)
{
	m_target = target;
	updateTargetPos();

	m_last_target_pos = m_target_pos;
	updateCameraPos();

	m_update_cam = true;
}

void GUIScene::updateTargetPos()
{
	m_last_target_pos = m_target_pos;
	m_target->updateAbsolutePosition();
	m_target_pos = m_target->getAbsolutePosition();
}

void GUIScene::setCameraRotation(v3f rot)
{
	correctBounds(rot);

	core::matrix4 mat;
	mat.setRotationDegrees(rot);

	m_cam_pos = mat.rotateAndScaleVect(v3f(0.f, 0.f, m_cam_distance));

	m_cam_pos += m_target_pos;
	m_cam->setPosition(m_cam_pos);
	m_update_cam = false;
}

bool GUIScene::correctBounds(v3f &rot)
{
	const float ROTATION_MAX_1 = 60.0f;
	const float ROTATION_MAX_2 = 300.0f;

	// Limit and correct the rotation when needed
	if (rot.X < 90.f) {
		if (rot.X > ROTATION_MAX_1) {
			rot.X = ROTATION_MAX_1;
			return true;
		}
	} else if (rot.X < ROTATION_MAX_2) {
		rot.X = ROTATION_MAX_2;
		return true;
	}

	// Not modified
	return false;
}

void GUIScene::cameraLoop()
{
	updateCameraPos();
	updateTargetPos();

	if (m_target_pos != m_last_target_pos)
		m_update_cam = true;

	if (m_update_cam) {
		m_cam_pos = m_target_pos + (m_cam_pos - m_target_pos).normalize() * m_cam_distance;

		v3f rot = getCameraRotation();
		if (correctBounds(rot))
			setCameraRotation(rot);

		m_cam->setPosition(m_cam_pos);
		m_cam->setTarget(m_target_pos);

		m_update_cam = false;
	}
}
