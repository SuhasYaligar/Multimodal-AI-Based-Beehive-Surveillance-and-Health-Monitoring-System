/*
 * BHMS - Beehive Health Monitoring System (sensor node)
 * ESP32 + NAU7802 load cell + 2x DHT22 + INMP441 mic -> Adafruit IO (MQTT)
 *
 * Cycle: wake -> climate -> weight -> 60 s acoustic analysis -> publish -> deep sleep
 * Copy secrets.h.example to secrets.h and fill in your own credentials.
 */

#include <WiFi.h>
#include <driver/i2s.h>
#include <Preferences.h>
#include "arduinoFFT.h"
#include "HX711.h"
#include "DHT.h"
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"
#include "Adafruit_NAU7802.h"
#include "secrets.h"   // WLAN_SSID, WLAN_PASS, AIO_USERNAME, AIO_KEY

// ======================== CONFIGURATION ========================
#define AIO_SERVER "io.adafruit.com"
#define AIO_SERVERPORT 1883

// INMP441 Microphone Pins
#define I2S_WS 25
#define I2S_SD 32
#define I2S_SCK 33
#define I2S_PORT I2S_NUM_0

// HX711 Load Cell Pins (legacy path, NAU7802 is used for weight)
const int LOADCELL_DOUT_PIN = 5;
const int LOADCELL_SCK_PIN = 4;
const float CALIBRATION_FACTOR = 110.960;
const long FIXED_TARE_OFFSET = 0;
const float TEMP_BASELINE = 28.1;       // °C reference baseline point
const float DRIFT_COEFFICIENT = 16.1;   // Grams shifted per degree Celsius

// Interrupt pin to reset the load cell
const int MAINTENANCE_PIN = GPIO_NUM_14;  // Must be an RTC GPIO

#define I2C_SDA 21
#define I2C_SCL 22
// DHT22 Climate Sensor Pins
#define DHTPIN_HIVE 26
#define DHTPIN_ENV 27
#define DHTTYPE DHT22

// Audio Parameters
#define SAMPLES 1024
#define SAMPLING_FREQ 16000

#define AMPLITUDE_THRESHOLD 3000.0
#define DESERTED_THRESHOLD 800.0
#define SMOOTHING_FACTOR 5

#define DEEP_SLEEP_TIME_SEC 480    // Deep sleep interval in seconds
#define SENSING_DURATION_MS 60000  // Hive sound monitoring for 60 seconds
// ===============================================================

struct ClimateData {
  float temperature;
  float humidity;
  float envtemp;
};

double vReal[SAMPLES];
double vImag[SAMPLES];
int32_t raw_samples[SAMPLES];

float freq_history[SMOOTHING_FACTOR] = { 0 };
int history_index = 0;

WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

// Registered Publisher Handles
Adafruit_MQTT_Publish hiveStatusPub = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/hive-status");
Adafruit_MQTT_Publish hiveWeightPub = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/hive-weight");
Adafruit_MQTT_Publish hiveRawWeightPub = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/hive-raw-weight");
Adafruit_MQTT_Publish hiveTempPub = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/hive-temp");
Adafruit_MQTT_Publish hiveHumidPub = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/hive-humid");
Adafruit_MQTT_Publish envTempPub   = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/env-temp");

ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);
HX711 scale;
Adafruit_NAU7802 nau;
long zeroOffset = 0;
bool nauReady = false;
Preferences preferences;
DHT dht_hive(DHTPIN_HIVE, DHTTYPE);
DHT dht_env(DHTPIN_ENV, DHTTYPE);

void setup_i2s() {
  const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLING_FREQ,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
}

// Initialize NAU7802 over I2C
bool setup_nau7802() {
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!nau.begin()) {
    Serial.println("ERROR: NAU7802 not found. Check SDA/SCL and power.");
    return false;
  }

  nau.setLDO(NAU7802_3V0);
  nau.setGain(NAU7802_GAIN_128);
  nau.setRate(NAU7802_RATE_10SPS);

  if (!nau.calibrate(NAU7802_CALMOD_INTERNAL)) {
    Serial.println("WARNING: NAU7802 internal calibration failed.");
  }
  return true;
}

// Average raw readings; 40 readings take about 4 seconds at 10 SPS
long read_average_raw(int sampleCount) {
  int64_t sum = 0;

  for (int i = 0; i < sampleCount; i++) {
    unsigned long waitStart = millis();
    while (!nau.available()) {
      delay(1);
      if (millis() - waitStart > 3000) {
        Serial.println("ERROR: Timed out waiting for NAU7802.");
        return LONG_MIN;
      }
    }
    sum += nau.read();
  }

  return (long)(sum / sampleCount);
}

