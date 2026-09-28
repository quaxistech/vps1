-- Registry seed.
--
-- Every row was verified against the live source before being written: the
-- endpoint was called, the payload parsed, and the denomination confirmed from
-- the returned magnitude as well as the source's own title. Nothing here is a
-- guessed ticker.
--
-- Four providers are implemented in this version, and between them they cover
-- every must-have instrument. Others that were verified to work — the ECB Data
-- Portal, the US Treasury CSV, Cboe, LBMA and chinamoney.com.cn first-hand —
-- are deliberately NOT seeded: an unimplemented row in this table would claim
-- a capability the binary does not have.

BEGIN;

INSERT INTO macro_providers (id, code, display_name, access, base_url, terms_url,
                             rate_limit_per_min, rate_limit_source, rate_limit_ref,
                             attribution_notice, enabled, notes) VALUES

-- Dukascopy's .bi5 datafeed is the backend of their public Historical Data
-- Export tool. Dukascopy documents the tool and the JForex library but
-- publishes NOTHING about this endpoint -- no schema, no limit, no stability
-- promise. The figure below is therefore measured, not read: sustained bursts
-- draw 503s followed by minutes of refused connections. It is set far under
-- what the server nominally accepts, because an undocumented limit can be
-- tightened without notice and a backfill that gets an IP blocked is worse
-- than a backfill that takes an extra hour.
(1, 'dukascopy', 'Dukascopy Bank historical datafeed', 'keyless',
    'https://datafeed.dukascopy.com',
    'https://www.dukascopy.com/swiss/english/marketwatch/historical/',
    6, 'observed', NULL, NULL, true,
    'LZMA alone-format .bi5; one file per hour (ticks) and per day (1m bars, exactly 1440 records). UTC, bar-open timestamps, month is ZERO-BASED in the path. Bid and ask are separate files. Prices are integers on an instrument-specific scale the feed does not carry. Measured 2026-09-10: after an initial burst the endpoint answers 200 for only ~44% of requests, and that figure is the SAME at 15s, 30s and 45s spacing (4/8, 3/8, 4/8) -- so pacing is not the lever and retries are what carry a backfill through. The rate above is courtesy, not a fix.'),

-- FRED publishes its throttle explicitly: "Up to 2 requests per second are
-- allowed before being served a 429 error code", and warns that not complying
-- "can result in a temporary block".
(2, 'fred', 'Federal Reserve Bank of St. Louis (FRED)', 'keyless',
    'https://fred.stlouisfed.org', 'https://fred.stlouisfed.org/docs/api/terms_of_use.html',
    120, 'documented', 'https://fred.stlouisfed.org/docs/api/fred/v2/errors.html',
    'This product uses the FRED(R) API but is not endorsed or certified by the Federal Reserve Bank of St. Louis.',
    true,
    'graph/fredgraph.csv?id=SERIES serves a whole series with no key. Absent observations are written "."; they are stored as MISSING with no value. Daily at best -- never presented as intraday.'),
(3, 'alfred', 'ALFRED vintage archive (St. Louis Fed)', 'keyless',
    'https://alfred.stlouisfed.org', 'https://fred.stlouisfed.org/docs/api/terms_of_use.html',
    120, 'documented', 'https://fred.stlouisfed.org/docs/api/fred/v2/errors.html',
    'This product uses the FRED(R) API but is not endorsed or certified by the Federal Reserve Bank of St. Louis.',
    true,
    'graph/alfredgraph.csv?id=SERIES&vintage_date=D returns the series as it was known on D. The value column is named SERIES_YYYYMMDD and is checked, so a server that ignored the vintage cannot pass a modern revision off as a past value.'),

-- Frankfurter states: "There are no quotas. Requests are rate-limited to
-- prevent abuse, but there are no monthly or daily caps." No number is
-- published, so the figure is ours.
(5, 'frankfurter', 'Frankfurter aggregated central-bank rates', 'keyless',
    'https://api.frankfurter.dev', 'https://frankfurter.dev',
    60, 'observed', NULL, NULL, true,
    'v2 API over 97 official sources, each declaring its own rate_type. ALWAYS queried with an explicit providers= filter: the unfiltered endpoint returns a blend that is nobody''s published rate. Symbols are written PROVIDER:BASE:QUOTE.'),

