from prediction.cli import main

MOCK_SHEET = "prediction/mock_data/mock_sheet.csv"


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
    assert "WARNING: MQ-135 disagrees" in captured.out
    assert "NOTE: 1 reading(s) fell outside the Q10-supported" in captured.out
    assert "NOTE: 1 reading gap(s) longer than" in captured.out
    assert "NOTE: 2 reading(s) dropped for an implausible temperature." in captured.out
    assert out_path.exists()
    assert len(out_path.read_text(encoding="utf-8").strip().splitlines()) == 2  # header + 1 row
