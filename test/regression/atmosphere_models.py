#!/usr/bin/env python
"""
Tests of PISM's atmosphere models and modifiers.
"""

import PISM
from PISM.testing import *
import os
import numpy as np
from unittest import TestCase

config = PISM.Context().config

seconds_per_year = 365 * 86400
# ensure that this is the correct year length
config.set_string("time.calendar", "365_day")

# silence models' initialization messages
PISM.Context().log.set_threshold(1)

options = PISM.PETSc.Options()

def write_state(model):
    "Test writing of the model state"

    o_filename = "tmp_model_state.nc"
    o_diagnostics = "tmp_diagnostics.nc"

    try:
        output = PISM.util.prepare_output(o_filename)
        model.define_model_state(output)
        model.write_model_state(output)
        output.close()

        ds = model.diagnostics()
        output = PISM.util.prepare_output(o_diagnostics)

        for d in ds:
            ds[d].define(output, PISM.PISM_DOUBLE)

        for d in ds:
            ds[d].compute().write(output)

        output.close()

    finally:
        os.remove(o_filename)
        os.remove(o_diagnostics)

def check_model(model, T, P, ts=None, Ts=None, Ps=None):
    check(model.mean_annual_temp(), T)
    check(model.mean_precipitation(), P)

    model.init_timeseries(ts)

    try:
        model.begin_pointwise_access()
        np.testing.assert_almost_equal(model.temp_time_series(0, 0), Ts)
        np.testing.assert_almost_equal(model.precip_time_series(0, 0), Ps)
    finally:
        model.end_pointwise_access()

    write_state(model)

    model.max_timestep(ts[0])

def check_modifier(model, modifier, T=0.0, P=0.0, ts=None, Ts=None, Ps=None):
    check_difference(modifier.mean_annual_temp(),
                     model.mean_annual_temp(),
                     T)
    check_difference(modifier.mean_precipitation(),
                     model.mean_precipitation(),
                     P)

    model.init_timeseries(ts)
    modifier.init_timeseries(ts)

    try:
        model.begin_pointwise_access()
        modifier.begin_pointwise_access()

        Ts_model = np.array(model.temp_time_series(0, 0))
        Ts_modifier = np.array(modifier.temp_time_series(0, 0))

        Ps_model = np.array(model.precip_time_series(0, 0))
        Ps_modifier = np.array(modifier.precip_time_series(0, 0))

        np.testing.assert_almost_equal(Ts_modifier - Ts_model, Ts)
        np.testing.assert_almost_equal(Ps_modifier - Ps_model, Ps)
    finally:
        modifier.end_pointwise_access()
        model.end_pointwise_access()

    write_state(modifier)

    modifier.max_timestep(ts[0])

def precipitation(grid, value):
    precip = PISM.IceModelVec2S(grid, "precipitation", PISM.WITHOUT_GHOSTS)
    precip.set_attrs("climate", "precipitation", "kg m-2 s-1", "precipitation_flux")
    precip.set(value)
    return precip

def air_temperature(grid, value):
    temperature = PISM.IceModelVec2S(grid, "air_temp", PISM.WITHOUT_GHOSTS)
    temperature.set_attrs("climate", "near-surface air temperature", "Kelvin", "")
    temperature.set(value)
    return temperature

class PIK(TestCase):
    def setUp(self):
        self.filename = "atmosphere_pik_input.nc"
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)

        self.geometry.latitude.set(-80.0)

        self.P = 10.0           # this is very high, but that's fine

        precipitation(self.grid, self.P).dump(self.filename)

        config.set_string("atmosphere.pik.file", self.filename)

    def test_atmosphere_pik(self):
        "Model 'pik'"

        parameterizations = {"martin" : (248.13, [248.13], [self.P]),
                             "huybrechts_dewolde" : (252.59, [237.973373], [self.P]),
                             "martin_huybrechts_dewolde" : (248.13, [233.51337298], [self.P]),
                             "era_interim" : (256.27, [243.0939774], [self.P]),
                             "era_interim_sin" : (255.31577, [241.7975841], [self.P]),
                             "era_interim_lon" : (248.886139, [233.3678998], [self.P])}

        for p, (T, Ts, Ps) in parameterizations.items():
            config.set_string("atmosphere.pik.parameterization", p)

            model = PISM.AtmospherePIK(self.grid)
            model.init(self.geometry)

            # t and dt are irrelevant here
            model.update(self.geometry, 0, 1)

            check_model(model, T=T, P=self.P, ts=[0.5], Ts=Ts, Ps=Ps)

        assert model.max_timestep(0).infinite()

        try:
            config.set_string("atmosphere.pik.parameterization", "invalid")
            model = PISM.AtmospherePIK(self.grid)
            assert False, "failed to catch an invalid parameterization"
        except RuntimeError:
            pass

    def tearDown(self):
        os.remove(self.filename)

