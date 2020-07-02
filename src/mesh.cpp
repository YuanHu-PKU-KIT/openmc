#include "openmc/mesh.h"

#include <algorithm> // for copy, equal, min, min_element
#include <cstddef> // for size_t
#include <cmath>  // for ceil
#include <memory> // for allocator
#include <string>

#include <sstream>
#include <fstream>

#ifdef OPENMC_MPI
#include "mpi.h"
#endif
#include <fmt/core.h> // for fmt
#include "xtensor/xbuilder.hpp"
#include "xtensor/xeval.hpp"
#include "xtensor/xmath.hpp"
#include "xtensor/xsort.hpp"
#include "xtensor/xtensor.hpp"
#include "xtensor/xview.hpp"

#include "openmc/capi.h"
#include "openmc/constants.h"
#include "openmc/error.h"
#include "openmc/hdf5_interface.h"
#include "openmc/message_passing.h"
#include "openmc/search.h"
#include "openmc/settings.h"
#include "openmc/tallies/filter.h"
#include "openmc/xml_interface.h"

namespace openmc {

//==============================================================================
// Global variables
//==============================================================================

namespace model {

std::vector<std::unique_ptr<Mesh>> meshes;
std::unordered_map<int32_t, int32_t> mesh_map;

} // namespace model

//==============================================================================
// Helper functions
//==============================================================================

//! Update an intersection point if the given candidate is closer.
//
//! The first 6 arguments are coordinates for the starting point of a particle
//! and its intersection with a mesh surface.  If the distance between these
//! two points is shorter than the given `min_distance`, then the `r` argument
//! will be updated to match the intersection point, and `min_distance` will
//! also be updated.

inline bool check_intersection_point(double x1, double x0, double y1,
  double y0, double z1, double z0, Position& r, double& min_distance)
{
  double dist = std::pow(x1-x0, 2) + std::pow(y1-y0, 2) + std::pow(z1-z0, 2);
  if (dist < min_distance) {
    r.x = x1;
    r.y = y1;
    r.z = z1;
    min_distance = dist;
    return true;
  }
  return false;
}

//==============================================================================
// Mesh implementation
//==============================================================================

Mesh::Mesh(pugi::xml_node node)
{
  // Copy mesh id
  if (check_for_node(node, "id")) {
    id_ = std::stoi(get_node_value(node, "id"));

    // Check to make sure 'id' hasn't been used
    if (model::mesh_map.find(id_) != model::mesh_map.end()) {
      fatal_error("Two or more meshes use the same unique ID: " +
        std::to_string(id_));
    }
  }
}

//==============================================================================
// Structured Mesh implementation
//==============================================================================

std::string
StructuredMesh::bin_label(int bin) const {
  std::vector<int> ijk(n_dimension_);
  get_indices_from_bin(bin, ijk.data());

  if (n_dimension_ > 2) {
    return fmt::format("Mesh Index ({}, {}, {})", ijk[0], ijk[1], ijk[2]);
  } else if (n_dimension_ > 1) {
    return fmt::format("Mesh Index ({}, {})", ijk[0], ijk[1]);
  } else {
    return fmt::format("Mesh Index ({})", ijk[0]) ;
  }
}

//==============================================================================
// RegularMesh implementation
//==============================================================================

RegularMesh::RegularMesh(pugi::xml_node node)
  : StructuredMesh {node}
{
  // Determine number of dimensions for mesh
  if (check_for_node(node, "dimension")) {
    shape_ = get_node_xarray<int>(node, "dimension");
    int n = n_dimension_ = shape_.size();
    if (n != 1 && n != 2 && n != 3) {
      fatal_error("Mesh must be one, two, or three dimensions.");
    }

    // Check that dimensions are all greater than zero
    if (xt::any(shape_ <= 0)) {
      fatal_error("All entries on the <dimension> element for a tally "
        "mesh must be positive.");
    }
  }

  // Check for lower-left coordinates
  if (check_for_node(node, "lower_left")) {
    // Read mesh lower-left corner location
    lower_left_ = get_node_xarray<double>(node, "lower_left");
  } else {
    fatal_error("Must specify <lower_left> on a mesh.");
  }

  if (check_for_node(node, "width")) {
    // Make sure both upper-right or width were specified
    if (check_for_node(node, "upper_right")) {
      fatal_error("Cannot specify both <upper_right> and <width> on a mesh.");
    }

    width_ = get_node_xarray<double>(node, "width");

    // Check to ensure width has same dimensions
    auto n = width_.size();
    if (n != lower_left_.size()) {
      fatal_error("Number of entries on <width> must be the same as "
        "the number of entries on <lower_left>.");
    }

    // Check for negative widths
    if (xt::any(width_ < 0.0)) {
      fatal_error("Cannot have a negative <width> on a tally mesh.");
    }

    // Set width and upper right coordinate
    upper_right_ = xt::eval(lower_left_ + shape_ * width_);

  } else if (check_for_node(node, "upper_right")) {
    upper_right_ = get_node_xarray<double>(node, "upper_right");

    // Check to ensure width has same dimensions
    auto n = upper_right_.size();
    if (n != lower_left_.size()) {
      fatal_error("Number of entries on <upper_right> must be the "
        "same as the number of entries on <lower_left>.");
    }

    // Check that upper-right is above lower-left
    if (xt::any(upper_right_ < lower_left_)) {
      fatal_error("The <upper_right> coordinates must be greater than "
        "the <lower_left> coordinates on a tally mesh.");
    }

    // Set width
    if (shape_.size() > 0) {
      width_ = xt::eval((upper_right_ - lower_left_) / shape_);
    }
  } else {
    fatal_error("Must specify either <upper_right> and <width> on a mesh.");
  }

  // Make sure lower_left and dimension match
  if (shape_.size() > 0) {
    if (shape_.size() != lower_left_.size()) {
      fatal_error("Number of entries on <lower_left> must be the same "
        "as the number of entries on <dimension>.");
    }

    // Set volume fraction
    volume_frac_ = 1.0/xt::prod(shape_)();
  }
}

int RegularMesh::get_bin(Position r) const
{
  // Loop over the dimensions of the mesh
  for (int i = 0; i < n_dimension_; ++i) {
    // Check for cases where particle is outside of mesh
    if (r[i] < lower_left_[i]) {
      return -1;
    } else if (r[i] > upper_right_[i]) {
      return -1;
    }
  }

  // Determine indices
  std::vector<int> ijk(n_dimension_);
  bool in_mesh;
  get_indices(r, ijk.data(), &in_mesh);
  if (!in_mesh) return -1;

  // Convert indices to bin
  return get_bin_from_indices(ijk.data());
}

int RegularMesh::get_bin_from_indices(const int* ijk) const
{
  switch (n_dimension_) {
  case 1:
    return ijk[0] - 1;
  case 2:
    return (ijk[1] - 1)*shape_[0] + ijk[0] - 1;
  case 3:
    return ((ijk[2] - 1)*shape_[1] + (ijk[1] - 1))*shape_[0] + ijk[0] - 1;
  default:
    throw std::runtime_error{"Invalid number of mesh dimensions"};
  }
}

void RegularMesh::get_indices(Position r, int* ijk, bool* in_mesh) const
{
  // Find particle in mesh
  *in_mesh = true;
  for (int i = 0; i < n_dimension_; ++i) {
    ijk[i] = std::ceil((r[i] - lower_left_[i]) / width_[i]);

    // Check if indices are within bounds
    if (ijk[i] < 1 || ijk[i] > shape_[i]) *in_mesh = false;
  }
}

void RegularMesh::get_indices_from_bin(int bin, int* ijk) const
{
  if (n_dimension_ == 1) {
    ijk[0] = bin + 1;
  } else if (n_dimension_ == 2) {
    ijk[0] = bin % shape_[0] + 1;
    ijk[1] = bin / shape_[0] + 1;
  } else if (n_dimension_ == 3) {
    ijk[0] = bin % shape_[0] + 1;
    ijk[1] = (bin % (shape_[0] * shape_[1])) / shape_[0] + 1;
    ijk[2] = bin / (shape_[0] * shape_[1]) + 1;
  }
}

int RegularMesh::n_bins() const
{
  int n_bins = 1;
  for (auto dim : shape_) n_bins *= dim;
  return n_bins;
}

int RegularMesh::n_surface_bins() const
{
  return 4 * n_dimension_ * n_bins();
}

bool RegularMesh::intersects(Position& r0, Position r1, int* ijk) const
{
  switch(n_dimension_) {
  case 1:
    return intersects_1d(r0, r1, ijk);
  case 2:
    return intersects_2d(r0, r1, ijk);
  case 3:
    return intersects_3d(r0, r1, ijk);
  default:
    throw std::runtime_error{"Invalid number of mesh dimensions."};
  }
}

bool RegularMesh::intersects_1d(Position& r0, Position r1, int* ijk) const
{
  // Copy coordinates of starting point
  double x0 = r0.x;
  double y0 = r0.y;
  double z0 = r0.z;

  // Copy coordinates of ending point
  double x1 = r1.x;
  double y1 = r1.y;
  double z1 = r1.z;

  // Copy coordinates of mesh lower_left and upper_right
  double xm0 = lower_left_[0];
  double xm1 = upper_right_[0];

  double min_dist = INFTY;

  // Check if line intersects left surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm0 && x1 > xm0) || (x0 > xm0 && x1 < xm0)) {
    double yi = y0 + (xm0 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm0 - x0) * (z1 - z0) / (x1 - x0);
    if (check_intersection_point(xm0, x0, yi, yi, zi, zi, r0, min_dist)) {
      ijk[0] = 1;
    }
  }

  // Check if line intersects right surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm1 && x1 > xm1) || (x0 > xm1 && x1 < xm1)) {
    double yi = y0 + (xm1 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm1 - x0) * (z1 - z0) / (x1 - x0);
    if (check_intersection_point(xm1, x0, yi, yi, zi, zi, r0, min_dist)) {
      ijk[0] = shape_[0];
    }
  }

  return min_dist < INFTY;
}

bool RegularMesh::intersects_2d(Position& r0, Position r1, int* ijk) const
{
  // Copy coordinates of starting point
  double x0 = r0.x;
  double y0 = r0.y;
  double z0 = r0.z;

  // Copy coordinates of ending point
  double x1 = r1.x;
  double y1 = r1.y;
  double z1 = r1.z;

  // Copy coordinates of mesh lower_left
  double xm0 = lower_left_[0];
  double ym0 = lower_left_[1];

  // Copy coordinates of mesh upper_right
  double xm1 = upper_right_[0];
  double ym1 = upper_right_[1];

  double min_dist = INFTY;

  // Check if line intersects left surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm0 && x1 > xm0) || (x0 > xm0 && x1 < xm0)) {
    double yi = y0 + (xm0 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm0 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xm0, x0, yi, y0, zi, zi, r0, min_dist)) {
        ijk[0] = 1;
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
      }
    }
  }

  // Check if line intersects back surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym0 && y1 > ym0) || (y0 > ym0 && y1 < ym0)) {
    double xi = x0 + (ym0 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym0 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1) {
      if (check_intersection_point(xi, x0, ym0, y0, zi, zi, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = 1;
      }
    }
  }

  // Check if line intersects right surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm1 && x1 > xm1) || (x0 > xm1 && x1 < xm1)) {
    double yi = y0 + (xm1 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm1 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xm1, x0, yi, y0, zi, zi, r0, min_dist)) {
        ijk[0] = shape_[0];
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
      }
    }
  }

  // Check if line intersects front surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym1 && y1 > ym1) || (y0 > ym1 && y1 < ym1)) {
    double xi = x0 + (ym1 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym1 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1) {
      if (check_intersection_point(xi, x0, ym1, y0, zi, zi, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = shape_[1];
      }
    }
  }

  return min_dist < INFTY;
}