-- Registered and disabled. Enabling one is a configuration change, not a code
-- change. Credentials are read from the environment at run time and are never
-- stored in this table, in configuration, or in any log line.
(20, 'tiingo', 'Tiingo', 'api_key_required',
    'https://api.tiingo.com', 'https://www.tiingo.com/about/pricing',
    -- Documented Starter tier is 50 requests/hour, i.e. 0.83/min. Stored as 1
    -- because this column is per-minute and must not overstate the allowance;
    -- the hourly, daily and symbol caps are the binding constraints.
    1, 'documented', 'https://www.tiingo.com/about/pricing', NULL, false,
    'Starter (free): 50 requests/hour, 1000/day, 1 GB/month, 500 unique symbols/month. Credential from ALTMACRO_TIINGO_TOKEN.'),
(21, 'twelvedata', 'Twelve Data', 'api_key_required',
    'https://api.twelvedata.com', 'https://twelvedata.com/pricing',
    8, 'documented', 'https://twelvedata.com/pricing', NULL, false,
    'Basic (free): 8 requests/minute. That budget cannot carry a bulk intraday backfill, so it is not a candidate for primary history. Credential from ALTMACRO_TWELVEDATA_TOKEN.');

INSERT INTO macro_provider_metadata (provider_id) SELECT id FROM macro_providers;

COMMIT;

BEGIN;

-- ---------------------------------------------------------------------------
-- Instruments
-- ---------------------------------------------------------------------------
--
-- earliest_available_timestamp is left NULL. It is written by the bootstrap
-- from what the provider actually served, so that it records evidence rather
-- than intention. History is never synthesised backwards to make a range look
-- complete.

INSERT INTO macro_instruments (code, display_name, series_class, unit, base_asset, quote_asset,
                               denomination, native_frequency, session_calendar, derivation) VALUES

-- --- CNY / CNH: two semantics here, three in the system, never merged -------
--
-- These are not alternative sources for one number. Checked on a single date:
-- the offshore market traded 6.7096, the Federal Reserve's H.10 reference read
-- 6.7108, and the PBOC's central parity was set at 6.7766. A model that treats
-- them as interchangeable reads a policy spread as measurement noise.
('FX.USDCNH', 'USD/CNH offshore spot', 'MARKET', 'price_ratio', 'USD', 'CNH',
 'CNH per USD', 'tick', 'fx_24x5', NULL),
('REF.USDCNY.FED', 'USD/CNY spot reference (Federal Reserve H.10)', 'OFFICIAL_REFERENCE',
 'price_ratio', 'USD', 'CNY', 'CNY per USD', 'daily', 'us_business', NULL),
('REF.USDCNY.PBOC', 'USD/CNY central parity (PBOC via CFETS)', 'OFFICIAL_REFERENCE',
 'price_ratio', 'USD', 'CNY', 'CNY per USD', 'daily', 'cn_business', NULL),

-- --- Intraday FX and metals -------------------------------------------------
('METAL.XAUUSD', 'Gold spot', 'MARKET', 'price_ratio', 'XAU', 'USD',
 'USD per troy ounce', 'tick', 'fx_24x5', NULL),
('FX.EURUSD', 'EUR/USD spot', 'MARKET', 'price_ratio', 'EUR', 'USD',
 'USD per EUR', 'tick', 'fx_24x5', NULL),
('FX.USDJPY', 'USD/JPY spot', 'MARKET', 'price_ratio', 'USD', 'JPY',
 'JPY per USD', 'tick', 'fx_24x5', NULL),

-- --- Daily FX references ----------------------------------------------------
-- USD/KRW has no keyless intraday source: Dukascopy answers 404 for USDKRW,
-- and a deliberately nonexistent control symbol answers 404 in exactly the
-- same way, so that is real absence and not a transport problem. It is carried
-- as a daily reference only and must never be presented as intraday.
('REF.USDKRW.FED', 'USD/KRW spot reference (Federal Reserve H.10)', 'OFFICIAL_REFERENCE',
 'price_ratio', 'USD', 'KRW', 'KRW per USD', 'daily', 'us_business', NULL),
('REF.GBPUSD.FED', 'GBP/USD spot reference (Federal Reserve H.10)', 'OFFICIAL_REFERENCE',
 'price_ratio', 'GBP', 'USD', 'USD per GBP', 'daily', 'us_business', NULL),
