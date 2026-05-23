import serial
from datetime import datetime
import time

COM_PORT = 'COM9'
BAUD_RATE = 9600

print(f"Listening on {COM_PORT} for SYNC_REQ...")

ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=1)
ser.reset_input_buffer()
ser.reset_output_buffer()

while True:
    try:
        raw = ser.readline()
        line = raw.decode('ascii', errors='ignore').strip()

        if line:
            print(f"STM32: {line}")

        if "SYNC_REQ" in line:
            time.sleep(0.1)                          # let STM32 enter HAL_UART_Receive
            now = datetime.now()
            sync_string = f"SYNC:{now.hour:02d}{now.minute:02d}{now.second:02d}\r\n"
            ser.reset_input_buffer()                 # clear any echo garbage
            ser.write(sync_string.encode('ascii'))
            ser.flush()
            print(f"Sent: {sync_string.strip()}")

    except serial.SerialException as e:
        print(f"Serial error: {e}")
        print("Attempting to reconnect...")
        time.sleep(2)
        try:
            ser.close()
            ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=1)
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            print("Reconnected.")
        except Exception as re:
            print(f"Reconnect failed: {re}")

    except Exception as e:
        print(f"Error: {e}")
        time.sleep(1)
