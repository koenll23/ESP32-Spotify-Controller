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

// Minimal color palette
#define UI_BLACK 0x0000
#define UI_WHITE 0xFFFF
#define UI_GRAY 0x4208
#define UI_LIGHT_GRAY 0x8410
#define UI_ACCENT 0x1DB8

const char* host = "api.spotify.com";
const char* token_host = "accounts.spotify.com";
const int httpsPort = 443;
WiFiClientSecure client;
unsigned long lastTokenTime = 0;
const unsigned long tokenExpireTime = 3600000;
String access_token = "";
bool isPlaying = false;
bool hasValidData = false;
int connectionRetries = 0;
const int maxRetries = 3;

// Enhanced track timing for local counting with recalibration
unsigned long trackStartTime = 0;
long serverProgress = 0;
long trackDuration = 0;
String currentTrackId = "";
String lastTrackName = "";
String lastArtistName = "";
bool wasPlayingBeforeUpdate = false;
unsigned long lastServerUpdate = 0;
long localProgressOffset = 0;

// Hardware pins
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

// Animation variables
int dotCount = 0;
unsigned long lastDotTime = 0;

// UI state tracking for efficiency
bool uiInitialized = false;
int lastProgressWidth = -1;
String lastDisplayedTime = "";
float lastDisplayedTemp = -999;

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= tft.height()) return 0;
    tft.pushImage(x, y, w, h, bitmap);
    return 1;
}

// Simple loading animation
void showLoadingScreen(String message) {
    tft.fillScreen(UI_BLACK);
    
    // Simple loading dots
    if (millis() - lastDotTime > 400) {
        dotCount = (dotCount + 1) % 4;
        lastDotTime = millis();
    }
    
    tft.setTextColor(UI_WHITE, UI_BLACK);
    tft.setTextSize(1);
    int textWidth = message.length() * 6;
    tft.setCursor((160 - textWidth) / 2, 60);
    tft.print(message);
    
    String dots = "";
    for(int i = 0; i < dotCount; i++) {
        dots += ".";
    }
    tft.setCursor((160 - textWidth) / 2 + textWidth, 60);
    tft.fillRect((160 - textWidth) / 2 + textWidth, 60, 24, 8, UI_BLACK);
    tft.print(dots);
}

bool getFile(String url, String filename) {
    if (SPIFFS.exists(filename)) return true;
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(url);
    http.setTimeout(8000); // Reduced timeout for faster response

    int httpCode = http.GET();
    if (httpCode > 0) {
        fs::File f = SPIFFS.open(filename, "w+");
        if (!f) {
            http.end();
            return false;
        }

        if (httpCode == HTTP_CODE_OK) {
            int total = http.getSize();
            int len = total;
            uint8_t buff[256] = { 0 }; // Increased buffer size for faster transfer
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
        http.end();
        return true;
    }
    http.end();
    return false;
}

String getAccessToken() {
    client.setTimeout(8000); // Reduced timeout
    
    if (!client.connect(token_host, httpsPort)) {
        return "";
    }

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

    unsigned long timeout = millis() + 8000;
    while (client.connected() && millis() < timeout) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break;
    }

    if (millis() >= timeout) {
        client.stop();
        return "";
    }

    String response = client.readString();
    client.stop();
    
    int startIndex = response.indexOf("\"access_token\":\"") + 16;
    int endIndex = response.indexOf("\"", startIndex);
    if (startIndex > 15 && endIndex > startIndex) {
        return response.substring(startIndex, endIndex);
    }

    return "";
}

// Initialize clean UI - only draw static elements
void initMainUI() {
    tft.fillScreen(UI_BLACK);
    
    // Draw album art placeholder (will be updated when track loads)
    tft.drawRect(10, 10, 64, 64, UI_GRAY);
    
    // Simple divider line
    tft.drawFastHLine(0, 90, 160, UI_GRAY);
    
    uiInitialized = true;
    // Don't show "No track" text - wait for actual data
}

// Get current progress with enhanced local tracking and recalibration
long getCurrentProgress() {
    if (!hasValidData || trackDuration <= 0) return 0;
    
    // If not playing, return last known server progress
    if (!isPlaying) {
        return serverProgress;
    }
    
    // Calculate elapsed time since last server update
    unsigned long timeSinceUpdate = millis() - lastServerUpdate;
    
    // Local progress = server progress + time elapsed since last update
    long calculatedProgress = serverProgress + timeSinceUpdate;
    
    // Ensure we don't exceed track duration
    if (calculatedProgress > trackDuration) {
        calculatedProgress = trackDuration;
    }
    
    // Ensure we don't go negative
    if (calculatedProgress < 0) {
        calculatedProgress = 0;
    }
    
    return calculatedProgress;
}

