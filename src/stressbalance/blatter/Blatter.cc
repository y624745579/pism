/* Copyright (C) 2020 PISM Authors
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

#include <cassert>              // assert
#include <cmath>                // std::pow, std::fabs
#include <algorithm>            // std::max
#include <cstring>              // memset

#include "Blatter.hh"
#include "pism/util/fem/FEM.hh"
#include "pism/util/error_handling.hh"
#include "pism/util/Vector2.hh"

#include "DataAccess.hh"
#include "grid_hierarchy.hh"
#include "pism/util/node_types.hh"

#include "pism/rheology/FlowLaw.hh"
#include "pism/rheology/FlowLawFactory.hh"

namespace pism {
namespace stressbalance {

const Vector2 u_exterior = {0.0, 0.0};

/*!
 * 2D input parameters
 */
struct Parameters : public ColumnInfo{
  // basal yield stress
  double tauc;
  // sea level elevation (used to determine if a location is grounded)
  double sea_level;
};

/*!
 * Returns true if a node is in the Dirichlet part of the boundary, false otherwise.
 */
static bool dirichlet_node(const DMDALocalInfo &info, const fem::Element3::GlobalIndex& I) {
  (void) info;
  return I.k == 0;
}

/*! Dirichlet BC
*/
static Vector2 u_bc(double x, double y, double z) {
  (void) x;
  (void) y;
  (void) z;
  return {0.0, 0.0};
}

/*!
 * Right hand side
 */
static double F(double x, double y, double z) {
  (void) x;
  (void) y;
  (void) z;
  return 0.0;
}

/*!
 * Neumann BC
 */
static Vector2 G(double x, double y, double z, const Vector3 &N) {
  (void) x;
  (void) y;
  (void) z;
  (void) N;
  return {0.0, 0.0};
}

static Vector2 dirichlet_scale(double dx, double dy, double dz) {
  return {dx * dy / dz + dx * dz / dy + 4.0 * dy * dz / dx,
          dx * dy / dz + 4.0 * dx * dz / dy + dy * dz / dx};
}

