// ============================================================
// Arduino Mega - TPMS Sensor Data Receiver + ThingSpeak Upload
// ============================================================

#define SIM_SERIAL      Serial1
#define GSM_PWRKEY_PIN  3
#define GSM_RESET_PIN   2

// ---- ThingSpeak configuration ----
const char* TS_API_KEY = "HKC25AWLCSFCKBEV";   // Write API Key
const char* TS_HOST     = "api.thingspeak.com";
const char* TS_PATH     = "/update";

// ---- Rate limiting ----
unsigned long lastUploadMillis = 0;
const unsigned long UPLOAD_INTERVAL = 15000UL;

// ---- GSM state ----
bool gsmReady = false;

// =============================================================
// GSM HELPERS
// =============================================================

// Clears any stale/leftover bytes sitting in the GSM serial buffer
// before we send a new command. This is the fix for "gibberish"
// responses caused by trailing bytes from the previous command
// bleeding into the next one.
void flushGSMBuffer()
{
    while (SIM_SERIAL.available())
    {
        SIM_SERIAL.read();
    }
}

bool sendATcommand(String command, String expected_response, unsigned int timeout)
{
    flushGSMBuffer();          // clear stale bytes before sending
    delay(50);                 // tiny settle time

    String response = "";
    SIM_SERIAL.println(command);
    unsigned long start = millis();
    while (millis() - start < timeout)
    {
        if (SIM_SERIAL.available())
        {
            char c = SIM_SERIAL.read();
            response += c;
            if (response.indexOf(expected_response) != -1)
            {
                delay(100);     // let any trailing bytes finish arriving
                Serial.print(F("AT OK | "));
                Serial.println(response);
                return true;
            }
        }
    }
    Serial.print(F("AT TIMEOUT | Cmd: "));
    Serial.print(command);
    Serial.print(F(" | Response: "));
    Serial.println(response);
    return false;
}

void hardwareResetGSM()
{
    delay(2000);
    digitalWrite(GSM_PWRKEY_PIN, HIGH);
    Serial.println(F("Resetting A7672S..."));
    digitalWrite(GSM_PWRKEY_PIN, LOW);
    delay(200);
    digitalWrite(GSM_PWRKEY_PIN, HIGH);
    delay(10000);
    Serial.println(F("Reset complete."));
}

bool initializeGSM()
{
    if (!sendATcommand("AT", "OK", 5000))
    {
        Serial.println(F("ERROR: Module not responding to AT"));
        return false;
    }
    Serial.println(F("GSM module alive."));

    sendATcommand("AT+CPIN?",  "READY", 5000);
    sendATcommand("AT+CSQ",    "OK",    5000);
    sendATcommand("AT+CREG?",  "OK",    5000);
    sendATcommand("AT+CGATT?", "OK",    5000);

    sendATcommand("AT+CGDCONT=1,\"IP\",\"airtelgprs.com\"", "OK", 5000);

    bool registered = sendATcommand("AT+CREG?", "+CREG: 0,1", 5000) ||
                      sendATcommand("AT+CREG?", "+CREG: 0,5", 5000);

    if (!registered)
    {
        Serial.println(F("WARNING: Not registered on network yet."));
    }
    else
    {
        Serial.println(F("Network registered. GSM ready."));
    }

    return true;
}

// ---- ThingSpeak upload (plain HTTP GET) ----
void sendToThingSpeak(float pressure, int temperature, uint16_t minor)
{
    String url = "http://";
    url += TS_HOST;
    url += TS_PATH;
    url += "?api_key=";
    url += TS_API_KEY;
    url += "&field1=" + String(pressure, 2);
    url += "&field2=" + String(temperature);
    url += "&field3=" + String(minor);

    Serial.print(F("Uploading to ThingSpeak: "));
    Serial.println(url);

    if (!sendATcommand("AT+HTTPINIT", "OK", 5000))
    {
        Serial.println(F("HTTPINIT failed"));
        return;
    }

    if (!sendATcommand("AT+HTTPPARA=\"URL\",\"" + url + "\"", "OK", 5000))
    {
        Serial.println(F("HTTPPARA URL failed"));
        sendATcommand("AT+HTTPTERM", "OK", 5000);
        return;
    }

    bool ok = sendATcommand("AT+HTTPACTION=0", "+HTTPACTION: 0,200", 30000);

    if (ok)
    {
        Serial.println(F("ThingSpeak upload successful."));
        delay(300);   // give the module a moment before reading the response body
        sendATcommand("AT+HTTPREAD", "OK", 10000);
    }
    else
    {
        Serial.println(F("ThingSpeak upload failed."));
    }

    sendATcommand("AT+HTTPTERM", "OK", 5000);
    flushGSMBuffer();   // final safety clear before returning to BLE loop
}

// =============================================================
// BLE PARSING
// =============================================================

