"""
Windows PC telemetry sender for ESP8266 PC + Room Monitor.
Requirements: pip install psutil requests wmi
For CPU/GPU temperature and GPU load, run LibreHardwareMonitor as Administrator
and enable its WMI interface (Options -> Remote Web Server is NOT required;
enable WMI in the app settings if your version provides it).
"""
import time
import json
import logging
import subprocess
import psutil
import requests

# ---------- CHANGE THIS to the ESP8266 IP shown in Arduino Serial Monitor ----------
ESP_IP = "192.168.1.123"
POST_URL = f"http://{ESP_IP}/api"
SEND_EVERY_SECONDS = 2
# ----------------------------------------------------------------------------------

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s: %(message)s")

try:
    import wmi
except ImportError:
    wmi = None

def get_lhm_sensors():
    """Return sensor values from LibreHardwareMonitor WMI, if available."""
    result = []
    if wmi is None:
        return result
    try:
        conn = wmi.WMI(namespace=r"root\LibreHardwareMonitor")
        for sensor in conn.Sensor():
            try:
                result.append({
                    "name": str(sensor.Name or ""),
                    "identifier": str(sensor.Identifier or ""),
                    "type": str(sensor.SensorType or ""),
                    "value": float(sensor.Value)
                })
            except (TypeError, ValueError, AttributeError):
                continue
    except Exception as exc:
        logging.debug("LHM WMI unavailable: %s", exc)
    return result

def pick_sensor(sensors, sensor_type, keywords, exclude=()):
    candidates = []
    for s in sensors:
        if s["type"].lower() != sensor_type.lower():
            continue
        name = s["name"].lower()
        ident = s["identifier"].lower()
        if any(k in name or k in ident for k in keywords) and not any(k in name or k in ident for k in exclude):
            candidates.append(s)
    # Prefer a sensor with a clear device-specific name; otherwise first match.
    return candidates[0]["value"] if candidates else None

def collect():
    # CPU load and RAM work without LibreHardwareMonitor.
    cpu = psutil.cpu_percent(interval=None)
    vm = psutil.virtual_memory()
    ram_used_gb = (vm.total - vm.available) / (1024 ** 3)
    ram_total_gb = vm.total / (1024 ** 3)

    sensors = get_lhm_sensors()
    cpu_temp = pick_sensor(sensors, "Temperature",
                           ["cpu package", "cpu core", "tctl", "tdie", "core average", "cpu"])
    gpu_temp = pick_sensor(sensors, "Temperature",
                           ["gpu core", "gpu temperature", "hot spot", "gpu"])
    # Avoid picking a GPU memory temperature for the GPU core temperature if a core sensor exists.
    if gpu_temp is None:
        gpu_temp = pick_sensor(sensors, "Temperature", ["gpu", "graphics"])

    gpu_usage = pick_sensor(sensors, "Load", ["gpu core", "gpu", "d3d 3d", "graphics"])
    payload = {
        "cpu": round(cpu, 1),
        "ram_percent": round(vm.percent, 1),
        "ram_used_gb": round(ram_used_gb, 1),
        "ram_total_gb": round(ram_total_gb, 1),
        "cpu_temp": round(cpu_temp, 1) if cpu_temp is not None else -999,
        "gpu": round(gpu_usage, 1) if gpu_usage is not None else -1,
        "gpu_temp": round(gpu_temp, 1) if gpu_temp is not None else -999
    }
    return payload

def main():
    # Prime psutil so the first CPU reading is meaningful.
    psutil.cpu_percent(interval=1)
    logging.info("Sending telemetry to %s", POST_URL)
    while True:
        payload = collect()
        try:
            response = requests.post(POST_URL, json=payload, timeout=2)
            response.raise_for_status()
            logging.info("Sent: CPU %s%%, RAM %s%%, GPU %s%%, CPU temp %s C, GPU temp %s C",
                         payload["cpu"], payload["ram_percent"], payload["gpu"],
                         payload["cpu_temp"], payload["gpu_temp"])
        except requests.RequestException as exc:
            logging.warning("Could not reach ESP8266 (%s). Check IP and same Wi-Fi.", exc)
        time.sleep(SEND_EVERY_SECONDS)

if __name__ == "__main__":
    main()