void Blatter::compute_residual(DMDALocalInfo *petsc_info,
                               const Vector2 ***x, Vector2 ***R) {
  auto info = grid_transpose(*petsc_info);

  // Stencil width of 1 is not very important, but if info.sw > 1 will lead to more
  // redundant computation (we would be looping over elements that don't contribute to any
  // owned nodes).
  assert(info.sw == 1);

  // Compute grid spacing from domain dimensions and the grid size
  double
    x_min = m_grid_info.x_min,
    x_max = m_grid_info.x_max,
    y_min = m_grid_info.y_min,
    y_max = m_grid_info.y_max,
    dx = (x_max - x_min) / (info.mx - 1),
    dy = (y_max - y_min) / (info.my - 1);

  fem::Q1Element3 element(info, dx, dy, fem::Q13DQuadrature8());
  fem::Q1Element3Face face(dx, dy, fem::Q1Quadrature4());

  DataAccess<Parameters**> P(info.da, 2, GHOSTED);
  DataAccess<double***> F(info.da, 3, GHOSTED);

  // Compute the residual at Dirichlet BC nodes and reset the residual to zero elsewhere.
  //
  // Setting it to zero is necessary because we call DMDASNESSetFunctionLocal() with
  // INSERT_VALUES.
  //
  // here we loop over all the *owned* nodes
  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {
      for (int k = info.zs; k < info.zs + info.zm; k++) {

        // Dirichlet nodes
        if (dirichlet_node(info, {i, j, k}) or
            (int)P[j][i].node_type == NODE_EXTERIOR) {
          double dz = std::max(P[j][i].thickness, m_grid_info.min_thickness) / (info.mz - 1);

          // FIXME: eta should be included in the scaling
          Vector2 s = dirichlet_scale(dx, dy, dz);

          Vector2 U_bc;
          if (dirichlet_node(info, {i, j, k})) {
            double
              xx = grid_xy(x_min, dx, i),
              yy = grid_xy(y_min, dy, j),
              b  = P[j][i].bed,
              H  = P[j][i].thickness,
              zz = grid_z(b, H, info.mz, k);
            U_bc = u_bc(xx, yy, zz);
          } else {
            U_bc = u_exterior;
          }

          Vector2 r = x[j][i][k] - U_bc;

          R[j][i][k] = {r.u * s.u, r.v * s.v}; // STORAGE_ORDER
        } else {
          R[j][i][k] = 0.0;     // STORAGE_ORDER
        }
      }
    }
  }

  // values at element nodes
  const int Nk_max = 8;
  int Nk = element.n_chi();
  assert(Nk <= Nk_max);

  double x_nodal[Nk_max], y_nodal[Nk_max], B_nodal[Nk_max], s_nodal[Nk_max];
  std::vector<double> z_nodal(Nk);

  Vector2 R_nodal[Nk_max], u_nodal[Nk_max];
  int node_type[Nk_max];

  // values at quadrature points
  const int Nq_max = 16;
  int Nq = element.n_pts();
  assert(Nq <= Nq_max);

  Vector2 u[Nq_max], u_x[Nq_max], u_y[Nq_max], u_z[Nq_max];
  double xq[Nq_max], yq[Nq_max], zq[Nq_max], Bq[Nq_max];

  double s[Nq_max], s_x[Nq_max], s_y[Nq_max], s_z[Nq_max];

  // make sure that xq, yq, zq and big enough for quadrature points on element faces
  assert(face.n_pts() <= Nq_max);

  // loop over all the elements that have at least one owned node
  for (int j = info.gys; j < info.gys + info.gym - 1; j++) {
    for (int i = info.gxs; i < info.gxs + info.gxm - 1; i++) {
      for (int k = info.gzs; k < info.gzs + info.gzm - 1; k++) {

        // Reset element residual to zero in preparation.
        memset(R_nodal, 0, sizeof(R_nodal));

        // Compute coordinates of the nodes of this element and fetch node types.
        for (int n = 0; n < Nk; ++n) {
          auto I = element.local_to_global(i, j, k, n);

          auto p = P[I.j][I.i];

          node_type[n] = p.node_type;

          x_nodal[n] = grid_xy(x_min, dx, I.i);
          y_nodal[n] = grid_xy(y_min, dy, I.j);
          z_nodal[n] = grid_z(p.bed, p.thickness, info.mz, I.k);

          s_nodal[n] = p.bed + p.thickness;
        }

        // skip ice-free elements
        {
          // an element is exterior if one or more of its nodes are "exterior"
          bool exterior = false;
          for (int n = 0; n < Nk; ++n) {
            if (node_type[n] == NODE_EXTERIOR) {
              exterior = true;
              break;
            }
          }

          if (exterior) {
            continue;
          }
        }

        // compute values of chi, chi_x, chi_y, chi_z and quadrature weights at quadrature
        // points on this physical element
        element.reset(i, j, k, z_nodal);

        // Get nodal values of F.
        element.nodal_values((double***)F, B_nodal);

        // Get nodal values of u.
        element.nodal_values(x, u_nodal);

        // Take care of Dirichlet BC: don't contribute to Dirichlet nodes and set nodal
        // values of the current iterate to Dirichlet BC values.
        for (int n = 0; n < Nk; ++n) {
          auto I = element.local_to_global(n);
          if (dirichlet_node(info, I)) {
            element.mark_row_invalid(n);
            u_nodal[n] = u_bc(x_nodal[n], y_nodal[n], z_nodal[n]);
          }
        }

        // evaluate u and its partial derivatives at quadrature points
        element.evaluate(u_nodal, u, u_x, u_y, u_z);

        // evaluate B (ice hardness) at quadrature points
        element.evaluate(B_nodal, Bq);

        // compute the surface gradient at quadrature points
        // we could do this faster by using the fact that s does not depend on z
        element.evaluate(s_nodal, s, s_x, s_y, s_z);

        // loop over all quadrature points
        for (int q = 0; q < Nq; ++q) {
          auto W = element.weight(q);

          double
            ux = u_x[q].u,
            uy = u_y[q].u,
            uz = u_z[q].u,
            vx = u_x[q].v,
            vy = u_y[q].v,
            vz = u_z[q].v;

          double gamma = (ux * ux + vy * vy + ux * vy +
                          0.25 * ((uy + vx) * (uy + vx) + uz * uz + vz * vz));

          double eta;
          m_flow_law->effective_viscosity(Bq[q], gamma, &eta, nullptr);

          // loop over all test functions
          for (int t = 0; t < Nk; ++t) {
            const auto &psi = element.chi(q, t);

            R_nodal[t].u += W * (eta * (psi.dx * (4.0 * ux + 2.0 * vy) +
                                        psi.dy * (uy + vx) +
                                        psi.dz * uz) +
                                 psi.val * s_x[q]);
            R_nodal[t].v += W * (eta * (psi.dx * (uy + vx) +
                                        psi.dy * (2.0 * ux + 4.0 * vy) +
                                        psi.dz * vz) +
                                 psi.val * s_y[q]);
          }
        }

        // loop over all faces
        for (int f = 0; f < fem::q13d::n_faces; ++f) {
          auto nodes = fem::q13d::incident_nodes[f];
          // Loop over all nodes corresponding to this face. A face is a part of the
          // Neumann boundary if all four nodes are Neumann nodes. If a node is *both* a
          // Neumann and a Dirichlet node (this may happen), then we treat it as a Neumann
          // node here: add_contribution() will do the right thing later.
          bool neumann = true;
          for (int n = 0; n < 4; ++n) {
            if (not (node_type[nodes[n]] == NODE_BOUNDARY)) {
              neumann = false;
            }
          }

          if (neumann) {
            face.reset(f, z_nodal);

            // compute physical coordinates of quadrature points on this face
            face.evaluate(x_nodal, xq);
            face.evaluate(y_nodal, yq);
            face.evaluate(z_nodal.data(), zq);

            // loop over all quadrature points
            for (int q = 0; q < face.n_pts(); ++q) {
              auto W = face.weight(q);
              auto N = face.normal(q);

              // loop over all test functions
              for (int t = 0; t < Nk; ++t) {
                auto psi = face.chi(q, t);

                // FIXME: stress BC
                R_nodal[t] += - W * psi * G(xq[q], yq[q], zq[q], N);
              }
            }
          } // end of "if (neumann)"
        } // end of the loop over element faces

        element.add_contribution(R_nodal, R);
      } // end of the loop over i
    } // end of the loop over j
  } // end of the loop over k
}

