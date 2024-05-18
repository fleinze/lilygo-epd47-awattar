/*
AWATTAR-Strompreis-Display
Autor Florian Heinze

License CC BY-NC-SA 4.0

*/

#include "epd_driver.h"

#include <WiFiManager.h>  //version 2
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>  //version 7
#include <Preferences.h>
#include "esp_sleep.h"
#include "esp_adc_cal.h"
#include "esp_sntp.h"

#include "pcf8563.h"
#include <Wire.h>
PCF8563_Class rtc;

//fonts
#include "notosans8.h"
#include "notosans8b.h"
#include "notosans10b.h"
#include "notosans12.h"
#include "notosans12b.h"
#include "notosans18.h"
#include "notosans18b.h"
#include "sourceserif24b.h"
#include "sourceserif18b.h"

uint8_t *framebuffer;

Preferences preferences;

WiFiManager wm;

//Standardwerte
#define DEF_NEW_VAL_HOUR 14
#define DEF_NEW_VAL_MINUTE 5
#define DEF_AWATTAR_URL "https://api.awattar.at/v1/marketdata"
#define DEF_NTP_SRV "pool.ntp.org"
#define DEF_SHOWBATT true

//Pins
const uint8_t BATT_PIN = 14;
const uint8_t BUTTON_PIN = 21;
const uint8_t SDA_PIN = 18;
const uint8_t SCL_PIN = 17;

//ap config
const char *APNAME = "strompreis-disp";
const char *APPASS = "EPEX-Spot";
const uint8_t CONNECT_RETRIES = 2;           // 2 retries
const uint16_t CONFIG_PORTAL_TIMEOUT = 300;  // 5 minutes = 300s
const char *HOSTNAME = "strompreis-disp";
const char *TITLE = "Strompreis Dislay";
std::vector<const char *> MENUIDS = { "wifi", "wifinoscan", "param", "info", "exit", "sep", "update" };

//time
const char *time_zone = "CET-1CEST,M3.5.0,M10.5.0/3";  // timezone germany/austria with daylight saving
const int NTP_TIMEOUT = 300;                           // in 1/10s seconds
bool synced = false;

//Persistente daten zwischen sleep-cycles
RTC_DATA_ATTR float marketprice[48];
RTC_DATA_ATTR time_t start_timestamp[48];
RTC_DATA_ATTR unsigned int len = 0;

//Error
enum ErrorType {
  NO_ERROR = 0,
  NO_DATA_RECEIVED = 1,
  NOT_ENOUGH_HOURS_RECEIVED = 2
};
ErrorType error = NO_ERROR;

//variablen für Einstellugen
int new_val_hour;
int new_val_minute;
String awattarurl;
String ntpsrv;
bool showbatt;

