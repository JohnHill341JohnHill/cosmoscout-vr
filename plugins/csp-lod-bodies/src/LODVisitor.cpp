////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
////////////////////////////////////////////////////////////////////////////////////////////////////

// SPDX-FileCopyrightText: German Aerospace Center (DLR) <cosmoscout@dlr.de>
// SPDX-License-Identifier: MIT

#include "LODVisitor.hpp"

#include "PlanetParameters.hpp"
#include "RenderDataDEM.hpp"
#include "RenderDataImg.hpp"
#include "TileTextureArray.hpp"
#include "TreeManagerBase.hpp"
#include "logger.hpp"

#include <VistaBase/VistaStreamUtils.h>
#include <glm/gtc/matrix_inverse.hpp>

namespace csp::lodbodies {

////////////////////////////////////////////////////////////////////////////////////////////////////

namespace {
// Initially reserve storage for this many entries in the lists produced
// by LODVisitor (mLoadDEM, mLoadIMG, mRenderDEM, mRenderIMG).
// The lists are still grown as needed, but this reduces the number of
// re-allocations.
std::size_t const PreAllocSize = 200;

////////////////////////////////////////////////////////////////////////////////////////////////////

// Returns if the tile bounds @a tb intersect the @a frustum.
// For each plane of the @a frustum determine if any corner of the
// bounding box is inside the plane's halfspace. If all corners are
// outside one halfspace the bounding box is outside the frustum
// and the algorithm stops early.
// TODO There is potential for optimization here, the paper
// "Optimized View Frustum Culling - Algorithms for Bounding Boxes"
// http://www.cse.chalmers.se/~uffe/vfc_bbox.pdf
// contains ideas (for example how to avoid testing all 8 corners).
bool testInFrustum(Frustum const& frustum, BoundingBox<double> const& tb) {
  bool result = true;

  glm::dvec3 const& tbMin = tb.getMin();
  glm::dvec3 const& tbMax = tb.getMax();

  // 8 corners of tile's bounding box
  std::array<glm::dvec3, 8> tbPnts = {
      {glm::dvec3(tbMin[0], tbMin[1], tbMin[2]), glm::dvec3(tbMax[0], tbMin[1], tbMin[2]),
          glm::dvec3(tbMax[0], tbMin[1], tbMax[2]), glm::dvec3(tbMin[0], tbMin[1], tbMax[2]),

          glm::dvec3(tbMin[0], tbMax[1], tbMin[2]), glm::dvec3(tbMax[0], tbMax[1], tbMin[2]),
          glm::dvec3(tbMax[0], tbMax[1], tbMax[2]), glm::dvec3(tbMin[0], tbMax[1], tbMax[2])}};

  // loop over planes of frustum
  auto pIt  = frustum.getPlanes().begin();
  auto pEnd = frustum.getPlanes().end();

  for (std::size_t i = 0; pIt != pEnd; ++pIt, ++i) {
    glm::dvec3 const normal(*pIt);
    double const     d       = -(*pIt)[3];
    bool             outside = true;

    // test if any BB corner is inside the halfspace defined
    // by the current plane
    for (auto const& tbPnt : tbPnts) {
      if (glm::dot(normal, tbPnt) >= d) {
        // corner j is inside - stop testing
        outside = false;
        break;
      }
    }

    // if all corners are outside the halfspace, the bounding box
    // is outside - stop testing
    if (outside) {
      result = false;
      break;
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

// Returns true if one the eight tile bbox corner points is not occluded by a proxy sphere.
// Culls tiles behind the horizon.
bool testFrontFacing(glm::dvec3 const& camPos, PlanetParameters const* params,
    BoundingBox<double> const& tb, TreeManagerBase* treeMgrDEM) {
  assert(treeMgrDEM != nullptr);

  // Get minimum height of all base patches (needed for radius of proxy culling sphere)
  auto minHeight(std::numeric_limits<float>::max());
  for (int i(0); i < TileQuadTree::sNumRoots; ++i) {
    auto*       tile       = treeMgrDEM->getTree()->getRoot(i)->getTile();
    auto const& castedTile = dynamic_cast<Tile<float> const&>(*tile);
    minHeight              = std::min(minHeight, castedTile.getMinMaxPyramid()->getMin());
  }

  double dProxyRadius = std::min(params->mRadii.x, std::min(params->mRadii.y, params->mRadii.z)) +
                        (minHeight * params->mHeightScale);

  glm::dvec3 const& tbMin = tb.getMin();
  glm::dvec3 const& tbMax = tb.getMax();

  // 8 corners of tile's bounding box
  std::array<glm::dvec3, 8> tbPnts = {
      {glm::dvec3(tbMin[0], tbMin[1], tbMin[2]), glm::dvec3(tbMax[0], tbMin[1], tbMin[2]),
          glm::dvec3(tbMax[0], tbMin[1], tbMax[2]), glm::dvec3(tbMin[0], tbMin[1], tbMax[2]),

          glm::dvec3(tbMin[0], tbMax[1], tbMin[2]), glm::dvec3(tbMax[0], tbMax[1], tbMin[2]),
          glm::dvec3(tbMax[0], tbMax[1], tbMax[2]), glm::dvec3(tbMin[0], tbMax[1], tbMax[2])}};

  // Simple ray-sphere intersection test for every corner point
  for (auto const& tbPnt : tbPnts) {
    double     dRayLength = glm::length(tbPnt - camPos);
    glm::dvec3 vRayDir    = (tbPnt - camPos) / dRayLength;
    double     b          = glm::dot(camPos, vRayDir);
    double     c          = glm::dot(camPos, camPos) - dProxyRadius * dProxyRadius;
    double     fDet       = b * b - c;
    // No intersection between corner and camera position: Tile visible!:
    if (fDet < 0.0) {
      return true;
    }

    fDet = std::sqrt(fDet);
    // Both intersection points are behind the camera but tile is in front
    // (presumes tiles to be frustum culled already!!!)
    // E.g. While travelling in a deep crater and looking above
    if ((-b - fDet) < 0.0 && (-b + fDet) < 0.0) {
      return true;
    }

    // Tile in front of planet:
    if (dRayLength < -b - fDet) {
      return true;
    }
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

// Tests if @a node can be refined, which is the case if all 4
// children are present and uploaded to the GPU.
bool childrenAvailable(TileNode* node, TreeManagerBase* treeMgr) {
  for (int i = 0; i < 4; ++i) {
    TileNode* child = node->getChild(i);

    // child is not loaded -> can not refine
    if (child == nullptr) {
      return false;
    }

    RenderData* rd = treeMgr->findRData(child);

    // child is not on GPU -> can not refine
    if (!rd || rd->getTexLayer() < 0) {
      return false;
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////

/* explicit */
LODVisitor::LODVisitor(
    PlanetParameters const& params, TreeManagerBase* treeMgrDEM, TreeManagerBase* treeMgrIMG)
    : TileVisitor<LODVisitor>(treeMgrDEM ? treeMgrDEM->getTree() : nullptr,
          treeMgrIMG ? treeMgrIMG->getTree() : nullptr)
    , mParams(&params)
    , mTreeMgrDEM(nullptr)
    , mTreeMgrIMG(nullptr)
    , mViewport()
    , mMatVM()
    , mMatP()
    , mLodData()
    , mCullData()
    , mStackTop(-1)
    , mFrameCount(0)
    , mUpdateLOD(true)
    , mUpdateCulling(true) {
  setTreeManagerDEM(treeMgrDEM);
  setTreeManagerIMG(treeMgrIMG);

  mStack.resize(sMaxStackDepth);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::setTreeManagerDEM(TreeManagerBase* treeMgr) {
  // unset tree from OLD tree manager
  if (mTreeMgrDEM) {
    setTreeDEM(nullptr);
  }

  mStack.clear();
  mStack.resize(sMaxStackDepth);

  mTreeMgrDEM = treeMgr;

  // set tree from NEW tree manager
  if (mTreeMgrDEM) {
    setTreeDEM(mTreeMgrDEM->getTree());
    mLoadDEM.reserve(PreAllocSize);
    mRenderDEM.reserve(PreAllocSize);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::setTreeManagerIMG(TreeManagerBase* treeMgr) {
  // unset tree from OLD tree manager
  if (mTreeMgrIMG) {
    setTreeIMG(nullptr);
  }

  mTreeMgrIMG = treeMgr;

  // set tree from NEW tree manager
  if (mTreeMgrIMG) {
    setTreeIMG(mTreeMgrIMG->getTree());
    mLoadIMG.reserve(PreAllocSize);
    mRenderIMG.reserve(PreAllocSize);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool LODVisitor::preTraverse() {
  bool result = true;

  // update derived matrices from mMatP, mMatVM
  if (mUpdateLOD) {
    mLodData.mMatVM = mMatVM;
    mLodData.mMatP  = mMatP;
    mLodData.mFrustumES.setFromMatrix(mMatP);
    mLodData.mViewport = mViewport;
  }

  if (mUpdateCulling) {
    mCullData.mFrustumMS.setFromMatrix(mMatP * mMatVM);
    mCullData.mMatN   = glm::inverseTranspose(glm::f64mat3x3(mMatVM));
    auto v4CamPos     = glm::inverse(mMatVM)[3];
    mCullData.mCamPos = glm::dvec3(v4CamPos[0], v4CamPos[1], v4CamPos[2]);
  }

  // clear load/render lists
  mLoadDEM.clear();
  mLoadIMG.clear();
  mRenderDEM.clear();
  mRenderIMG.clear();
  mStackTop = -1;

  // make sure root nodes are present
  for (int i = 0; i < TileQuadTree::sNumRoots; ++i) {
    if (mTreeDEM) {
      if (!mTreeDEM->getRoot(i)) {
        mLoadDEM.emplace_back(0, i);
        result = false;
      }
    }

    if (mTreeIMG) {
      if (!mTreeIMG->getRoot(i)) {
        mLoadIMG.emplace_back(0, i);
        result = false;
      }
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool LODVisitor::preVisitRoot(TileId const& tileId) {
  LODState& state = getLODState();

  // fetch RenderDataDEM for visited node and mark as used in this frame
  if (mTreeMgrDEM && state.mNodeDEM) {
    auto* rd     = mTreeMgrDEM->find<RenderDataDEM>(state.mNodeDEM);
    state.mRdDEM = rd;
    state.mRdDEM->setLastFrame(mFrameCount);
  } else {
    state.mRdDEM = nullptr;
  }

  // fetch RenderDataImg for visited node and mark as used in this frame
  if (mTreeMgrIMG && state.mNodeIMG) {
    auto* rd     = mTreeMgrIMG->find<RenderDataImg>(state.mNodeIMG);
    state.mRdIMG = rd;
    state.mRdIMG->setLastFrame(mFrameCount);
  } else {
    state.mRdIMG = nullptr;
  }

  return visitNode(tileId);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::postVisitRoot(TileId const& /*tileId*/) {
  // nothing to do
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool LODVisitor::preVisit(TileId const& tileId) {

  LODState& state  = getLODState();
  LODState& stateP = getLODState(tileId.level() - 1); // parent state

  // fetch RenderDataDEM for visited node and mark as used in this frame
  if (mTreeMgrDEM && state.mNodeDEM) {
    auto* rd     = mTreeMgrDEM->find<RenderDataDEM>(state.mNodeDEM);
    state.mRdDEM = rd;
    state.mRdDEM->setLastFrame(mFrameCount);
  } else {
    state.mRdDEM = stateP.mRdDEM;
  }

  // fetch RenderDataImg for visited node and mark as used in this frame
  if (mTreeMgrIMG && state.mNodeIMG) {
    auto* rd     = mTreeMgrIMG->find<RenderDataImg>(state.mNodeIMG);
    state.mRdIMG = rd;
    state.mRdIMG->setLastFrame(mFrameCount);
  } else {
    state.mRdIMG = stateP.mRdIMG;
  }

  return visitNode(tileId);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool LODVisitor::visitNode(TileId const& tileId) {
  // Determine if the current node is visible (using the DEM node).
  // This is done by testing for an intersection between the camera
  // frustum and the DEM node's bounding box.
  //
  // If the node is visible:
  //      Determine if the node has sufficient resolution for the current
  //      view: see testNeedRefine() for details on the algorithm.
  //
  //      If node should be refined:
  //          Determine if it is possible to refine the node, see
  //          handleRefine() for details.
  //      Else:
  //          draw this level

  bool result  = false;
  bool visible = testVisible(tileId, mTreeMgrDEM);

  if (visible) {
    // should this node be refined to achieve desired resolution?
    bool needRefine = testNeedRefine(tileId);

    if (needRefine) {
      result = handleRefine(tileId);
    } else {
      // resolution is sufficient
      drawLevel();
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool LODVisitor::handleRefine(TileId const& /*tileId*/) {
  bool      result = false;
  LODState& state  = getLODState();

  // test if nodes can be refined
  bool childrenDemAvailable =
      state.mNodeDEM ? childrenAvailable(state.mNodeDEM, mTreeMgrDEM) : false;
  bool childrenImgAvailable =
      state.mNodeIMG ? childrenAvailable(state.mNodeIMG, mTreeMgrIMG) : false;

  if (mTreeMgrDEM != nullptr && mTreeMgrIMG != nullptr) {
    // DEM and IMG data

    // request to load missing children
    if (!childrenDemAvailable) {
      addLoadChildrenDEM(state.mNodeDEM);
    }

    if (!childrenImgAvailable) {
      addLoadChildrenIMG(state.mNodeIMG);
    }

    if (childrenDemAvailable && childrenImgAvailable) {
      result = true;
    } else {
      // can not refine, draw this level
      drawLevel();
    }
  } else if (mTreeMgrDEM != nullptr && mTreeMgrIMG == nullptr) {
    // DEM data only

    if (childrenDemAvailable) {
      // tree can be refined, visit children
      result = true;
    } else {
      addLoadChildrenDEM(state.mNodeDEM);

      // can not refine, draw this level
      drawLevel();
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::addLoadChildrenDEM(TileNode* node) {
  if (node && node->getLevel() < mParams->mMaxLevel) {
    TileId const& tileId = node->getTileId();

    for (int i = 0; i < 4; ++i) {
      if (!node->getChild(i)) {
        mLoadDEM.push_back(HEALPix::getChildTileId(tileId, i));
      } else {
        // mark child as used to avoid it being removed while waiting
        // for its siblings to be loaded
        RenderData* rd = mTreeMgrDEM->findRData(node->getChild(i));
        rd->setLastFrame(mFrameCount);
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::addLoadChildrenIMG(TileNode* node) {
  if (node && node->getLevel() < mParams->mMaxLevel) {
    TileId const& tileId = node->getTileId();

    for (int i = 0; i < 4; ++i) {
      if (!node->getChild(i)) {
        mLoadIMG.push_back(HEALPix::getChildTileId(tileId, i));
      } else {
        // mark child as used to avoid it being removed while waiting
        // for its siblings to be loaded
        RenderData* rd = mTreeMgrIMG->findRData(node->getChild(i));
        rd->setLastFrame(mFrameCount);
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool LODVisitor::testVisible(TileId const& tileId, TreeManagerBase* treeMgrDEM) {
  bool      result = false;
  LODState& state  = getLODState();

  BoundingBox<double> const& tb = state.mRdDEM->getBounds();

  result = testInFrustum(mCullData.mFrustumMS, tb);

  if (result) {
    result = testFrontFacing(mCullData.mCamPos, mParams, tb, treeMgrDEM);
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool LODVisitor::testNeedRefine(TileId const& tileId) {
  bool      result = false;
  LODState& state  = getLODState();

  if (state.mNodeDEM) {
    BoundingBox<double> tb = state.mRdDEM->getBounds();

    glm::dvec3 const& tbMin    = tb.getMin();
    glm::dvec3 const& tbMax    = tb.getMax();
    glm::dvec3        tbCenter = 0.5 * (tbMin + tbMax);

    // A tile is refined if the solid angle it occupies when seen from the
    // camera is above a given threshold. To estimate the solid angle, the
    // angles between the vector from the camera to the bounding box center
    // and all vectors from the camera to all eight corners of the bounding
    // box are calculated and the maximum of those is taken.

    // 8 corners of tile's bounding box
    std::array<glm::dvec3, 8> tbDirs = {
        {glm::normalize(glm::dvec3(tbMin.x, tbMin.y, tbMin.z) - mCullData.mCamPos),
            glm::normalize(glm::dvec3(tbMax.x, tbMin.y, tbMin.z) - mCullData.mCamPos),
            glm::normalize(glm::dvec3(tbMax.x, tbMin.y, tbMax.z) - mCullData.mCamPos),
            glm::normalize(glm::dvec3(tbMin.x, tbMin.y, tbMax.z) - mCullData.mCamPos),
            glm::normalize(glm::dvec3(tbMin.x, tbMax.y, tbMin.z) - mCullData.mCamPos),
            glm::normalize(glm::dvec3(tbMax.x, tbMax.y, tbMin.z) - mCullData.mCamPos),
            glm::normalize(glm::dvec3(tbMax.x, tbMax.y, tbMax.z) - mCullData.mCamPos),
            glm::normalize(glm::dvec3(tbMin.x, tbMax.y, tbMax.z) - mCullData.mCamPos)}};

    glm::dvec3 centerDir = glm::normalize(tbCenter - mCullData.mCamPos);

    double maxAngle(0.0);

    for (auto& tbDir : tbDirs) {
      maxAngle = std::max(std::acos(std::min(1.0, glm::dot(tbDir, centerDir))), maxAngle);
    }

    // calculate field of view
    double fov =
        std::max(mLodData.mFrustumES.getHorizontalFOV(), mLodData.mFrustumES.getVerticalFOV());

    double ratio = maxAngle / fov * mParams->mLodFactor;

    result = ratio > 10.0;

    if (mParams->mMinLevel > tileId.level()) {
      result = true;
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::drawLevel() {
  LODState& state = getLODState();

  if (mTreeMgrDEM) {
    // check node is available (either for this level or highest resolution
    // currently loaded) and has RenderDataDEM
    assert(state.mNodeDEM);
    assert(state.mRdDEM);

    state.mRdDEM->addFlag(RenderDataDEM::Flags::eRender);
    mRenderDEM.push_back(state.mRdDEM);
  }

  if (mTreeMgrIMG) {
    // check node is available (either for this level or highest resolution
    // currently loaded) and has RenderDataIMG
    assert(state.mNodeIMG);
    assert(state.mRdIMG);

    mRenderIMG.push_back(state.mRdIMG);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TreeManagerBase* LODVisitor::getTreeManagerDEM() const {
  return mTreeMgrDEM;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

TreeManagerBase* LODVisitor::getTreeManagerIMG() const {
  return mTreeMgrIMG;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int LODVisitor::getFrameCount() const {
  return mFrameCount;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::setFrameCount(int frameCount) {
  mFrameCount = frameCount;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

glm::ivec4 const& LODVisitor::getViewport() const {
  return mViewport;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::setViewport(glm::ivec4 const& vp) {
  mViewport = vp;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

glm::dmat4 const& LODVisitor::getModelview() const {
  return mMatVM;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::setModelview(glm::dmat4 const& m) {
  mMatVM = m;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

glm::dmat4 const& LODVisitor::getProjection() const {
  return mMatP;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::setProjection(glm::dmat4 const& m) {
  mMatP = m;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::setUpdateLOD(bool enable) {
  mUpdateLOD = enable;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool LODVisitor::getUpdateLOD() const {
  return mUpdateLOD;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::setUpdateCulling(bool enable) {
  mUpdateCulling = enable;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool LODVisitor::getUpdateCulling() const {
  return mUpdateCulling;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

std::vector<TileId> const& LODVisitor::getLoadDEM() const {
  return mLoadDEM;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

std::vector<TileId> const& LODVisitor::getLoadIMG() const {
  return mLoadIMG;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

std::vector<RenderData*> const& LODVisitor::getRenderDEM() const {
  return mRenderDEM;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

std::vector<RenderData*> const& LODVisitor::getRenderIMG() const {
  return mRenderIMG;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::pushState() {
  mStackTop += 1;

  // check that stack does not overflow
  assert(mStackTop < static_cast<int>(sMaxStackDepth));
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void LODVisitor::popState() {
  mStackTop -= 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

LODVisitor::StateBase& LODVisitor::getState() {
  // check that stack is valid
  assert(mStackTop >= 0);
  return mStack.at(mStackTop);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

LODVisitor::StateBase const& LODVisitor::getState() const {
  // check that stack is valid
  assert(mStackTop >= 0);
  return mStack.at(mStackTop);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

LODVisitor::LODState& LODVisitor::getLODState(int level /*= -1*/) {
  // check that stack is valid
  assert(mStackTop >= 0);
  return mStack.at(level >= 0 ? level : mStackTop);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

LODVisitor::LODState const& LODVisitor::getLODState(int level /*= -1*/) const {
  // check that stack is valid
  assert(mStackTop >= 0);
  return mStack.at(level >= 0 ? level : mStackTop);
}
////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace csp::lodbodies