void Blatter::compute_jacobian(DMDALocalInfo *petsc_info,
                               const Vector2 ***x, Mat A, Mat J) {
  auto info = grid_transpose(*petsc_info);

  (void) x;

  // Zero out the Jacobian in preparation for updating it.
  PetscErrorCode ierr = MatZeroEntries(J);
  PISM_CHK(ierr, "MatZeroEntries");

  // Stencil width of 1 is not very important, but if info.sw > 1 will lead to more
  // redundant computation (we would be looping over elements that don't contribute to any
  // owned nodes).
  assert(info.sw == 1);

  // Compute grid spacing from domain dimensions and the grid size
  double
    x_min = m_grid_info.x_min,
    x_max = m_grid_info.x_max,
    y_min = m_grid_info.y_min,
    y_max = m_grid_info.y_max,
    dx = (x_max - x_min) / (info.mx - 1),
    dy = (y_max - y_min) / (info.my - 1);

  fem::Q1Element3 element(info, dx, dy, fem::Q13DQuadrature8());

  DataAccess<Parameters**> P(info.da, 2, GHOSTED);
  DataAccess<double***> hardness(info.da, 3, GHOSTED);

  const int Nk = fem::q13d::n_chi;

  const int Nq_max = 16;
  Vector2 u[Nq_max], u_x[Nq_max], u_y[Nq_max], u_z[Nq_max];
  double Bq[Nq_max];
  const int Nq = element.n_pts();
  assert(Nq <= Nq_max);

  Vector2 u_nodal[Nk];
  int node_type[Nk];
  double x_nodal[Nk], y_nodal[Nk], B_nodal[Nk];
  std::vector<double> z_nodal(Nk);

  // loop over all the elements that have at least one owned node
  for (int j = info.gys; j < info.gys + info.gym - 1; j++) {
    for (int i = info.gxs; i < info.gxs + info.gxm - 1; i++) {
      for (int k = info.gzs; k < info.gzs + info.gzm - 1; k++) {

        // Element-local Jacobian matrix (there are Nk vector valued degrees of freedom
        // per element, for a total of Nk*Nk = 64 entries in the local Jacobian.
        double K[2*Nk][2*Nk];
        memset(K, 0, sizeof(K));

        // Compute coordinates of the nodes of this element and fetch node types.
        for (int n = 0; n < Nk; ++n) {
          auto I = element.local_to_global(i, j, k, n);

          auto p = P[I.j][I.i];

          node_type[n] = p.node_type;

          x_nodal[n] = grid_xy(x_min, dx, I.i);
          y_nodal[n] = grid_xy(y_min, dy, I.j);
          z_nodal[n] = grid_z(p.bed, p.thickness, info.mz, I.k);
        }

        // skip ice-free elements
        {
          // an element is exterior if one or more of its nodes are "exterior"
          bool exterior = false;
          for (int n = 0; n < Nk; ++n) {
            if (node_type[n] == NODE_EXTERIOR) {
              exterior = true;
              break;
            }
          }

          if (exterior) {
            continue;
          }
        }

        // compute values of chi, chi_x, chi_y, chi_z and quadrature weights at quadrature
        // points on this physical element
        element.reset(i, j, k, z_nodal);

        // Get nodal values of u.
        element.nodal_values(x, u_nodal);

        // Don't contribute to Dirichlet nodes
        for (int n = 0; n < Nk; ++n) {
          auto I = element.local_to_global(n);
          if (dirichlet_node(info, I)) {
            element.mark_row_invalid(n);
            element.mark_col_invalid(n);
            u_nodal[n] = u_bc(x_nodal[n], y_nodal[n], z_nodal[n]);
          }
        }

        // evaluate partial derivatives at quadrature points
        element.evaluate(u_nodal, u, u_x, u_y, u_z);

        // evaluate hardness at quadrature points
        element.nodal_values((double***)hardness, B_nodal);
        element.evaluate(B_nodal, Bq);

        // loop over all quadrature points
        for (int q = 0; q < Nq; ++q) {
          auto W = element.weight(q);

          double
            ux = u_x[q].u,
            uy = u_y[q].u,
            uz = u_z[q].u,
            vx = u_x[q].v,
            vy = u_y[q].v,
            vz = u_z[q].v;

          double gamma = (ux * ux + vy * vy + ux * vy +
                          0.25 * ((uy + vx) * (uy + vx) + uz * uz + vz * vz));

          double eta, deta;
          m_flow_law->effective_viscosity(Bq[q], gamma, &eta, &deta);

          // loop over test and trial functions, computing the upper-triangular part of
          // the element Jacobian
          for (int t = 0; t < Nk; ++t) {
            auto psi = element.chi(q, t);
            for (int s = t; s < Nk; ++s) {
              auto phi = element.chi(q, s);

              double
                gamma_u = 2.0 * ux * phi.dx + vy * phi.dx + 0.5 * phi.dy * (uy + vx) + 0.5 * uz * phi.dz,
                gamma_v = 2.0 * vy * phi.dy + ux * phi.dy + 0.5 * phi.dx * (uy + vx) + 0.5 * vz * phi.dz;

              double
                eta_u = deta * gamma_u,
                eta_v = deta * gamma_v;

              // Picard part
              K[t * 2 + 0][s * 2 + 0] += W * eta * (4.0 * psi.dx * phi.dx + psi.dy * phi.dy + psi.dz * phi.dz);
              K[t * 2 + 0][s * 2 + 1] += W * eta * (2.0 * psi.dx * phi.dy + psi.dy * phi.dx);
              K[t * 2 + 1][s * 2 + 0] += W * eta * (2.0 * psi.dy * phi.dx + psi.dx * phi.dy);
              K[t * 2 + 1][s * 2 + 1] += W * eta * (4.0 * psi.dy * phi.dy + psi.dx * phi.dx + psi.dz * phi.dz);
              // extra Newton terms
              K[t * 2 + 0][s * 2 + 0] += W * eta_u * (psi.dx * (4.0 * ux + 2.0 * vy) +
                                                      psi.dy * (uy + vx) +
                                                      psi.dz * uz);
              K[t * 2 + 0][s * 2 + 1] += W * eta_v * (psi.dx * (4.0 * ux + 2.0 * vy) +
                                                      psi.dy * (uy + vx) +
                                                      psi.dz * uz);
              K[t * 2 + 1][s * 2 + 0] += W * eta_u * (psi.dx * (uy + vx) +
                                                      psi.dy * (4.0 * vy + 2.0 * ux) +
                                                      psi.dz * vz);
              K[t * 2 + 1][s * 2 + 1] += W * eta_v * (psi.dx * (uy + vx) +
                                                      psi.dy * (4.0 * vy + 2.0 * ux) +
                                                      psi.dz * vz);
            }
          }
        } // end of the loop over q

        // fill the lower-triangular part of the element Jacobian using the fact that J is
        // symmetric
        for (int t = 0; t < Nk; ++t) {
          for (int s = 0; s < t; ++s) {
            K[t * 2 + 0][s * 2 + 0] = K[s * 2 + 0][t * 2 + 0];
            K[t * 2 + 1][s * 2 + 0] = K[s * 2 + 0][t * 2 + 1];
            K[t * 2 + 0][s * 2 + 1] = K[s * 2 + 1][t * 2 + 0];
            K[t * 2 + 1][s * 2 + 1] = K[s * 2 + 1][t * 2 + 1];
          }
        }

        element.add_contribution(&K[0][0], J);
      } // end of the loop over i
    } // end of the loop over j
  } // end of the loop over k

  // take care of Dirichlet nodes (both explicit and grid points outside the domain)
  //
  // here we loop over all the *owned* nodes
  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {
      for (int k = info.zs; k < info.zs + info.zm; k++) {
        if ((int)P[j][i].node_type == NODE_EXTERIOR or dirichlet_node(info, {i, j, k})) {

          double
            dz = std::max(P[j][i].thickness, m_grid_info.min_thickness) / (info.mz - 1);
          auto scaling = dirichlet_scale(dx, dy, dz);
          // FIXME: eta should be included in the scaling
          double identity[4] = {scaling.u, 0, 0, scaling.v};

          MatStencil row;
          row.i = k;            // STORAGE_ORDER
          row.j = i;            // STORAGE_ORDER
          row.k = j;            // STORAGE_ORDER
          ierr = MatSetValuesBlockedStencil(J, 1, &row, 1, &row, identity, ADD_VALUES);
          PISM_CHK(ierr, "MatSetValuesBlockedStencil"); // this may throw
        }
      }
    }
  }

  ierr = MatAssemblyBegin(J, MAT_FINAL_ASSEMBLY); PISM_CHK(ierr, "MatAssemblyBegin");
  ierr = MatAssemblyEnd(J, MAT_FINAL_ASSEMBLY); PISM_CHK(ierr, "MatAssemblyEnd");
  if (A != J) {
    ierr = MatAssemblyBegin(A, MAT_FINAL_ASSEMBLY); PISM_CHK(ierr, "MatAssemblyBegin");
    ierr = MatAssemblyEnd(A, MAT_FINAL_ASSEMBLY); PISM_CHK(ierr, "MatAssemblyEnd");
  }

  ierr = MatSetOption(J, MAT_NEW_NONZERO_LOCATION_ERR, PETSC_TRUE);
  PISM_CHK(ierr, "MatSetOption");

  ierr = MatSetOption(J, MAT_SYMMETRIC, PETSC_TRUE);
  PISM_CHK(ierr, "MatSetOption");
}

