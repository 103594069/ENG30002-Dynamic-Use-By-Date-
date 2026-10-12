from prediction.q10_model import (
    classify_status,
    derive_q10_from_ratio,
    pct_shelf_life_used,
    rate_factor,
)


def test_rate_factor_is_one_at_reference_temperature():
    assert rate_factor(4.0, t_ref_c=4.0, q10=1.75) == 1.0


def test_rate_factor_is_q10_ten_degrees_above_reference():
    assert abs(rate_factor(14.0, t_ref_c=4.0, q10=1.75) - 1.75) < 1e-6


def test_derive_q10_from_ratio_matches_mumbre_numbers():
    # 12 days / 4 days = 3x slower at 4C across a 21C gap -> Q10 ~= 1.69.
    assert abs(derive_q10_from_ratio(3.0, 21.0) - 1.69) < 0.01


def test_pct_shelf_life_used():
    assert pct_shelf_life_used(24.0, 48.0) == 50.0


def test_classify_status_bands():
    assert classify_status(50.0, use_soon_pct=70.0, past_due_pct=100.0) == "Fresh"
    assert classify_status(80.0, use_soon_pct=70.0, past_due_pct=100.0) == "Use soon"
    assert classify_status(120.0, use_soon_pct=70.0, past_due_pct=100.0) == "Past use-by"