// Update only progress bar without flashing - with optimization
void updateProgressBar() {
    if (trackDuration <= 0) return;
    
    long currentProgress = getCurrentProgress();
    float progressPercentage = (float)currentProgress / (float)trackDuration;
    int progressWidth = (int)(progressPercentage * 160);
    if (progressWidth > 160) progressWidth = 160;
    if (progressWidth < 0) progressWidth = 0;
    
    // Only update if progress width changed
    if (progressWidth != lastProgressWidth) {
        lastProgressWidth = progressWidth;
        
        // Clear and redraw progress bar
        tft.drawFastHLine(0, 127, 160, UI_GRAY);
        if (progressWidth > 0) {
            tft.drawFastHLine(0, 127, progressWidth, UI_WHITE);
        }
    }
}

// Update only time display without flashing - with optimization
void updateTimeDisplay() {
    long currentProgress = getCurrentProgress();
    
    int duration_minutes = trackDuration / 60000;
    int duration_seconds = (trackDuration % 60000) / 1000;
    int progress_minutes = currentProgress / 60000;
    int progress_seconds = (currentProgress % 60000) / 1000;

    String timeString = String(progress_minutes) + ":" + 
                       (progress_seconds < 10 ? "0" : "") + String(progress_seconds) + " / " +
                       String(duration_minutes) + ":" + 
                       (duration_seconds < 10 ? "0" : "") + String(duration_seconds);
    
    // Only update if time display changed
    if (timeString != lastDisplayedTime) {
        lastDisplayedTime = timeString;
        
        tft.setTextColor(UI_LIGHT_GRAY, UI_BLACK);
        tft.setTextSize(1);
        
        // Progress time
        tft.fillRect(10, 95, 30, 8, UI_BLACK);
        tft.setCursor(10, 95);
        tft.printf("%d:%02d", progress_minutes, progress_seconds);
        
        // Duration time
        tft.fillRect(120, 95, 30, 8, UI_BLACK);
        tft.setCursor(120, 95);
        tft.printf("%d:%02d", duration_minutes, duration_seconds);
    }
}

