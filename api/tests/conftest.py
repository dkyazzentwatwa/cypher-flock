import sys
from pathlib import Path

import pytest


API_DIR = Path(__file__).resolve().parents[1]
if str(API_DIR) not in sys.path:
    sys.path.insert(0, str(API_DIR))

import flockyou


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "exports").mkdir(exist_ok=True)
    flockyou.app.config.update(TESTING=True)
    flockyou.detections.clear()
    flockyou.cumulative_detections.clear()
    flockyou.gps_history.clear()
    flockyou.gps_data = None
    flockyou.next_detection_id = 1
    flockyou.oui_database = {"AABBCC": "Test Maker"}
    yield flockyou.app.test_client()
    flockyou.detections.clear()
    flockyou.cumulative_detections.clear()
    flockyou.gps_history.clear()
    flockyou.gps_data = None
