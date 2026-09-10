#include "config_manager.h"
#include <SD.h>
#include <Preferences.h>
#include <ESPConfig.h>
#include <ESPConfigSerialInterface.h>

#define CONFIG_PATH "/PDAputer/config.txt"
#define MAX_ENTRIES 16
#define MAX_KEY_LEN 32
#define MAX_VAL_LEN 128

namespace ConfigManager {

using namespace ESPConfig;
Preferences prefs;
ESPConfigManager config;
ESPConfigSerialInterface configSerial(Serial, config, "PDAputer");

struct Entry {
    char key[MAX_KEY_LEN];
    char val[MAX_VAL_LEN];
};

static Entry s_entries[MAX_ENTRIES];
static int   s_count = 0;
static bool  s_loaded = false;
static bool  s_prefs_started = false;
static bool  sd_available = false;
static bool  s_config_started = false;
static String s_pref_values[8];
static uint8_t s_pref_value_idx = 0;

// Trim leading/trailing whitespace in-place
static void trim(char* s) {
    // Leading
    char* start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    // Trailing
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

bool load() {
    s_count = 0;
    s_loaded = false;

    if (!s_prefs_started) {
        s_prefs_started = prefs.begin("config", false);
    }
    
    File f = SD.open(CONFIG_PATH, FILE_READ);
    if (!f) {
        Serial.printf("[CONFIG] %s not found\n", CONFIG_PATH);
        s_loaded = s_prefs_started; // Default to Preferences if config file is missing
        return s_prefs_started; // Default to Preferences if config file is missing
    }

    char line[MAX_KEY_LEN + MAX_VAL_LEN + 4];
    while (f.available() && s_count < MAX_ENTRIES) {
        size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0';
        trim(line);

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') continue;

        // Find '=' separator
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char* key = line;
        char* val = eq + 1;
        trim(key);
        trim(val);

        if (key[0] == '\0') continue;

        strncpy(s_entries[s_count].key, key, MAX_KEY_LEN - 1);
        s_entries[s_count].key[MAX_KEY_LEN - 1] = '\0';
        strncpy(s_entries[s_count].val, val, MAX_VAL_LEN - 1);
        s_entries[s_count].val[MAX_VAL_LEN - 1] = '\0';
        s_count++;
    }

    f.close();
    s_loaded = true;
    sd_available = true;
    Serial.printf("[CONFIG] Loaded %d entries from %s\n", s_count, CONFIG_PATH);
    return true;
}

bool isLoaded() {
    return s_loaded;
}

const char* get(const char* key, const char* defaultVal) {
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_entries[i].key, key) == 0) {
            return s_entries[i].val;
        }
    }
    String& value = s_pref_values[s_pref_value_idx++ % 8];
    value = s_prefs_started ? prefs.getString(key, String(defaultVal)) : String(defaultVal);
    return value.c_str(); // Fallback to Preferences
}

const char* getDeviceName()   { return get("device_name", "PDA"); }
const char* getWifiSSID()     { return get("wifi_ssid"); }
const char* getWifiPassword() { return get("wifi_password"); }
const char* getTimezone()     { return get("timezone"); }
const char* getApiKey()       { return get("api_key"); }
const char* getTimeFormat()   { return get("time_format", "12"); }
const char* getBrightness()   { return get("brightness", "128"); }
const char* getTone()         { return get("tone", "128"); }

bool set(const char* key, const char* value) {

    if (s_prefs_started) {
        prefs.putString(key, value); // Save to Preferences
    }

    if (!sd_available) {
        Serial.printf("[CONFIG] SD not available, skipping save for %s\n", key);
        return true; // Skip SD save if not available
    }

    // Update in-memory
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_entries[i].key, key) == 0) {
            strncpy(s_entries[i].val, value, MAX_VAL_LEN - 1);
            s_entries[i].val[MAX_VAL_LEN - 1] = '\0';
            goto write_file;
        }
    }
    // Add new entry
    if (s_count < MAX_ENTRIES) {
        strncpy(s_entries[s_count].key, key, MAX_KEY_LEN - 1);
        s_entries[s_count].key[MAX_KEY_LEN - 1] = '\0';
        strncpy(s_entries[s_count].val, value, MAX_VAL_LEN - 1);
        s_entries[s_count].val[MAX_VAL_LEN - 1] = '\0';
        s_count++;
    }

write_file:
    // Rewrite config file
    SD.remove(CONFIG_PATH);
    File f = SD.open(CONFIG_PATH, FILE_WRITE);
    if (!f) return false;
    f.println("# PDAputer Configuration");
    for (int i = 0; i < s_count; i++) {
        f.printf("%s = %s\n", s_entries[i].key, s_entries[i].val);
    }
    f.close();
    Serial.printf("[CONFIG] Saved %s = %s\n", key, value);
    return true;
}

void handleAnyConfigChanged(const ConfigField& field) {
  set(field.key.c_str(), field.value.c_str());
}

void start() {
    if (s_config_started) {
        return;
    }

    config.addString("device_name", "Device Name", String(getDeviceName()));
    config.addString("wifi_ssid", "WiFi SSID", String(getWifiSSID()));
    config.addString("wifi_password", "WiFi Password", String(getWifiPassword()));
    config.addString("timezone", "Timezone", String(getTimezone()));
    config.addString("api_key", "API Key", String(getApiKey()));
    config.addString("time_format", "Time Format (12/24)", String(getTimeFormat()));
    config.addInteger("brightness", "Brightness", atoi(getBrightness()));
    config.addInteger("tone", "Tone", atoi(getTone()));

    config.addAction("esp_restart", "Restart Device", []() {
        ESP.restart();
    }, true);

    config.setOnChange(handleAnyConfigChanged);

    // configSerial.setPassword("change-me");
    configSerial.begin();

    Serial.println("[CONFIG] SerialConfig started");
    Serial.println(config.toJson());
    s_config_started = true;
}

void update() {
    configSerial.update();
}

} // namespace ConfigManager