// Call only when the load cell platform is EMPTY
bool tare_and_save_offset(int sampleCount) {
  Serial.println("Taring: ensure the load is REMOVED.");
  delay(1500);

  long newOffset = read_average_raw(sampleCount);
  if (newOffset == LONG_MIN) {
    Serial.println("Tare failed; offset not saved.");
    return false;
  }

  zeroOffset = newOffset;
  preferences.putLong("saved_offset", zeroOffset);
  Serial.print("Saved NAU7802 zero offset: ");
  Serial.println(zeroOffset);
  return true;
}

// 1. Get hive weight from NAU7802 (Wi-Fi is switched off during the measurement)
float get_hive_weight() {
  Serial.println("Pausing Wi-Fi for measurement...");
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  delay(1000);
  if (!nauReady) {
    Serial.println("NAU7802 unavailable.");
    return NAN;
  }

  delay(5000);

  unsigned long waitStart = millis();
  while (!nau.available() && millis() - waitStart < 3000) {
    delay(1);
  }

  if (!nau.available()) {
    Serial.println("NAU7802 reading timeout.");
    return NAN;
  }

  long rawReading = nau.read();
  float rawweight = (float)(rawReading - zeroOffset) / CALIBRATION_FACTOR;

  Serial.println("Reconnecting Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WLAN_SSID, WLAN_PASS);

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 20) {
    delay(250);
    timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi Reconnected!");
  } else {
    Serial.println("Wi-Fi reconnect timed out.");
  }

  Serial.print("Saved NAU7802 zero offset: ");
  Serial.println(zeroOffset);
  Serial.print("NAU raw weight: ");
  Serial.print(rawweight);
  return rawweight;
}

// Temperature drift compensation using the outside-air sensor
float get_corrected_weight(float raw_weight, float temp) {
  return (raw_weight - ((temp - TEMP_BASELINE) * DRIFT_COEFFICIENT));
}

// 2. Hive + environment climate
ClimateData get_hive_humtemp() {
  dht_hive.begin();
  dht_env.begin();
  delay(2000);  // Required initialization stabilization window

  ClimateData data;
  data.humidity = dht_hive.readHumidity();
  data.temperature = dht_hive.readTemperature();
  data.envtemp = dht_env.readTemperature();

  if (isnan(data.humidity) || isnan(data.temperature)) {
    Serial.println("Error: Failed to read from DHT22 sensor!");
    data.temperature = 0.0;
    data.humidity = 0.0;
    data.envtemp = 0.0;
    return data;
  }

  Serial.print("Climate Status -> Temp: ");
  Serial.print(data.temperature, 1);
  Serial.print("C | Hum: ");
  Serial.print(data.humidity, 1);
  Serial.println("%");

  return data;
}

// 3. Hive audio status (JSON string)
String get_hive_status() {
  setup_i2s();

  unsigned long sensing_start = millis();
  long healthyCount = 0;
  long agitatedCount = 0;
  long silentCount = 0;

  double runningFreqSum = 0;
  double runningAmpSum = 0;
  long validSampleCount = 0;

  Serial.println("Sampling hive acoustics...");

  while (millis() - sensing_start < SENSING_DURATION_MS) {
    size_t bytes_read = 0;
    esp_err_t result = i2s_read(I2S_PORT, &raw_samples, sizeof(raw_samples), &bytes_read, portMAX_DELAY);

    if (result == ESP_OK && bytes_read > 0) {
      int samples_read = bytes_read / 4;
      for (int i = 0; i < samples_read; i++) {
        vReal[i] = (double)(raw_samples[i] >> 14);
        vImag[i] = 0.0;
      }

      FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
      FFT.compute(FFT_FORWARD);
      FFT.complexToMagnitude();

      double peak = FFT.majorPeak();
      int peakIndex = round((peak * SAMPLES) / SAMPLING_FREQ);
      double peakAmplitude = vReal[peakIndex];

      if (peakAmplitude < AMPLITUDE_THRESHOLD) {
        // Below the working noise floor: count as a silent moment
        if (peakAmplitude < DESERTED_THRESHOLD) {
          silentCount++;
        }
      } else if (peak >= 200.0 && peak <= 600.0) {
        // Above threshold and inside the bee band: valid bee sound
        freq_history[history_index] = peak;
        history_index = (history_index + 1) % SMOOTHING_FACTOR;

        float smoothed_freq = 0;
        int valid_readings = 0;
        for (int i = 0; i < SMOOTHING_FACTOR; i++) {
          if (freq_history[i] > 0) {
            smoothed_freq += freq_history[i];
            valid_readings++;
          }
        }
        smoothed_freq /= valid_readings;

        runningFreqSum += smoothed_freq;
        runningAmpSum += peakAmplitude;
        validSampleCount++;

        if (smoothed_freq >= 200.0 && smoothed_freq <= 300.0) {
          healthyCount++;
        } else if (smoothed_freq > 300.0 && smoothed_freq <= 450.0) {
          agitatedCount++;
        }
      }
    }
    delay(50);
  }

  double finalAverageFreq = 0.0;
  double finalAverageAmp = 0.0;
  String final_status = "Ambient Noise / Inactive";

  if (validSampleCount > 0) {
    finalAverageFreq = runningFreqSum / validSampleCount;
    finalAverageAmp = runningAmpSum / validSampleCount;
  }

  // Valid bee data points mean the hive cannot be empty
  if (validSampleCount > 0) {
    if (agitatedCount > healthyCount) {
      final_status = "WARNING: Highly Agitated";
    } else {
      final_status = "Healthy / Normal Hive Hum";
    }
  } else if (silentCount > 0) {
    final_status = "Empty / Silent Hive";
  }

  i2s_driver_uninstall(I2S_PORT);

  char jsonBuffer[256];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
           "{\"status\":\"%s\",\"frequency\":%.1f,\"amplitude\":%.0f}",
           final_status.c_str(), finalAverageFreq, finalAverageAmp);

  return String(jsonBuffer);
}

