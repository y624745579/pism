// Copyright (C) 2009-2011 Ed Bueler
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
// version.
//
// PISM is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License
// along with PISM; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

#ifndef __varkenthSystem_hh
#define __varkenthSystem_hh

#include "enthSystem.hh"

//! Replacement column solver for enthalpy, to address R. Greve's concerns.
/*!
Like enthSystemCtx, just does a tridiagonal linear system for conservation
of energy in vertical column, using ice enthalpy.

Has additional enthalpy-dependent conductivity in
cold ice.  Everything is the same except that the conductivity in
solveThisColumn() has additional hardwired conductivity structure from
formula (4.37) in \ref GreveBlatter.

This represents some undesireable code duplication.  If we use this and think
it is worth keeping then FIXME: it should be made configurable and this code
duplication should be removed.
 */
class varkenthSystemCtx : public enthSystemCtx {

public:
  varkenthSystemCtx(const NCConfigVariable &config, IceModelVec3 &my_Enth3, int my_Mz, string my_prefix);
  ~varkenthSystemCtx();

  PetscErrorCode viewConstants(PetscViewer viewer, bool show_col_dependent);

  PetscErrorCode solveThisColumn(PetscScalar **x, PetscErrorCode &pivoterrorindex);

protected:
  PetscScalar       getvark(PetscScalar T);
  EnthalpyConverter *EC;  // needed to get temperature from enthalpy because that is
                          //   what conductivity depends on
};

#endif   //  ifndef __varkenthSystem_hh