class DeltaT(TestCase):
    def setUp(self):
        self.filename = "delta_T_input.nc"
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)
        self.geometry.ice_thickness.set(1000.0)
        self.model = PISM.AtmosphereUniform(self.grid)
        self.dT = -5.0

        create_scalar_forcing(self.filename, "delta_T", "Kelvin", [self.dT], [0])

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_delta_t(self):
        "Modifier Delta_T"

        modifier = PISM.AtmosphereDeltaT(self.grid, self.model)

        options.setValue("-atmosphere_delta_T_file", self.filename)

        modifier.init(self.geometry)
        modifier.update(self.geometry, 0, 1)

        check_modifier(self.model, modifier, T=self.dT, ts=[0.5], Ts=[self.dT], Ps=[0])

class DeltaP(TestCase):
    def setUp(self):
        self.filename = "delta_P_input.nc"
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)
        self.geometry.ice_thickness.set(1000.0)
        self.model = PISM.AtmosphereUniform(self.grid)
        self.dP = 5.0

        create_scalar_forcing(self.filename, "delta_P", "kg m-2 s-1", [self.dP], [0])

        options.setValue("-atmosphere_delta_P_file", self.filename)

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_delta_p(self):
        "Modifier 'delta_P'"

        modifier = PISM.AtmosphereDeltaP(self.grid, self.model)

        modifier.init(self.geometry)
        modifier.update(self.geometry, 0, 1)

        check_modifier(self.model, modifier, P=self.dP, ts=[0.5], Ts=[0], Ps=[self.dP])

class Given(TestCase):
    def setUp(self):
        self.filename = "atmosphere_given_input.nc"
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)

        self.P = 10.0
        self.T = 250.0

        output = PISM.util.prepare_output(self.filename)
        precipitation(self.grid, self.P).write(output)
        air_temperature(self.grid, self.T).write(output)

        config.set_string("atmosphere.given.file", self.filename)

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_given(self):
        "Model 'given'"

        model = PISM.AtmosphereGiven(self.grid)

        model.init(self.geometry)
        model.update(self.geometry, 0, 1)

        check_model(model, T=self.T, P=self.P, ts=[0.5], Ts=[self.T], Ps=[self.P])

class SeaRISE(TestCase):
    def setUp(self):
        self.filename = "atmosphere_searise_input.nc"
        self.grid = shallow_grid()

        self.geometry = PISM.Geometry(self.grid)
        self.geometry.latitude.set(70.0)
        self.geometry.longitude.set(-45.0)
        self.geometry.ice_thickness.set(2500.0)
        self.geometry.ensure_consistency(0.0)

        self.P = 10.0

        output = PISM.util.prepare_output(self.filename)
        precipitation(self.grid, self.P).write(output)

        options.setValue("-atmosphere_searise_greenland_file", self.filename)

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_searise_greenland(self):
        "Model 'searise_greenland'"

        model = PISM.AtmosphereSeaRISEGreenland(self.grid)

        model.init(self.geometry)

        model.update(self.geometry, 0, 1)

        check_model(model, P=self.P, T=251.9085, ts=[0.5], Ts=[238.66192632], Ps=[self.P])

