# test_blink_serial.py
import serial
import time

# Configuration du port série (adapte à ton cas)
port = "COM4"  # ou "/dev/ttyUSB0" sous Linux
baudrate = 115200
timeout = 1  # Timeout de lecture (secondes)

# Ouvre le port série
with serial.Serial(port, baudrate, timeout=timeout) as ser:
    print(f"✅ Port {port} ouvert. Attente des logs...")

    # Attends que l'ESP32 démarre (envoie des données)
    time.sleep(5)  # Attends 5 secondes pour le boot

    # Lit les données jusqu'à trouver la séquence
    buffer = ""
    expected_sequence = [
        "Turning the LED OFF!",
        "Turning the LED ON!",
        "Turning the LED OFF!",
        "Turning the LED ON!",
    ]

    start_time = time.time()
    while time.time() - start_time < 30:  # Timeout global de 30 secondes
        if ser.in_waiting > 0:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            buffer += line + "\n"
            print(f"Log reçu : {line}")  # Affiche les logs en temps réel

            # Vérifie si la séquence est complète
            if all(msg in buffer for msg in expected_sequence):
                print("✅ Tous les messages trouvés dans le bon ordre !")
                break
    else:
        print("❌ Timeout : séquence non trouvée.")
        print(f"Buffer reçu :\n{buffer}")
