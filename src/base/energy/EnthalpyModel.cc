/* Copyright (C) 2016 PISM Authors
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

#include "EnthalpyModel.hh"

#include "DrainageCalculator.hh"
#include "base/enthalpyConverter.hh"
#include "base/energy/enthSystem.hh"
#include "base/util/IceModelVec2CellType.hh"
#include "base/util/io/PIO.hh"
#include "utilities.hh"

namespace pism {
namespace energy {

EnthalpyModel::EnthalpyModel(IceGrid::ConstPtr grid,
                             stressbalance::StressBalance *stress_balance)
  : EnergyModel(grid, stress_balance) {
  // empty
}

void EnthalpyModel::restart_impl(const PIO &input_file, int record) {

  m_log->message(2, "* Restarting the enthalpy-based energy balance model from %s...\n",
                 input_file.inq_filename().c_str());

  m_basal_melt_rate.read(input_file, record);
  init_enthalpy(input_file, false, record);

  regrid("Energy balance model", m_basal_melt_rate, REGRID_WITHOUT_REGRID_VARS);
  regrid_enthalpy();
}

void EnthalpyModel::bootstrap_impl(const PIO &input_file,
                                   const IceModelVec2S &ice_thickness,
                                   const IceModelVec2S &surface_temperature,
                                   const IceModelVec2S &climatic_mass_balance,
                                   const IceModelVec2S &basal_heat_flux) {

  m_log->message(2, "* Bootstrapping the enthalpy-based energy balance model from %s...\n",
                 input_file.inq_filename().c_str());

  m_basal_melt_rate.regrid(input_file, OPTIONAL,
                           m_config->get_double("bootstrapping.defaults.bmelt"));

  bootstrap_ice_enthalpy(ice_thickness, surface_temperature, climatic_mass_balance,
                         basal_heat_flux, m_ice_enthalpy);

  regrid("Energy balance model", m_basal_melt_rate, REGRID_WITHOUT_REGRID_VARS);
  regrid_enthalpy();
}

void EnthalpyModel::initialize_impl(const IceModelVec2S &basal_melt_rate,
                                    const IceModelVec2S &ice_thickness,
                                    const IceModelVec2S &surface_temperature,
                                    const IceModelVec2S &climatic_mass_balance,
                                    const IceModelVec2S &basal_heat_flux) {

  m_log->message(2, "* Bootstrapping the enthalpy-based energy balance model...\n");

  m_basal_melt_rate.copy_from(basal_melt_rate);

  bootstrap_ice_enthalpy(ice_thickness, surface_temperature, climatic_mass_balance,
                         basal_heat_flux, m_ice_enthalpy);

  regrid("Energy balance model", m_basal_melt_rate, REGRID_WITHOUT_REGRID_VARS);
  regrid_enthalpy();
}

//! Update ice enthalpy field based on conservation of energy.
/*!
This method is documented by the page \ref bombproofenth and by [\ref
AschwandenBuelerKhroulevBlatter].

This method updates IceModelVec3 m_work and IceModelVec2S basal_melt_rate.
No communication of ghosts is done for any of these fields.

We use an instance of enthSystemCtx.

Regarding drainage, see [\ref AschwandenBuelerKhroulevBlatter] and references therein.

\image html BC-decision-chart.png "Setting the basal boundary condition"
 */

