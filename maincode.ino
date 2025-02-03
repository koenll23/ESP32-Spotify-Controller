// main code, this is the finished project with only the lines 232 and 237 as debugging
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "codes.h"
#include <TJpg_Decoder.h>
#include <HTTPClient.h>
#include "SPIFFS.h"
#include <SPI.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

const char* host = "api.spotify.com";
const char* token_host = "accounts.spotify.com";
const int httpsPort = 443;
WiFiClientSecure client;
unsigned long lastTokenTime = 0;
const unsigned long tokenExpireTime = 3600000; // 1 uur
String access_token = "";
bool isPlaying = false;
const int thermistorPin = 34;
const float seriesResistor = 10000.0;
const float nominalResistance = 10000.0;
const float nominalTemperature = 25.0;
const float betaCoefficient = 3950.0;
const float adcMax = 4095.0;
const int buttonPin1 = 12;
const int buttonPin2 = 13;
const int buttonPin3 = 14;
volatile bool buttonPressed1 = false;
volatile bool buttonPressed2 = false;
volatile bool buttonPressed3 = false;

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= tft.height()) return 0;
    tft.pushImage(x, y, w, h, bitmap);
    return 1;
}
void listSPIFFS(void) {
    fs::File root = SPIFFS.open("/");
    if (!root || !root.isDirectory()) return;

    fs::File file = root.openNextFile();
    while (file) {
        file = root.openNextFile();
    }
    delay(1000);
}

bool getFile(String url, String filename) {
    if (SPIFFS.exists(filename)) return 0;

    if ((WiFi.status() == WL_CONNECTED)) {
        HTTPClient http;
        http.begin(url);

        int httpCode = http.GET();
        if (httpCode > 0) {
            fs::File f = SPIFFS.open(filename, "w+");
            if (!f) return 0;

            if (httpCode == HTTP_CODE_OK) {
                int total = http.getSize();
                int len = total;
                uint8_t buff[128] = { 0 };
                WiFiClient * stream = http.getStreamPtr();

                while (http.connected() && (len > 0 || len == -1)) {
                    size_t size = stream->available();
                    if (size) {
                        int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
                        f.write(buff, c);
                        if (len > 0) len -= c;
                    }
                    yield();
                }
            }
            f.close();
        }
        http.end();
    }
    return 1;
}

String getAccessToken() {
    if (!client.connect(token_host, httpsPort)) return "";

    String body = "grant_type=refresh_token&refresh_token=" + String(refresh_token) +
                  "&client_id=" + String(client_id) +
                  "&client_secret=" + String(client_secret);

    client.println("POST /api/token HTTP/1.1");
    client.println("Host: accounts.spotify.com");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.print("Content-Length: ");
    client.println(body.length());
    client.println("Connection: close");
    client.println();
    client.print(body);

    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break;
    }

    String response = client.readString();
    int startIndex = response.indexOf("\"access_token\":\"") + 16;
    int endIndex = response.indexOf("\"", startIndex);
    if (startIndex > 15 && endIndex > startIndex) {
        return response.substring(startIndex, endIndex);
    }

    return "";
}

void getCurrentSong() {
    if (!client.connect(host, httpsPort)) return;

    client.println("GET /v1/me/player/currently-playing HTTP/1.1");
    client.println("Host: api.spotify.com");
    client.print("Authorization: Bearer ");
    client.println(access_token);
    client.println("Connection: close");
    client.println();

    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break;
    }

    String response = client.readString();
    if (response.length() == 0) return;

    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error) return;

    String trackName = doc["item"]["name"].as<String>();
    String artistName = doc["item"]["artists"][0]["name"].as<String>();
    isPlaying = doc["is_playing"].as<bool>();

    if (trackName.length() > 11) {
        trackName = trackName.substring(0, 11) + "...";
    }

    tft.fillRect(0, 50, 128, 16, TFT_BLACK);
    tft.setCursor(35, 50);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.print(trackName);

    if (artistName.length() > 8) {
        artistName = artistName.substring(0, 8) + "...";
    }

    tft.fillRect(74, 65, 128, 16, TFT_BLACK);
    tft.setCursor(74, 67);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.print(artistName);

    JsonArray images = doc["item"]["album"]["images"].as<JsonArray>();
    for (JsonObject image : images) {
        if (image["height"] == 300 && image["width"] == 300) {
            String url = image["url"].as<String>();
            SPIFFS.remove("/album_image.jpg");
            getFile(url, "/album_image.jpg");

            TJpgDec.setJpgScale(2);
            TJpgDec.setSwapBytes(true);
            TJpgDec.setCallback(tft_output);
            TJpgDec.drawFsJpg(0, 0, "/album_image.jpg");
            return;
        }
    }
}