PetscErrorCode Blatter::function_callback(DMDALocalInfo *info,
                                           const Vector2 ***x, Vector2 ***f,
                                           CallbackData *data) {
  try {
    data->solver->compute_residual(info, x, f);
  } catch (...) {
    MPI_Comm com = MPI_COMM_SELF;
    PetscErrorCode ierr = PetscObjectGetComm((PetscObject)data->da, &com); CHKERRQ(ierr);
    handle_fatal_errors(com);
    SETERRQ(com, 1, "A PISM callback failed");
  }
  return 0;
}

PetscErrorCode Blatter::jacobian_callback(DMDALocalInfo *info,
                                           const Vector2 ***x,
                                           Mat A, Mat J, CallbackData *data) {
  try {
    data->solver->compute_jacobian(info, x, A, J);
  } catch (...) {
    MPI_Comm com = MPI_COMM_SELF;
    PetscErrorCode ierr = PetscObjectGetComm((PetscObject)data->da, &com); CHKERRQ(ierr);
    handle_fatal_errors(com);
    SETERRQ(com, 1, "A PISM callback failed");
  }
  return 0;
}

/*!
 * Allocate the Blatter-Pattyn stress balance solver
 *
 * @param[in] grid PISM's grid.
 * @param[in] Mz number of vertical levels
 * @param[in] n_levels maximum number of grid levels to use
 */