('REF.AUDUSD.FED', 'AUD/USD spot reference (Federal Reserve H.10)', 'OFFICIAL_REFERENCE',
 'price_ratio', 'AUD', 'USD', 'USD per AUD', 'daily', 'us_business', NULL),
('REF.USDJPY.FED', 'USD/JPY spot reference (Federal Reserve H.10)', 'OFFICIAL_REFERENCE',
 'price_ratio', 'USD', 'JPY', 'JPY per USD', 'daily', 'us_business', NULL),
('REF.EURUSD.FED', 'EUR/USD spot reference (Federal Reserve H.10)', 'OFFICIAL_REFERENCE',
 'price_ratio', 'EUR', 'USD', 'USD per EUR', 'daily', 'us_business', NULL),

-- --- Rates ------------------------------------------------------------------
('RATE.US.2Y', 'US Treasury constant maturity 2Y (nominal)', 'OFFICIAL_REFERENCE',
 'percent_per_annum', NULL, NULL, 'percent per annum', 'daily', 'us_business', NULL),
('RATE.US.10Y', 'US Treasury constant maturity 10Y (nominal)', 'OFFICIAL_REFERENCE',
 'percent_per_annum', NULL, NULL, 'percent per annum', 'daily', 'us_business', NULL),
('RATE.US.10Y.REAL', 'US Treasury constant maturity 10Y (inflation-indexed)', 'OFFICIAL_REFERENCE',
 'percent_per_annum', NULL, NULL, 'percent per annum', 'daily', 'us_business', NULL),

-- --- Risk -------------------------------------------------------------------
('IDX.VIX', 'Cboe Volatility Index', 'OFFICIAL_REFERENCE',
 'index_points', NULL, NULL, 'index points', 'daily', 'us_equity', NULL),
('IDX.SPX', 'S&P 500 index', 'OFFICIAL_REFERENCE',
 'index_points', NULL, NULL, 'index points', 'daily', 'us_equity', NULL),
('IDX.NDX', 'NASDAQ-100 index', 'OFFICIAL_REFERENCE',
 'index_points', NULL, NULL, 'index points', 'daily', 'us_equity', NULL);

COMMIT;

BEGIN;

-- ---------------------------------------------------------------------------
-- Instrument -> provider bindings
-- ---------------------------------------------------------------------------
--
-- Every native_symbol below was called and returned a parseable payload.
-- Dukascopy answers 404 for a symbol it does not carry, verified against a
-- deliberately nonexistent control, so a 200 is positive evidence rather than
-- an assumption.
--
-- decode_scale exists only for Dukascopy, whose prices are scaled integers
-- with no scale in the payload. Each value below was established by decoding a
-- real file and checking the result against the same instrument's official
-- reference for the same date -- not inferred from the quote currency. The
-- sanity band is what makes a wrong scale loud: a factor-of-ten error produces
-- a series that is internally perfect and externally absurd, and without the
-- band it would pass every ordering check and be discovered months later by a
-- model that had already learned from it.

CREATE TEMP TABLE _src (code TEXT, provider TEXT, native TEXT, basis TEXT,
                        ts_semantics TEXT, tz TEXT, priority SMALLINT,
                        scale NUMERIC, smin NUMERIC, smax NUMERIC) ON COMMIT DROP;

INSERT INTO _src VALUES
-- Dukascopy: both sides of the market, stored as separate series.
-- Scales verified 2026-09-10: USDCNH bid decoded 6.70964 against the H.10
-- reference of 6.7108 the same week; XAUUSD decoded 4402.705 against the LBMA
-- PM auction of 4414.90 the same day.
('FX.USDCNH',    'dukascopy', 'USDCNH', 'bid', 'bar_open', 'UTC', 1, 0.00001,    5.0,    9.0),
('FX.USDCNH',    'dukascopy', 'USDCNH', 'ask', 'bar_open', 'UTC', 1, 0.00001,    5.0,    9.0),
('METAL.XAUUSD', 'dukascopy', 'XAUUSD', 'bid', 'bar_open', 'UTC', 1, 0.001,    500.0, 20000.0),
('METAL.XAUUSD', 'dukascopy', 'XAUUSD', 'ask', 'bar_open', 'UTC', 1, 0.001,    500.0, 20000.0),
('FX.EURUSD',    'dukascopy', 'EURUSD', 'bid', 'bar_open', 'UTC', 1, 0.00001,    0.5,    2.0),
('FX.EURUSD',    'dukascopy', 'EURUSD', 'ask', 'bar_open', 'UTC', 1, 0.00001,    0.5,    2.0),
('FX.USDJPY',    'dukascopy', 'USDJPY', 'bid', 'bar_open', 'UTC', 1, 0.001,     50.0,  400.0),
('FX.USDJPY',    'dukascopy', 'USDJPY', 'ask', 'bar_open', 'UTC', 1, 0.001,     50.0,  400.0),