void connect_wifi_and_mqtt(String jsonPayload, float currentWeight, float RawWeight, ClimateData climate) {
  Serial.print("\nConnecting to Wi-Fi: ");
  Serial.println(WLAN_SSID);
  WiFi.begin(WLAN_SSID, WLAN_PASS);

  unsigned long wifiTimeout = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiTimeout > 15000) {
      Serial.println("\nWi-Fi connection failed. Skipping transmission.");
      return;
    }
  }
  Serial.println("\nWi-Fi Connected!");

  int8_t ret;
  uint8_t retries = 3;
  Serial.println("Connecting to Adafruit IO MQTT...");
  while ((ret = mqtt.connect()) != 0) {
    Serial.println(mqtt.connectErrorString(ret));
    mqtt.disconnect();
    delay(2000);
    retries--;
    if (retries == 0) {
      Serial.println("MQTT Connection Failed.");
      return;
    }
  }
  Serial.println("MQTT Broker Connected!");

  // Publish sequentially with 1-second delays (Adafruit IO rate limit)
  if (hiveStatusPub.publish(jsonPayload.c_str())) Serial.println("Published Status JSON successfully!");
  else Serial.println("Status JSON failed!");
  delay(1000);

  if (hiveWeightPub.publish(currentWeight, 1)) Serial.println("Published Weight successfully!");
  else Serial.println("Weight failed!");
  delay(1000);

  if (hiveRawWeightPub.publish(RawWeight, 1)) Serial.println("Published Raw Weight successfully!");
  else Serial.println("Raw weight failed!");
  delay(1000);

  if (hiveTempPub.publish(climate.temperature, 1)) Serial.println("Published Temperature successfully!");
  else Serial.println("Temperature failed!");
  delay(1000);

  if (envTempPub.publish(climate.envtemp, 1)) Serial.println("Published Env Temperature successfully!");
  else Serial.println("Env temperature failed!");
  delay(1000);

  if (hiveHumidPub.publish(climate.humidity, 1)) Serial.println("Published Humidity successfully!");
  else Serial.println("Humidity failed!");
  delay(500);

  Serial.println("All individual feeds processing completed.");
  mqtt.disconnect();
  WiFi.disconnect(true);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  preferences.begin("loadcell", false);
  nauReady = setup_nau7802();

  if (!nauReady) {
    Serial.println("NAU7802 init failed. Restart/check wiring.");
    while (true) delay(1000);
  }

  // Check what woke up the ESP32
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    // Maintenance wake-up (pin pulled LOW): re-tare with an EMPTY platform
    Serial.println("Wakeup caused by maintenance pin interrupt.");
    tare_and_save_offset(40);
    Serial.println("Maintenance complete. Re-apply your load now.");
    delay(2000);
  }

  long savedOffset = preferences.getLong("saved_offset", LONG_MIN);
  if (savedOffset == LONG_MIN) {
    Serial.println("No saved NAU7802 baseline. Initial tare requires EMPTY platform.");
    tare_and_save_offset(40);
  } else {
    zeroOffset = savedOffset;
    Serial.print("Restored NAU7802 zero offset: ");
    Serial.println(zeroOffset);
  }

  Serial.println("\n--- Hive Awake Cycle Triggered ---");

  ClimateData hive_climate = get_hive_humtemp();
  float hive_raw_weight = get_hive_weight();
  float hive_weight = get_corrected_weight(hive_raw_weight, hive_climate.envtemp);

  // Acoustic analysis (about 60 s)
  String hive_status_json = get_hive_status();

  // Turn Wi-Fi on only after all sensing is finished
  connect_wifi_and_mqtt(hive_status_json, hive_weight, hive_raw_weight, hive_climate);

  Serial.println("Entering deep sleep...");
  esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_TIME_SEC * 1000000);

  // External wake-up for maintenance (pin LOW)
  pinMode(MAINTENANCE_PIN, INPUT_PULLUP);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)MAINTENANCE_PIN, 0);

  esp_deep_sleep_start();
}

void loop() {}