bool updateCurrentSong() {
    if (WiFi.status() != WL_CONNECTED) return false;
    
    // Store current state for recalibration
    bool wasPlayingBefore = isPlaying;
    long localProgressBefore = getCurrentProgress();
    
    client.setTimeout(6000); // Reduced timeout for faster response
    if (!client.connect(host, httpsPort)) {
        connectionRetries++;
        return false;
    }

    client.println("GET /v1/me/player/currently-playing HTTP/1.1");
    client.println("Host: api.spotify.com");
    client.print("Authorization: Bearer ");
    client.println(access_token);
    client.println("Connection: close");
    client.println();

    unsigned long timeout = millis() + 6000;
    while (client.connected() && millis() < timeout) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break;
    }

    if (millis() >= timeout) {
        client.stop();
        connectionRetries++;
        return false;
    }

    String response = client.readString();
    client.stop();

    if (response.length() == 0) return false;

    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error) return false;

    connectionRetries = 0;
    hasValidData = true;

    String trackName = doc["item"]["name"].as<String>();
    String artistName = doc["item"]["artists"][0]["name"].as<String>();
    String trackId = doc["item"]["id"].as<String>();
    bool newIsPlaying = doc["is_playing"].as<bool>();

    long duration_ms = doc["item"]["duration_ms"].as<long>();
    long progress_ms = doc["progress_ms"].as<long>();

    // RECALIBRATION: Update timing info and recalibrate local tracking
    unsigned long currentTime = millis();
    
    // Check if this is a new track
    bool isNewTrack = (trackId != currentTrackId);
    
    if (isNewTrack) {
        // New track - reset everything
        currentTrackId = trackId;
        serverProgress = progress_ms;
        trackDuration = duration_ms;
        lastServerUpdate = currentTime;
        trackStartTime = currentTime - progress_ms; // Backtrack to when track actually started
        
        Serial.printf("New track detected: %s - Progress: %ld/%ld\n", 
                     trackName.c_str(), progress_ms, duration_ms);
    } else {
        // Same track - recalibrate based on server vs local difference
        long serverVsLocal = progress_ms - localProgressBefore;
        
        // If server progress differs significantly from our local calculation, recalibrate
        if (abs(serverVsLocal) > 2000) { // More than 2 seconds difference
            Serial.printf("Recalibrating: Server=%ld, Local=%ld, Diff=%ld\n", 
                         progress_ms, localProgressBefore, serverVsLocal);
        }
        
        // Always update with server data for accuracy
        serverProgress = progress_ms;
        trackDuration = duration_ms;
        lastServerUpdate = currentTime;
        
        // Recalculate when track started based on current server progress
        if (newIsPlaying) {
            trackStartTime = currentTime - progress_ms;
        }
    }
    
    // Update play state
    wasPlayingBeforeUpdate = isPlaying;
    isPlaying = newIsPlaying;
    
    // Check if play state changed
    if (wasPlayingBefore != newIsPlaying) {
        // Update play/pause indicator
        tft.setTextColor(UI_WHITE, UI_BLACK);
        tft.setTextSize(1);
        tft.fillRect(84, 50, 20, 8, UI_BLACK);
        tft.setCursor(84, 50);
        tft.print(isPlaying ? ">" : "||");
        
        Serial.printf("Play state changed: %s -> %s\n", 
                     wasPlayingBefore ? "Playing" : "Paused",
                     isPlaying ? "Playing" : "Paused");
    }

    // Only update track info if track changed
    if (isNewTrack || trackName != lastTrackName) {
        lastTrackName = trackName;
        lastArtistName = artistName;
        
        // Clear text areas only
        tft.fillRect(78, 15, 82, 35, UI_BLACK); // Expanded clear area
        
        // Track name - prevent overlap with album art
        tft.setTextColor(UI_WHITE, UI_BLACK);
        tft.setTextSize(1);
        
        String displayTrack = trackName;
        if (displayTrack.length() > 13) {
            displayTrack = displayTrack.substring(0, 10) + "...";
        }
        
        tft.setCursor(78, 15);
        tft.print(displayTrack);

        // Artist name
        tft.setTextColor(UI_LIGHT_GRAY, UI_BLACK);
        String displayArtist = artistName;
        if (displayArtist.length() > 13) {
            displayArtist = displayArtist.substring(0, 10) + "...";
        }
        
        tft.setCursor(78, 30);
        tft.print(displayArtist);

        // Update album art
        JsonArray images = doc["item"]["album"]["images"].as<JsonArray>();
        bool foundImage = false;
        for (JsonObject image : images) {
            if (image["height"] == 64 && image["width"] == 64) {
                String url = image["url"].as<String>();
                SPIFFS.remove("/album_image.jpg");
                if (getFile(url, "/album_image.jpg")) {
                    // Clear album area first
                    tft.fillRect(10, 10, 64, 64, UI_BLACK);
                    TJpgDec.setJpgScale(1);
                    TJpgDec.setSwapBytes(true);
                    TJpgDec.setCallback(tft_output);
                    TJpgDec.drawFsJpg(10, 10, "/album_image.jpg");
                    foundImage = true;
                }
                break;
            }
        }
        
        // If no image found, show placeholder
        if (!foundImage) {
            tft.fillRect(10, 10, 64, 64, UI_BLACK);
            tft.drawRect(10, 10, 64, 64, UI_GRAY);
        }

        // Update play/pause indicator
        tft.setTextColor(UI_WHITE, UI_BLACK);
        tft.setCursor(84, 50);
        tft.print(isPlaying ? ">" : "||");
    }

    return true;
}

void sendSpotifyCommand(const char* endpoint, const char* method = "POST") {
    if (WiFi.status() != WL_CONNECTED) return;
    
    client.setTimeout(3000); // Faster timeout for commands
    if (!client.connect(host, httpsPort)) return;

    client.printf("%s %s HTTP/1.1\r\n", method, endpoint);
    client.println("Host: api.spotify.com");
    client.print("Authorization: Bearer ");
    client.println(access_token);
    client.println("Content-Length: 0");
    client.println("Connection: close");
    client.println();
    client.stop();
    
    // After sending command, update our local timing to reflect the change
    // This prevents progress bar jumping when server responds
    lastServerUpdate = millis();
    
    Serial.printf("Sent command: %s %s\n", method, endpoint);
}

void IRAM_ATTR handleButton1() { buttonPressed1 = true; }
void IRAM_ATTR handleButton2() { buttonPressed2 = true; }
void IRAM_ATTR handleButton3() { buttonPressed3 = true; }

