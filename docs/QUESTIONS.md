# Open questions (implementation -> architect)

Format: `Qn [status] question`, then what you tried, what you recommend, and
what you did meanwhile. The architect (Chris, in the desktop-app session)
answers by editing the entry to `[answered]` with the decision, and copies
durable decisions into docs/design/00-overview.md.

Q1 [answered] HTTP serving: FastCGI behind nginx (nginx does TLS and
    connection control). In-house responder, libfcgi as fallback. See D8 and
    04-query-api.md.
Q2 [answered] eccodes: distro package; CMake must also accept a hand-built
    copy via ECCODES_ROOT. See D9 and T11. (Not installed on the dev VM, so
    the implementing session may need to install it or test with the option
    off.)
Q3 [answered] Profile (sounding) storage. Decision (Chris): levels are a
    geometric value - a distance - so vertical searching and weighting is
    consistent with the other spatial dimensions (time stays separate). Where
    a level has no height (pressure only), use geopotential height from a
    recent GRIB as the lookup.
    Impl: one report per sounding; the values table has a `height_m f32`
    column = geometric height above MSL (same meaning as the report's
    height_m; NaN on input = the report's height, stored finite). Ingest
    converts reported geopotential heights to geometric and fills
    pressure-only levels from the GRIB geopotential-height field before
    add_report(). The tree still indexes reports (one point per sounding at
    its launch position/time). Tests: `segment_profile_levels`,
    `segment_round_trip`.
    Follow-ups (open, not blocking): (a) balloon drift - levels sit tens of km
    downwind of the launch site at the top; add per-level lat/lon (or a drift
    offset) now while the format is unreleased? (b) should profile levels
    ever be individual tree points (e.g. corridor search for a flight route at
    altitude), or does T4 always find the sounding and then interpolate
    within the profile by height? 01-obs-store.md "Data model" still
    mentions a `level` column - architect to update.

Q4 [open] Variable registry (`var_code`, `param_id`): who owns it and where
    does it live? Proposal: `docs/design/variables.md` + a generated header;
    start with ~25 common surface/upper-air variables.
Q5 [open] Small choices made in T1/T2 that the spec did not pin down; all
    cheap to change, please confirm or overrule:
    a) Config: a class whose name is built in (metar, synop, sounding, pirep,
       aircraft) may omit fields, which take the built-in value; any other
       name must give every field. Unknown keys are errors. Names must match
       `[a-z][a-z0-9_-]*`, <= 31 chars (they become directory names and fit
       the 32-byte name field in segment headers). vertical_scale defaults
       to 10 for every built-in class (only the metar example gave a value).
    b) Segment headers store the full metric (name, bucket_seconds, L_km,
       tau_seconds, vertical_scale, Earth radius); `ObsSegment::matches(cls)`
       tells T4 whether a segment's prebuilt tree is valid for the current
       config. Proposal for T4: a mismatching segment is an error at adoption
       (or rebuilt in RAM), never silently mixed.
    c) Sealing uses link() to the final name instead of rename(), so a
       sealed file can never be replaced (EEXIST -> error). Directories
       created on the way are fsynced too.
    d) Rows keep insertion order (tree ids = row index); report_key is
       supplied by the caller - no hashing helper yet (belongs to ingest,
       needs the report_type definition).