Blatter::Blatter(IceGrid::ConstPtr grid, int Mz, int n_levels)
  : ShallowStressBalance(grid) {

  auto pism_da = grid->get_dm(1, 0);

  int ierr = setup(*pism_da, Mz, n_levels);
  if (ierr != 0) {
    throw RuntimeError(PISM_ERROR_LOCATION,
                       "Failed to allocate a Blatter solver instance");
  }

  {
    int mz = Mz + grid_padding(Mz, n_levels);
    std::vector<double> sigma(mz);
    double dz = 1.0 / (mz - 1);
    for (int i = 0; i < mz; ++i) {
      sigma[i] = i * dz;
    }
    sigma.back() = 1.0;

    std::map<std::string,std::string> z_attrs =
      {{"axis", "Z"},
       {"long_name", "scaled Z-coordinate in the ice (z_base=0, z_surface=1)"},
       {"units", "1"},
       {"positive", "up"}};

    m_u.reset(new IceModelVec3Custom(grid, "u_velocity", "z_sigma", sigma, z_attrs));
    m_u->set_attrs("diagnostic", "u velocity component", "1", "1", "", 0);

    m_v.reset(new IceModelVec3Custom(grid, "v_velocity", "z_sigma", sigma, z_attrs));
    m_v->set_attrs("diagnostic", "v velocity component", "1", "1", "", 0);
  }

  {
    rheology::FlowLawFactory ice_factory("stress_balance.blatter.", m_config, m_EC);
    ice_factory.remove(ICE_GOLDSBY_KOHLSTEDT);

    ice_factory.set_default(m_config->get_string("stress_balance.blatter.flow_law"));

    m_flow_law = ice_factory.create();
  }
}