void EnthalpyModel::update_impl(double t, double dt, const EnergyModelInputs &inputs) {
  // current time does not matter here
  (void) t;

  EnthalpyConverter::Ptr EC = m_grid->ctx()->enthalpy_converter();

  const double
    ice_density  = m_config->get_double("constants.ice.density"),          // kg m-3
    bulgeEnthMax = m_config->get_double("energy.enthalpy_cold_bulge_max"); // J kg-1

  energy::DrainageCalculator dc(*m_config);

  inputs.check();

  // give them names that a bit shorter...
  const IceModelVec3
    &strain_heating3 = *inputs.strain_heating3,
    &u3              = *inputs.u3,
    &v3              = *inputs.v3,
    &w3              = *inputs.w3;

  const IceModelVec2CellType &cell_type = *inputs.cell_type;

  const IceModelVec2S
    &basal_frictional_heating = *inputs.basal_frictional_heating,
    &basal_heat_flux          = *inputs.basal_heat_flux,
    &ice_thickness            = *inputs.ice_thickness,
    &surface_liquid_fraction  = *inputs.surface_liquid_fraction,
    &shelf_base_temp          = *inputs.shelf_base_temp,
    &ice_surface_temp         = *inputs.surface_temp,
    &till_water_thickness     = *inputs.till_water_thickness;

  energy::enthSystemCtx system(m_grid->z(), "enth", m_grid->dx(), m_grid->dy(), dt,
                               *m_config, m_ice_enthalpy, u3, v3, w3, strain_heating3, EC);

  const size_t Mz_fine = system.z().size();
  const double dz = system.dz();
  std::vector<double> Enthnew(Mz_fine); // new enthalpy in column

  IceModelVec::AccessList list;

  list.add(ice_surface_temp);
  list.add(shelf_base_temp);
  list.add(surface_liquid_fraction);
  list.add(ice_thickness);
  list.add(basal_frictional_heating);
  list.add(basal_heat_flux);
  list.add(till_water_thickness);
  list.add(cell_type);

  list.add(u3);
  list.add(v3);
  list.add(w3);
  list.add(strain_heating3);

  list.add(m_basal_melt_rate);
  list.add(m_ice_enthalpy);
  list.add(m_work);

  unsigned int liquifiedCount = 0;

  ParallelSection loop(m_grid->com);
  try {
    for (Points pt(*m_grid); pt; pt.next()) {
      const int i = pt.i(), j = pt.j();

      const double H = ice_thickness(i, j);

      system.init(i, j, H);

      // enthalpy and pressures at top of ice
      const double
        depth_ks = H - system.ks() * dz,
        p_ks     = EC->pressure(depth_ks); // FIXME issue #15

      const double Enth_ks = EC->enthalpy_permissive(ice_surface_temp(i, j),
                                                     surface_liquid_fraction(i, j), p_ks);

      const bool ice_free_column = (system.ks() == 0);

      // deal completely with columns with no ice; enthalpy and basal_melt_rate need setting
      if (ice_free_column) {
        m_work.set_column(i, j, Enth_ks);
        // The floating basal melt rate will be set later; cover this
        // case and set to zero for now. Also, there is no basal melt
        // rate on ice free land and ice free ocean
        m_basal_melt_rate(i, j) = 0.0;
        continue;
      } // end of if (ice_free_column)

      if (system.lambda() < 1.0) {
        m_stats.reduced_accuracy_counter += 1; // count columns with lambda < 1
      }

      const bool
        is_floating        = cell_type.ocean(i, j),
        base_is_warm       = system.Enth(0) >= system.Enth_s(0),
        above_base_is_warm = system.Enth(1) >= system.Enth_s(1);

      // set boundary conditions and update enthalpy
      {
        system.set_surface_dirichlet_bc(Enth_ks);

        // determine lowest-level equation at bottom of ice; see
        // decision chart in the source code browser and page
        // documenting BOMBPROOF
        if (is_floating) {
          // floating base: Dirichlet application of known temperature from ocean
          //   coupler; assumes base of ice shelf has zero liquid fraction
          double Enth0 = EC->enthalpy_permissive(shelf_base_temp(i, j), 0.0, EC->pressure(H));

          system.set_basal_dirichlet_bc(Enth0);
        } else {
          // grounded ice warm and wet
          if (base_is_warm && (till_water_thickness(i, j) > 0.0)) {
            if (above_base_is_warm) {
              // temperate layer at base (Neumann) case:  q . n = 0  (K0 grad E . n = 0)
              system.set_basal_heat_flux(0.0);
            } else {
              // only the base is warm: E = E_s(p) (Dirichlet)
              // ( Assumes ice has zero liquid fraction. Is this a valid assumption here?
              system.set_basal_dirichlet_bc(system.Enth_s(0));
            }
          } else {
            // (Neumann) case:  q . n = q_lith . n + F_b
            // a) cold and dry base, or
            // b) base that is still warm from the last time step, but without basal water
            system.set_basal_heat_flux(basal_heat_flux(i, j) + basal_frictional_heating(i, j));
          }
        }

        // solve the system
        system.solve(Enthnew);

      }

      // post-process (drainage and bulge-limiting)
      double Hdrainedtotal = 0.0;
      double Hfrozen = 0.0;
      {
        // drain ice segments by mechanism in [\ref AschwandenBuelerKhroulevBlatter],
        //   using DrainageCalculator dc
        for (unsigned int k=0; k < system.ks(); k++) {
          if (Enthnew[k] > system.Enth_s(k)) { // avoid doing any more work if cold

            const double
              depth = H - k * dz,
              p     = EC->pressure(depth), // FIXME issue #15
              T_m   = EC->melting_temperature(p),
              L     = EC->L(T_m),
              omega = EC->water_fraction(Enthnew[k], p);

            if (Enthnew[k] >= system.Enth_s(k) + 0.5 * L) {
              liquifiedCount++; // count these rare events...
              Enthnew[k] = system.Enth_s(k) + 0.5 * L; //  but lose the energy
            }

            if (omega > 0.01) {                          // FIXME: make "0.01" configurable here
              double fractiondrained = dc.get_drainage_rate(omega) * dt; // pure number

              fractiondrained  = std::min(fractiondrained, omega - 0.01); // only drain down to 0.01
              Hdrainedtotal   += fractiondrained * dz; // always a positive contribution
              Enthnew[k]      -= fractiondrained * L;
            }
          }
        }

        // apply bulge limiter
        const double lowerEnthLimit = Enth_ks - bulgeEnthMax;
        for (unsigned int k=0; k < system.ks(); k++) {
          if (Enthnew[k] < lowerEnthLimit) {
            // Count grid points which have very large cold limit advection bulge... enthalpy not
            // too low.
            m_stats.bulge_counter += 1;
            Enthnew[k] = lowerEnthLimit;
          }
        }

        // if there is subglacial water, don't allow ice base enthalpy to be below
        // pressure-melting; that is, assume subglacial water is at the pressure-
        // melting temperature and enforce continuity of temperature
        {
          if (Enthnew[0] < system.Enth_s(0) && till_water_thickness(i,j) > 0.0) {
            const double E_difference = system.Enth_s(0) - Enthnew[0];

            const double depth = H,
              pressure         = EC->pressure(depth),
              T_m              = EC->melting_temperature(pressure);

            Enthnew[0] = system.Enth_s(0);
            // This adjustment creates energy out of nothing. We will
            // freeze some basal water, subtracting an equal amount of
            // energy, to make up for it.
            //
            // Note that [E_difference] = J/kg, so
            //
            // U_difference = E_difference * ice_density * dx * dy * (0.5*dz)
            //
            // is the amount of energy created (we changed enthalpy of
            // a block of ice with the volume equal to
            // dx*dy*(0.5*dz); note that the control volume
            // corresponding to the grid point at the base of the
            // column has thickness 0.5*dz, not dz).
            //
            // Also, [L] = J/kg, so
            //
            // U_freeze_on = L * ice_density * dx * dy * Hfrozen,
            //
            // is the amount of energy created by freezing a water
            // layer of thickness Hfrozen (using units of ice
            // equivalent thickness).
            //
            // Setting U_difference = U_freeze_on and solving for
            // Hfrozen, we find the thickness of the basal water layer
            // we need to freeze co restore energy conservation.

            Hfrozen = E_difference * (0.5*dz) / EC->L(T_m);
          }
        }

      } // end of post-processing

      // compute basal melt rate
      {
        bool base_is_cold = (Enthnew[0] < system.Enth_s(0)) && (till_water_thickness(i,j) == 0.0);
        // Determine melt rate, but only preliminarily because of
        // drainage, from heat flux out of bedrock, heat flux into
        // ice, and frictional heating
        if (is_floating) {
          // The floating basal melt rate will be set later; cover
          // this case and set to zero for now. Note that
          // Hdrainedtotal is discarded (the ocean model determines
          // the basal melt).
          m_basal_melt_rate(i, j) = 0.0;
        } else {
          if (base_is_cold) {
            m_basal_melt_rate(i, j) = 0.0;  // zero melt rate if cold base
          } else {
            const double
              p_0 = EC->pressure(H),
              p_1 = EC->pressure(H - dz), // FIXME issue #15
              Tpmp_0 = EC->melting_temperature(p_0);

            const bool k1_istemperate = EC->is_temperate(Enthnew[1], p_1); // level  z = + \Delta z
            double hf_up = 0.0;
            if (k1_istemperate) {
              const double
                Tpmp_1 = EC->melting_temperature(p_1);

              hf_up = -system.k_from_T(Tpmp_0) * (Tpmp_1 - Tpmp_0) / dz;
            } else {
              double T_0 = EC->temperature(Enthnew[0], p_0);
              const double K_0 = system.k_from_T(T_0) / EC->c();

              hf_up = -K_0 * (Enthnew[1] - Enthnew[0]) / dz;
            }

            // compute basal melt rate from flux balance:
            //
            // basal_melt_rate = - Mb / rho in [\ref AschwandenBuelerKhroulevBlatter];
            //
            // after we compute it we make sure there is no refreeze if
            // there is no available basal water
            m_basal_melt_rate(i, j) = (basal_frictional_heating(i, j) + basal_heat_flux(i, j) - hf_up) / (ice_density * EC->L(Tpmp_0));

            if (till_water_thickness(i, j) <= 0 && m_basal_melt_rate(i, j) < 0) {
              m_basal_melt_rate(i, j) = 0.0;
            }
          }

          // Add drained water from the column to basal melt rate.
          m_basal_melt_rate(i, j) += (Hdrainedtotal - Hfrozen) / dt;
        } // end of the grounded case
      } // end of the basal melt rate computation

      system.fine_to_coarse(Enthnew, i, j, m_work);
    }
  } catch (...) {
    loop.failed();
  }
  loop.check();

  // FIXME: use cell areas
  m_stats.liquified_ice_volume = ((double) liquifiedCount) * dz * m_grid->dx() * m_grid->dy();
}

