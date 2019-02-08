/* Copyright (C) 2016, 2017, 2018, 2019 PISM Authors
 *
 * This file is part of PISM.
 *
 * PISM is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * PISM is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PISM; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "FrontRetreat.hh"
#include "util/remove_narrow_tongues.hh"

#include "pism/util/iceModelVec.hh"
#include "pism/util/IceModelVec2CellType.hh"
#include "pism/util/MaxTimestep.hh"
#include "pism/util/pism_utilities.hh"
#include "pism/geometry/part_grid_threshold_thickness.hh"
#include "pism/geometry/Geometry.hh"

namespace pism {

FrontRetreatInputs::FrontRetreatInputs() {
  geometry = nullptr;

  bc_mask           = nullptr;
  ice_enthalpy      = nullptr;
  ice_velocity      = nullptr;
  frontal_melt_rate = nullptr;
}

FrontRetreat::FrontRetreat(IceGrid::ConstPtr g, unsigned int mask_stencil_width)
  : Component(g) {

  m_tmp.create(m_grid, "temporary_storage", WITH_GHOSTS, 1);
  m_tmp.set_attrs("internal", "additional mass loss at points near the front",
                  "m", "");

  m_horizontal_retreat_rate.create(m_grid, "horizontal_retreat_rate", WITHOUT_GHOSTS);
  m_horizontal_retreat_rate.set_attrs("diagnostic", "retreat rate", "m second-1", "");
  m_horizontal_retreat_rate.set_time_independent(false);
  m_horizontal_retreat_rate.metadata().set_string("glaciological_units", "m year-1");

  m_mask.create(m_grid, "m_mask", WITH_GHOSTS, mask_stencil_width);
  m_mask.set_attrs("internal", "cell type mask", "", "");

  m_surface_topography.create(m_grid, "m_surface_topography", WITH_GHOSTS, 1);
  m_surface_topography.set_attrs("internal", "surface topography", "m", "surface_altitude");

  m_restrict_timestep = m_config->get_boolean("geometry.front_retreat.use_cfl");
}

FrontRetreat::~FrontRetreat() {
  // empty
}


/*!
 * Compute the maximum time step length provided a horizontal retreat rate.
 */
FrontRetreat::Timestep FrontRetreat::max_timestep(const IceModelVec2S &horizontal_retreat_rate) const {

  IceGrid::ConstPtr grid = horizontal_retreat_rate.grid();
  units::System::Ptr sys = grid->ctx()->unit_system();

  using units::convert;

  // About 9 hours which corresponds to 10000 km year-1 on a 10 km grid
  double dt_min = convert(sys, 0.001, "years", "seconds");

  double
    retreat_rate_max  = 0.0,
    retreat_rate_mean = 0.0;
  int N_cells = 0;

  IceModelVec::AccessList list(horizontal_retreat_rate);

  for (Points pt(*grid); pt; pt.next()) {
    const int i = pt.i(), j = pt.j();

    const double C = horizontal_retreat_rate(i, j);

    if (C > 0.0) {
      N_cells           += 1;
      retreat_rate_mean += C;
      retreat_rate_max   = std::max(C, retreat_rate_max);
    }
  }

  N_cells           = GlobalSum(grid->com, N_cells);
  retreat_rate_mean = GlobalSum(grid->com, retreat_rate_mean);
  retreat_rate_max  = GlobalMax(grid->com, retreat_rate_max);

  if (N_cells > 0.0) {
    retreat_rate_mean /= N_cells;
  } else {
    retreat_rate_mean = 0.0;
  }

  double denom = retreat_rate_max / grid->dx();
  const double epsilon = convert(sys, 0.001 / (grid->dx() + grid->dy()), "seconds", "years");

  double dt = 1.0 / (denom + epsilon);

  return {MaxTimestep(std::max(dt, dt_min)), retreat_rate_max, retreat_rate_mean, N_cells};
}


/**
 * @brief Compute the maximum time-step length allowed by the CFL
 * condition applied to the retreat rate.
 */