-- FRED: daily reference values. The observation date is a date, not an
-- instant, and the timestamp semantics say so rather than implying a time the
-- source never stated. Titles confirmed from the source: DEXCHUS is "Chinese
-- Yuan Renminbi to U.S. Dollar", DEXUSUK is "U.S. Dollars to U.K. Pound
-- Sterling", and so on -- the direction of each pair is read, not assumed.
('REF.USDCNY.FED',  'fred', 'DEXCHUS',   'fixing', 'observation_date', 'America/New_York', 1, NULL,    1.0,     12.0),
('REF.USDKRW.FED',  'fred', 'DEXKOUS',   'fixing', 'observation_date', 'America/New_York', 1, NULL,  500.0,   3000.0),
('REF.GBPUSD.FED',  'fred', 'DEXUSUK',   'fixing', 'observation_date', 'America/New_York', 1, NULL,    0.8,      3.0),
('REF.AUDUSD.FED',  'fred', 'DEXUSAL',   'fixing', 'observation_date', 'America/New_York', 1, NULL,    0.3,      2.0),
('REF.USDJPY.FED',  'fred', 'DEXJPUS',   'fixing', 'observation_date', 'America/New_York', 1, NULL,   50.0,    400.0),
('REF.EURUSD.FED',  'fred', 'DEXUSEU',   'fixing', 'observation_date', 'America/New_York', 1, NULL,    0.5,      2.0),
-- Yields are legitimately negative, so the lower bound is below zero on
-- purpose: clamping it at zero would reject a real and important market state.
('RATE.US.2Y',      'fred', 'DGS2',      'not_applicable', 'observation_date', 'America/New_York', 1, NULL,  -5.0,  25.0),
('RATE.US.10Y',     'fred', 'DGS10',     'not_applicable', 'observation_date', 'America/New_York', 1, NULL,  -5.0,  25.0),
('RATE.US.10Y.REAL','fred', 'DFII10',    'not_applicable', 'observation_date', 'America/New_York', 1, NULL, -10.0,  15.0),
('IDX.VIX',         'fred', 'VIXCLS',    'close', 'observation_date', 'America/New_York', 1, NULL,     1.0,    200.0),
('IDX.SPX',         'fred', 'SP500',     'close', 'observation_date', 'America/New_York', 1, NULL,    50.0, 100000.0),
('IDX.NDX',         'fred', 'NASDAQ100', 'close', 'observation_date', 'America/New_York', 1, NULL,    50.0, 200000.0),

-- The PBOC fixing. Frankfurter's CFETS provider carries it back to 2006-01-04;
-- chinamoney.com.cn is the first-hand publisher but serves only the current
-- day, so it is deferred rather than seeded. The two were checked against each
-- other and agreed exactly (6.7766 on 2026-09-10), which is why the
-- republisher is trusted with the history.
('REF.USDCNY.PBOC', 'frankfurter', 'CFETS:USD:CNY', 'fixing', 'observation_date', 'Asia/Shanghai', 1, NULL, 1.0, 12.0);

INSERT INTO macro_instrument_sources
    (instrument_id, provider_id, native_symbol, price_basis, timestamp_semantics,
     source_timezone, priority, decode_scale, sanity_min, sanity_max)
SELECT i.id, p.id, s.native, s.basis, s.ts_semantics, s.tz, s.priority, s.scale, s.smin, s.smax
FROM _src s
JOIN macro_instruments i ON i.code = s.code
JOIN macro_providers   p ON p.code = s.provider;

COMMIT;