void setup() {

  Serial.begin(115200);
  bool startconfigportal = false;

  // Einstellungen im Dateisystem
  preferences.begin("strompreis", false);

  // Einstellungen zurücksetzen wenn Button gedrückt ist nach reset.
  pinMode(BUTTON_PIN, INPUT);
  unsigned long buttonPressTime = 0;
  const unsigned long longPressTime = 3000;  // Zeit für einen langen Druck in Millisekunden
  while (true) {                             // button prüfen. wenn button gedrückt ist, configportal starten. wenn länger als 3s gedrückt, alle settings resetten.
    if (!digitalRead(BUTTON_PIN)) {
      if (buttonPressTime == 0) {  // Wenn die Schaltfläche gerade gedrückt wurde
        buttonPressTime = millis();
        startconfigportal = true;
        Serial.println("start config portal");
      } else if (millis() - buttonPressTime > longPressTime) {  // Wenn die Schaltfläche lange genug gedrückt wurde
        Serial.println("reset wm settings");
        wm.resetSettings();
        preferences.clear();  // langer Druck
        break;                // Verlassen der Schleife
      }
    } else {
      break;  // Verlassen der Schleife, wenn die Schaltfläche losgelassen wird
    }
  }

  // Hole gepeicherte Einstellugnen
  new_val_hour = preferences.getInt("hour", DEF_NEW_VAL_HOUR);
  new_val_minute = preferences.getInt("minute", DEF_NEW_VAL_MINUTE);
  awattarurl = preferences.getString("url", DEF_AWATTAR_URL);
  ntpsrv = preferences.getString("ntpsrv", DEF_NTP_SRV);
  showbatt = preferences.getBool("showbatt", DEF_SHOWBATT);

  // Add custom parameters
  WiFiManagerParameter custom_awattarurl("url", "Awattar URL", awattarurl.c_str(), 100);
  char newvalHTML[400];
  sprintf(newvalHTML, "<div style=\"display: flex; justify-content: space-between;\"><div><label for='hour'>Awattar Update Stunde</label><br/><input id='hour' name='hour' maxlength='40' value='%d' type=\"number\" min=\"0\" max=\"23\"></div><div><label for='minute'>Awattar Update Minute</label><br/><input id='minute' name='minute' maxlength='40' value='%d' type=\"number\" min=\"0\" max=\"59\"></div></div>", new_val_hour, new_val_minute);
  WiFiManagerParameter custom_new_val(newvalHTML);
  WiFiManagerParameter custom_ntpsrv("ntpsrv", "NTP", ntpsrv.c_str(), 100);

  wm.addParameter(&custom_awattarurl);
  wm.addParameter(&custom_ntpsrv);
  wm.addParameter(&custom_new_val);
  if (showbatt) {
    WiFiManagerParameter custom_showbatt("<input type='checkbox' id='showbatt' name='showbatt' checked > <label for='showbatt'>Batterie anzeigen</label>");
    wm.addParameter(&custom_showbatt);
  } else {
    WiFiManagerParameter custom_showbatt("<input type='checkbox' id='showbatt' name='showbatt' > <label for='showbatt'>Batterie anzeigen</label>");
    wm.addParameter(&custom_showbatt);
  }

  //init epd und framebuffer
  epd_init();
  framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
  if (!framebuffer) Serial.println("Memory alloc failed!");
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);  //clear framebuffer
  epd_poweron();

  //init RTC
  setenv("TZ", time_zone, 1);
  tzset();
  Wire.begin(SDA_PIN, SCL_PIN);
  rtc.begin();
  rtc.syncToSystem();

  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  if (len == 48 && now >= start_timestamp[24] && now < start_timestamp[47]) {
    // wenn: alte Daten vorhanden und Zeit nach Mitternacht -> Daten reorganisieren und Diagram neu zeichnen ohne Wifi
    len = 24;
    for (int i = 0; i < 24; i++) {
      marketprice[i] = marketprice[i + 24];
      start_timestamp[i] = start_timestamp[i + 24];
    }
    synced = true;  // kein sntp sync, wir tun so als ob
  } else {
    // default case: mit Wifi verbinden und Daten holen
    wm.setDarkMode(true);                          // dark mode for cool
    wm.setAPCallback(configportalscreen);          // callbyck wenn autoconnect fehlschlägt
    wm.setConnectRetries(CONNECT_RETRIES);         // 2 retries
    wm.setConnectTimeout(30);                      // 30s timeout
    wm.setSaveConfigCallback(saveConfigCallback);  // saveConfigCallback
    wm.setTitle(TITLE);
    wm.setHostname(HOSTNAME);
    wm.setMenu(MENUIDS);

    bool res;
    if (!startconfigportal) {
      wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT);      // 5 min
      wm.setConfigPortalTimeoutCallback(wmTimeoutCallback);  // timeoutcallback, funktioniert nicht mit startConfigPortal
      res = wm.autoConnect(APNAME, APPASS);                  // autoconnect
    } else {
      res = wm.startConfigPortal(APNAME, APPASS);
      //ESP.restart();
    }

    if (!res) {
      Serial.println("Failed to connect");
    } else {
      Serial.println("connected...yeey :)");
      //draw rssi
      int rssi = WiFi.RSSI();
      DrawRSSI(908, 30, rssi);
    }

    //config ntp
    sntp_set_time_sync_notification_cb(sntpSyncCallback);
    configTzTime(time_zone, ntpsrv.c_str());

    time_t midnight = calculateMidnightTimestamp();
    char url[strlen(awattarurl.c_str()) + 40];
    sprintf(url, "%s?start=%ld000&end=%ld000", awattarurl.c_str(), midnight, (midnight + 172800UL));
    JsonDocument awattar = awattarGet(url);
    len = awattar["data"].size();
    if (len < 48 && (timeinfo.tm_hour > new_val_hour || (timeinfo.tm_hour >= new_val_hour && timeinfo.tm_min >= new_val_minute))) error = NOT_ENOUGH_HOURS_RECEIVED;  // not enought hours received
    else if (len == 0) error = NO_DATA_RECEIVED;                                                                                                                      // no data received
    else error = NO_ERROR;
    Serial.print("hours received: ");
    Serial.println(len);

    for (int i = 0; i < len; i++) {  // kopiere daten in persistente variablen
      marketprice[i] = awattar["data"][i]["marketprice"];
      start_timestamp[i] = ((uint64_t)awattar["data"][i]["start_timestamp"]) / 1000;
    }
  }

  if (showbatt) DrawBattery(770, 30);          // draw battery indicator
  diagram(marketprice, start_timestamp, len);  // draw diagram

  Serial.print("going to sleep for us: ");

  time_t rawtime;
  getLocalTime(&timeinfo);
  rawtime = mktime(&timeinfo);
  uint64_t sleep_time;
  uint16_t currentHour = timeinfo.tm_hour;
  uint16_t currentMinute = timeinfo.tm_min;
  uint16_t currentSecond = timeinfo.tm_sec;
  uint32_t secondsUntilMidnight = (24 * 60 * 60) - (currentHour * 60 * 60 + currentMinute * 60 + currentSecond);
  if (currentHour > new_val_hour || (currentHour == new_val_hour && currentMinute >= new_val_minute)) {
    sleep_time = ((uint64_t)secondsUntilMidnight + 600) * 1000000;  //  600s nach mitternacht
  } else {
    sleep_time = ((uint64_t)secondsUntilMidnight - ((24 - new_val_hour) * 3600) + (60 * new_val_minute)) * 1000000;  // bis new_val_minute nach new_val_hour
  }
  if (error == NOT_ENOUGH_HOURS_RECEIVED && !(currentHour > new_val_hour || (currentHour == new_val_hour && currentMinute >= new_val_minute))) {  // nicht genug daten empfangen
    sleep_time = 3600000000;                                                                                                                      // 1 h
  } else if (error == NO_DATA_RECEIVED) {                                                                                                         // keine Daten empfangen
    sleep_time = 300000000;                                                                                                                       // 5 min
  }
  Serial.println(sleep_time);
  // uint64_t sleep_time = 60000000; // debug

  // next update string
  int cursor_x = 680;
  int cursor_y = 538;
  rawtime += sleep_time / 1000000;
  timeinfo = *localtime(&rawtime);
  char datetimeString[20];
  strftime(datetimeString, sizeof(datetimeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  writeln((GFXfont *)&NotoSans8, "Next Update:  ", &cursor_x, &cursor_y, framebuffer);
  writeln((GFXfont *)&NotoSans8, datetimeString, &cursor_x, &cursor_y, framebuffer);

  epd_draw_grayscale_image(epd_full_screen(), framebuffer);  //frambuffer zeichnen

  int n = 0;
  while (!synced && n < NTP_TIMEOUT) {  //auf sntp sync warten
    delay(100);
    n++;
  }

  epd_poweroff_all();

  esp_sleep_enable_timer_wakeup(sleep_time);
  esp_deep_sleep_start();  //sleep
}

void loop() {
  //never reached
}

void saveConfigCallback() {
  Serial.println("save config");
  int temp_hour = -1;
  if (wm.server->hasArg("hour")) {
    temp_hour = atoi(wm.server->arg("hour").c_str());
  }
  int temp_minute = -1;
  if (wm.server->hasArg("minute")) {
    temp_minute = atoi(wm.server->arg("minute").c_str());
  }
  if (temp_hour >= 0 && temp_hour <= 23) {  //daten prüfen
    new_val_hour = temp_hour;
    preferences.putInt("hour", new_val_hour);
  }
  if (temp_minute >= 0 && temp_minute <= 59) {  //daten prüfen
    new_val_minute = temp_minute;
    preferences.putInt("minute", new_val_minute);
  }
  if (wm.server->hasArg("url")) {
    awattarurl = wm.server->arg("url");
    preferences.putString("url", awattarurl);
  }
  if (wm.server->hasArg("ntpsrv")) {
    ntpsrv = wm.server->arg("ntpsrv");
    preferences.putString("ntpsrv", ntpsrv);
  }
  if (wm.server->hasArg("showbatt")) {
    showbatt = true;
  } else {
    showbatt = false;
  }
  preferences.putBool("showbatt", showbatt);
}


// callback-function für sntp-sync
void sntpSyncCallback(struct timeval *t) {
  Serial.println("sntp synced.");
  rtc.syncToRtc();
  synced = true;
}

void diagram(float marketprice[], time_t start_timestamp[], unsigned int len) {
  epd_clear();

  float max_height1 = 0.0;
  float min_height1 = 0.0;
  float max_height2 = 0.0;
  float min_height2 = 0.0;
  int cursor_x = 0;
  int cursor_y = 0;
  const int LEFT_MARGIN = 13;

  //Überschrift
  cursor_x = 275;
  cursor_y = 35;
  writeln((GFXfont *)&SourceSerif18B, "Strompreis [€/MWh]", &cursor_x, &cursor_y, framebuffer);

  // datum
  char buf[11];
  time_t datum = start_timestamp[0];
  strftime(buf, sizeof(buf), "%d.%m.%Y", localtime(&datum));
  cursor_x = LEFT_MARGIN + 3;
  cursor_y = 25;
  writeln((GFXfont *)&NotoSans12B, buf, &cursor_x, &cursor_y, framebuffer);
  if (len > 24) {
    cursor_x = LEFT_MARGIN + 3;
    cursor_y = 308;
    datum = start_timestamp[24];
    strftime(buf, sizeof(buf), "%d.%m.%Y", localtime(&datum));
    writeln((GFXfont *)&NotoSans12B, buf, &cursor_x, &cursor_y, framebuffer);
  }

  // min_height1, max_height1 für die ersten 24h
  for (int i = 0; i < min(len, (unsigned int)24); ++i) {
    if (marketprice[i] > max_height1) {
      max_height1 = marketprice[i];
    }
    if (marketprice[i] < min_height1) {
      min_height1 = marketprice[i];
    }
  }
  //min_height2, max_height2 für die zweiten 24h
  if (len > 24) {
    for (int i = 24; i < len; ++i) {
      if (marketprice[i] > max_height2) {
        max_height2 = marketprice[i];
      }
      if (marketprice[i] < min_height2) {
        min_height2 = marketprice[i];
      }
    }
  }
  float diagram_height = 200.0;
  if (len <= 24) diagram_height = 430.0;
  //scale_factor1 für die ersten 24h
  float scale_factor1 = diagram_height / (max_height1 - min_height1);
  //scale_factor2 für die zweiten 24h
  float scale_factor2 = 0;
  if (len > 24) scale_factor2 = diagram_height / (max_height2 - min_height2);

  // rect_width
  int rect_width = 950 / 24;

  // balken zeichnen
  for (int i = 0; i < len; ++i) {
    // höhe berechnen
    int rect_height = i < 24 ? abs(marketprice[i]) * scale_factor1 : abs(marketprice[i]) * scale_factor2;

    // x-coord berechnen
    int x = i % 24 * rect_width + LEFT_MARGIN;  // 18 px from left

    // y-coord berechnen
    int bottom = i < 24 ? 250 - (int)(scale_factor1 * (-min_height1)) : 500 - (int)(scale_factor2 * (-min_height2));
    if (len <= 24) bottom = 495 - (int)(scale_factor1 * (-min_height1));

    int y;
    if (marketprice[i] > 0) y = bottom - rect_height;
    else y = bottom;

    // balken zeichnen
    epd_fill_rect(x, y, rect_width - 5, rect_height, 180, framebuffer);
    epd_draw_rect(x, y, rect_width - 5, rect_height, 0, framebuffer);

    // preise schreiben
    char value[6];
    sprintf(value, "%.0f", marketprice[i]);
    if (marketprice[i] >= 0 && marketprice[i] < 9.5) cursor_x = x + 12;
    else if (marketprice[i] >= 9.5 && marketprice[i] < 99.5) cursor_x = x + 7;
    else if (marketprice[i] >= 99.5) cursor_x = x + 2;
    else if (marketprice[i] < 0 && marketprice[i] >= -10) cursor_x = x + 7;
    else if (marketprice[i] < -10 && marketprice[i] >= -100) cursor_x = x + 2;
    else cursor_x = x - 1;
    FontProperties props = {
      .fg_color = 0,
      .bg_color = 15,
      .fallback_glyph = 0,
      .flags = 0
    };
    if (i >= 24 && i <= 27 && y < 322) {  // Position bei kollision mit datum
      cursor_y = y + 15;
      props.bg_color = 12;
    } else cursor_y = y - 2;
    write_mode((GFXfont *)&NotoSans8B, value, &cursor_x, &cursor_y, framebuffer, BLACK_ON_WHITE, &props);
  }

  // stunden
  for (int i = 0; i < 24; ++i) {
    cursor_x = i * rect_width + LEFT_MARGIN + 3;  
    if (len <= 24) cursor_y = 523;
    else cursor_y = 280;
    char hour[3];
    sprintf(hour, "%02d", i);
    writeln((GFXfont *)&NotoSans12, hour, &cursor_x, &cursor_y, framebuffer);
  }

  cursor_x = 3;
  cursor_y = 538;

  //write last update string
  struct tm time;
  getLocalTime(&time);
  char datetimeString[20];
  strftime(datetimeString, sizeof(datetimeString), "%Y-%m-%d %H:%M:%S", &time);
  writeln((GFXfont *)&NotoSans8, "Last Update:  ", &cursor_x, &cursor_y, framebuffer);
  writeln((GFXfont *)&NotoSans8, datetimeString, &cursor_x, &cursor_y, framebuffer);
}


time_t calculateMidnightTimestamp() {
  // aktuelle zeit
  time_t now = time(nullptr);

  // in struct tm umwandeln
  struct tm *currentTime = localtime(&now);

  // auf mitternacht setzen
  currentTime->tm_hour = 0;
  currentTime->tm_min = 0;
  currentTime->tm_sec = 0;

  // modifiziertes struct in timestamp umwandeln
  return mktime(currentTime);
}

JsonDocument awattarGet(const char *url) {
  HTTPClient http;
  JsonDocument json;

  Serial.print("Connecting to: ");
  Serial.println(url);

  // Start  HTTP request
  http.begin(url);

  // GET request
  int httpResponseCode = http.GET();

  // erfolgreich?
  if (httpResponseCode > 0) {
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    // daten empfangen und json holen
    DeserializationError error = deserializeJson(json, http.getStream());
    if (error) {
      Serial.print("deserializeJson() failed: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.print("Error code: ");
    Serial.println(httpResponseCode);
  }

  // Close connection
  http.end();
  return json;
}

void wmTimeoutCallback() {
  epd_clear();
  int32_t posx = 50;
  int32_t posy = 90;
  write_string((GFXfont *)&NotoSans18, "WLAN-Fehler, bitte resetten", &posx, &posy, framebuffer);
  posx = 50;
  write_string((GFXfont *)&NotoSans18, "Neuer Verbindungsversuch in 30 min", &posx, &posy, framebuffer);
  posx = 3;
  posy = 538;
  //last update string
  struct tm time;
  getLocalTime(&time);
  char datetimeString[20];
  strftime(datetimeString, sizeof(datetimeString), "%Y-%m-%d %H:%M:%S", &time);
  writeln((GFXfont *)&NotoSans8, datetimeString, &posx, &posy, framebuffer);
  if (showbatt) DrawBattery(820, 30);
  epd_draw_grayscale_image(epd_full_screen(), framebuffer);
  epd_poweroff_all();
  uint64_t sleep_time = 1800000000;  //30 min
  esp_sleep_enable_timer_wakeup(sleep_time);
  esp_deep_sleep_start();
}

void configportalscreen(WiFiManager *myWiFiManager) {
  epd_clear();
  Serial.println("Entered Configuration Mode");
  //titel
  int32_t posx = 50;
  int32_t posy = 90;
  write_string((GFXfont *)&SourceSerif24B, "Dynamisches Strompreisdisplay", &posx, &posy, framebuffer);

  posx = 50;
  //grund für aufruf
  if (myWiFiManager->getWiFiIsSaved()) {
    Serial.println("Configportal reason: wifi not found");
    write_string((GFXfont *)&NotoSans18, "Konfiguriertes WLAN nicht gefunden", &posx, &posy, framebuffer);
  } else {
    write_string((GFXfont *)&NotoSans18, "Kein WLAN konfiguriert", &posx, &posy, framebuffer);
    Serial.println("Configportal reason: No Wifi configured");
  }
  posx = 50;
  write_string((GFXfont *)&NotoSans18, "Bitte mit Konfigurationsseite verbinden", &posx, &posy, framebuffer);
  posx = 50;
  posy = 290;
  write_mode((GFXfont *)&NotoSans18, "SSID: ", &posx, &posy, framebuffer, BLACK_ON_WHITE, NULL);
  posx = 250;
  write_string((GFXfont *)&NotoSans18B, myWiFiManager->getConfigPortalSSID().c_str(), &posx, &posy, framebuffer);
  posx = 50;
  write_mode((GFXfont *)&NotoSans18, "Passwort: ", &posx, &posy, framebuffer, BLACK_ON_WHITE, NULL);
  posx = 250;
  write_string((GFXfont *)&NotoSans18B, APPASS, &posx, &posy, framebuffer);
  posx = 50;
  write_mode((GFXfont *)&NotoSans18, "IP: ", &posx, &posy, framebuffer, BLACK_ON_WHITE, NULL);
  posx = 250;
  write_string((GFXfont *)&NotoSans18B, WiFi.softAPIP().toString().c_str(), &posx, &posy, framebuffer);
  epd_draw_rect(45, 255, 510, 148, 0, framebuffer);
  posx = 3;
  posy = 538;
  //last update string
  struct tm time;
  getLocalTime(&time);
  char datetimeString[20];
  strftime(datetimeString, sizeof(datetimeString), "%Y-%m-%d %H:%M:%S", &time);
  writeln((GFXfont *)&NotoSans8, datetimeString, &posx, &posy, framebuffer);
  if (showbatt) DrawBattery(820, 30);
  epd_draw_grayscale_image(epd_full_screen(), framebuffer);
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);  //framebuffer löschen
}

void DrawBattery(int x, int y) {
  uint8_t percentage = 100;
  int vref = 1100;
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    Serial.printf("eFuse Vref:%u mV\n", adc_chars.vref);
    vref = adc_chars.vref;
  }
  float voltage = (float)analogRead(BATT_PIN) / 4096.0 * 6.566 * (vref / 1000.0);
  if (voltage > 2) {
    Serial.println("Voltage = " + String(voltage));
    if (voltage >= 4.1617) percentage = 100;  //https://www.e3s-conferences.org/articles/e3sconf/pdf/2021/10/e3sconf_icies2020_00097.pdf
    else if (voltage >= 4.0913) percentage = 95;
    else if (voltage >= 4.0749) percentage = 90;
    else if (voltage >= 4.0606) percentage = 85;
    else if (voltage >= 4.0153) percentage = 80;
    else if (voltage >= 3.9592) percentage = 75;
    else if (voltage >= 3.9164) percentage = 70;
    else if (voltage >= 3.8687) percentage = 65;
    else if (voltage >= 3.8163) percentage = 60;
    else if (voltage >= 3.7735) percentage = 55;
    else if (voltage >= 3.7317) percentage = 50;
    else if (voltage >= 3.6892) percentage = 45;
    else if (voltage >= 3.6396) percentage = 40;
    else if (voltage >= 3.5677) percentage = 35;
    else if (voltage >= 3.5208) percentage = 30;
    else if (voltage >= 3.4712) percentage = 25;
    else if (voltage >= 3.3860) percentage = 20;
    else if (voltage >= 3.2880) percentage = 15;
    else if (voltage >= 3.2037) percentage = 10;
    else if (voltage >= 3.0747) percentage = 5;
    else percentage = 0;
    epd_draw_rect(x, y - 14, 40, 15, 0, framebuffer);                           //draw outline
    epd_fill_rect(x + 40, y - 10, 4, 7, 0, framebuffer);                        //draw battery pole
    epd_fill_rect(x + 2, y - 12, 36 * percentage / 100.0, 11, 0, framebuffer);  //draw filling according to state of charge
    x = x + 46;
    write_string((GFXfont *)&NotoSans8B, (String(percentage) + "%  " + String(voltage, 1) + "V").c_str(), &x, &y, framebuffer);  //write string
  }
}

void DrawRSSI(int x, int y, int rssi) {
  Serial.println("RSSI = " + String(rssi));
  if (rssi >= -100) epd_fill_rect(x + 8, y - 6, 6, 6, 0, framebuffer);    //1 bar -100dbm
  if (rssi >= -87) epd_fill_rect(x + 16, y - 12, 6, 12, 0, framebuffer);  //2 bar -87.5dbm
  if (rssi >= -75) epd_fill_rect(x + 24, y - 18, 6, 18, 0, framebuffer);  //3 bar -75dbm
  if (rssi >= -62) epd_fill_rect(x + 32, y - 24, 6, 24, 0, framebuffer);  //4 bar -62.5dbm
  if (rssi >= -50) epd_fill_rect(x + 40, y - 30, 6, 30, 0, framebuffer);  //5 bar -50dbm
}