void drawProgressBar(long progress_ms, long duration_ms) {
    float progressPercentage = (float)progress_ms / (float)duration_ms;
    int progressWidth = (int)(progressPercentage * 64);
    if (progressWidth > 64) {
        progressWidth = 64;
    }
    tft.fillRoundRect(5, tft.height() - 30, 64, 5, 2, TFT_DARKGREY);
    tft.fillRoundRect(5, tft.height() - 30, progressWidth, 5, 2, TFT_WHITE);
}

void IRAM_ATTR handleButton1() {
    buttonPressed1 = true;
}
void IRAM_ATTR handleButton2() {
    buttonPressed2 = true;
}
void IRAM_ATTR handleButton3() {
    buttonPressed3 = true;
}

float readTemperature() {
    int rawValue = analogRead(thermistorPin);
    float resistance = seriesResistor / ((adcMax / rawValue) - 1);
    float temperature = 1.0 / (log(resistance / nominalResistance) / betaCoefficient + 1.0 / (nominalTemperature + 273.15)) - 273.15;
    return temperature;
}

void setup() {
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);

    float temperature = readTemperature();
    tft.print("Temp: ");
    tft.print(temperature, 1);
    tft.println(" C");

    Serial.begin(115200);
    pinMode(buttonPin1, INPUT_PULLUP);
    pinMode(buttonPin2, INPUT_PULLUP);
    pinMode(buttonPin3, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(buttonPin1), handleButton1, FALLING);
    attachInterrupt(digitalPinToInterrupt(buttonPin2), handleButton2, FALLING);
    attachInterrupt(digitalPinToInterrupt(buttonPin3), handleButton3, FALLING);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
    }
    Serial.println("Connected to Wi-Fi!"); // now in english

    if (!SPIFFS.begin()) {
        while (1) yield();
    }
    Serial.println("SPIFFS initialized!"); // now in english

    client.setCACert(digicert_root_g2);
    access_token = getAccessToken();
    if (access_token.isEmpty()) {
        return;
    } else {
        lastTokenTime = millis();
    }

    tft.begin();
    tft.fillScreen(TFT_BLACK);
}