bool RegularMesh::intersects_3d(Position& r0, Position r1, int* ijk) const
{
  // Copy coordinates of starting point
  double x0 = r0.x;
  double y0 = r0.y;
  double z0 = r0.z;

  // Copy coordinates of ending point
  double x1 = r1.x;
  double y1 = r1.y;
  double z1 = r1.z;

  // Copy coordinates of mesh lower_left
  double xm0 = lower_left_[0];
  double ym0 = lower_left_[1];
  double zm0 = lower_left_[2];

  // Copy coordinates of mesh upper_right
  double xm1 = upper_right_[0];
  double ym1 = upper_right_[1];
  double zm1 = upper_right_[2];

  double min_dist = INFTY;

  // Check if line intersects left surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm0 && x1 > xm0) || (x0 > xm0 && x1 < xm0)) {
    double yi = y0 + (xm0 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm0 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xm0, x0, yi, y0, zi, z0, r0, min_dist)) {
        ijk[0] = 1;
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
        ijk[2] = std::ceil((zi - lower_left_[2]) / width_[2]);
      }
    }
  }

  // Check if line intersects back surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym0 && y1 > ym0) || (y0 > ym0 && y1 < ym0)) {
    double xi = x0 + (ym0 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym0 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xi, x0, ym0, y0, zi, z0, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = 1;
        ijk[2] = std::ceil((zi - lower_left_[2]) / width_[2]);
      }
    }
  }

  // Check if line intersects bottom surface -- calculate the intersection
  // point (x,y)
  if ((z0 < zm0 && z1 > zm0) || (z0 > zm0 && z1 < zm0)) {
    double xi = x0 + (zm0 - z0) * (x1 - x0) / (z1 - z0);
    double yi = y0 + (zm0 - z0) * (y1 - y0) / (z1 - z0);
    if (xi >= xm0 && xi < xm1 && yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xi, x0, yi, y0, zm0, z0, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
        ijk[2] = 1;
      }
    }
  }

  // Check if line intersects right surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm1 && x1 > xm1) || (x0 > xm1 && x1 < xm1)) {
    double yi = y0 + (xm1 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm1 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xm1, x0, yi, y0, zi, z0, r0, min_dist)) {
        ijk[0] = shape_[0];
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
        ijk[2] = std::ceil((zi - lower_left_[2]) / width_[2]);
      }
    }
  }

  // Check if line intersects front surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym1 && y1 > ym1) || (y0 > ym1 && y1 < ym1)) {
    double xi = x0 + (ym1 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym1 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xi, x0, ym1, y0, zi, z0, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = shape_[1];
        ijk[2] = std::ceil((zi - lower_left_[2]) / width_[2]);
      }
    }
  }

  // Check if line intersects top surface -- calculate the intersection point
  // (x,y)
  if ((z0 < zm1 && z1 > zm1) || (z0 > zm1 && z1 < zm1)) {
    double xi = x0 + (zm1 - z0) * (x1 - x0) / (z1 - z0);
    double yi = y0 + (zm1 - z0) * (y1 - y0) / (z1 - z0);
    if (xi >= xm0 && xi < xm1 && yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xi, x0, yi, y0, zm1, z0, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
        ijk[2] = shape_[2];
      }
    }
  }

  return min_dist < INFTY;
}

void RegularMesh::bins_crossed(const Particle& p, std::vector<int>& bins,
                               std::vector<double>& lengths) const
{
  // ========================================================================
  // Determine where the track intersects the mesh and if it intersects at all.

  // Copy the starting and ending coordinates of the particle.
  Position last_r {p.r_last_};
  Position r {p.r()};
  Direction u {p.u()};

  // Compute the length of the entire track.
  double total_distance = (r - last_r).norm();

  // While determining if this track intersects the mesh, offset the starting
  // and ending coords by a bit.  This avoid finite-precision errors that can
  // occur when the mesh surfaces coincide with lattice or geometric surfaces.
  Position r0 = last_r + TINY_BIT*u;
  Position r1 = r - TINY_BIT*u;

  // Determine the mesh indices for the starting and ending coords.
  int n = n_dimension_;
  std::vector<int> ijk0(n), ijk1(n);
  bool start_in_mesh;
  get_indices(r0, ijk0.data(), &start_in_mesh);
  bool end_in_mesh;
  get_indices(r1, ijk1.data(), &end_in_mesh);

  // Reset coordinates and check for a mesh intersection if necessary.
  if (start_in_mesh) {
    // The initial coords lie in the mesh, use those coords for tallying.
    r0 = last_r;
  } else {
    // The initial coords do not lie in the mesh.  Check to see if the particle
    // eventually intersects the mesh and compute the relevant coords and
    // indices.
    if (!intersects(r0, r1, ijk0.data())) return;
  }
  r1 = r;

  // The TINY_BIT offsets above mean that the preceding logic cannot always find
  // the correct ijk0 and ijk1 indices. For tracks shorter than 2*TINY_BIT, just
  // assume the track lies in only one mesh bin. These tracks are very short so
  // any error caused by this assumption will be small. It is important that
  // ijk0 values are used rather than ijk1 because the previous logic guarantees
  // ijk0 is a valid mesh bin.
  if (total_distance < 2*TINY_BIT) {
    for (int i = 0; i < n; ++i) ijk1[i] = ijk0[i];
  }

  // ========================================================================
  // Find which mesh cells are traversed and the length of each traversal.

  while (true) {
    if (ijk0 == ijk1) {
      // The track ends in this cell.  Use the particle end location rather
      // than the mesh surface and stop iterating.
      double distance = (r1 - r0).norm();
      bins.push_back(get_bin_from_indices(ijk0.data()));
      lengths.push_back(distance / total_distance);
      break;
    }

    // The track exits this cell.  Determine the distance to each mesh surface.
    std::vector<double> d(n);
    for (int k = 0; k < n; ++k) {
      if (std::fabs(u[k]) < FP_PRECISION) {
        d[k] = INFTY;
      } else if (u[k] > 0) {
        double xyz_cross = lower_left_[k] + ijk0[k] * width_[k];
        d[k] = (xyz_cross - r0[k]) / u[k];
      } else {
        double xyz_cross = lower_left_[k] + (ijk0[k] - 1) * width_[k];
        d[k] = (xyz_cross - r0[k]) / u[k];
      }
    }

    // Pick the closest mesh surface and append this traversal to the output.
    auto j = std::min_element(d.begin(), d.end()) - d.begin();
    double distance = d[j];
    bins.push_back(get_bin_from_indices(ijk0.data()));
    lengths.push_back(distance / total_distance);

    // Translate to the oncoming mesh surface.
    r0 += distance * u;

    // Increment the indices into the next mesh cell.
    if (u[j] > 0.0) {
      ++ijk0[j];
    } else {
      --ijk0[j];
    }

    // If the next indices are invalid, then the track has left the mesh and
    // we are done.
    bool in_mesh = true;
    for (int i = 0; i < n; ++i) {
      if (ijk0[i] < 1 || ijk0[i] > shape_[i]) {
        in_mesh = false;
        break;
      }
    }
    if (!in_mesh) break;
  }
}

void RegularMesh::surface_bins_crossed(const Particle& p,
                                       std::vector<int>& bins) const
{
  // ========================================================================
  // Determine if the track intersects the tally mesh.

  // Copy the starting and ending coordinates of the particle.
  Position r0 {p.r_last_current_};
  Position r1 {p.r()};
  Direction u {p.u()};

  // Determine indices for starting and ending location.
  int n = n_dimension_;
  std::vector<int> ijk0(n), ijk1(n);
  bool start_in_mesh;
  get_indices(r0, ijk0.data(), &start_in_mesh);
  bool end_in_mesh;
  get_indices(r1, ijk1.data(), &end_in_mesh);

  // Check if the track intersects any part of the mesh.
  if (!start_in_mesh) {
    Position r0_copy = r0;
    std::vector<int> ijk0_copy(ijk0);
    if (!intersects(r0_copy, r1, ijk0_copy.data())) return;
  }

  // ========================================================================
  // Find which mesh surfaces are crossed.

  // Calculate number of surface crossings
  int n_cross = 0;
  for (int i = 0; i < n; ++i) n_cross += std::abs(ijk1[i] - ijk0[i]);
  if (n_cross == 0) return;

  // Bounding coordinates
  Position xyz_cross;
  for (int i = 0; i < n; ++i) {
    if (u[i] > 0.0) {
      xyz_cross[i] = lower_left_[i] + ijk0[i] * width_[i];
    } else {
      xyz_cross[i] = lower_left_[i] + (ijk0[i] - 1) * width_[i];
    }
  }

  for (int j = 0; j < n_cross; ++j) {
    // Set the distances to infinity
    Position d {INFTY, INFTY, INFTY};

    // Determine closest bounding surface. We need to treat
    // special case where the cosine of the angle is zero since this would
    // result in a divide-by-zero.
    double distance = INFTY;
    for (int i = 0; i < n; ++i) {
      if (u[i] == 0) {
        d[i] = INFTY;
      } else {
        d[i] = (xyz_cross[i] - r0[i])/u[i];
      }
      distance = std::min(distance, d[i]);
    }

    // Loop over the dimensions
    for (int i = 0; i < n; ++i) {
      // Check whether distance is the shortest distance
      if (distance == d[i]) {

        // Check whether the current indices are within the mesh bounds
        bool in_mesh = true;
        for (int j = 0; j < n; ++j) {
          if (ijk0[j] < 1 || ijk0[j] > shape_[j]) {
            in_mesh = false;
            break;
          }
        }

        // Check whether particle is moving in positive i direction
        if (u[i] > 0) {

          // Outward current on i max surface
          if (in_mesh) {
            int i_surf = 4*i + 3;
            int i_mesh = get_bin_from_indices(ijk0.data());
            int i_bin = 4*n*i_mesh + i_surf - 1;

            bins.push_back(i_bin);
          }

          // Advance position
          ++ijk0[i];
          xyz_cross[i] += width_[i];
          in_mesh = true;
          for (int j = 0; j < n; ++j) {
            if (ijk0[j] < 1 || ijk0[j] > shape_[j]) {
              in_mesh = false;
              break;
            }
          }

          // If the particle crossed the surface, tally the inward current on
          // i min surface
          if (in_mesh) {
            int i_surf = 4*i + 2;
            int i_mesh = get_bin_from_indices(ijk0.data());
            int i_bin = 4*n*i_mesh + i_surf - 1;

            bins.push_back(i_bin);
          }

        } else {
          // The particle is moving in the negative i direction

          // Outward current on i min surface
          if (in_mesh) {
            int i_surf = 4*i + 1;
            int i_mesh = get_bin_from_indices(ijk0.data());
            int i_bin = 4*n*i_mesh + i_surf - 1;

            bins.push_back(i_bin);
          }

          // Advance position
          --ijk0[i];
          xyz_cross[i] -= width_[i];
          in_mesh = true;
          for (int j = 0; j < n; ++j) {
            if (ijk0[j] < 1 || ijk0[j] > shape_[j]) {
              in_mesh = false;
              break;
            }
          }

          // If the particle crossed the surface, tally the inward current on
          // i max surface
          if (in_mesh) {
            int i_surf = 4*i + 4;
            int i_mesh = get_bin_from_indices(ijk0.data());
            int i_bin = 4*n*i_mesh + i_surf - 1;

            bins.push_back(i_bin);
          }
        }
      }
    }

    // Calculate new coordinates
    r0 += distance * u;
  }
}