class YearlyCycle(TestCase):
    def setUp(self):
        self.filename = "yearly_cycle.nc"
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)

        self.T_mean = 250.0
        self.T_summer = 270.0
        self.P = 15.0

        output = PISM.util.prepare_output(self.filename)
        precipitation(self.grid, self.P).write(output)

        T_mean = PISM.IceModelVec2S(self.grid, "air_temp_mean_annual", PISM.WITHOUT_GHOSTS)
        T_mean.set_attrs("climate", "mean annual near-surface air temperature", "K", "")
        T_mean.set(self.T_mean)
        T_mean.write(output)

        T_summer = PISM.IceModelVec2S(self.grid, "air_temp_mean_summer", PISM.WITHOUT_GHOSTS)
        T_summer.set_attrs("climate", "mean summer near-surface air temperature", "K", "")
        T_summer.set(self.T_summer)
        T_summer.write(output)

        options.setValue("-atmosphere_yearly_cycle_file", self.filename)

        # FIXME: test "-atmosphere_yearly_cycle_scaling_file", too

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_yearly_cycle(self):
        "Model 'yearly_cycle'"

        model = PISM.AtmosphereCosineYearlyCycle(self.grid)

        model.init(self.geometry)

        one_year = 365 * 86400.0
        model.update(self.geometry, 0, one_year)

        summer_peak = config.get_double("atmosphere.fausto_air_temp.summer_peak_day") / 365.0

        ts = np.linspace(0, one_year, 13)
        cycle = np.cos(2.0 * np.pi * (ts / one_year - summer_peak))
        T = (self.T_summer - self.T_mean) * cycle + self.T_mean
        P = np.zeros_like(T) + self.P

        check_model(model, T=self.T_mean, P=self.P, ts=ts, Ts=T, Ps=P)

class OneStation(TestCase):
    def setUp(self):
        self.filename = "one_station.nc"
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)
        self.T = 263.15
        self.P = 10.0

        time_name = config.get_string("time.dimension_name")

        output = PISM.util.prepare_output(self.filename, append_time=True)

        output.redef()
        output.def_var("precipitation", PISM.PISM_DOUBLE, [time_name])
        output.put_att_text("precipitation", "units", "kg m-2 s-1")

        output.def_var("air_temp", PISM.PISM_DOUBLE, [time_name])
        output.put_att_text("air_temp", "units", "Kelvin")

        output.put_1d_var("precipitation", 0, 1, [self.P])
        output.put_1d_var("air_temp", 0, 1, [self.T])

        output.close()

        options.setValue("-atmosphere_one_station_file", self.filename)

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_one_station(self):
        "Model 'weather_station'"

        model = PISM.AtmosphereWeatherStation(self.grid)

        model.init(self.geometry)

        model.update(self.geometry, 0, 1)

        check_model(model, P=self.P, T=self.T, ts=[0.5], Ts=[self.T], Ps=[self.P])

class Uniform(TestCase):
    def setUp(self):
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)
        self.P = 5.0
        self.T = 250.0

        config.set_double("atmosphere.uniform.temperature", self.T)
        config.set_double("atmosphere.uniform.precipitation", self.P)

    def test_atmosphere_uniform(self):
        "Model 'uniform'"
        model = PISM.AtmosphereUniform(self.grid)

        model.init(self.geometry)

        model.update(self.geometry, 0, 1)

        P = PISM.util.convert(self.P, "kg m-2 year-1", "kg m-2 s-1")
        check_model(model, T=self.T, P=P, ts=[0.5], Ts=[self.T], Ps=[P])

