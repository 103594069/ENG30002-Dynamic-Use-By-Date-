from datetime import datetime, timedelta

from prediction.models import Reading
from prediction.q10_model import (
    accumulate_equivalent_hours,
    classify_status,
    count_outside_q10_range,
    derive_q10_from_ratio,
    filter_plausible_temperatures,
    forecast_temperature,
    mq135_disagrees,
    pct_shelf_life_used,
    predict_use_by,
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


def _reading(ts: datetime, temp_c) -> Reading:
    return Reading(
        server_ts=ts, device_ts=None, trial="T", source="TEST",
        temp_c=temp_c, humidity_pct=None, pressure_hpa=None, lux=None,
        mq135_raw=None, gas_base=None, gas_increase=None, risk=None,
    )


def test_accumulate_equivalent_hours_at_reference_temp():
    readings = [
        _reading(datetime(2026, 1, 1, 0, 0), 4.0),
        _reading(datetime(2026, 1, 1, 10, 0), 4.0),
    ]
    result = accumulate_equivalent_hours(readings, t_ref_c=4.0, q10=1.75, max_dt_hours=24.0)
    assert abs(result.equivalent_hours - 10.0) < 1e-6
    assert result.gaps == []


def test_accumulate_equivalent_hours_caps_long_gaps():
    readings = [
        _reading(datetime(2026, 1, 1, 0, 0), 4.0),
        _reading(datetime(2026, 1, 1, 5, 0), 4.0),  # 5h gap, cap at 2h
    ]
    result = accumulate_equivalent_hours(readings, t_ref_c=4.0, q10=1.75, max_dt_hours=2.0)
    assert abs(result.equivalent_hours - 2.0) < 1e-6
    assert len(result.gaps) == 1
    assert result.gaps[0].actual_hours == 5.0
    assert result.gaps[0].capped_hours == 2.0


def test_filter_plausible_temperatures_drops_missing_and_out_of_range():
    readings = [
        _reading(datetime(2026, 1, 1, 0, 0), 4.0),
        _reading(datetime(2026, 1, 1, 0, 30), None),
        _reading(datetime(2026, 1, 1, 1, 0), 90.0),
    ]
    clean, dropped = filter_plausible_temperatures(readings, (-10.0, 60.0))
    assert len(clean) == 1
    assert dropped == 2


def test_forecast_temperature_averages_window():
    now = datetime(2026, 1, 2, 0, 0)
    readings = [
        _reading(now - timedelta(hours=1), 20.0),
        _reading(now - timedelta(hours=2), 24.0),
        _reading(now - timedelta(hours=20), 100.0),  # outside the 12h window
    ]
    assert forecast_temperature(readings, window_hours=12.0, now=now) == 22.0


def test_forecast_temperature_returns_none_when_window_is_empty():
    now = datetime(2026, 1, 2, 0, 0)
    readings = [_reading(now - timedelta(hours=20), 100.0)]
    assert forecast_temperature(readings, window_hours=12.0, now=now) is None


def test_predict_use_by_remaining_hours_at_reference_rate():
    now = datetime(2026, 1, 1, 0, 0)
    prediction = predict_use_by(
        equivalent_hours=10.0, l_ref_hours=48.0, forecast_temp_c=4.0,
        t_ref_c=4.0, q10=1.75, now=now,
    )
    assert abs(prediction.remaining_hours - 38.0) < 1e-6
    assert prediction.predicted_use_by == now + timedelta(hours=38.0)


def test_mq135_disagrees_only_when_model_says_fresh_but_sensor_says_danger():
    assert mq135_disagrees("Fresh", gas_increase=6.0, danger_ratio=5.0) is True
    assert mq135_disagrees("Fresh", gas_increase=1.0, danger_ratio=5.0) is False
    assert mq135_disagrees("Use soon", gas_increase=6.0, danger_ratio=5.0) is False
    assert mq135_disagrees("Fresh", gas_increase=None, danger_ratio=5.0) is False


def test_count_outside_q10_range_counts_readings_outside_the_source_data_range():
    readings = [
        _reading(datetime(2026, 1, 1, 0, 0), 4.0),   # inside (4.0, 25.0)
        _reading(datetime(2026, 1, 1, 1, 0), 3.9),   # below range
        _reading(datetime(2026, 1, 1, 2, 0), 30.0),  # above range
        _reading(datetime(2026, 1, 1, 3, 0), None),  # missing, not counted here
    ]
    assert count_outside_q10_range(readings, (4.0, 25.0)) == 2
