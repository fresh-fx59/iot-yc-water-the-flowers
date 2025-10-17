#ifndef OTA_H
#define OTA_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <LittleFS.h>
#include <secret.h>

const char* host = "esp32-watering";
const char* update_path = "/firmware";
const char* update_username = OTA_USER;
const char* update_password = OTA_PASSWORD;

WebServer httpServer(80);

// Forward declaration
class WateringSystem;

// Global pointer - will be set by main.cpp
WateringSystem* g_wateringSystem_ptr = nullptr;

// Forward declarations of functions
void setupOta();
void loopOta();
void registerApiHandlers();  // Forward declaration only

const char* updateSuccessPage = 
R"(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Update Success</title>
  <meta http-equiv="refresh" content="10;url=/">
  <style>
    body { font-family: Arial, sans-serif; margin: 40px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); text-align: center; min-height: 100vh; display: flex; align-items: center; justify-content: center; }
    .container { background: white; padding: 40px; border-radius: 12px; box-shadow: 0 20px 60px rgba(0,0,0,0.3); max-width: 500px; }
    .success { color: #4CAF50; font-size: 48px; }
  </style>
</head>
<body>
<div class="container">
  <div class="success">✓</div>
  <h1>Update Successful!</h1>
  <p>Device is rebooting...</p>
  <p>This page will redirect in 10 seconds.</p>
</div>
</body>
</html>)";

bool checkAuth() {
  if (!httpServer.authenticate(update_username, update_password)) {
    httpServer.requestAuthentication();
    return false;
  }
  return true;
}

// Serve files from LittleFS
void serveFile(const String& path, const String& contentType) {
  Serial.printf("Attempting to serve: %s\n", path.c_str());
  
  if (!LittleFS.exists(path)) {
    Serial.printf("ERROR: File not found: %s\n", path.c_str());
    httpServer.send(404, "text/plain", "File not found: " + path);
    return;
  }

  File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.printf("ERROR: Failed to open file: %s\n", path.c_str());
    httpServer.send(500, "text/plain", "Failed to open file");
    return;
  }

  Serial.printf("✓ Serving file: %s (%d bytes)\n", path.c_str(), file.size());
  httpServer.streamFile(file, contentType);
  file.close();
}

void setWateringSystemRef(WateringSystem* ws) {
  if (ws == nullptr) {
    Serial.println("ERROR: setWateringSystemRef called with nullptr!");
    return;
  }
  g_wateringSystem_ptr = ws;
  Serial.println("✓ WateringSystem reference set for web server");
  Serial.printf("  Setting: g_wateringSystem_ptr = 0x%p\n", (void*)g_wateringSystem_ptr);
  Serial.printf("  Verify: g_wateringSystem_ptr = 0x%p\n", (void*)g_wateringSystem_ptr);
}

// Getter to verify the pointer
WateringSystem* getWateringSystemRef() {
  Serial.printf("getWateringSystemRef returning: 0x%p\n", (void*)g_wateringSystem_ptr);
  return g_wateringSystem_ptr;
}