MaxTimestep FrontRetreat::max_timestep(const FrontRetreatInputs &inputs,
                                       double t) const {
  (void) t;

  if (not m_restrict_timestep) {
    return MaxTimestep();
  }

  IceModelVec2S &horizontal_retreat_rate = m_tmp;

  compute_retreat_rate(inputs, horizontal_retreat_rate);

  auto info = max_timestep(horizontal_retreat_rate);

  m_log->message(3,
                 "  front retreat: maximum rate = %.2f m/year gives dt=%.5f years\n"
                 "                 mean rate    = %.2f m/year over %d cells\n",
                 convert(m_sys, info.rate_max, "m second-1", "m year-1"),
                 convert(m_sys, info.dt.value(), "seconds", "years"),
                 convert(m_sys, info.rate_mean, "m second-1", "m year-1"),
                 info.N_cells);

  return info.dt;
}

/*!
 * Adjust the mask near domain boundaries to avoid "wrapping around."
 */
void FrontRetreat::prepare_mask(const IceModelVec2CellType &input,
                                       IceModelVec2CellType &output) const {

  output.copy_from(input);

  if (m_config->get_boolean("geometry.front_retreat.wrap_around")) {
    return;
  }

  IceModelVec::AccessList list(output);

  const int Mx = m_grid->Mx();
  const int My = m_grid->My();

  ParallelSection loop(m_grid->com);
  try {
    for (PointsWithGhosts p(*m_grid); p; p.next()) {
      const int i = p.i(), j = p.j();

      if (i < 0 or i >= Mx or j < 0 or j >= My) {
        output(i, j) = MASK_ICE_FREE_OCEAN;
      }
    }
  } catch (...) {
    loop.failed();
  }
  loop.check();
}
void FrontRetreat::update_geometry(double dt,
                                   const IceModelVec2S &sea_level,
                                   const IceModelVec2S &bed_topography,
                                   const IceModelVec2Int &bc_mask,
                                   const IceModelVec2S &horizontal_retreat_rate,
                                   IceModelVec2CellType &cell_type,
                                   IceModelVec2S &Href,
                                   IceModelVec2S &ice_thickness) {

  GeometryCalculator gc(*m_config);
  gc.compute_surface(sea_level, bed_topography, ice_thickness, m_surface_topography);

  const double dx = m_grid->dx();

  m_tmp.set(0.0);

  IceModelVec::AccessList list{&ice_thickness, &bc_mask,
      &bed_topography, &sea_level, &cell_type, &Href, &m_tmp, &horizontal_retreat_rate,
      &m_surface_topography};

  // Prepare to loop over neighbors: directions
  const Direction dirs[] = {North, East, South, West};

  // Step 1: Apply the computed horizontal retreat rate:
  for (Points pt(*m_grid); pt; pt.next()) {
    const int i = pt.i(), j = pt.j();

    if (bc_mask(i, j) > 0.5) {
      // don't modify cells marked as Dirichlet B.C. locations
      continue;
    }

    const double rate = horizontal_retreat_rate(i, j);

    if (cell_type.ice_free(i, j) and rate > 0.0) {
      // apply retreat rate at the margin (i.e. to partially-filled cells) only

      const double Href_old = Href(i, j);

      // Compute the number of floating neighbors and the neighbor-averaged ice thickness:
      double H_threshold = part_grid_threshold_thickness(cell_type.int_star(i, j),
                                                         ice_thickness.star(i, j),
                                                         m_surface_topography.star(i, j),
                                                         bed_topography(i, j));

      // Calculate mass loss with respect to the associated ice thickness and the grid size:
      const double Href_change = -dt * rate * H_threshold / dx; // in m

      if (Href_old + Href_change >= 0.0) {
        // Href is high enough to absorb the mass loss
        Href(i, j) = Href_old + Href_change;
      } else {
        Href(i, j) = 0.0;
        // Href is below Href_change: need to distribute mass loss to neighboring points

        // Find the number of neighbors to distribute to.
        //
        // We consider floating cells and grounded cells with the base below sea level. In other
        // words, additional mass losses are distributed to shelf calving fronts and grounded marine
        // termini.
        int N = 0;
        {
          auto
            M  = cell_type.int_star(i, j),
            BC = bc_mask.int_star(i, j);

          auto
            bed = bed_topography.star(i, j),
            sl  = sea_level.star(i, j);

          for (int n = 0; n < 4; ++n) {
            Direction direction = dirs[n];
            int m = M[direction];
            int bc = BC[direction];

            if (bc == 0 and     // distribute to regular (*not* Dirichlet B.C.) neighbors only
                (mask::floating_ice(m) or
                 (mask::grounded_ice(m) and bed[direction] < sl[direction]))) {
              N += 1;
            }
          }
        }

        if (N > 0) {
          m_tmp(i, j) = (Href_old + Href_change) / (double)N;
        } else {
          // No shelf calving front of grounded terminus to distribute to: retreat stops here.
          m_tmp(i, j) = 0.0;
        }
      }

    } // end of "if (rate > 0.0)"
  }   // end of loop over grid points

  // Step 2: update ice thickness and Href in neighboring cells if we need to propagate mass losses.
  m_tmp.update_ghosts();

  for (Points p(*m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    // Note: this condition has to match the one in step 1 above.
    if (bc_mask.as_int(i, j) == 0 and
        (cell_type.floating_ice(i, j) or
         (cell_type.grounded_ice(i, j) and bed_topography(i, j) < sea_level(i, j)))) {

      const double delta_H = (m_tmp(i + 1, j) + m_tmp(i - 1, j) +
                              m_tmp(i, j + 1) + m_tmp(i, j - 1));

      if (delta_H < 0.0) {
        Href(i, j) = ice_thickness(i, j) + delta_H; // in m
        ice_thickness(i, j) = 0.0;
      }

      // Stop retreat if the current cell does not have enough ice to absorb the loss.
      if (Href(i, j) < 0.0) {
        Href(i, j) = 0.0;
      }

    }
  }

  // need to update ghosts of thickness to compute mask in place
  ice_thickness.update_ghosts();

  // update cell_type
  gc.set_icefree_thickness(m_config->get_double("stress_balance.ice_free_thickness_standard"));
  gc.compute_mask(sea_level, bed_topography, ice_thickness, cell_type);

  // remove narrow ice tongues
  remove_narrow_tongues(cell_type, ice_thickness);

  // update cell_type again
  gc.compute_mask(sea_level, bed_topography, ice_thickness, cell_type);
}

/*! Update ice geometry and mask using the computed horizontal retreat rate.
 * @param[in] dt time step, seconds
 * @param[in] sea_level sea level elevation, meters
 * @param[in] thickness_bc_mask Dirichlet B.C. mask for the ice thickness
 * @param[in] bed_topography bed elevation, meters
 * @param[in,out] mask cell type mask
 * @param[in,out] Href "area specific volume"
 * @param[in,out] ice_thickness ice thickness
 *
 * FIXME: we don't really need to call remove_narrow_tongues here: it is necessary when we use a
 * calving parameterization which uses strain rates (eigen-calving), but it may not be appropriate
 * with a frontal melt parameterization.
 */
void FrontRetreat::update(double dt,
                          const FrontRetreatInputs &inputs,
                          IceModelVec2CellType &mask,
                          IceModelVec2S &Href,
                          IceModelVec2S &ice_thickness) {

  compute_retreat_rate(inputs, m_horizontal_retreat_rate);

  update_geometry(dt,
                  inputs.geometry->sea_level_elevation,
                  inputs.geometry->bed_elevation,
                  *inputs.bc_mask,
                  m_horizontal_retreat_rate,
                  mask, Href, ice_thickness);
}

const IceModelVec2S& FrontRetreat::retreat_rate() const {
  return m_horizontal_retreat_rate;
}

FrontRetreatRate::FrontRetreatRate(const FrontRetreat *m,
                                   const std::string &name,
                                   const std::string &long_name)
  : Diag<FrontRetreat>(m) {

  /* set metadata: */
  m_vars = {SpatialVariableMetadata(m_sys, name)};

  set_attrs(long_name, "",
            "m second-1", "m year-1", 0);
}

IceModelVec::Ptr FrontRetreatRate::compute_impl() const {

  IceModelVec2S::Ptr result(new IceModelVec2S(m_grid, "", WITHOUT_GHOSTS));
  result->metadata(0) = m_vars[0];

  result->copy_from(model->retreat_rate());

  return result;
}

} // end of namespace pism
