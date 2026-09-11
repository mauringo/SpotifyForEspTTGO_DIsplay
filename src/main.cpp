#include "secrets.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SpotifyEsp32.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>






// Set TRUE once if you want to ignore/delete the saved token
// and perform a fresh Spotify browser login.
const bool FORCE_NEW_LOGIN = false;
const unsigned long REBOOT_INTERVAL_MS = 55UL * 60UL * 1000UL;

Preferences prefs;
Spotify* sp = nullptr;

TFT_eSPI tft = TFT_eSPI();

String refreshToken;
String lastArtist = "";
String lastTrack = "";
bool lastKnownPlaying = false;

// Original TTGO T-Display: S1 = GPIO0, S2 = GPIO35 (board pull-up).
const uint8_t BUTTON_S1 = 0;
const uint8_t BUTTON_S2 = 35;
QueueHandle_t buttonEvents = nullptr;

// Sample independently of blocking Spotify requests. Queue only debounced
// press edges, so holding a button sends one command until it is released.
void sampleButtons(void*)
{
    const uint8_t pins[] = {BUTTON_S1, BUTTON_S2};
    int previous[] = {HIGH, HIGH};
    int stable[] = {HIGH, HIGH};
    unsigned long changedAt[] = {0, 0};
    for (;;)
    {
        for (uint8_t i = 0; i < 2; ++i)
        {
            int level = digitalRead(pins[i]);
            unsigned long now = millis();
            if (level != previous[i])
            {
                previous[i] = level;
                changedAt[i] = now;
            }
            if (level != stable[i] && now - changedAt[i] >= 40)
            {
                stable[i] = level;
                if (level == LOW)
                    xQueueSend(buttonEvents, &i, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setupButtons()
{
    pinMode(BUTTON_S1, INPUT_PULLUP);
    pinMode(BUTTON_S2, INPUT); // GPIO35 has no internal pull-up.
    buttonEvents = xQueueCreate(4, sizeof(uint8_t));
    if (buttonEvents == nullptr)
    {
        Serial.println("Unable to allocate button queue.");
        return;
    }
    if (xTaskCreate(sampleButtons, "buttons", 2048, nullptr, 1, nullptr) != pdPASS)
    {
        vQueueDelete(buttonEvents);
        buttonEvents = nullptr;
        Serial.println("Unable to start button task.");
    }
}

bool handleButton()
{
    uint8_t button;
    if (buttonEvents == nullptr || xQueueReceive(buttonEvents, &button, 0) != pdTRUE)
        return false;

    // Keep all Spotify calls in the main task; the client is shared.
    response result;
    if (button == 0)
    {
        result = sp->is_playing() ? sp->pause_playback() : sp->start_a_users_playback();
    }
    else
    {
        result = sp->skip_to_next();
    }
    Serial.printf("S%u command: HTTP %d\n", button + 1, result.status_code);
    return true;
}

void waitForPlaybackPoll()
{
    unsigned long started = millis();
    while (millis() - started < 1000)
    {
        if (handleButton())
            return; // Refresh the screen after a command.
        delay(10);
    }
}

void showStatus(const String& text)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 20);
    tft.println(text);
}

void showTrack(const String& artist, const String& track, bool isPlaying = true)
{
    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(8, 10);
    tft.println(track);

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(8, 80);
    tft.println(artist);

    // Keep playback status inside the 135-pixel landscape display.
    const int statusY = tft.height() - 24;
    tft.fillRect(0, statusY, tft.width(), 24, TFT_BLACK);
    tft.setTextColor(isPlaying ? TFT_YELLOW : TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(8, statusY + 4);
    tft.print(isPlaying ? "PLAYING" : "PAUSED");
}

void renderLastSongOnly(bool isPlaying)
{
    if (lastArtist.length() == 0 || lastTrack.length() == 0)
    {
        showStatus(isPlaying ? "Playing" : "Paused");
        return;
    }

    showTrack(lastArtist, lastTrack, isPlaying);
}

void showPausedPage(const String& artist, const String& track)
{
    if (artist.length() == 0 || track.length() == 0)
    {
        showStatus("Paused");
        return;
    }

    showTrack(artist, track, false);
}

void setupDisplay()
{
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 20);
    tft.println("Spotify");
    tft.println("ESP32");

    delay(1000);
}

void connect_to_wifi()
{
    showStatus("WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(SSID, PASSWORD);

    Serial.print("Connecting to WiFi");

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("Connected to WiFi");

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    showStatus("WiFi OK");
    delay(800);
}

void syncTime()
{
    Serial.print("Synchronizing time");
    showStatus("Sync time...");

    configTime(
        0,
        0,
        "pool.ntp.org",
        "time.nist.gov"
    );

    time_t now = time(nullptr);

    while (now < 1700000000)
    {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }

    Serial.println();
    Serial.println("Time synchronized");

    struct tm timeinfo;

    if (gmtime_r(&now, &timeinfo))
    {
        char buffer[64];
        strftime(
            buffer,
            sizeof(buffer),
            "%Y-%m-%d %H:%M:%S UTC",
            &timeinfo
        );

        Serial.println(buffer);
    }

    showStatus("Time OK");
    delay(800);
}

void saveRefreshToken()
{
    user_tokens tokens = sp->get_user_tokens();

    if (
        tokens.refresh_token != nullptr &&
        strlen(tokens.refresh_token) > 0
    )
    {
        String newToken = tokens.refresh_token;

        prefs.putString("refresh_token", newToken);
        refreshToken = newToken;

        Serial.println("Refresh token written to NVS.");
    }
    else
    {
        Serial.println("ERROR: Spotify did not return a refresh token.");
    }
}

void setupSpotify()
{
    prefs.begin("spotify", false);

    if (FORCE_NEW_LOGIN)
    {
        Serial.println();
        Serial.println("FORCE_NEW_LOGIN enabled.");
        Serial.println("Deleting saved Spotify refresh token...");

        prefs.remove("refresh_token");
        refreshToken = "";
    }
    else
    {
        refreshToken = prefs.getString("refresh_token", "");
    }

    if (refreshToken.length() > 0)
    {
        Serial.println("Refresh token found.");
        Serial.println("Using saved Spotify login.");

        showStatus("Spotify login");

        sp = new Spotify(
            CLIENT_ID,
            CLIENT_SECRET,
            refreshToken.c_str()
        );
    }
    else
    {
        Serial.println("No refresh token found.");
        Serial.println("Starting browser authentication.");

        showStatus("Spotify auth");

        sp = new Spotify(
            CLIENT_ID,
            CLIENT_SECRET
        );
    }

    sp->set_scopes("user-read-playback-state user-read-currently-playing user-modify-playback-state");
    sp->set_log_level(SPOTIFY_LOG_DEBUG);
    sp->begin();

    if (refreshToken.length() == 0)
    {
        Serial.println();
        Serial.println("Open the Spotify authentication URL.");

        showStatus("Open auth URL");

        while (!sp->is_auth())
        {
            sp->handle_client();
            delay(50);
        }

        Serial.println("Browser authentication completed.");
        saveRefreshToken();
        Serial.println("New refresh token saved.");
    }
    else
    {
        if (!sp->get_access_token())
        {
            Serial.println();
            Serial.println("Saved refresh token failed.");
            Serial.println("Deleting it from NVS...");

            prefs.remove("refresh_token");

            Serial.println("Restarting ESP32 for a new login...");

            delay(2000);
            ESP.restart();
        }

        Serial.println("Spotify authenticated.");
    }

    showStatus("Spotify OK");
    delay(1000);

    if (!sp->is_playing())
    {
        showStatus("Paused");
    }
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    setupDisplay();
    connect_to_wifi();
    syncTime();
    setupSpotify();
    setupButtons();

    showStatus("Ready");
    delay(1000);
}

void loop()
{
    static unsigned long lastRebootCheck = millis();

    if (sp == nullptr)
    {
        delay(1000);
        return;
    }

    if (millis() - lastRebootCheck >= REBOOT_INTERVAL_MS)
    {
        Serial.println();
        Serial.println("Reboot timer reached: restarting ESP32.");
        showStatus("Restarting...");
        delay(1500);
        ESP.restart();
    }

    handleButton();

    bool isPlaying = sp->is_playing();
    Serial.print("Spotify playback: ");
    Serial.println(isPlaying ? "PLAYING" : "NOT PLAYING");

    String artist = sp->current_artist_names();
    String track = sp->current_track_name();

    bool artistValid =
        artist.length() > 0 &&
        artist != "Something went wrong" &&
        artist != "null";

    bool trackValid =
        track.length() > 0 &&
        track != "Something went wrong" &&
        track != "null";

    if (artistValid && trackValid)
    {
        if (
            artist != lastArtist ||
            track != lastTrack
        )
        {
            lastArtist = artist;
            lastTrack = track;

            Serial.println();
            Serial.println("==============================");
            Serial.print("Artist: ");
            Serial.println(artist);
            Serial.print("Track: ");
            Serial.println(track);
            Serial.println("==============================");
        }
    }

    if (!isPlaying)
    {
        lastKnownPlaying = false;
        renderLastSongOnly(false);

        Serial.println("Spotify is paused or stopped.");
        waitForPlaybackPoll();
        return;
    }

    if (lastArtist.length() > 0 && lastTrack.length() > 0)
    {
        lastKnownPlaying = true;
        renderLastSongOnly(true);
    }
    else if (artistValid && trackValid)
    {
        lastArtist = artist;
        lastTrack = track;
        renderLastSongOnly(true);
    }
    else
    {
        showStatus("Playing");
    }

    waitForPlaybackPoll();
}