std::pair<std::vector<double>, std::vector<double>>
RegularMesh::plot(Position plot_ll, Position plot_ur) const
{
  // Figure out which axes lie in the plane of the plot.
  std::array<int, 2> axes {-1, -1};
  if (plot_ur.z == plot_ll.z) {
    axes[0] = 0;
    if (n_dimension_ > 1) axes[1] = 1;
  } else if (plot_ur.y == plot_ll.y) {
    axes[0] = 0;
    if (n_dimension_ > 2) axes[1] = 2;
  } else if (plot_ur.x == plot_ll.x) {
    if (n_dimension_ > 1) axes[0] = 1;
    if (n_dimension_ > 2) axes[1] = 2;
  } else {
    fatal_error("Can only plot mesh lines on an axis-aligned plot");
  }

  // Get the coordinates of the mesh lines along both of the axes.
  std::array<std::vector<double>, 2> axis_lines;
  for (int i_ax = 0; i_ax < 2; ++i_ax) {
    int axis = axes[i_ax];
    if (axis == -1) continue;
    auto& lines {axis_lines[i_ax]};

    double coord = lower_left_[axis];
    for (int i = 0; i < shape_[axis] + 1; ++i) {
      if (coord >= plot_ll[axis] && coord <= plot_ur[axis])
        lines.push_back(coord);
      coord += width_[axis];
    }
  }

  return {axis_lines[0], axis_lines[1]};
}

void RegularMesh::to_hdf5(hid_t group) const
{
  hid_t mesh_group = create_group(group, "mesh " + std::to_string(id_));

  write_dataset(mesh_group, "type", "regular");
  write_dataset(mesh_group, "dimension", shape_);
  write_dataset(mesh_group, "lower_left", lower_left_);
  write_dataset(mesh_group, "upper_right", upper_right_);
  write_dataset(mesh_group, "width", width_);

  close_group(mesh_group);
}

xt::xtensor<double, 1>
RegularMesh::count_sites(const Particle::Bank* bank,
                         int64_t length,
                         bool* outside) const
{
  // Determine shape of array for counts
  std::size_t m = this->n_bins();
  std::vector<std::size_t> shape = {m};

  // Create array of zeros
  xt::xarray<double> cnt {shape, 0.0};
  bool outside_ = false;

  for (int64_t i = 0; i < length; i++) {
    const auto& site = bank[i];

    // determine scoring bin for entropy mesh
    int mesh_bin = get_bin(site.r);

    // if outside mesh, skip particle
    if (mesh_bin < 0) {
      outside_ = true;
      continue;
    }

    // Add to appropriate bin
    cnt(mesh_bin) += site.wgt;
  }

  // Create copy of count data. Since ownership will be acquired by xtensor,
  // std::allocator must be used to avoid Valgrind mismatched free() / delete
  // warnings.
  int total = cnt.size();
  double* cnt_reduced = std::allocator<double>{}.allocate(total);

#ifdef OPENMC_MPI
  // collect values from all processors
  MPI_Reduce(cnt.data(), cnt_reduced, total, MPI_DOUBLE, MPI_SUM, 0,
    mpi::intracomm);

  // Check if there were sites outside the mesh for any processor
  if (outside) {
    MPI_Reduce(&outside_, outside, 1, MPI_C_BOOL, MPI_LOR, 0, mpi::intracomm);
  }
#else
  std::copy(cnt.data(), cnt.data() + total, cnt_reduced);
  if (outside) *outside = outside_;
#endif

  // Adapt reduced values in array back into an xarray
  auto arr = xt::adapt(cnt_reduced, total, xt::acquire_ownership(), shape);
  xt::xarray<double> counts = arr;

  return counts;
}

//==============================================================================
// RectilinearMesh implementation
//==============================================================================

RectilinearMesh::RectilinearMesh(pugi::xml_node node)
  : StructuredMesh {node}
{
  n_dimension_ = 3;

  grid_.resize(3);
  grid_[0] = get_node_array<double>(node, "x_grid");
  grid_[1] = get_node_array<double>(node, "y_grid");
  grid_[2] = get_node_array<double>(node, "z_grid");

  shape_ = {static_cast<int>(grid_[0].size()) - 1,
            static_cast<int>(grid_[1].size()) - 1,
            static_cast<int>(grid_[2].size()) - 1};

  for (const auto& g : grid_) {
    if (g.size() < 2) fatal_error("x-, y-, and z- grids for rectilinear meshes "
      "must each have at least 2 points");
    for (int i = 1; i < g.size(); ++i) {
      if (g[i] <= g[i-1]) fatal_error("Values in for x-, y-, and z- grids for "
        "rectilinear meshes must be sorted and unique.");
    }
  }

  lower_left_ = {grid_[0].front(), grid_[1].front(), grid_[2].front()};
  upper_right_ = {grid_[0].back(), grid_[1].back(), grid_[2].back()};
}

void RectilinearMesh::bins_crossed(const Particle& p, std::vector<int>& bins,
                                   std::vector<double>& lengths) const
{
  // ========================================================================
  // Determine where the track intersects the mesh and if it intersects at all.

  // Copy the starting and ending coordinates of the particle.
  Position last_r {p.r_last_};
  Position r {p.r()};
  Direction u {p.u()};

  // Compute the length of the entire track.
  double total_distance = (r - last_r).norm();

  // While determining if this track intersects the mesh, offset the starting
  // and ending coords by a bit.  This avoid finite-precision errors that can
  // occur when the mesh surfaces coincide with lattice or geometric surfaces.
  Position r0 = last_r + TINY_BIT*u;
  Position r1 = r - TINY_BIT*u;

  // Determine the mesh indices for the starting and ending coords.
  int ijk0[3], ijk1[3];
  bool start_in_mesh;
  get_indices(r0, ijk0, &start_in_mesh);
  bool end_in_mesh;
  get_indices(r1, ijk1, &end_in_mesh);

  // Reset coordinates and check for a mesh intersection if necessary.
  if (start_in_mesh) {
    // The initial coords lie in the mesh, use those coords for tallying.
    r0 = last_r;
  } else {
    // The initial coords do not lie in the mesh.  Check to see if the particle
    // eventually intersects the mesh and compute the relevant coords and
    // indices.
    if (!intersects(r0, r1, ijk0)) return;
  }
  r1 = r;

  // The TINY_BIT offsets above mean that the preceding logic cannot always find
  // the correct ijk0 and ijk1 indices. For tracks shorter than 2*TINY_BIT, just
  // assume the track lies in only one mesh bin. These tracks are very short so
  // any error caused by this assumption will be small. It is important that
  // ijk0 values are used rather than ijk1 because the previous logic guarantees
  // ijk0 is a valid mesh bin.
  if (total_distance < 2*TINY_BIT) {
    for (int i = 0; i < 3; ++i) ijk1[i] = ijk0[i];
  }

  // ========================================================================
  // Find which mesh cells are traversed and the length of each traversal.

  while (true) {
    if (std::equal(ijk0, ijk0+3, ijk1)) {
      // The track ends in this cell.  Use the particle end location rather
      // than the mesh surface and stop iterating.
      double distance = (r1 - r0).norm();
      bins.push_back(get_bin_from_indices(ijk0));
      lengths.push_back(distance / total_distance);
      break;
    }

    // The track exits this cell.  Determine the distance to each mesh surface.
    double d[3];
    for (int k = 0; k < 3; ++k) {
      if (std::fabs(u[k]) < FP_PRECISION) {
        d[k] = INFTY;
      } else if (u[k] > 0) {
        double xyz_cross = grid_[k][ijk0[k]];
        d[k] = (xyz_cross - r0[k]) / u[k];
      } else {
        double xyz_cross = grid_[k][ijk0[k] - 1];
        d[k] = (xyz_cross - r0[k]) / u[k];
      }
    }

    // Pick the closest mesh surface and append this traversal to the output.
    auto j = std::min_element(d, d+3) - d;
    double distance = d[j];
    bins.push_back(get_bin_from_indices(ijk0));
    lengths.push_back(distance / total_distance);

    // Translate to the oncoming mesh surface.
    r0 += distance * u;

    // Increment the indices into the next mesh cell.
    if (u[j] > 0.0) {
      ++ijk0[j];
    } else {
      --ijk0[j];
    }

    // If the next indices are invalid, then the track has left the mesh and
    // we are done.
    bool in_mesh = true;
    for (int i = 0; i < 3; ++i) {
      if (ijk0[i] < 1 || ijk0[i] > shape_[i]) {
        in_mesh = false;
        break;
      }
    }
    if (!in_mesh) break;
  }
}