void setupOta() {
  delay(2000);
  
  Serial.println();
  Serial.println();
  Serial.println("=================================");
  Serial.println("Setting up Web Server...");
  Serial.println("=================================");

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("ERROR: LittleFS Mount Failed");
    Serial.println("Attempting to format and remount...");
    LittleFS.format();
    if (!LittleFS.begin()) {
      Serial.println("FATAL: LittleFS initialization failed!");
      return;
    }
  }
  Serial.println("✓ LittleFS mounted successfully");
  
  // List files in filesystem
  Serial.println("\nFiles in LittleFS:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.printf("  - %s (%d bytes)\n", file.name(), file.size());
    file = root.openNextFile();
  }
  Serial.println();

  // Start mDNS
  if (MDNS.begin(host)) {
    Serial.println("✓ mDNS responder started");
  } else {
    Serial.println("ERROR: mDNS responder failed!");
  }

  // Serve index.html from root
  httpServer.on("/", HTTP_GET, []() {
    Serial.println("GET / requested");
    serveFile("/web/index.html", "text/html");
  });

  // Serve CSS
  httpServer.on("/css/style.css", HTTP_GET, []() {
    Serial.println("GET /css/style.css requested");
    serveFile("/web/css/style.css", "text/css");
  });

  // Serve JavaScript
  httpServer.on("/js/app.js", HTTP_GET, []() {
    Serial.println("GET /js/app.js requested");
    serveFile("/web/js/app.js", "application/javascript");
  });

  // Firmware update page
  httpServer.on("/firmware", HTTP_GET, []() {
    if (!checkAuth()) return;
    
    String firmwarePage = R"(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Firmware Update</title>
  <style>
    body { font-family: Arial, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; display: flex; align-items: center; justify-content: center; }
    .container { background: white; padding: 40px; border-radius: 12px; box-shadow: 0 20px 60px rgba(0,0,0,0.3); max-width: 500px; width: 100%; }
    h1 { color: #333; margin-bottom: 10px; }
    .info { background: #e3f2fd; padding: 15px; border-radius: 4px; margin: 20px 0; border-left: 4px solid #2196F3; color: #1565c0; font-size: 14px; }
    input[type=file] { display: block; margin: 20px 0; padding: 10px; width: 100%; }
    input[type=submit] { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 12px 30px; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; width: 100%; font-weight: bold; }
    input[type=submit]:hover { opacity: 0.9; }
    a { color: #667eea; text-decoration: none; }
  </style>
</head>
<body>
  <div class="container">
    <h1>🔧 Firmware Update</h1>
    <div class="info">
      <strong>Device:</strong> Watering System<br>
      <strong>Platform:</strong> ESP32-S3<br>
      <strong>Endpoint:</strong> /firmware
    </div>
    <form method='POST' action='/firmware' enctype='multipart/form-data'>
      <input type='file' name='update' accept='.bin' required>
      <input type='submit' value='Update Firmware'>
    </form>
    <div style="margin-top: 20px; text-align: center;">
      <a href="/">← Back to Control Panel</a>
    </div>
  </div>
</body>
</html>)";
    
    httpServer.sendHeader("Connection", "close");
    httpServer.send(200, "text/html", firmwarePage);
  });

  // Handle firmware upload
  httpServer.on("/firmware", HTTP_POST, []() {
    if (!checkAuth()) return;
    httpServer.send(200, "text/html", updateSuccessPage);
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = httpServer.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      } else {
        Serial.printf("Progress: %d%%\r", (Update.progress() * 100) / Update.size());
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("\nUpdate Success: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  // Status endpoint
  httpServer.on("/status", HTTP_GET, []() {
    String json = "{";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"chip_model\":\"" + String(ESP.getChipModel()) + "\",";
    json += "\"cpu_freq\":" + String(ESP.getCpuFreqMHz());
    json += "}";
    httpServer.send(200, "application/json", json);
  });

  // Handle 404
  httpServer.onNotFound([]() {
    httpServer.send(404, "text/plain", "Not Found");
  });

  httpServer.begin();
  MDNS.addService("http", "tcp", 80);
  
  Serial.println("=================================");
  Serial.println("Web Control Server Ready!");
  Serial.printf("Control Panel: http://%s.local\n", host);
  Serial.print("Or: http://");
  Serial.print(WiFi.localIP());
  Serial.println();
  Serial.printf("Firmware Update: http://%s.local/firmware\n", host);
  Serial.printf("Username: %s\n", update_username);
  Serial.printf("Password: %s\n", update_password);
  Serial.println("=================================");
  
  // Register API handlers LAST, after httpServer.begin()
  // Implementation is in main.cpp
  Serial.println("Registering API handlers...");
  registerApiHandlers();
  Serial.println("✓ API handlers registration complete");
}

void loopOta() {
  httpServer.handleClient();
}

#endif // OTA_H