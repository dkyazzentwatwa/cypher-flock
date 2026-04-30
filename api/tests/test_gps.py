import time

import flockyou


def test_parse_nmea_gga_sentence():
    parsed = flockyou.parse_nmea_sentence(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"
    )

    assert parsed["latitude"] == 48.1173
    assert parsed["longitude"] == 11.51666667
    assert parsed["fix_quality"] == 1
    assert parsed["satellites"] == 8


def test_gps_ports_route(client, monkeypatch):
    class Port:
        device = "/dev/cu.test"
        description = "Test GPS"
        manufacturer = "GPS Inc"
        product = "NMEA"
        vid = 1234
        pid = 5678

    monkeypatch.setattr(flockyou.serial.tools.list_ports, "comports", lambda: [Port()])

    ports = client.get("/api/gps/ports").get_json()
    assert ports[0]["device"] == "/dev/cu.test"
    assert ports[0]["manufacturer"] == "GPS Inc"


def test_serial_detection_gets_best_recent_gps(client):
    flockyou.gps_history.append({
        "latitude": 40.0,
        "longitude": -75.0,
        "altitude": 20,
        "timestamp": "101010",
        "satellites": 5,
        "fix_quality": 1,
        "system_timestamp": time.time(),
    })

    flockyou.add_detection_from_serial({
        "detection_method": "wifi_oui_addr2",
        "protocol": "wifi_2_4ghz",
        "mac_address": "aa:bb:cc:00:00:01",
        "rssi": -55,
    })

    assert flockyou.detections[0]["gps"]["latitude"] == 40.0
    assert flockyou.detections[0]["timestamp_source"] == "gps"