class Anomaly(TestCase):
    def setUp(self):
        self.filename = "delta_T_input.nc"
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)
        self.geometry.ice_thickness.set(1000.0)
        self.model = PISM.AtmosphereUniform(self.grid)
        self.dT = -5.0
        self.dP = 20.0

        dT = PISM.IceModelVec2S(self.grid, "air_temp_anomaly", PISM.WITHOUT_GHOSTS)
        dT.set_attrs("climate", "air temperature anomaly", "Kelvin", "")
        dT.set(self.dT)

        dP = PISM.IceModelVec2S(self.grid, "precipitation_anomaly", PISM.WITHOUT_GHOSTS)
        dP.set_attrs("climate", "precipitation anomaly", "kg m-2 s-1", "")
        dP.set(self.dP)

        output = PISM.util.prepare_output(self.filename)
        dT.write(output)
        dP.write(output)

        config.set_string("atmosphere.anomaly.file", self.filename)

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_anomaly(self):
        "Modifier 'anomaly'"

        modifier = PISM.AtmosphereAnomaly(self.grid, self.model)

        modifier.init(self.geometry)

        modifier.update(self.geometry, 0, 1)

        check_modifier(self.model, modifier, T=self.dT, P=self.dP,
                       ts=[0.5], Ts=[self.dT], Ps=[self.dP])

class PaleoPrecip(TestCase):
    def setUp(self):
        self.filename = "paleo_precip_input.nc"
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)
        self.geometry.ice_thickness.set(1000.0)
        self.model = PISM.AtmosphereUniform(self.grid)
        self.dT = 5.0

        create_scalar_forcing(self.filename, "delta_T", "Kelvin", [self.dT], [0])

        options.setValue("-atmosphere_paleo_precip_file", self.filename)

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_paleo_precip(self):
        "Modifier 'paleo_precip'"

        modifier = PISM.AtmospherePaleoPrecip(self.grid, self.model)

        modifier.init(self.geometry)

        modifier.update(self.geometry, 0, 1)

        check_modifier(self.model, modifier, P=1.3373514942327523e-05,
                       ts=[0.5], Ts=[0], Ps=[1.33735149e-05])

class FracP(TestCase):
    def setUp(self):
        self.filename = "frac_P_input.nc"
        self.grid = shallow_grid()
        self.geometry = PISM.Geometry(self.grid)
        self.geometry.ice_thickness.set(1000.0)
        self.model = PISM.AtmosphereUniform(self.grid)
        self.P_ratio = 5.0

        create_scalar_forcing(self.filename, "frac_P", "1", [self.P_ratio], [0])

        options.setValue("-atmosphere_frac_P_file", self.filename)

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_frac_p(self):
        "Modifier 'frac_P'"

        modifier = PISM.AtmosphereFracP(self.grid, self.model)

        modifier.init(self.geometry)

        modifier.update(self.geometry, 0, 1)

        check_ratio(modifier.mean_precipitation(), self.model.mean_precipitation(),
                    self.P_ratio)

        check_modifier(self.model, modifier, T=0, P=0.00012675505856327396,
                       ts=[0.5], Ts=[0], Ps=[0.00012676])


class LapseRates(TestCase):
    def setUp(self):
        self.filename = "reference_surface.nc"
        self.grid = shallow_grid()
        self.model = PISM.AtmosphereUniform(self.grid)
        self.dTdz = 1.0         # Kelvin per km
        self.dPdz = 1000.0      # (kg/m^2)/year per km
        self.dz = 1000.0        # m
        self.dT = -self.dTdz * self.dz / 1000.0
        self.dP = -PISM.util.convert(self.dPdz * self.dz / 1000.0, "kg m-2 year-1", "kg m-2 s-1")

        self.geometry = PISM.Geometry(self.grid)

        # save current surface elevation to use it as a "reference" surface elevation
        self.geometry.ice_surface_elevation.dump(self.filename)

        config.set_string("atmosphere.lapse_rate.file", self.filename)

        config.set_double("atmosphere.lapse_rate.precipitation_lapse_rate", self.dPdz)

        options.setValue("-temp_lapse_rate", self.dTdz)

    def tearDown(self):
        os.remove(self.filename)

    def test_atmosphere_lapse_rate(self):
        "Modifier 'lapse_rate'"

        modifier = PISM.AtmosphereLapseRates(self.grid, self.model)

        modifier.init(self.geometry)

        # change surface elevation
        self.geometry.ice_surface_elevation.shift(self.dz)

        # check that the temperature changed accordingly
        modifier.update(self.geometry, 0, 1)
        check_modifier(self.model, modifier, T=self.dT, P=self.dP,
                       ts=[0.5], Ts=[self.dT], Ps=[self.dP])