void RectilinearMesh::surface_bins_crossed(const Particle& p,
                                           std::vector<int>& bins) const
{
  // ========================================================================
  // Determine if the track intersects the tally mesh.

  // Copy the starting and ending coordinates of the particle.
  Position r0 {p.r_last_current_};
  Position r1 {p.r()};
  Direction u {p.u()};

  // Determine indices for starting and ending location.
  int ijk0[3], ijk1[3];
  bool start_in_mesh;
  get_indices(r0, ijk0, &start_in_mesh);
  bool end_in_mesh;
  get_indices(r1, ijk1, &end_in_mesh);

  // If the starting coordinates do not lie in the mesh, compute the coords and
  // mesh indices of the first intersection, and add the bin for this first
  // intersection.  Return if the particle does not intersect the mesh at all.
  if (!start_in_mesh) {
    // Compute the incoming intersection coordinates and indices.
    if (!intersects(r0, r1, ijk0)) return;

    // Determine which surface the particle entered.
    double min_dist = INFTY;
    int i_surf;
    for (int i = 0; i < 3; ++i) {
      if (u[i] > 0.0 && ijk0[i] == 1) {
        double d = std::abs(r0[i] - grid_[i][0]);
        if (d < min_dist) {
          min_dist = d;
          i_surf = 4*i + 2;
        }
      } else if (u[i] < 0.0 && ijk0[i] == shape_[i]) {
        double d = std::abs(r0[i] - grid_[i][shape_[i]]);
        if (d < min_dist) {
          min_dist = d;
          i_surf = 4*i + 4;
        }
      } // u[i] == 0 intentionally skipped
    }

    // Add the incoming current bin.
    int i_mesh = get_bin_from_indices(ijk0);
    int i_bin = 4*3*i_mesh + i_surf - 1;
    bins.push_back(i_bin);
  }

  // If the ending coordinates do not lie in the mesh, compute the coords and
  // mesh indices of the last intersection, and add the bin for this last
  // intersection.
  if (!end_in_mesh) {
    // Compute the outgoing intersection coordinates and indices.
    intersects(r1, r0, ijk1);

    // Determine which surface the particle exited.
    double min_dist = INFTY;
    int i_surf;
    for (int i = 0; i < 3; ++i) {
      if (u[i] > 0.0 && ijk1[i] == shape_[i]) {
        double d = std::abs(r1[i] - grid_[i][shape_[i]]);
        if (d < min_dist) {
          min_dist = d;
          i_surf = 4*i + 3;
        }
      } else if (u[i] < 0.0 && ijk1[i] == 1) {
        double d = std::abs(r1[i] - grid_[i][0]);
        if (d < min_dist) {
          min_dist = d;
          i_surf = 4*i + 1;
        }
      } // u[i] == 0 intentionally skipped
    }

    // Add the outgoing current bin.
    int i_mesh = get_bin_from_indices(ijk1);
    int i_bin = 4*3*i_mesh + i_surf - 1;
    bins.push_back(i_bin);
  }

  // ========================================================================
  // Find which mesh surfaces are crossed.

  // Calculate number of surface crossings
  int n_cross = 0;
  for (int i = 0; i < 3; ++i) n_cross += std::abs(ijk1[i] - ijk0[i]);
  if (n_cross == 0) return;

  // Bounding coordinates
  Position xyz_cross;
  for (int i = 0; i < 3; ++i) {
    if (u[i] > 0.0) {
      xyz_cross[i] = grid_[i][ijk0[i]];
    } else {
      xyz_cross[i] = grid_[i][ijk0[i] - 1];
    }
  }

  for (int j = 0; j < n_cross; ++j) {
    // Set the distances to infinity
    Position d {INFTY, INFTY, INFTY};

    // Determine closest bounding surface. We need to treat
    // special case where the cosine of the angle is zero since this would
    // result in a divide-by-zero.
    double distance = INFTY;
    for (int i = 0; i < 3; ++i) {
      if (u[i] == 0) {
        d[i] = INFTY;
      } else {
        d[i] = (xyz_cross[i] - r0[i])/u[i];
      }
      distance = std::min(distance, d[i]);
    }

    // Loop over the dimensions
    for (int i = 0; i < 3; ++i) {
      // Check whether distance is the shortest distance
      if (distance == d[i]) {

        // Check whether particle is moving in positive i direction
        if (u[i] > 0) {

          // Outward current on i max surface
          int i_surf = 4*i + 3;
          int i_mesh = get_bin_from_indices(ijk0);
          int i_bin = 4*3*i_mesh + i_surf - 1;
          bins.push_back(i_bin);

          // Advance position
          ++ijk0[i];
          xyz_cross[i] = grid_[i][ijk0[i]];

          // Inward current on i min surface
          i_surf = 4*i + 2;
          i_mesh = get_bin_from_indices(ijk0);
          i_bin = 4*3*i_mesh + i_surf - 1;
          bins.push_back(i_bin);

        } else {
          // The particle is moving in the negative i direction

          // Outward current on i min surface
          int i_surf = 4*i + 1;
          int i_mesh = get_bin_from_indices(ijk0);
          int i_bin = 4*3*i_mesh + i_surf - 1;
          bins.push_back(i_bin);

          // Advance position
          --ijk0[i];
          xyz_cross[i] = grid_[i][ijk0[i] - 1];

          // Inward current on i min surface
          i_surf = 4*i + 4;
          i_mesh = get_bin_from_indices(ijk0);
          i_bin = 4*3*i_mesh + i_surf - 1;
          bins.push_back(i_bin);
        }
      }
    }

    // Calculate new coordinates
    r0 += distance * u;
  }
}

int RectilinearMesh::get_bin(Position r) const
{
  // Determine indices
  int ijk[3];
  bool in_mesh;
  get_indices(r, ijk, &in_mesh);
  if (!in_mesh) return -1;

  // Convert indices to bin
  return get_bin_from_indices(ijk);
}

int RectilinearMesh::get_bin_from_indices(const int* ijk) const
{
  return ((ijk[2] - 1)*shape_[1] + (ijk[1] - 1))*shape_[0] + ijk[0] - 1;
}

void RectilinearMesh::get_indices(Position r, int* ijk, bool* in_mesh) const
{
  *in_mesh = true;

  for (int i = 0; i < 3; ++i) {
    if (r[i] < grid_[i].front() || r[i] > grid_[i].back()) {
      ijk[i] = -1;
      *in_mesh = false;
    } else {
      ijk[i] = lower_bound_index(grid_[i].begin(), grid_[i].end(), r[i]) + 1;
    }
  }
}

void RectilinearMesh::get_indices_from_bin(int bin, int* ijk) const
{
  ijk[0] = bin % shape_[0] + 1;
  ijk[1] = (bin % (shape_[0] * shape_[1])) / shape_[0] + 1;
  ijk[2] = bin / (shape_[0] * shape_[1]) + 1;
}

int RectilinearMesh::n_bins() const
{
  return xt::prod(shape_)();
}

int RectilinearMesh::n_surface_bins() const
{
  return 4 * n_dimension_ * n_bins();
}

std::pair<std::vector<double>, std::vector<double>>
RectilinearMesh::plot(Position plot_ll, Position plot_ur) const
{
  // Figure out which axes lie in the plane of the plot.
  std::array<int, 2> axes {-1, -1};
  if (plot_ur.z == plot_ll.z) {
    axes = {0, 1};
  } else if (plot_ur.y == plot_ll.y) {
    axes = {0, 2};
  } else if (plot_ur.x == plot_ll.x) {
    axes = {1, 2};
  } else {
    fatal_error("Can only plot mesh lines on an axis-aligned plot");
  }

  // Get the coordinates of the mesh lines along both of the axes.
  std::array<std::vector<double>, 2> axis_lines;
  for (int i_ax = 0; i_ax < 2; ++i_ax) {
    int axis = axes[i_ax];
    std::vector<double>& lines {axis_lines[i_ax]};

    for (auto coord : grid_[axis]) {
      if (coord >= plot_ll[axis] && coord <= plot_ur[axis])
        lines.push_back(coord);
    }
  }

  return {axis_lines[0], axis_lines[1]};
}

void RectilinearMesh::to_hdf5(hid_t group) const
{
  hid_t mesh_group = create_group(group, "mesh " + std::to_string(id_));

  write_dataset(mesh_group, "type", "rectilinear");
  write_dataset(mesh_group, "x_grid", grid_[0]);
  write_dataset(mesh_group, "y_grid", grid_[1]);
  write_dataset(mesh_group, "z_grid", grid_[2]);

  close_group(mesh_group);
}

bool RectilinearMesh::intersects(Position& r0, Position r1, int* ijk) const
{
  // Copy coordinates of starting point
  double x0 = r0.x;
  double y0 = r0.y;
  double z0 = r0.z;

  // Copy coordinates of ending point
  double x1 = r1.x;
  double y1 = r1.y;
  double z1 = r1.z;

  // Copy coordinates of mesh lower_left
  double xm0 = grid_[0].front();
  double ym0 = grid_[1].front();
  double zm0 = grid_[2].front();

  // Copy coordinates of mesh upper_right
  double xm1 = grid_[0].back();
  double ym1 = grid_[1].back();
  double zm1 = grid_[2].back();

  double min_dist = INFTY;

  // Check if line intersects left surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm0 && x1 > xm0) || (x0 > xm0 && x1 < xm0)) {
    double yi = y0 + (xm0 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm0 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xm0, x0, yi, y0, zi, z0, r0, min_dist)) {
        ijk[0] = 1;
        ijk[1] = lower_bound_index(grid_[1].begin(), grid_[1].end(), yi) + 1;
        ijk[2] = lower_bound_index(grid_[2].begin(), grid_[2].end(), zi) + 1;
      }
    }
  }

  // Check if line intersects back surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym0 && y1 > ym0) || (y0 > ym0 && y1 < ym0)) {
    double xi = x0 + (ym0 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym0 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xi, x0, ym0, y0, zi, z0, r0, min_dist)) {
        ijk[0] = lower_bound_index(grid_[0].begin(), grid_[0].end(), xi) + 1;
        ijk[1] = 1;
        ijk[2] = lower_bound_index(grid_[2].begin(), grid_[2].end(), zi) + 1;
      }
    }
  }

  // Check if line intersects bottom surface -- calculate the intersection
  // point (x,y)
  if ((z0 < zm0 && z1 > zm0) || (z0 > zm0 && z1 < zm0)) {
    double xi = x0 + (zm0 - z0) * (x1 - x0) / (z1 - z0);
    double yi = y0 + (zm0 - z0) * (y1 - y0) / (z1 - z0);
    if (xi >= xm0 && xi < xm1 && yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xi, x0, yi, y0, zm0, z0, r0, min_dist)) {
        ijk[0] = lower_bound_index(grid_[0].begin(), grid_[0].end(), xi) + 1;
        ijk[1] = lower_bound_index(grid_[1].begin(), grid_[1].end(), yi) + 1;
        ijk[2] = 1;
      }
    }
  }

  // Check if line intersects right surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm1 && x1 > xm1) || (x0 > xm1 && x1 < xm1)) {
    double yi = y0 + (xm1 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm1 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xm1, x0, yi, y0, zi, z0, r0, min_dist)) {
        ijk[0] = shape_[0];
        ijk[1] = lower_bound_index(grid_[1].begin(), grid_[1].end(), yi) + 1;
        ijk[2] = lower_bound_index(grid_[2].begin(), grid_[2].end(), zi) + 1;
      }
    }
  }

  // Check if line intersects front surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym1 && y1 > ym1) || (y0 > ym1 && y1 < ym1)) {
    double xi = x0 + (ym1 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym1 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xi, x0, ym1, y0, zi, z0, r0, min_dist)) {
        ijk[0] = lower_bound_index(grid_[0].begin(), grid_[0].end(), xi) + 1;
        ijk[1] = shape_[1];
        ijk[2] = lower_bound_index(grid_[2].begin(), grid_[2].end(), zi) + 1;
      }
    }
  }

  // Check if line intersects top surface -- calculate the intersection point
  // (x,y)
  if ((z0 < zm1 && z1 > zm1) || (z0 > zm1 && z1 < zm1)) {
    double xi = x0 + (zm1 - z0) * (x1 - x0) / (z1 - z0);
    double yi = y0 + (zm1 - z0) * (y1 - y0) / (z1 - z0);
    if (xi >= xm0 && xi < xm1 && yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xi, x0, yi, y0, zm1, z0, r0, min_dist)) {
        ijk[0] = lower_bound_index(grid_[0].begin(), grid_[0].end(), xi) + 1;
        ijk[1] = lower_bound_index(grid_[1].begin(), grid_[1].end(), yi) + 1;
        ijk[2] = shape_[2];
      }
    }
  }

  return min_dist < INFTY;
}

//==============================================================================
// WeightWindowMesh implementation, add by Yuan
//==============================================================================