/*
  The decision above was produced using this TikZ source code:

% Define block styles
\tikzstyle{decision} = [ellipse, draw, text width=7em, text badly centered, inner sep=2pt]
\tikzstyle{block} = [rectangle, draw, text width=5em, text badly centered, rounded corners, minimum height=4em]
\tikzstyle{line} = [draw, -latex']

\begin{tikzpicture}[node distance = 3cm, auto]
    % Place nodes
    \node (invisiblestart) {};

    \node [decision, below of=invisiblestart, text height=0.2cm] (coldvstemp) {$H<H_{\text s}(p)$ ?};
    \node [decision, left of=coldvstemp, xshift=-4em] (excludebad) {$\eta_{\text b}>0$ ?};
    \node [block, below of=excludebad, text width=6em] (fixbad) {$H := H_{\text s}(p)$};

    % edges
    \path [line] (invisiblestart) -- (coldvstemp);
    \path [line] (excludebad) -- node [text width=6em] {yes (consider base to be temperate)} (fixbad);

    % cold branch:
    \node [block, left of=fixbad, text width=7.5em] (coldmodeltype) {Eqn (49) is Neumann b.c.~for Eqn (67); $M_b=0$};
    % edges
    \path [line] (coldvstemp) -- node {yes} (excludebad);
    \path [line] (excludebad) -- node {no} (coldmodeltype);

    % temperate branch
    \node [block, below of=coldvstemp, text width=12em] (qtemperate) {$\nabla H \cdot \bn=0$ is Neumann b.c.~for Eqn (67)};
    \node [decision, below left of=qtemperate, text width=8em] (tempthick) {positive thickness of temperate ice at base?};
    \node [block, below right of=tempthick, text width=10em] (Mbforqtemperate) {$\bq = - k(H,p)\nabla T_{\text m}(p)$ \\ at ice base};
    \node [block, below left of=tempthick, text width=9em, xshift=-4em] (Mbforqcold) {$\bq = - K_{\text i}(H) \nabla H$ \\ at ice base};
    \node [block, below left of=Mbforqtemperate, text width=9em] (getMbtemp) {compute $M_b$ from Eqn (50) or (66)};

    % edges
    \path [line] (fixbad) -- (qtemperate);
    \path [line] (coldvstemp) -- node {no} (qtemperate);
    \path [line] (tempthick) -- node [above] {no} (Mbforqcold);
    \path [line] (tempthick) -- node {yes} (Mbforqtemperate);
    \path [line] (qtemperate) -- (tempthick);
    \path [line] (Mbforqcold) -- node {} (getMbtemp);
    \path [line] (Mbforqtemperate) -- node {} (getMbtemp);
\end{tikzpicture}
 */

void EnthalpyModel::define_model_state_impl(const PIO &output) const {
  m_ice_enthalpy.define(output);
  m_basal_melt_rate.define(output);
}

void EnthalpyModel::write_model_state_impl(const PIO &output) const {
  m_ice_enthalpy.write(output);
  m_basal_melt_rate.write(output);
}

} // end of namespace energy
} // end of namespace pism
