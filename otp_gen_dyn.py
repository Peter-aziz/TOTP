import hmac
import hashlib
from datetime import datetime

# 1. The exact same 16-byte secret key from your STM32 code
secret_key = bytes([
    0x4D, 0x79, 0x53, 0x65, 0x63, 0x72, 0x65, 0x74,
    0x4B, 0x65, 0x79, 0x31, 0x32, 0x33, 0x34, 0x35
])

# 2. Get the actual current time from the Windows PC
now = datetime.now()
hour = now.hour
minute = now.minute

# 3. Convert PC standard time to BCD to match the STM32 RTC module
bcd_hour = ((hour // 10) << 4) | (hour % 10)
bcd_minute = ((minute // 10) << 4) | (minute % 10)

# Create the 2-byte seed exactly like your C code: [BCD Hour, BCD Min]
time_seed = bytes([bcd_hour, bcd_minute])

# 4. Compute the HMAC-SHA1 hash
mac_out = hmac.new(secret_key, time_seed, hashlib.sha1).digest()

# 5. Dynamic Truncation (RFC 4226) to extract the 6-digit code
# Get the offset from the last nibble of the 20-byte hash
offset = mac_out[19] & 0x0F

# Extract 4 bytes at the offset, dropping the most significant bit
binary = (
    ((mac_out[offset] & 0x7F) << 24) |
    ((mac_out[offset + 1] & 0xFF) << 16) |
    ((mac_out[offset + 2] & 0xFF) << 8) |
    (mac_out[offset + 3] & 0xFF)
)

# Modulo 1 million to get a standard 6-digit OTP
otp = binary % 1000000

# Print the results
print("--- PC Live OTP Generator ---")
print(f"System Time (24h) : {hour:02d}:{minute:02d}")
print(f"RTC BCD Hex Seed  : {bcd_hour:02X}:{bcd_minute:02X}")
print(f"Generated OTP     : {otp:06d}")
print("-----------------------------")