WeightWindowMesh::WeightWindowMesh(pugi::xml_node node)
  : StructuredMesh {node}
{
  using namespace pugi;
  
  mesh_.n_dimension_ = 3;
  mesh_.grid_.resize(3);

  std::vector<double> coarse_x;
  std::vector<double> coarse_y;
  std::vector<double> coarse_z;
  std::vector<int> shape_x;
  std::vector<int> shape_y;
  std::vector<int> shape_z;
  std::vector<double> width_x;
  std::vector<double> width_y;
  std::vector<double> width_z;
  
  // default parameters
  n_ww = false;                  // flag for neutron use weight window
  p_ww = false;                  // flag for photon use weight window
  user_defined_biasing = false;  // flag for weight biasing in energy
  
  // WWP
  // neutron
  n_upper_ratio    = 5.;         // upper weight window = upper_ratio * lower weight window
  n_survival_ratio = 3.;         // survival weight = survival_ratio * lower weight window
  n_max_split      = 5;          // max number of split particles
  n_multiplier     = 1.;         // multiplier for weight window lower bounds

  // photon
  p_upper_ratio    = 5.;         // upper weight window = upper_ratio * lower weight window
  p_survival_ratio = 3.;         // survival weight = survival_ratio * lower weight window
  p_max_split      = 5;          // max number of split particles
  p_multiplier     = 1.;         // multiplier for weight window lower bounds

  // weight window type
  if (check_for_node(node, "type")) {
    ww_type = std::stoi(get_node_value(node,"type"));
    if (ww_type!=0 && ww_type!=1 )   fatal_error("Must assign weight window input file type, 0 for wwinp file with only lower weight window, 1 for MCNP wwinp file");
  } else { fatal_error("Must assign weight window input file type, 0 for wwinp file with only lower weight window, 1 for MCNP wwinp file");  }

  if (ww_type) {      // ww_type=1 for MCNP wwinp file
    // open wwinp file
    std::ifstream wwfile; 
    wwfile.open("wwinp");
    if(!wwfile.is_open ())  fatal_error("Open weight window file failure, wwinp file does not exist.");

    // parameters for MCNP wwinp file
    // BLOCK 1
    int ww_if, ww_iv, ww_ni, ww_nr;
    std::vector<int> ww_ne;
    double ww_nfx, ww_nfy, ww_nfz, ww_x0, ww_y0, ww_z0, ww_ncx, ww_ncy, ww_ncz, ww_nwg;
    // BLOCK 2 & BLOCK 3 have been defined before
     
    // reading wwinp file, BLOCK 1
    double ww;
    wwfile>>ww_if;
    wwfile>>ww_iv;
    wwfile>>ww_ni;
    wwfile>>ww_nr;
    for (int i=0; i<ww_ni; i++) {   
      wwfile>>ww;
      ww_ne.push_back(ww);
    } 
    wwfile>>ww_nfx;        
    wwfile>>ww_nfy;   
    wwfile>>ww_nfz;   
    wwfile>>ww_x0;    
    wwfile>>ww_y0;    
    wwfile>>ww_z0;    
    wwfile>>ww_ncx;   
    wwfile>>ww_ncy;   
    wwfile>>ww_ncz;   
    wwfile>>ww_nwg;    
    if (ww_nr!=10)  fatal_error("Only cartesian WWINP is currently supported");
      
    mesh_.lower_left_ = { ww_x0, ww_y0, ww_z0 };
    mesh_.shape_ = { static_cast<int>(ww_nfx), static_cast<int>(ww_nfy), static_cast<int>(ww_nfz) };

    // reading wwinp file, BLOCK 2
    for (int i=0; i<ww_ncx; i++) {
      if (i==0) { wwfile>>ww; coarse_x.push_back(ww); }      // the first value is x0, the origin point
      wwfile>>ww;   // nfmx(i)
      shape_x.push_back(ww);
      wwfile>>ww;   // x(i)
      coarse_x.push_back(ww);      
      wwfile>>ww;   // rx(i)        
    }
      
    for (int i=0; i<ww_ncy; i++) {
      if (i==0) { wwfile>>ww; coarse_y.push_back(ww); }      // the first value is y0, the origin point
      wwfile>>ww;   // nfmy(i)
      shape_y.push_back(ww);
      wwfile>>ww;   // y(i)
      coarse_y.push_back(ww);       
      wwfile>>ww;   // ry(i)        
    }

    for (int i=0; i<ww_ncz; i++) {
      if (i==0) { wwfile>>ww; coarse_z.push_back(ww); }      // the first value is z0, the origin point
      wwfile>>ww;   // nfmz(i)
      shape_z.push_back(ww);
      wwfile>>ww;   // z(i)
      coarse_z.push_back(ww);      
      wwfile>>ww;   // rz(i)        
    }

    mesh_.upper_right_ = { coarse_x.back(), coarse_y.back(), coarse_z.back() };
     
    // locations of fine mesh in x direction
    for (int i=0; i<coarse_x.size()-1; i++) {
      width_x.push_back( (coarse_x.at(i+1)-coarse_x.at(i))/shape_x.at(i) );
      for (int j=0; j<shape_x.at(i); j++)  mesh_.grid_[0].push_back( coarse_x.at(i)+width_x.back()*j );
    }
    mesh_.grid_[0].push_back(coarse_x.back());

    // locations of fine meshes in y direction
    for (int i=0; i<coarse_y.size()-1; i++) {
      width_y.push_back( (coarse_y.at(i+1)-coarse_y.at(i))/shape_y.at(i) );
      for (int j=0; j<shape_y.at(i); j++)  mesh_.grid_[1].push_back( coarse_y.at(i)+width_y.back()*j );
    }
    mesh_.grid_[1].push_back(coarse_y.back());

    // locations of fine meshes in z direction
    for (int i=0; i<coarse_z.size()-1; i++) {
      width_z.push_back( (coarse_z.at(i+1)-coarse_z.at(i))/shape_z.at(i) );
      for (int j=0; j<shape_z.at(i); j++)  mesh_.grid_[2].push_back( coarse_z.at(i)+width_z.back()*j );
    }
    mesh_.grid_[2].push_back(coarse_z.back());
 
    // reading wwinp file, BLOCK 3
    // energy group & weight window for neutron
    if (ww_ne.at(0) != 0 ) {
      n_ww = true;  // turn on the flag
      
      // energy group
      n_energy_group.push_back(0);
      for (int j=0; j<ww_ne.at(0); j++) {
        wwfile>>ww;
        n_energy_group.push_back(ww*1e6);   
      }  
        
      // weight window
      for (int j=0; j<ww_ne.at(0); j++) {
        for (int kk=0; kk<ww_nfx*ww_nfy*ww_nfz; kk++) { 
          wwfile>>ww;
          n_ww_lower.push_back(ww);   
        }
      }    
    }
      
    // energy group & weight window for photon
    if ( ww_ni == 2 && ww_ne.at(1) !=0 ) {
      p_ww = true;  // turn on the flag
        
      // energy group
      p_energy_group.push_back(0);
      for (int j=0; j<ww_ne.at(1); j++) {
        wwfile>>ww;
        p_energy_group.push_back(ww*1e6);   
      }  
        
      // weight window
      for (int j=0; j<ww_ne.at(1); j++) {
        for (int kk=0; kk<ww_nfx*ww_nfy*ww_nfz; kk++) { 
          wwfile>>ww;
          p_ww_lower.push_back(ww);   
        }
      }   
    }
    wwfile.close();  
  } else {        // ww_type=0 for wwinp file with only weight window
    // Lower-left coordinates for mesh
    if (check_for_node(node, "origin")) {
      auto value = get_node_xarray<double>(node, "origin");
      if (value.size() != 3)      fatal_error("The origin point must be 3 dimension.");
      mesh_.lower_left_ = { value.at(0), value.at(1), value.at(2) };
    }

    // Locations of the coarse meshes in x direction
    if (check_for_node(node, "xmesh")) {
      auto value = get_node_xarray<double>(node, "xmesh");
      for (int i=0; i<value.size(); i++)  coarse_x.push_back(value.at(i));
    }

    // Number of fine meshes within corresponding coarse meshes in x direction
    if (check_for_node(node, "xints")) {
      auto value = get_node_xarray<int>(node, "xints");
      for (int i=0; i<value.size(); i++)  shape_x.push_back(value.at(i));
      if (coarse_x.size() != shape_x.size())      fatal_error("The number of xmesh and xints must be same.");
    }

    // locations of fine meshes in x direction
    for (int i=0; i<coarse_x.size(); i++) {
      if (i==0) { 
        width_x.push_back( (coarse_x.at(0)-mesh_.lower_left_[0])/shape_x.at(0) );
        for (int j=0; j<shape_x.at(i); j++)  mesh_.grid_[0].push_back( mesh_.lower_left_[0]+width_x.back()*j );
      } else {
        width_x.push_back( (coarse_x.at(i)-coarse_x.at(i-1))/shape_x.at(i) );
        for (int j=0; j<shape_x.at(i); j++)  mesh_.grid_[0].push_back( coarse_x.at(i-1)+width_x.back()*j );
      }
    }
    mesh_.grid_[0].push_back(coarse_x.back());
    
    // Locations of the coarse meshes in y direction
    if (check_for_node(node, "ymesh")) {
      auto value = get_node_xarray<double>(node, "ymesh");
      for (int i=0; i<value.size(); i++)  coarse_y.push_back(value.at(i));
    }

    // Number of fine meshes within corresponding coarse meshes in y direction
    if (check_for_node(node, "yints")) {
      auto value = get_node_xarray<int>(node, "yints");
      for (int i=0; i<value.size(); i++)  shape_y.push_back(value.at(i));
      if (coarse_y.size() != shape_y.size())      fatal_error("The number of ymesh and yints must be same.");
    }

    // locations of fine meshes in y direction
    for (int i=0; i<coarse_y.size(); i++) {
      if (i==0) { 
        width_y.push_back( (coarse_y.at(0)-mesh_.lower_left_[1])/shape_y.at(0) );
        for (int j=0; j<shape_y.at(i); j++)  mesh_.grid_[1].push_back( mesh_.lower_left_[1]+width_y.back()*j );
      } else {
        width_y.push_back( (coarse_y.at(i)-coarse_y.at(i-1))/shape_y.at(i) );
        for (int j=0; j<shape_y.at(i); j++) mesh_. grid_[1].push_back( coarse_y.at(i-1)+width_y.back()*j );
      }
    }
    mesh_.grid_[1].push_back(coarse_y.back());
    
    // Locations of the coarse meshes in z direction
    if (check_for_node(node, "zmesh")) {
      auto value = get_node_xarray<double>(node, "zmesh");
      for (int i=0; i<value.size(); i++)  coarse_z.push_back(value.at(i));
    }

    // Number of fine meshes within corresponding coarse meshes in z direction
    if (check_for_node(node, "zints")) {
      auto value = get_node_xarray<int>(node, "zints");
      for (int i=0; i<value.size(); i++)  shape_z.push_back(value.at(i));
      if (coarse_z.size() != shape_z.size())      fatal_error("The number of zmesh and zints must be same.");
    }

    // locations of fine meshes in z direction
    for (int i=0; i<coarse_z.size(); i++) {
      if (i==0) { 
        width_z.push_back( (coarse_z.at(0)-mesh_.lower_left_[2])/shape_z.at(0) );
        for (int j=0; j<shape_z.at(i); j++)  mesh_.grid_[2].push_back( mesh_.lower_left_[2]+width_z.back()*j );
      } else {
        width_z.push_back( (coarse_z.at(i)-coarse_z.at(i-1))/shape_z.at(i) );
        for (int j=0; j<shape_z.at(i); j++)  mesh_.grid_[2].push_back( coarse_z.at(i-1)+width_z.back()*j );
      }
    }
    mesh_.grid_[2].push_back(coarse_z.back());
      
    mesh_.upper_right_ = { coarse_x.back(), coarse_y.back(), coarse_z.back() };
    
    // Energy group
    if (check_for_node(node, "energy")) {
      xml_node weightwindow_energy = node.child("energy");
      // energy group for neutron
      if (check_for_node(weightwindow_energy,"neutron")) {
        n_ww=true; // turn on the flag
        auto value = get_node_xarray<double>(weightwindow_energy, "neutron");
        n_energy_group.push_back(0);
        for (int i=0; i<value.size(); i++)  n_energy_group.push_back(value.at(i));
      }
             
      // energy group for photon
      if (check_for_node(weightwindow_energy,"photon")) {
        if (!settings::photon_transport) { fatal_error("Photon transport is not on but weight window for photon is used"); }  // check if photon transport is on
        p_ww=true; // turn on the flag
        auto value = get_node_xarray<double>(weightwindow_energy, "photon");
        p_energy_group.push_back(0);
        for (int i=0; i<value.size(); i++)  p_energy_group.push_back(value.at(i));
      }   
    } else { fatal_error("Must assign energy group for weight window"); }  

    // read wwinp file
    std::ifstream wwfile; 
    wwfile.open("wwinp");
    if(!wwfile.is_open ())  fatal_error("Open weight window file failure, wwinp file does not exist.");

    double ww=0.0;
    mesh_.shape_ = {0, 0, 0};
    for (int i=0; i<shape_x.size(); i++) mesh_.shape_[0]=+shape_x.at(i); 
    for (int j=0; j<shape_y.size(); j++) mesh_.shape_[1]=+shape_y.at(j);
    for (int k=0; k<shape_z.size(); k++) mesh_.shape_[2]=+shape_z.at(k);
      
    if (n_ww) {
      for (int i=0; i<mesh_.shape_[0]*mesh_.shape_[1]*mesh_.shape_[2]*(n_energy_group.size()-1); i++) {   
        wwfile>>ww;
        n_ww_lower.push_back(ww);
      }  
    } 
      
    if (p_ww) {
      for (int i=0; i<mesh_.shape_[0]*mesh_.shape_[1]*mesh_.shape_[2]*(p_energy_group.size()-1); i++) {   
        wwfile>>ww;
        p_ww_lower.push_back(ww);
      }  
    } 
    wwfile.close();
  }
    
  // WWP-- weight window parameters
  // neutron
  if (check_for_node(node, "neutron_parameters")) {
    xml_node neutron_wwp = node.child("neutron_parameters");
        
    // upper weight window
    if (check_for_node(neutron_wwp,"upper")) {
      n_upper_ratio = std::stod(get_node_value(neutron_wwp,"upper"));
      if (n_upper_ratio<2 )   fatal_error("Ratio of upper/lower weight window must bigger than 2.");
    }
      
    // survival weight window
    if (check_for_node(neutron_wwp, "survival")) {
      n_survival_ratio = std::stod(get_node_value(neutron_wwp,"survival"));
      if (n_survival_ratio<=1 || n_survival_ratio>= n_upper_ratio )  
        fatal_error("Ratio of survival/lower weight window must bigger than 1 and less than upper/lower ratio.");
    }
      
    // max split
    if (check_for_node(neutron_wwp, "max_split")) {
      n_max_split = std::stoi(get_node_value(neutron_wwp,"max_split"));
      if (n_max_split<=1 )   fatal_error("Max split number must bigger than 1.");
    }
      
    // multiplier for weight window lower bounds
    if (check_for_node(neutron_wwp, "multiplier")) {
      n_multiplier = std::stod(get_node_value(neutron_wwp,"multiplier"));
      if (n_multiplier<=0 )   fatal_error("Multiplier for lower weight window must bigger than 0.");
    }      
  }
  // neutron      
        
  // photon
  if (check_for_node(node, "photon_parameters")) {
    xml_node photon_wwp = node.child("photon_parameters");
        
    // upper weight window
    if (check_for_node(photon_wwp,"upper")) {
      p_upper_ratio = std::stod(get_node_value(photon_wwp,"upper"));
      if (p_upper_ratio<2 )   fatal_error("Ratio of upper/lower weight window must bigger than 2.");
    }
      
    // survival weight window
    if (check_for_node(photon_wwp, "survival")) {
      p_survival_ratio = std::stod(get_node_value(photon_wwp,"survival"));
      if (p_survival_ratio<=1 || p_survival_ratio>= p_upper_ratio )  
        fatal_error("Ratio of survival/lower weight window must bigger than 1 and less than upper/lower ratio.");
    }
      
    // max split
    if (check_for_node(photon_wwp, "max_split")) {
      p_max_split = std::stoi(get_node_value(photon_wwp,"max_split"));
      if (p_max_split<=1 )   fatal_error("Max split number must bigger than 1.");
    }
      
    // multiplier for weight window lower bounds
    if (check_for_node(photon_wwp, "multiplier")) {
      p_multiplier = std::stod(get_node_value(photon_wwp,"multiplier"));
      if (p_multiplier<=0 )   fatal_error("Multiplier for lower weight window must bigger than 0.");
    }     
  }
  // photon
    
  // user defined source weight biasing in energy
  if (check_for_node(node, "user_defined_biasing")) {
    user_defined_biasing = true;
    xml_node node_user_defined_biasing = node.child("user_defined_biasing");
    
    // energy group for source weight biasing
    if (check_for_node(node_user_defined_biasing, "biasing_energy")) {
      auto value = get_node_xarray<double>(node_user_defined_biasing, "biasing_energy");
      biasing_energy.push_back(0.);
      for (int i=0; i<value.size(); i++)   biasing_energy.push_back(value.at(i));
    } else {
      fatal_error("Must provide energy group for biasing.");
    }

    // origin possibility for each energy group
    if (check_for_node(node_user_defined_biasing, "origin_possibility")) {
      auto value = get_node_xarray<double>(node_user_defined_biasing, "origin_possibility");
      if (value.size()!=biasing_energy.size()-1)  fatal_error("Origin_possibility and biasing_energy must have the same number of input.");
      origin_possibility.push_back(0.);
      cumulative_possibility.push_back(0.);
      for (int i=0; i<value.size(); i++) {
        origin_possibility.push_back(value.at(i));
        cumulative_possibility.push_back(0.);
      }
      // normalization
      double total_possibility=0.0;
      for (int i=0; i<origin_possibility.size(); i++)   total_possibility += origin_possibility.at(i);
      for (int i=1; i<origin_possibility.size(); i++)  {   
        origin_possibility.at(i) = origin_possibility.at(i) / total_possibility;
        cumulative_possibility.at(i) = cumulative_possibility.at(i-1) + origin_possibility.at(i);
      }
    } else {
      fatal_error("Must provide origin_possibility for each group.");
    }
      
    // biasing weight for each energy group
    if (check_for_node(node_user_defined_biasing, "biasing")) {
      auto value = get_node_xarray<double>(node_user_defined_biasing, "biasing");
      if (value.size()!=biasing_energy.size()-1)  fatal_error("Biasing and biasing_energy must have the same number of input.");
      biasing.push_back(0);
      cumulative_biasing.push_back(0);
      for (int i=0; i<value.size(); i++) {
        biasing.push_back(value.at(i));
        cumulative_biasing.push_back(0);
      }
      // normalization
      double total_possibility=0.0;
      for (int i=0; i<biasing.size(); i++)   total_possibility += biasing.at(i);
      for (int i=1; i<biasing.size(); i++) {
        biasing.at(i) = biasing.at(i) / total_possibility;
        cumulative_biasing.at(i) = cumulative_biasing.at(i-1) + biasing.at(i);
      }
    } else {
      fatal_error("Must provide biasing for each energy group.");
    }
  }

  for (const auto& g : mesh_.grid_) {
    if (g.size() < 2) fatal_error("x-, y-, and z- grids for rectilinear meshes "
      "must each have at least 2 points");
    for (int i = 1; i < g.size(); ++i) {
      if (g[i] <= g[i-1]) fatal_error("Values in for x-, y-, and z- grids for "
        "rectilinear meshes must be sorted and unique.");
    }
  }
}
  
