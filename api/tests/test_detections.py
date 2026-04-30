import csv
import io

import flockyou


def test_post_and_get_detection(client):
    payload = {
        "event": "detection",
        "detection_method": "wifi_oui_addr2",
        "protocol": "wifi_2_4ghz",
        "mac_address": "aa:bb:cc:11:22:33",
        "rssi": -51,
        "channel": 6,
        "confidence": 70,
        "confidence_label": "HIGH",
    }

    response = client.post("/api/detections", json=payload)
    assert response.status_code == 200

    detections = client.get("/api/detections").get_json()
    assert len(detections) == 1
    assert detections[0]["manufacturer"] == "Test Maker"
    assert detections[0]["confidence_label"] == "HIGH"

    filtered = client.get("/api/detections?filter=wifi_oui_addr2").get_json()
    assert len(filtered) == 1


def test_csv_export_contains_detection(client):
    client.post("/api/detections", json={
        "detection_method": "ble_raven_uuid",
        "protocol": "bluetooth_le",
        "mac_address": "aa:bb:cc:44:55:66",
        "device_name": "Raven",
        "rssi": -60,
        "confidence": 90,
        "confidence_label": "CERTAIN",
    })

    response = client.get("/api/export/csv")
    assert response.status_code == 200
    rows = list(csv.DictReader(io.StringIO(response.data.decode("utf-8"))))
    assert rows[0]["detection_method"] == "ble_raven_uuid"
    assert rows[0]["device_name"] == "Raven"


def test_kml_export_includes_gps_detection(client):
    flockyou.gps_data = {
        "latitude": 37.7749,
        "longitude": -122.4194,
        "altitude": 12,
        "timestamp": "120000",
        "satellites": 7,
        "fix_quality": 1,
    }
    client.post("/api/detections", json={
        "detection_method": "wifi_ssid",
        "protocol": "wifi_2_4ghz",
        "mac_address": "aa:bb:cc:77:88:99",
        "rssi": -48,
    })

    response = client.get("/api/export/kml")
    assert response.status_code == 200
    body = response.data.decode("utf-8")
    assert "<kml" in body
    assert "-122.4194,37.7749" in body
