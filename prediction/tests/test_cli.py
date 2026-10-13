from pathlib import Path

from prediction.cli import main

# Resolved from this file's own location, not the cwd, so the suite runs
# from the repo root AND from inside prediction/.
MOCK_SHEET = str(Path(__file__).resolve().parent.parent / "mock_data" / "mock_sheet.csv")
MOCK_SHEET_EXPORT = str(
    Path(__file__).resolve().parent.parent / "mock_data" / "mock_sheet_export.csv"
)


def test_cli_runs_end_to_end_and_writes_predictions(tmp_path, capsys):
    out_path = tmp_path / "predictions.csv"
    main([
        "--readings", MOCK_SHEET,
        "--format", "https",
        "--predictions-out", str(out_path),
        "--trial", "ChickenTrial1",
        "--now", "2026-10-01T21:00:00",
    ])

    captured = capsys.readouterr()
    assert "Status: Fresh" in captured.out
    assert "Remaining hours:" in captured.out
    assert "WARNING: MQ-135 disagrees" in captured.out
    assert "NOTE: 1 reading(s) fell outside the Q10-supported" in captured.out
    assert "NOTE: 1 reading gap(s) longer than" in captured.out
    assert "NOTE: 2 reading(s) dropped for an implausible temperature." in captured.out
    assert out_path.exists()
    assert len(out_path.read_text(encoding="utf-8").strip().splitlines()) == 2  # header + 1 row


def test_cli_runs_against_the_real_sheet_export_schema(tmp_path, capsys):
    main([
        "--readings", MOCK_SHEET_EXPORT,
        "--format", "sheetexport",
        "--predictions-out", str(tmp_path / "predictions.csv"),
        "--now", "2026-10-10T22:00:00",
    ])

    captured = capsys.readouterr()
    assert "Status:" in captured.out


def test_cli_once_per_day_writes_once_then_skips_a_same_day_rerun(tmp_path, capsys):
    out_path = tmp_path / "daily_predictions.csv"
    args = [
        "--readings", MOCK_SHEET_EXPORT,
        "--format", "sheetexport",
        "--predictions-out", str(out_path),
        "--once-per-day",
        "--now", "2026-10-12T22:00:00",
    ]

    main(args)
    main(args)  # same --now date -> should skip, not duplicate

    captured = capsys.readouterr()
    assert "Skipped: a prediction for 2026-10-12 was already recorded." in captured.out
    lines = out_path.read_text(encoding="utf-8").strip().splitlines()
    assert len(lines) == 2  # header + 1 row, second run was a no-op