/*!
 * Restrict 2D and 3D model parameters from a fine grid to a coarse grid.
 *
 * Re-compute node types from geometry.
 *
 * This hook is called every time SNES needs to update coarse-grid data.
 *
 * FIXME: parameters restricted by this hook do not change from one SNES iteration to the
 * next, so we can return early after the first one.
 */
static PetscErrorCode blatter_restriction_hook(DM fine,
                                               Mat mrestrict, Vec rscale, Mat inject,
                                               DM coarse, void *ctx) {
  // Get rid of warnings about unused arguments
  (void) mrestrict;
  (void) rscale;
  (void) inject;
  GridInfo *grid_info = (GridInfo*)ctx;

  PetscErrorCode ierr;
  ierr = restrict_data(fine, coarse, "2D_DM"); CHKERRQ(ierr);
  ierr = restrict_data(fine, coarse, "3D_DM"); CHKERRQ(ierr);

  compute_node_type(coarse, grid_info->min_thickness);

  return 0;
}

PetscErrorCode blatter_coarsening_hook(DM dm_fine, DM dm_coarse, void *ctx) {
  PetscErrorCode ierr;

  ierr = setup_level(dm_coarse, *(GridInfo*)ctx); CHKERRQ(ierr);

  ierr = DMCoarsenHookAdd(dm_coarse, blatter_coarsening_hook, blatter_restriction_hook, ctx); CHKERRQ(ierr);

  // 2D
  ierr = create_restriction(dm_fine, dm_coarse, "2D_DM"); CHKERRQ(ierr);

  // 3D
  ierr = create_restriction(dm_fine, dm_coarse, "3D_DM"); CHKERRQ(ierr);

  return 0;
}