int findMajorMinorOffset(uint8_t *data, int len)
{
    int i = 0;
    while (i < len)
    {
        uint8_t adLength = data[i];
        if (adLength == 0 || i + adLength >= len) break;

        uint8_t adType = data[i + 1];

        if (adType == 0xFF)
        {
            int companyDataStart = i + 2;
            int majorOffset = companyDataStart + 2 + 1 + 1 + 16;

            if (majorOffset + 3 <= len)
            {
                return majorOffset;
            }
        }
        i += adLength + 1;
    }
    return -1;
}

void decodeAndUpload(uint16_t major, uint16_t minor, String mac)
{
    uint8_t pressureByte = (minor >> 8) & 0xFF;
    uint8_t tempByte     =  minor       & 0xFF;

    float pressure_kPa = pressureByte * 2.5;
    float pressure_psi = pressure_kPa * 0.145038;
    int   temperature_C = tempByte - 40;

    Serial.print(F("Pressure: "));
    Serial.print(pressure_psi, 2);
    Serial.println(F(" psi"));

    Serial.print(F("Temperature: "));
    Serial.print(temperature_C);
    Serial.println(F(" C"));

    unsigned long now = millis();
    if (now - lastUploadMillis >= UPLOAD_INTERVAL)
    {
        lastUploadMillis = now;
        if (gsmReady)
        {
            sendToThingSpeak(pressure_psi, temperature_C, minor);
        }
        else
        {
            Serial.println(F("GSM not ready - skipping upload"));
        }
    }
    else
    {
        unsigned long secondsLeft = (UPLOAD_INTERVAL - (now - lastUploadMillis)) / 1000;
        Serial.print(F("Next upload in "));
        Serial.print(secondsLeft);
        Serial.println(F("s"));
    }
}

// Returns true only if the string is non-empty, even length,
// and contains nothing but valid hex characters. This is what
// catches corrupted/overflowed BLE lines before they get parsed
// into nonsense pressure/temperature values.
bool isValidHex(const String &s)
{
    if (s.length() == 0 || s.length() % 2 != 0) return false;
    for (unsigned int i = 0; i < s.length(); i++)
    {
        char c = s[i];
        bool isHexChar = (c >= '0' && c <= '9') ||
                          (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F');
        if (!isHexChar) return false;
    }
    return true;
}

void processLine(String line)
{
    int commaIndex = line.indexOf(',');
    if (commaIndex == -1)
    {
        Serial.println("Malformed line (no comma): " + line);
        return;
    }

    String mac     = line.substring(0, commaIndex);
    String hexData = line.substring(commaIndex + 1);
    hexData.trim();

    // Reject anything that isn't clean hex - this is the guard
    // against corrupted/overflowed serial data being parsed as
    // if it were a valid BLE payload.
    if (!isValidHex(hexData))
    {
        Serial.println(F("Rejected line - invalid/corrupted hex data."));
        return;
    }

    Serial.println(F("---"));
    Serial.println("Sensor MAC: " + mac);
    Serial.println("Raw hex: "    + hexData);

    int numBytes = hexData.length() / 2;
    uint8_t rawBytes[40];
    if (numBytes > 40) numBytes = 40;

    for (int i = 0; i < numBytes; i++)
    {
        String byteStr = hexData.substring(i * 2, i * 2 + 2);
        rawBytes[i] = (uint8_t) strtol(byteStr.c_str(), NULL, 16);
    }

    int majorMinorOffset = findMajorMinorOffset(rawBytes, numBytes);

    if (majorMinorOffset == -1)
    {
        Serial.println(F("Could not locate Major/Minor in payload."));
        return;
    }

    uint16_t major = (rawBytes[majorMinorOffset]     << 8) | rawBytes[majorMinorOffset + 1];
    uint16_t minor = (rawBytes[majorMinorOffset + 2] << 8) | rawBytes[majorMinorOffset + 3];

    Serial.print(F("Major: ")); Serial.println(major);
    Serial.print(F("Minor: ")); Serial.println(minor);

    decodeAndUpload(major, minor, mac);
}

// =============================================================
// SETUP AND LOOP
// =============================================================

void setup()
{
    Serial.begin(115200);
    SIM_SERIAL.begin(115200);
    Serial3.begin(115200);

    pinMode(GSM_PWRKEY_PIN, OUTPUT);
    pinMode(GSM_RESET_PIN,  OUTPUT);
    digitalWrite(GSM_PWRKEY_PIN, HIGH);
    digitalWrite(GSM_RESET_PIN,  HIGH);

    Serial.println(F("TPMS Receiver + ThingSpeak Upload starting..."));

    hardwareResetGSM();
    gsmReady = initializeGSM();

    if (gsmReady)
    {
        Serial.println(F("System ready. Waiting for BLE sensor data..."));
    }
    else
    {
        Serial.println(F("GSM init failed. Will receive BLE data but cannot upload."));
    }
}

void loop()
{
    if (Serial3.available())
    {
        String line = Serial3.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) return;
        processLine(line);
    }
}