//! source weight biasing in energy
void WeightWindowMesh::weight_biasing(Particle::Bank& site, uint64_t* seed) 
{
  int i=0;
  double random_number=prn(seed);
  for (i=0; i<cumulative_biasing.size()-1; i++) 
    if ( cumulative_biasing.at(i) <= random_number && random_number < cumulative_biasing.at(i+1) )  break;
  site.E = biasing_energy.at(i) + ( biasing_energy.at(i+1)-biasing_energy.at(i) ) * prn(seed);
  site.wgt = site.wgt * origin_possibility.at(i+1) / biasing.at(i+1);
}
  
//==============================================================================
// Helper functions for the C API
//==============================================================================

int
check_mesh(int32_t index)
{
  if (index < 0 || index >= model::meshes.size()) {
    set_errmsg("Index in meshes array is out of bounds.");
    return OPENMC_E_OUT_OF_BOUNDS;
  }
  return 0;
}

int
check_regular_mesh(int32_t index, RegularMesh** mesh)
{
  if (int err = check_mesh(index)) return err;
  *mesh = dynamic_cast<RegularMesh*>(model::meshes[index].get());
  if (!*mesh) {
    set_errmsg("This function is only valid for regular meshes.");
    return OPENMC_E_INVALID_TYPE;
  }
  return 0;
}

//==============================================================================
// C API functions
//==============================================================================

RegularMesh* get_regular_mesh(int32_t index) {
  return dynamic_cast<RegularMesh*>(model::meshes[index].get());
}

//! Extend the meshes array by n elements
extern "C" int
openmc_extend_meshes(int32_t n, int32_t* index_start, int32_t* index_end)
{
  if (index_start) *index_start = model::meshes.size();
  for (int i = 0; i < n; ++i) {
    model::meshes.push_back(std::make_unique<RegularMesh>());
  }
  if (index_end) *index_end = model::meshes.size() - 1;

  return 0;
}

//! Return the index in the meshes array of a mesh with a given ID
extern "C" int
openmc_get_mesh_index(int32_t id, int32_t* index)
{
  auto pair = model::mesh_map.find(id);
  if (pair == model::mesh_map.end()) {
    set_errmsg("No mesh exists with ID=" + std::to_string(id) + ".");
    return OPENMC_E_INVALID_ID;
  }
  *index = pair->second;
  return 0;
}

// Return the ID of a mesh
extern "C" int
openmc_mesh_get_id(int32_t index, int32_t* id)
{
  if (int err = check_mesh(index)) return err;
  *id = model::meshes[index]->id_;
  return 0;
}