void loop() {
    static unsigned long lastCheckTime = 0;
    static unsigned long lastPrintTime = 0;
    static String lastTrackName = "";
    static String currentTrackName = "";
    static String currentArtistName = "";
    static unsigned long lastTempReadTime = 0;

    if (buttonPressed3) {
        buttonPressed3 = false;

        if (client.connect(host, httpsPort)) {
            client.println("POST /v1/me/player/previous HTTP/1.1");
            client.println("Host: api.spotify.com");
            client.print("Authorization: Bearer ");
            client.println(access_token);
            client.println("Content-Length: 0");
            client.println("Connection: close");
            client.println();
            client.stop();
        }
    }

    if (buttonPressed2) {
        buttonPressed2 = false;

        const char* command = isPlaying ? "pause" : "play";

        if (client.connect(host, httpsPort)) {
            client.printf("PUT /v1/me/player/%s HTTP/1.1\r\n", command);
            client.println("Host: api.spotify.com");
            client.print("Authorization: Bearer ");
            client.println(access_token);
            client.println("Content-Length: 0");
            client.println("Connection: close");
            client.println();
            client.stop();
        }
    }

    if (buttonPressed1) {
        buttonPressed1 = false;

        if (client.connect(host, httpsPort)) {
            client.println("POST /v1/me/player/next HTTP/1.1");
            client.println("Host: api.spotify.com");
            client.print("Authorization: Bearer ");
            client.println(access_token);
            client.println("Content-Length: 0");
            client.println("Connection: close");
            client.println();
            client.stop();
        }
    }

    if (millis() - lastTokenTime > tokenExpireTime) {
        access_token = getAccessToken();
        lastTokenTime = millis();
    }

    if (millis() - lastCheckTime >= 1000) {
        lastCheckTime = millis();

        if (!client.connect(host, httpsPort)) return;

        client.println("GET /v1/me/player/currently-playing HTTP/1.1");
        client.println("Host: api.spotify.com");
        client.print("Authorization: Bearer ");
        client.println(access_token);
        client.println("Connection: close");
        client.println();

        while (client.connected()) {
            String line = client.readStringUntil('\n');
            if (line == "\r") break;
        }

        String response = client.readString();

        if (response.length() == 0) return;

        StaticJsonDocument<2048> doc;
        DeserializationError error = deserializeJson(doc, response);

        if (error) return;

        currentTrackName = doc["item"]["name"].as<String>();
        currentArtistName = doc["item"]["artists"][0]["name"].as<String>();
        isPlaying = doc["is_playing"].as<bool>();

        long duration_ms = doc["item"]["duration_ms"].as<long>();
        long progress_ms = doc["progress_ms"].as<long>();

        int duration_minutes = duration_ms / 60000;
        int duration_seconds = (duration_ms % 60000) / 1000;
        int progress_minutes = progress_ms / 60000;
        int progress_seconds = (progress_ms % 60000) / 1000;

        drawProgressBar(progress_ms, duration_ms);

        if (currentTrackName != lastTrackName) {
            lastTrackName = currentTrackName;
            JsonArray images = doc["item"]["album"]["images"].as<JsonArray>();

            for (JsonObject image : images) {
                if (image["height"] == 64 && image["width"] == 64) {
                    String url = image["url"].as<String>();
                    SPIFFS.remove("/album_image.jpg");
                    getFile(url, "/album_image.jpg");

                    int imageX = 5;
                    int imageY = 20;
                    int imageWidth = 120;
                    int imageHeight = 120;

                    tft.setRotation(1);
                    TJpgDec.setJpgScale(1);
                    TJpgDec.setSwapBytes(true);
                    TJpgDec.setCallback(tft_output);
                    TJpgDec.drawFsJpg(imageX, imageY, "/album_image.jpg");

                    tft.fillRect(74, 33, 128, 16, TFT_BLACK);
                    tft.setCursor(74, 35);
                    tft.setTextSize(1);
                    tft.setTextColor(TFT_WHITE, TFT_BLACK);

                    if (currentTrackName.length() > 11) {
                        currentTrackName = currentTrackName.substring(0, 11) + "...";
                    }
                    tft.print(currentTrackName);

                    if (currentArtistName.length() > 8) {
                        currentArtistName = currentArtistName.substring(0, 8) + "...";
                    }

                    tft.fillRect(74, 50, 128, 16, TFT_BLACK);
                    tft.setCursor(74, 52);
                    tft.setTextSize(1);
                    tft.setTextColor(TFT_WHITE, TFT_BLACK);
                    tft.print(currentArtistName);
                    break;
                }
            }
        }

        tft.setCursor(4, tft.height() - 11);
        tft.fillRect(0, tft.height() - 18, 40, 15, TFT_BLACK);
        tft.printf("%d:%02d", progress_minutes, progress_seconds);

        tft.setCursor(tft.width() - 110, tft.height() - 11);
        tft.fillRect(tft.width() - 110, tft.height() - 18 - 20, 40, 8, TFT_BLACK);
        tft.printf("%d:%02d", duration_minutes, duration_seconds);
    }

    if (millis() - lastTempReadTime >= 10000) {
        lastTempReadTime = millis();
        int temperature = readTemperature();
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(tft.width() - 26, 0);
        tft.fillRect(tft.width() - 50, 0, 50, 20, TFT_BLACK);
        tft.print(temperature);
        tft.print(" C");
    }
}
