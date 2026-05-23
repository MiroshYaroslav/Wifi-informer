Import("env")
import socket

def get_esp32_ip(target_hostname):
    try:
        # Додаємо .local, бо це стандарт mDNS
        hostname = f"{target_hostname}.local"
        print(f"🔍 [OTA] Шукаю пристрій: {hostname}")
        ip = socket.gethostbyname(hostname)
        print(f"✅ [OTA] Знайдено: {ip}")
        return ip
    except Exception:
        print(f"❌ [OTA] Не вдалося знайти {target_hostname}.local через mDNS.")
        return None

# Виконуємо пошук
esp_ip = get_esp32_ip("smart-dashboard")

if esp_ip:
    env.Replace(UPLOAD_PORT=esp_ip)
    env.Append(UPLOAD_FLAGS=["--auth=admin123"])
else:
    # Якщо не знайшлося, даємо зрозумілий виняток, а не "dummy_ip"
    print("⚠️ [OTA] ПРЕДУПРЕЖДЕНИЕ: ESP32 не знайдено в мережі. Переконайся, що ти в тому ж Wi-Fi.")