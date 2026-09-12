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
#include "ApiSchedule.h"
#include "ButtonClicks.h"






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
String currentTrackUri;
bool lastKnownPlaying = false;
bool playbackKnown = false;
ApiSchedule apiSchedule;
QueueHandle_t buttonEvents = nullptr;

void showStatus(const String& text);
void renderLastSongOnly(bool isPlaying);
bool refreshPlayback();

bool acceptResponse(const response& result)
{
    if (result.status_code == 429)
    {
        apiSchedule.rateLimited(millis());
        if (buttonEvents != nullptr)
            xQueueReset(buttonEvents);
        Serial.printf("Spotify rate limited: waiting %lu seconds.\n",
                      static_cast<unsigned long>(apiSchedule.cooldownMs() / 1000));
        showStatus("Rate limited\nWaiting...");
        return false;
    }
    if (result.status_code < 200 || result.status_code >= 300)
    {
        apiSchedule.failed(millis());
        Serial.printf("Spotify request failed: HTTP %d\n", result.status_code);
        return false;
    }
    apiSchedule.succeeded();
    return true;
}

// Original TTGO T-Display: S1 = GPIO0, S2 = GPIO35 (board pull-up).
const uint8_t BUTTON_S1 = 0;
const uint8_t BUTTON_S2 = 35;

// Sample independently of blocking Spotify requests. Queue only debounced
// clicks, so double-click recognition continues while network requests block.
void sampleButtons(void*)
{
    const uint8_t pins[] = {BUTTON_S1, BUTTON_S2};
    int previous[] = {HIGH, HIGH};
    int stable[] = {HIGH, HIGH};
    unsigned long changedAt[] = {0, 0};
    ButtonClicks clicks;
    for (;;)
    {
        ButtonAction action = clicks.tick(millis());
        if (action != ButtonAction::None)
            xQueueSend(buttonEvents, &action, 0);
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
                {
                    action = i == 0 ? clicks.press(now) : ButtonAction::NextTrack;
                    if (action != ButtonAction::None)
                        xQueueSend(buttonEvents, &action, 0);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setupButtons()
{
    pinMode(BUTTON_S1, INPUT_PULLUP);
    pinMode(BUTTON_S2, INPUT); // GPIO35 has no internal pull-up.
    buttonEvents = xQueueCreate(4, sizeof(ButtonAction));
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
    ButtonAction button;
    if (buttonEvents == nullptr || xQueueReceive(buttonEvents, &button, 0) != pdTRUE)
        return false;
    if (apiSchedule.blocked(millis()))
        return false; // Discard presses during cooldown instead of replaying them later.

    if (button == ButtonAction::SaveTrack)
    {
        // Fetch current identity so a track change cannot save stale display data.
        if (!refreshPlayback())
            return false;
        if (!currentTrackUri.startsWith("spotify:track:"))
        {
            showStatus("No track to save");
            return false;
        }
        const char* uris[] = {currentTrackUri.c_str()};
        response result = sp->save_items_to_library(1, uris);
        Serial.printf("Save favourite: HTTP %d\n", result.status_code);
        if (!acceptResponse(result))
        {
            if (result.status_code == 403)
                showStatus("Save denied\nCheck login");
            else if (result.status_code != 429)
                showStatus("Save failed");
            return false;
        }
        showStatus("Saved to\nfavourites");
        apiSchedule.afterCommand(millis());
        return true;
    }

    // One shared response supplies both metadata and playback state.
    // Refresh stale state before toggling; never assume an error means playing.
    if (button == ButtonAction::TogglePlayback && (!playbackKnown || apiSchedule.pollDue(millis())))
    {
        if (!refreshPlayback())
            return false;
    }

    response result = button == ButtonAction::TogglePlayback
        ? (lastKnownPlaying ? sp->pause_playback() : sp->start_a_users_playback())
        : sp->skip_to_next();
    Serial.printf("S%u command: HTTP %d\n", button == ButtonAction::TogglePlayback ? 1u : 2u, result.status_code);
    if (!acceptResponse(result))
        return false;
    if (button == ButtonAction::TogglePlayback)
    {
        lastKnownPlaying = !lastKnownPlaying;
        playbackKnown = true;
        renderLastSongOnly(lastKnownPlaying);
    }
    apiSchedule.afterCommand(millis());
    return true;
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

    sp->set_scopes("user-read-playback-state user-read-currently-playing user-modify-playback-state user-library-modify");
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

bool refreshPlayback()
{
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["item"]["name"] = true;
    filter["item"]["uri"] = true;
    filter["item"]["is_local"] = true;
    filter["item"]["artists"][0]["name"] = true;
    response result = sp->get_currently_playing_track(filter);
    apiSchedule.afterPoll(millis());
    if (!acceptResponse(result))
        return false;

    currentTrackUri = "";
    if (result.status_code == 204)
    {
        playbackKnown = true;
        lastKnownPlaying = false;
        renderLastSongOnly(false);
        return true;
    }
    if (!result.reply["is_playing"].is<bool>())
    {
        apiSchedule.failed(millis());
        Serial.println("Spotify returned invalid playback data; keeping last display.");
        return false;
    }

    if (!(result.reply["item"]["is_local"] | false))
        currentTrackUri = result.reply["item"]["uri"] | "";
    lastKnownPlaying = result.reply["is_playing"].as<bool>();
    playbackKnown = true;
    String track = result.reply["item"]["name"] | "";
    String artist;
    for (JsonObject entry : result.reply["item"]["artists"].as<JsonArray>())
    {
        const char* name = entry["name"] | "";
        if (*name == '\0')
            continue;
        if (!artist.isEmpty())
            artist += ", ";
        artist += name;
    }
    if (!track.isEmpty() && !artist.isEmpty())
    {
        lastTrack = track;
        lastArtist = artist;
    }
    renderLastSongOnly(lastKnownPlaying);
    return true;
}

void loop()
{
    static unsigned long lastRebootCheck = millis();
    if (sp == nullptr)
    {
        delay(1000);
        return;
    }

    // A scheduled restart must not bypass a rate-limit cooldown.
    if (millis() - lastRebootCheck >= REBOOT_INTERVAL_MS && !apiSchedule.blocked(millis()))
    {
        showStatus("Restarting...");
        delay(1500);
        ESP.restart();
    }

    handleButton();
    if (apiSchedule.pollDue(millis()))
        refreshPlayback();
    delay(10);
}