PetscErrorCode Blatter::setup(DM pism_da, int Mz, int n_levels) {
  PetscErrorCode ierr;
  // DM
  //
  // Note: in the PISM's DA pism_da PETSc's and PISM's meaning of x and y are the same.
  {
    PetscInt dim, Mx, My, Nx, Ny;
    PetscInt
      Nz            = 1,
      dof           = 2,        // u and v velocity components
      stencil_width = 1;

    ierr = DMDAGetInfo(pism_da,
                       &dim,
                       &Mx,
                       &My,
                       NULL,             // Mz
                       &Nx,              // number of processors in y-direction
                       &Ny,              // number of processors in x-direction
                       NULL,             // ditto, z-direction
                       NULL,             // number of degrees of freedom per node
                       NULL,             // stencil width
                       NULL, NULL, NULL, // types of ghost nodes at the boundary
                       NULL);            // stencil width
    CHKERRQ(ierr);

    assert(dim == 2);

    const PetscInt *lx, *ly;
    ierr = DMDAGetOwnershipRanges(pism_da, &lx, &ly, NULL); CHKERRQ(ierr);

    // make copies of lx and ly so that we can pad the domain
    std::vector<PetscInt>  new_lx(Nx), new_ly(Ny);
    {
      for (int k = 0; k < Nx; ++k) {
        new_lx[k] = lx[k];
      }
      for (int k = 0; k < Ny; ++k) {
        new_ly[k] = ly[k];
      }
    }

    double
      x_max = m_grid->Lx(),
      x_min = -x_max,
      y_max = m_grid->Ly(),
      y_min = -y_max;

    // pad the domain and the grid to allow for n_levels multigrid levels
    {
      // x direction
      {
        int pad_x = grid_padding(Mx, n_levels);

        new_lx[Nx - 1] += pad_x;
        Mx             += pad_x;
        x_max          += pad_x * m_grid->dx();
      }

      // y direction
      {
        int pad_y = grid_padding(My, n_levels);

        new_ly[Ny - 1] += pad_y;
        My             += pad_y;
        y_max          += pad_y * m_grid->dy();
      }

      // z direction
      Mz += grid_padding(Mz, n_levels);
    }

    ierr = DMDACreate3d(PETSC_COMM_WORLD,
                        DM_BOUNDARY_NONE, DM_BOUNDARY_NONE, DM_BOUNDARY_NONE, // STORAGE_ORDER
                        DMDA_STENCIL_BOX,
                        Mz, Mx, My,                         // STORAGE_ORDER
                        Nz, Nx, Ny,                         // STORAGE_ORDER
                        dof,                                // dof
                        stencil_width,                      // stencil width
                        NULL, new_lx.data(), new_ly.data(), // STORAGE_ORDER
                        m_da.rawptr()); CHKERRQ(ierr);

    ierr = DMSetFromOptions(m_da); CHKERRQ(ierr);

    ierr = DMSetUp(m_da); CHKERRQ(ierr);

    double min_thickness = 0.1;

    m_grid_info = {x_min, x_max,
                   y_min, y_max,
                   min_thickness,
                   sizeof(Parameters)/sizeof(double)};

    // set up 2D and 3D parameter storage
    ierr = setup_level(m_da, m_grid_info); CHKERRQ(ierr);

    // tell PETSc how to coarsen this grid and how to restrict data to a coarser grid
    ierr = DMCoarsenHookAdd(m_da, blatter_coarsening_hook, blatter_restriction_hook, &m_grid_info);
    CHKERRQ(ierr);
  }

  // Vec
  {
    ierr = DMCreateGlobalVector(m_da, m_x.rawptr()); CHKERRQ(ierr);
  }

  // SNES
  {
    ierr = SNESCreate(m_grid->com, m_snes.rawptr()); CHKERRQ(ierr);

    // ierr = SNESSetOptionsPrefix(m_snes, "blatter_"); CHKERRQ(ierr);

    ierr = SNESSetDM(m_snes, m_da); CHKERRQ(ierr);

    m_callback_data.da = m_da;
    m_callback_data.solver = this;

    ierr = DMDASNESSetFunctionLocal(m_da, INSERT_VALUES,
                                    (DMDASNESFunction)function_callback,
                                    &m_callback_data); CHKERRQ(ierr);

    ierr = DMDASNESSetJacobianLocal(m_da,
                                    (DMDASNESJacobian)jacobian_callback,
                                    &m_callback_data); CHKERRQ(ierr);

    ierr = SNESSetFromOptions(m_snes); CHKERRQ(ierr);
  }

  // set the initial guess
  // FIXME: I will need to read this from a file
  ierr = VecSet(m_x, 0.0); CHKERRQ(ierr);

  return 0;
}

