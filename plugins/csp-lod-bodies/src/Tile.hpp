////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
////////////////////////////////////////////////////////////////////////////////////////////////////

// SPDX-FileCopyrightText: German Aerospace Center (DLR) <cosmoscout@dlr.de>
// SPDX-License-Identifier: MIT

#ifndef CSP_LOD_BODIES_TILE_HPP
#define CSP_LOD_BODIES_TILE_HPP

#include "TileBase.hpp"

namespace csp::lodbodies {

/// Concrete class storing data samples of the template argument type T.
template <typename T>
class Tile : public TileBase {
 public:
  using value_type = T;

  explicit Tile(int level, glm::int64 patchIdx, uint32_t resolution);

  Tile(Tile const& other) = delete;
  Tile(Tile&& other)      = delete;

  Tile& operator=(Tile const& other) = delete;
  Tile& operator=(Tile&& other) = delete;

  ~Tile() override;

  static std::type_info const& getStaticTypeId();
  static TileDataType          getStaticDataType();

  std::type_info const& getTypeId() const override;
  TileDataType          getDataType() const override;

  void const* getDataPtr() const override;

  std::vector<T> const& data() const;
  std::vector<T>&       data();

 private:
  std::vector<T> mData;
};

namespace detail {

/// DataTypeTrait<T> is used to map from a type T to the corresponding TileDataType enum value.
/// To support additional data types stored in a Tile add a specialization. Do not forget to add a
/// definition of the static member in Tile.cpp! Only declare base template, define explicit
/// specializations for supported types below - this causes a convenient compile error if an attempt
/// is made to instantiate Tile<T> with an unsupported type T
template <typename T>
struct DataTypeTrait;

template <>
struct DataTypeTrait<float> {
  static TileDataType const value = TileDataType::eElevation;
};

template <>
struct DataTypeTrait<glm::u8vec3> {
  static TileDataType const value = TileDataType::eColor;
};
} // namespace detail

template <typename T>
Tile<T>::Tile(int level, glm::int64 patchIdx, uint32_t resolution)
    : TileBase(level, patchIdx, resolution)
    , mData(resolution * resolution) {
}

template <typename T>
Tile<T>::~Tile() = default;

template <typename T>
std::type_info const& Tile<T>::getStaticTypeId() {
  return typeid(T);
}

template <typename T>
TileDataType Tile<T>::getStaticDataType() {
  return detail::DataTypeTrait<T>::value;
}

template <typename T>
std::type_info const& Tile<T>::getTypeId() const {
  return getStaticTypeId();
}

template <typename T>
TileDataType Tile<T>::getDataType() const {
  return getStaticDataType();
}

template <typename T>
void const* Tile<T>::getDataPtr() const {
  return static_cast<void const*>(mData.data());
}

template <typename T>
std::vector<T> const& Tile<T>::data() const {
  return mData;
}

template <typename T>
std::vector<T>& Tile<T>::data() {
  return mData;
}

} // namespace csp::lodbodies

#endif // CSP_LOD_BODIES_TILE_HPP