//! Set the ID of a mesh
extern "C" int
openmc_mesh_set_id(int32_t index, int32_t id)
{
  if (int err = check_mesh(index)) return err;
  model::meshes[index]->id_ = id;
  model::mesh_map[id] = index;
  return 0;
}

//! Get the dimension of a mesh
extern "C" int
openmc_mesh_get_dimension(int32_t index, int** dims, int* n)
{
  RegularMesh* mesh;
  if (int err = check_regular_mesh(index, &mesh)) return err;
  *dims = mesh->shape_.data();
  *n = mesh->n_dimension_;
  return 0;
}

//! Set the dimension of a mesh
extern "C" int
openmc_mesh_set_dimension(int32_t index, int n, const int* dims)
{
  RegularMesh* mesh;
  if (int err = check_regular_mesh(index, &mesh)) return err;

  // Copy dimension
  std::vector<std::size_t> shape = {static_cast<std::size_t>(n)};
  mesh->shape_ = xt::adapt(dims, n, xt::no_ownership(), shape);
  mesh->n_dimension_ = mesh->shape_.size();
  return 0;
}

//! Get the mesh parameters
extern "C" int
openmc_mesh_get_params(int32_t index, double** ll, double** ur, double** width, int* n)
{
  RegularMesh* m;
  if (int err = check_regular_mesh(index, &m)) return err;

  if (m->lower_left_.dimension() == 0) {
    set_errmsg("Mesh parameters have not been set.");
    return OPENMC_E_ALLOCATE;
  }

  *ll = m->lower_left_.data();
  *ur = m->upper_right_.data();
  *width = m->width_.data();
  *n = m->n_dimension_;
  return 0;
}

//! Set the mesh parameters
extern "C" int
openmc_mesh_set_params(int32_t index, int n, const double* ll, const double* ur,
                       const double* width)
{
  RegularMesh* m;
  if (int err = check_regular_mesh(index, &m)) return err;

  std::vector<std::size_t> shape = {static_cast<std::size_t>(n)};
  if (ll && ur) {
    m->lower_left_ = xt::adapt(ll, n, xt::no_ownership(), shape);
    m->upper_right_ = xt::adapt(ur, n, xt::no_ownership(), shape);
    m->width_ = (m->upper_right_ - m->lower_left_) / m->shape_;
  } else if (ll && width) {
    m->lower_left_ = xt::adapt(ll, n, xt::no_ownership(), shape);
    m->width_ = xt::adapt(width, n, xt::no_ownership(), shape);
    m->upper_right_ = m->lower_left_ + m->shape_ * m->width_;
  } else if (ur && width) {
    m->upper_right_ = xt::adapt(ur, n, xt::no_ownership(), shape);
    m->width_ = xt::adapt(width, n, xt::no_ownership(), shape);
    m->lower_left_ = m->upper_right_ - m->shape_ * m->width_;
  } else {
    set_errmsg("At least two parameters must be specified.");
    return OPENMC_E_INVALID_ARGUMENT;
  }

  return 0;
}

#ifdef DAGMC

UnstructuredMesh::UnstructuredMesh(pugi::xml_node node) : Mesh(node)
{
  // unstructured always assumed to be 3D
  n_dimension_ = 3;

  // check the mesh type
  if (check_for_node(node, "type")) {
    auto temp = get_node_value(node, "type", true, true);
    if (temp != "unstructured") {
      fatal_error("Invalid mesh type: " + temp);
    }
  }

  // get the filename of the unstructured mesh to load
  if (check_for_node(node, "filename")) {
    filename_ = get_node_value(node, "filename");
  } else {
    fatal_error("No filename supplied for unstructured mesh with ID: " +
                std::to_string(id_));
  }

  // create MOAB instance
  mbi_ = std::make_unique<moab::Core>();
  // load unstructured mesh file
  moab::ErrorCode rval = mbi_->load_file(filename_.c_str());
  if (rval != moab::MB_SUCCESS) {
    fatal_error("Failed to load the unstructured mesh file: " + filename_);
  }

  // set member range of tetrahedral entities
  rval = mbi_->get_entities_by_dimension(0, n_dimension_, ehs_);
  if (rval != moab::MB_SUCCESS) {
    fatal_error("Failed to get all tetrahedral elements");
  }

  if (!ehs_.all_of_type(moab::MBTET)) {
    warning("Non-tetrahedral elements found in unstructured "
            "mesh file: " + filename_);
  }

  // make an entity set for all tetrahedra
  // this is used for convenience later in output
  rval = mbi_->create_meshset(moab::MESHSET_SET, tetset_);
  if (rval != moab::MB_SUCCESS) {
    fatal_error("Failed to create an entity set for the tetrahedral elements");
  }

  rval = mbi_->add_entities(tetset_, ehs_);
  if (rval != moab::MB_SUCCESS) {
    fatal_error("Failed to add tetrahedra to an entity set.");
  }

  // build acceleration data structures
  compute_barycentric_data(ehs_);
  build_kdtree(ehs_);
}

void
UnstructuredMesh::build_kdtree(const moab::Range& all_tets)
{
  moab::Range all_tris;
  int adj_dim = 2;
  moab::ErrorCode rval = mbi_->get_adjacencies(all_tets,
                                               adj_dim,
                                               true,
                                               all_tris,
                                               moab::Interface::UNION);
  if (rval != moab::MB_SUCCESS) {
    fatal_error("Failed to get adjacent triangles for tets");
  }

  if (!all_tris.all_of_type(moab::MBTRI)) {
    warning("Non-triangle elements found in tet adjacencies in "
            "unstructured mesh file: " + filename_);
  }

  // combine into one range
  moab::Range all_tets_and_tris;
  all_tets_and_tris.merge(all_tets);
  all_tets_and_tris.merge(all_tris);

  // create a kd-tree instance
  kdtree_ = std::make_unique<moab::AdaptiveKDTree>(mbi_.get());

  // build the tree
  rval = kdtree_->build_tree(all_tets_and_tris, &kdtree_root_);
  if (rval != moab::MB_SUCCESS) {
    fatal_error("Failed to construct KDTree for the "
                "unstructured mesh file: " + filename_);
  }
}

void
UnstructuredMesh::intersect_track(const moab::CartVect& start,
                                  const moab::CartVect& dir,
                                  double track_len,
                                  std::vector<double>& hits) const {
  hits.clear();

  moab::ErrorCode rval;
  std::vector<moab::EntityHandle> tris;
  // get all intersections with triangles in the tet mesh
  // (distances are relative to the start point, not the previous intersection)
  rval = kdtree_->ray_intersect_triangles(kdtree_root_,
                                          FP_COINCIDENT,
                                          dir.array(),
                                          start.array(),
                                          tris,
                                          hits,
                                          0,
                                          track_len);
  if (rval != moab::MB_SUCCESS) {
    fatal_error("Failed to compute intersections on unstructured mesh: " + filename_);
  }

  // remove duplicate intersection distances
  std::unique(hits.begin(), hits.end());

  // sorts by first component of std::pair by default
  std::sort(hits.begin(), hits.end());
}

void
UnstructuredMesh::bins_crossed(const Particle& p,
                               std::vector<int>& bins,
                               std::vector<double>& lengths) const
{
  Position last_r{p.r_last_};
  Position r{p.r()};
  Direction u{p.u()};
  u /= u.norm();
  moab::CartVect r0(last_r.x, last_r.y, last_r.z);
  moab::CartVect r1(r.x, r.y, r.z);
  moab::CartVect dir(u.x, u.y, u.z);

  double track_len = (r1 - r0).length();

  r0 -= TINY_BIT * dir;
  r1 += TINY_BIT * dir;

  std::vector<double> hits;
  intersect_track(r0, dir, track_len, hits);

  bins.clear();
  lengths.clear();

  // if there are no intersections the track may lie entirely
  // within a single tet. If this is the case, apply entire
  // score to that tet and return.
  if (hits.size() == 0) {
    Position midpoint = last_r + u * (track_len * 0.5);
    int bin = this->get_bin(midpoint);
    if (bin != -1) {
      bins.push_back(bin);
      lengths.push_back(1.0);
    }
    return;
  }

  // for each segment in the set of tracks, try to look up a tet
  // at the midpoint of the segment
  Position current = last_r;
  double last_dist = 0.0;
  for (const auto& hit : hits) {
    // get the segment length
    double segment_length = hit - last_dist;
    last_dist = hit;
    // find the midpoint of this segment
    Position midpoint = current + u * (segment_length * 0.5);
    // try to find a tet for this position
    int bin = this->get_bin(midpoint);

    // determine the start point for this segment
    current = last_r + u * hit;

    if (bin == -1) {
      continue;
    }

    bins.push_back(bin);
    lengths.push_back(segment_length / track_len);

  }

  // tally remaining portion of track after last hit if
  // the last segment of the track is in the mesh but doesn't
  // reach the other side of the tet
  if (hits.back() < track_len) {
    Position segment_start = last_r + u * hits.back();
    double segment_length = track_len - hits.back();
    Position midpoint = segment_start + u * (segment_length * 0.5);
    int bin = this->get_bin(midpoint);
    if (bin != -1) {
      bins.push_back(bin);
      lengths.push_back(segment_length / track_len);
    }
  }
};

moab::EntityHandle
UnstructuredMesh::get_tet(const Position& r) const
{
  moab::CartVect pos(r.x, r.y, r.z);
  // find the leaf of the kd-tree for this position
  moab::AdaptiveKDTreeIter kdtree_iter;
  moab::ErrorCode rval = kdtree_->point_search(pos.array(), kdtree_iter);
  if (rval != moab::MB_SUCCESS) { return 0; }

  // retrieve the tet elements of this leaf
  moab::EntityHandle leaf = kdtree_iter.handle();
  moab::Range tets;
  rval = mbi_->get_entities_by_dimension(leaf, 3, tets, false);
  if (rval != moab::MB_SUCCESS) {
    warning("MOAB error finding tets.");
  }

  // loop over the tets in this leaf, returning the containing tet if found
  for (const auto& tet : tets) {
    if (point_in_tet(pos, tet)) {
      return tet;
    }
  }

  // if no tet is found, return an invalid handle
  return 0;
}

double UnstructuredMesh::tet_volume(moab::EntityHandle tet) const {
 std::vector<moab::EntityHandle> conn;
 moab::ErrorCode rval = mbi_->get_connectivity(&tet, 1, conn);
 if (rval != moab::MB_SUCCESS) {
   fatal_error("Failed to get tet connectivity");
 }

 moab::CartVect p[4];
 rval = mbi_->get_coords(conn.data(), conn.size(), p[0].array());
 if (rval != moab::MB_SUCCESS) {
   fatal_error("Failed to get tet coords");
 }

 return 1.0 / 6.0 * (((p[1] - p[0]) * (p[2] - p[0])) % (p[3] - p[0]));
}

void UnstructuredMesh::surface_bins_crossed(const Particle& p, std::vector<int>& bins) const {
  // TODO: Implement triangle crossings here
  throw std::runtime_error{"Unstructured mesh surface tallies are not implemented."};
}

