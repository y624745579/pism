// Copyright (C) 2008-2016 Ed Bueler, Constantine Khroulev, Ricarda Winkelmann,
// Gudfinna Adalgeirsdottir and Andy Aschwanden
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 3 of the License, or (at your option) any later
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

// Implementation of the atmosphere model using constant-in-time precipitation
// and a cosine yearly cycle for near-surface air temperatures.

#include <gsl/gsl_math.h>       // M_PI, GSL_NAN

#include "PAYearlyCycle.hh"
#include "base/util/PISMTime.hh"
#include "base/util/IceGrid.hh"
#include "base/util/PISMConfigInterface.hh"
#include "base/util/io/io_helpers.hh"
#include "base/util/pism_utilities.hh"

namespace pism {
namespace atmosphere {

YearlyCycle::YearlyCycle(IceGrid::ConstPtr g)
  : AtmosphereModel(g) {

  m_snow_temp_july_day = m_config->get_double("snow_temp_july_day");

  // Allocate internal IceModelVecs:
  m_air_temp_mean_annual.create(m_grid, "air_temp_mean_annual", WITHOUT_GHOSTS);
  m_air_temp_mean_annual.set_attrs("diagnostic",
                                   "mean annual near-surface air temperature (without sub-year time-dependence or forcing)",
                                   "K",
                                   "");  // no CF standard_name ??
  m_air_temp_mean_annual.metadata().set_string("source", m_reference);

  m_air_temp_mean_july.create(m_grid, "air_temp_mean_july", WITHOUT_GHOSTS);
  m_air_temp_mean_july.set_attrs("diagnostic",
                                 "mean July near-surface air temperature (without sub-year time-dependence or forcing)",
                                 "Kelvin",
                                 "");  // no CF standard_name ??
  m_air_temp_mean_july.metadata().set_string("source", m_reference);

  m_precipitation_vec.create(m_grid, "precipitation", WITHOUT_GHOSTS);
  m_precipitation_vec.metadata(0) = m_precipitation;
  // reset the name
  m_precipitation_vec.metadata(0).set_name("precipitation");
  m_precipitation_vec.write_in_glaciological_units = true;
  m_precipitation_vec.set_time_independent(true);
}

YearlyCycle::~YearlyCycle() {
  // empty
}

//! Allocates memory and reads in the precipitaion data.
void YearlyCycle::init_impl() {
  m_t = m_dt = GSL_NAN;  // every re-init restarts the clock

  InputOptions opts = process_input_options(m_grid->com);
  init_internal(opts.filename, opts.type == INIT_BOOTSTRAP, opts.record);
}

//! Read precipitation data from a given file.
void YearlyCycle::init_internal(const std::string &input_filename, bool do_regrid,
                                unsigned int start) {
  // read precipitation rate from file
  m_log->message(2,
             "    reading mean annual ice-equivalent precipitation rate 'precipitation'\n"
             "      from %s ... \n",
             input_filename.c_str());
  if (do_regrid == true) {
    m_precipitation_vec.regrid(input_filename, CRITICAL); // fails if not found!
  } else {
    m_precipitation_vec.read(input_filename, start); // fails if not found!
  }
}

void YearlyCycle::add_vars_to_output_impl(const std::string &keyword, std::set<std::string> &result) {
  result.insert("precipitation");

  if (keyword == "big" || keyword == "2dbig") {
    result.insert("air_temp_mean_annual");
    result.insert("air_temp_mean_july");
  }
}


void YearlyCycle::define_variables_impl(const std::set<std::string> &vars, const PIO &nc, IO_Type nctype) {

  if (set_contains(vars, "air_temp_mean_annual")) {
    m_air_temp_mean_annual.define(nc, nctype);
  }

  if (set_contains(vars, "air_temp_mean_july")) {
    m_air_temp_mean_july.define(nc, nctype);
  }

  if (set_contains(vars, "precipitation")) {
    m_precipitation_vec.define(nc, nctype);
  }
}


void YearlyCycle::write_variables_impl(const std::set<std::string> &vars, const PIO &nc) {

  if (set_contains(vars, "air_temp_mean_annual")) {
    m_air_temp_mean_annual.write(nc);
  }

  if (set_contains(vars, "air_temp_mean_july")) {
    m_air_temp_mean_july.write(nc);
  }

  if (set_contains(vars, "precipitation")) {
    m_precipitation_vec.write(nc);
  }
}

//! Copies the stored precipitation field into result.
void YearlyCycle::mean_precipitation_impl(IceModelVec2S &result) {
  result.copy_from(m_precipitation_vec);
}

//! Copies the stored mean annual near-surface air temperature field into result.
void YearlyCycle::mean_annual_temp_impl(IceModelVec2S &result) {
  result.copy_from(m_air_temp_mean_annual);
}

void YearlyCycle::init_timeseries_impl(const std::vector<double> &ts) {
  // constants related to the standard yearly cycle
  const double
    julyday_fraction = m_grid->ctx()->time()->day_of_the_year_to_day_fraction(m_snow_temp_july_day);

  size_t N = ts.size();

  m_ts_times.resize(N);
  m_cosine_cycle.resize(N);
  for (unsigned int k = 0; k < m_ts_times.size(); k++) {
    double tk = m_grid->ctx()->time()->year_fraction(ts[k]) - julyday_fraction;

    m_ts_times[k] = ts[k];
    m_cosine_cycle[k] = cos(2.0 * M_PI * tk);
  }
}

void YearlyCycle::precip_time_series_impl(int i, int j, std::vector<double> &result) {
  for (unsigned int k = 0; k < m_ts_times.size(); k++) {
    result[k] = m_precipitation_vec(i,j);
  }
}

void YearlyCycle::temp_time_series_impl(int i, int j, std::vector<double> &result) {

  for (unsigned int k = 0; k < m_ts_times.size(); ++k) {
    result[k] = m_air_temp_mean_annual(i,j) + (m_air_temp_mean_july(i,j) - m_air_temp_mean_annual(i,j)) * m_cosine_cycle[k];
  }
}

void YearlyCycle::begin_pointwise_access_impl() {
  m_air_temp_mean_annual.begin_access();
  m_air_temp_mean_july.begin_access();
  m_precipitation_vec.begin_access();
}

void YearlyCycle::end_pointwise_access_impl() {
  m_air_temp_mean_annual.end_access();
  m_air_temp_mean_july.end_access();
  m_precipitation_vec.end_access();
}


} // end of namespace atmosphere
} // end of namespace pism