float readTemperature() {
    int rawValue = analogRead(thermistorPin);
    if (rawValue == 0) return -999;
    
    float resistance = seriesResistor / ((adcMax / rawValue) - 1);
    float temperature = 1.0 / (log(resistance / nominalResistance) / betaCoefficient + 1.0 / (nominalTemperature + 273.15)) - 273.15;
    return temperature;
}

void setup() {
    Serial.begin(115200);
    
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(UI_BLACK);
    
    // Simple startup
    showLoadingScreen("Starting");
    delay(500); // Reduced delay

    pinMode(buttonPin1, INPUT_PULLUP);
    pinMode(buttonPin2, INPUT_PULLUP);
    pinMode(buttonPin3, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(buttonPin1), handleButton1, FALLING);
    attachInterrupt(digitalPinToInterrupt(buttonPin2), handleButton2, FALLING);
    attachInterrupt(digitalPinToInterrupt(buttonPin3), handleButton3, FALLING);

    // WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        showLoadingScreen("WiFi");
        delay(300); // Faster check interval
    }

    // SPIFFS
    showLoadingScreen("Storage");
    if (!SPIFFS.begin()) {
        tft.fillScreen(UI_BLACK);
        tft.setTextColor(UI_WHITE);
        tft.setCursor(10, 60);
        tft.print("Storage Error");
        while (1) yield();
    }

    // Spotify
    showLoadingScreen("Spotify");
    client.setCACert(digicert_root_g2);
    access_token = getAccessToken();
    if (access_token.isEmpty()) {
        tft.fillScreen(UI_BLACK);
        tft.setTextColor(UI_WHITE);
        tft.setCursor(10, 60);
        tft.print("Auth Error");
        delay(3000);
    } else {
        lastTokenTime = millis();
    }

    initMainUI();
    
    // Try to get initial song data immediately and establish baseline
    if (!access_token.isEmpty()) {
        Serial.println("Getting initial track data...");
        if (updateCurrentSong()) {
            Serial.println("Initial track data loaded successfully");
        } else {
            Serial.println("Failed to load initial track data");
        }
    }
}

void loop() {
    static unsigned long lastCheckTime = 0;
    static unsigned long lastTempReadTime = 0;
    static unsigned long lastProgressUpdate = 0;

    // Button handling
    if (buttonPressed3) {
        buttonPressed3 = false;
        sendSpotifyCommand("/v1/me/player/previous");
        delay(100); // Small delay to prevent double-press
    }

    if (buttonPressed2) {
        buttonPressed2 = false;
        const char* command = isPlaying ? "/v1/me/player/pause" : "/v1/me/player/play";
        sendSpotifyCommand(command, "PUT");
        delay(100);
    }

    if (buttonPressed1) {
        buttonPressed1 = false;
        sendSpotifyCommand("/v1/me/player/next");
        delay(100);
    }

    // Token refresh
    if (millis() - lastTokenTime > tokenExpireTime) {
        access_token = getAccessToken();
        lastTokenTime = millis();
    }

    // Update progress bar and time every 250ms for smoother experience
    if (millis() - lastProgressUpdate >= 250 && hasValidData) {
        lastProgressUpdate = millis();
        updateProgressBar();
        updateTimeDisplay();
    }

    // Update song info from server every 4 seconds (frequent enough for accurate recalibration)
    if (millis() - lastCheckTime >= 4000) {
        lastCheckTime = millis();
        
        if (connectionRetries >= maxRetries) {
            // Simple error display - only if we had valid data before
            if (hasValidData) {
                tft.fillRect(60, 110, 60, 8, UI_BLACK);
                tft.setTextColor(UI_GRAY, UI_BLACK);
                tft.setTextSize(1);
                tft.setCursor(60, 110);
                tft.print("Connection lost");
            }
            connectionRetries = 0;
            delay(1000);
        } else {
            Serial.printf("Updating song data... (Local progress: %ld)\n", getCurrentProgress());
            updateCurrentSong();
        }
    }

    // Temperature - corner display, less frequent updates
    if (millis() - lastTempReadTime >= 10000) { // Reduced to 10 seconds
        lastTempReadTime = millis();
        float temperature = readTemperature();
        
        if (temperature > -999 && abs(temperature - lastDisplayedTemp) > 0.5) { // Only update if significant change
            lastDisplayedTemp = temperature;
            tft.setTextColor(UI_GRAY, UI_BLACK);
            tft.setTextSize(1);
            tft.fillRect(125, 0, 35, 8, UI_BLACK);
            tft.setCursor(125, 0);
            tft.printf("%.0fC", temperature);
        }
    }
}