int
UnstructuredMesh::get_bin(Position r) const {
  moab::EntityHandle tet = get_tet(r);
  if (tet == 0) {
    return -1;
  } else {
    return get_bin_from_ent_handle(tet);
  }
}

void
UnstructuredMesh::compute_barycentric_data(const moab::Range& tets) {
  moab::ErrorCode rval;

  baryc_data_.clear();
  baryc_data_.resize(tets.size());

  // compute the barycentric data for each tet element
  // and store it as a 3x3 matrix
  for (auto& tet : tets) {
    std::vector<moab::EntityHandle> verts;
    rval = mbi_->get_connectivity(&tet, 1, verts);
    if (rval != moab::MB_SUCCESS) {
      fatal_error("Failed to get connectivity of tet on umesh: " + filename_);
    }

    moab::CartVect p[4];
    rval = mbi_->get_coords(verts.data(), verts.size(), p[0].array());
    if (rval != moab::MB_SUCCESS) {
      fatal_error("Failed to get coordinates of a tet in umesh: " + filename_);
    }

    moab::Matrix3 a(p[1] - p[0], p[2] - p[0], p[3] - p[0], true);

    // invert now to avoid this cost later
    a = a.transpose().inverse();
    baryc_data_.at(get_bin_from_ent_handle(tet)) = a;
  }
}

void
UnstructuredMesh::to_hdf5(hid_t group) const
{
    hid_t mesh_group = create_group(group, fmt::format("mesh {}", id_));

    write_dataset(mesh_group, "type", "unstructured");
    write_dataset(mesh_group, "filename", filename_);

    // write volume and centroid of each tet
    std::vector<double> tet_vols;
    xt::xtensor<double, 2> centroids({ehs_.size(), 3});
    for (int i = 0; i < ehs_.size(); i++) {
      const auto& eh = ehs_[i];
      tet_vols.emplace_back(this->tet_volume(eh));
      Position c = this->centroid(eh);
      xt::view(centroids, i, xt::all()) = xt::xarray<double>({c.x, c.y, c.z});
    }

    write_dataset(mesh_group, "volumes", tet_vols);
    write_dataset(mesh_group, "centroids", centroids);

    close_group(mesh_group);
}

bool
UnstructuredMesh::point_in_tet(const moab::CartVect& r, moab::EntityHandle tet) const {

  moab::ErrorCode rval;

  // get tet vertices
  std::vector<moab::EntityHandle> verts;
  rval = mbi_->get_connectivity(&tet, 1, verts);
  if (rval != moab::MB_SUCCESS) {
    warning("Failed to get vertices of tet in umesh: " + filename_);
    return false;
  }

  // first vertex is used as a reference point for the barycentric data -
  // retrieve its coordinates
  moab::CartVect p_zero;
  rval = mbi_->get_coords(verts.data(), 1, p_zero.array());
  if (rval != moab::MB_SUCCESS) {
    warning("Failed to get coordinates of a vertex in "
            "unstructured mesh: " + filename_);
    return false;
  }

  // look up barycentric data
  int idx = get_bin_from_ent_handle(tet);
  const moab::Matrix3& a_inv = baryc_data_[idx];

  moab::CartVect bary_coords = a_inv * (r - p_zero);

  return (bary_coords[0] >= 0.0 &&
          bary_coords[1] >= 0.0 &&
          bary_coords[2] >= 0.0 &&
          bary_coords[0] + bary_coords[1] + bary_coords[2] <= 1.0);
}

int
UnstructuredMesh::get_bin_from_index(int idx) const {
  if (idx >= n_bins()) {
    fatal_error(fmt::format("Invalid bin index: {}", idx));
  }
  return ehs_[idx] - ehs_[0];
}

int
UnstructuredMesh::get_index(const Position& r,
                            bool* in_mesh) const {
  int bin = get_bin(r);
  *in_mesh = bin != -1;
  return bin;
}

int UnstructuredMesh::get_index_from_bin(int bin) const {
  return bin;
}

std::pair<std::vector<double>, std::vector<double>>
UnstructuredMesh::plot(Position plot_ll, Position plot_ur) const {
  // TODO: Implement mesh lines
  return {};
}

int
UnstructuredMesh::get_bin_from_ent_handle(moab::EntityHandle eh) const {
  int bin = eh - ehs_[0];
  if (bin >= n_bins()) {
    fatal_error(fmt::format("Invalid bin: {}", bin));
  }
  return bin;
}

moab::EntityHandle
UnstructuredMesh::get_ent_handle_from_bin(int bin) const {
  if (bin >= n_bins()) {
    fatal_error(fmt::format("Invalid bin index: ", bin));
  }
  return ehs_[bin];
}

int UnstructuredMesh::n_bins() const {
  return ehs_.size();
}

int UnstructuredMesh::n_surface_bins() const {
  // collect all triangles in the set of tets for this mesh
  moab::Range tris;
  moab::ErrorCode rval;
  rval = mbi_->get_entities_by_type(0, moab::MBTRI, tris);
  if (rval != moab::MB_SUCCESS) {
    warning("Failed to get all triangles in the mesh instance");
    return -1;
  }
  return 2 * tris.size();
}

Position
UnstructuredMesh::centroid(moab::EntityHandle tet) const {
  moab::ErrorCode rval;

  // look up the tet connectivity
  std::vector<moab::EntityHandle> conn;
  rval = mbi_->get_connectivity(&tet, 1, conn);
  if (rval != moab::MB_SUCCESS) {
    warning("Failed to get connectivity of a mesh element.");
    return {};
  }

  // get the coordinates
  std::vector<moab::CartVect> coords(conn.size());
  rval = mbi_->get_coords(conn.data(), conn.size(), coords[0].array());
  if (rval != moab::MB_SUCCESS) {
    warning("Failed to get the coordinates of a mesh element.");
    return {};
  }

  // compute the centroid of the element vertices
  moab::CartVect centroid(0.0, 0.0, 0.0);
  for(const auto& coord : coords) {
    centroid += coord;
  }
  centroid /= double(coords.size());

  return {centroid[0], centroid[1], centroid[2]};
}

std::string
UnstructuredMesh::bin_label(int bin) const {
  return fmt::format("Mesh Index ({})", bin);
};

std::pair<moab::Tag, moab::Tag>
UnstructuredMesh::get_score_tags(std::string score) const {
  moab::ErrorCode rval;
  // add a tag to the mesh
  // all scores are treated as a single value
  // with an uncertainty
  moab::Tag value_tag;

  // create the value tag if not present and get handle
  double default_val = 0.0;
  auto val_string = score + "_mean";
  rval = mbi_->tag_get_handle(val_string.c_str(),
                              1,
                              moab::MB_TYPE_DOUBLE,
                              value_tag,
                              moab::MB_TAG_DENSE|moab::MB_TAG_CREAT,
                              &default_val);
  if (rval != moab::MB_SUCCESS) {
    auto msg = fmt::format("Could not create or retrieve the value tag for the score {}"
                           " on unstructured mesh {}", score, id_);
    fatal_error(msg);
  }

  // create the std dev tag if not present and get handle
  moab::Tag error_tag;
  std::string err_string = score + "_std_dev";
  rval = mbi_->tag_get_handle(err_string.c_str(),
                              1,
                              moab::MB_TYPE_DOUBLE,
                              error_tag,
                              moab::MB_TAG_DENSE|moab::MB_TAG_CREAT,
                              &default_val);
  if (rval != moab::MB_SUCCESS) {
    auto msg = fmt::format("Could not create or retrieve the error tag for the score {}"
                           " on unstructured mesh {}", score, id_);
    fatal_error(msg);
  }

  // return the populated tag handles
  return {value_tag, error_tag};
}

void
UnstructuredMesh::add_score(std::string score) const {
  auto score_tags = this->get_score_tags(score);
}

void
UnstructuredMesh::set_score_data(const std::string& score,
                                 std::vector<double> values,
                                 std::vector<double> std_dev) const {
  auto score_tags = this->get_score_tags(score);

  // normalize tally values by element volume
  for (int i = 0; i < ehs_.size(); i++) {
    auto eh = this->get_ent_handle_from_bin(i);
    double volume = this->tet_volume(eh);
    values[i] /= volume;
    std_dev[i] /= volume;
  }

  moab::ErrorCode rval;
  // set the score value
  rval = mbi_->tag_set_data(score_tags.first, ehs_, values.data());
  if (rval != moab::MB_SUCCESS) {
    auto msg = fmt::format("Failed to set the tally value for score '{}' "
                           "on unstructured mesh {}", score, id_);
    warning(msg);
  }

  // set the error value
  rval = mbi_->tag_set_data(score_tags.second, ehs_, std_dev.data());
  if (rval != moab::MB_SUCCESS) {
    auto msg = fmt::format("Failed to set the tally error for score '{}' "
                           "on unstructured mesh {}", score, id_);
    warning(msg);
  }
}

void
UnstructuredMesh::write(std::string base_filename) const {
  // add extension to the base name
  auto filename = base_filename + ".vtk";
  write_message("Writing unstructured mesh " + filename + "...", 5);
  filename = settings::path_output + filename;

  // write the tetrahedral elements of the mesh only
  // to avoid clutter from zero-value data on other
  // elements during visualization
  moab::ErrorCode rval;
  rval = mbi_->write_mesh(filename.c_str(), &tetset_, 1);
  if (rval != moab::MB_SUCCESS) {
    auto msg = fmt::format("Failed to write unstructured mesh {}", id_);
    warning(msg);
  }
}

#endif

//==============================================================================
// Non-member functions
//==============================================================================

void read_meshes(pugi::xml_node root)
{
  for (auto node : root.children("mesh")) {
    std::string mesh_type;
    if (check_for_node(node, "type")) {
      mesh_type = get_node_value(node, "type", true, true);
    } else {
      mesh_type = "regular";
    }

    // Read mesh and add to vector
    if (mesh_type == "regular") {
      model::meshes.push_back(std::make_unique<RegularMesh>(node));
    } else if (mesh_type == "rectilinear") {
      model::meshes.push_back(std::make_unique<RectilinearMesh>(node));
#ifdef DAGMC
    } else if (mesh_type == "unstructured") {
      model::meshes.push_back(std::make_unique<UnstructuredMesh>(node));
#else
    } else if (mesh_type == "unstructured") {
      fatal_error("Unstructured mesh support is disabled.");
#endif
    } else {
      fatal_error("Invalid mesh type: " + mesh_type);
    }

    // Map ID to position in vector
    model::mesh_map[model::meshes.back()->id_] = model::meshes.size() - 1;
  }
}

void meshes_to_hdf5(hid_t group)
{
  // Write number of meshes
  hid_t meshes_group = create_group(group, "meshes");
  int32_t n_meshes = model::meshes.size();
  write_attribute(meshes_group, "n_meshes", n_meshes);

  if (n_meshes > 0) {
    // Write IDs of meshes
    std::vector<int> ids;
    for (const auto& m : model::meshes) {
      m->to_hdf5(meshes_group);
      ids.push_back(m->id_);
    }
    write_attribute(meshes_group, "ids", ids);
  }

  close_group(meshes_group);
}

void free_memory_mesh()
{
  model::meshes.clear();
  model::mesh_map.clear();
}

extern "C" int n_meshes() { return model::meshes.size(); }

} // namespace openmc
