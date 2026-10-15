-- Which plan-geometry definition produced a plan (QUALITY-01/02/03, DESIGN-02,
-- QUALITY-05). Additive; existing rows are left exactly as written.
--
-- Version 1 is every row written before this column existed: AltML's excursion
-- FRACTIONS read as bps (a forecast 1 % move became a 0.0001 % target, nudged
-- one tick off the entry), the LONG excursion used for SHORT plans, and a tick
-- derived from the price magnitude. Version 2 (trade_plan.hpp
-- kPlanGeometryVersion) reads side-aware bps magnitudes from the candidate's
-- ACTIVE prediction, rounds to the venue's tick at T0 and never rounds a stop
-- toward its entry. Outcomes of the two describe different plans and are
-- compared, learned from and reported within one version only; NULL here
-- means version 1.
ALTER TABLE altstrategy.trade_signal_proposals ADD COLUMN IF NOT EXISTS plan_geometry_version SMALLINT;
ALTER TABLE altstrategy.trade_plan_variants ADD COLUMN IF NOT EXISTS plan_geometry_version SMALLINT;

-- The grid the variant's prices sit on, and where it came from:
-- instrument_spec (AltSpotter instrument_spec_versions in force at T0),
-- config, or derived (a guess from the price magnitude).
ALTER TABLE altstrategy.trade_plan_variants ADD COLUMN IF NOT EXISTS tick_size DOUBLE PRECISION;
ALTER TABLE altstrategy.trade_plan_variants ADD COLUMN IF NOT EXISTS tick_source TEXT;