/*!
 * Bottom surface elevation
 */
static double b(double x, double y) {
  (void) x;
  (void) y;
  return 0.0;
}

/*!
 * Domain thickness
 */
static double H(double x, double y) {
  double w = 1.0;
  if (std::fabs(x) <= w and std::fabs(y) <= w) {
    return 1.0;
  }
  return 0.0;
}

/*!
 * Set 2D parameters on the finest grid.
 */
void Blatter::init_2d_parameters() {

  DMDALocalInfo info;
  int ierr = DMDAGetLocalInfo(m_da, &info);
  PISM_CHK(ierr, "DMDAGetLocalInfo");
  info = grid_transpose(info);

  // Compute grid spacing from domain dimensions and the grid size
  double
    x_min = m_grid_info.x_min,
    x_max = m_grid_info.x_max,
    y_min = m_grid_info.y_min,
    y_max = m_grid_info.y_max,
    dx = (x_max - x_min) / (info.mx - 1),
    dy = (y_max - y_min) / (info.my - 1);

  DataAccess<Parameters**> P(m_da, 2, NOT_GHOSTED);

  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {
      double x = grid_xy(x_min, dx, i);
      double y = grid_xy(y_min, dy, j);

      P[j][i].bed = b(x, y);
      P[j][i].thickness = H(x, y);
    }
  }

  compute_node_type(m_da, m_grid_info.min_thickness);
}

/*!
 * Set 3D parameters on the finest grid.
 */
void Blatter::init_3d_parameters() {

  DMDALocalInfo info;
  int ierr = DMDAGetLocalInfo(m_da, &info); PISM_CHK(ierr, "DMDAGetLocalInfo");
  info = grid_transpose(info);

  // Compute grid spacing from domain dimensions and the grid size
  double
    x_min = m_grid_info.x_min,
    x_max = m_grid_info.x_max,
    y_min = m_grid_info.y_min,
    y_max = m_grid_info.y_max,
    dx = (x_max - x_min) / (info.mx - 1),
    dy = (y_max - y_min) / (info.my - 1);

  DataAccess<Parameters**> P2(m_da, 2, NOT_GHOSTED);
  DataAccess<double***> P3(m_da, 3, NOT_GHOSTED);

  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {
      for (int k = info.zs; k < info.zs + info.zm; k++) {
        double
          xx = grid_xy(x_min, dx, i),
          yy = grid_xy(y_min, dy, j),
          b  = P2[j][i].bed,
          H  = P2[j][i].thickness,
          zz = grid_z(b, H, info.mz, k);

        P3[j][i][k] = F(xx, yy, zz); // STORAGE_ORDER
      }
    }
  }
}

Blatter::~Blatter() {
  // empty
}

void Blatter::update(const Inputs &inputs, bool) {
  (void) inputs;

  init_2d_parameters();
  init_3d_parameters();

  int ierr = SNESSolve(m_snes, NULL, m_x); PISM_CHK(ierr, "SNESSolve");

  // copy the solution from m_x to m_u, m_v
  {
    Vector2 ***x = nullptr;
    ierr = DMDAVecGetArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecGetArray");

    int Mz = m_u->levels().size();

    IceModelVec::AccessList list{m_u.get(), m_v.get()};

    for (Points p(*m_grid); p; p.next()) {
      const int i = p.i(), j = p.j();

      auto u = m_u->get_column(i, j);
      auto v = m_v->get_column(i, j);

      for (int k = 0; k < Mz; ++k) {
        u[k] = x[j][i][k].u;      // STORAGE_ORDER
        v[k] = x[j][i][k].v;      // STORAGE_ORDER
      }
    }

    ierr = DMDAVecRestoreArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecRestoreArray");
  }
}

IceModelVec3Custom::Ptr Blatter::u_velocity() const {
  return m_u;
}

IceModelVec3Custom::Ptr Blatter::v_velocity() const {
  return m_v;
}

} // end of namespace stressbalance
} // end of namespace pism