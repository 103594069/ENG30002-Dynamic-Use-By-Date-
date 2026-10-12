"""Pure Q10 temperature-acceleration math for predicting a use-by date.

No file or network I/O lives here - see data_io.py for reading/writing
CSVs and pipeline.py for wiring this math to real data. Keeping this file
free of I/O is what makes every function here trivial to unit test.
"""
from __future__ import annotations


def rate_factor(temp_c: float, t_ref_c: float, q10: float) -> float:
    """Spoilage rate at temp_c, relative to the rate at t_ref_c.

    k(T) = Q10 ^ ((T - T_ref) / 10). k=1 means "spoiling at the reference
    rate", k=2 means "spoiling twice as fast as at the reference rate".
    """
    return q10 ** ((temp_c - t_ref_c) / 10.0)


def derive_q10_from_ratio(rate_ratio: float, delta_t_c: float) -> float:
    """Back out Q10 from a measured rate ratio over a temperature gap.

    If spoilage at the warmer temperature happened `rate_ratio` times
    faster than at the colder temperature, and the gap between them was
    `delta_t_c` degrees, this is the Q10 implied by that single
    measurement (Taoukis et al., 1997).
    """
    return rate_ratio ** (10.0 / delta_t_c)


def pct_shelf_life_used(equivalent_hours: float, l_ref_hours: float) -> float:
    return (equivalent_hours / l_ref_hours) * 100.0


def classify_status(pct_used: float, use_soon_pct: float, past_due_pct: float) -> str:
    if pct_used >= past_due_pct:
        return "Past use-by"
    if pct_used >= use_soon_pct:
        return "Use soon"
    return "Fresh"
