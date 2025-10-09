#ifndef OTA_H
#define OTA_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <secret.h>

const char* host = "esp32-watering";
const char* update_path = "/firmware";
const char* update_username = OTA_USER;
const char* update_password = OTA_PASSWORD;

WebServer httpServer(80);

// HTML page for firmware upload
const char* serverIndex = 
"<html><head><title>ESP32 Firmware Update</title>"
"<style>"
"body { font-family: Arial, sans-serif; margin: 40px; background: #f0f0f0; }"
"h1 { color: #333; }"
".container { background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); max-width: 500px; }"
"input[type=file] { margin: 20px 0; padding: 10px; border: 2px dashed #ccc; border-radius: 4px; width: 100%; }"
"input[type=submit] { background: #4CAF50; color: white; padding: 12px 30px; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; }"
"input[type=submit]:hover { background: #45a049; }"
".info { background: #e3f2fd; padding: 15px; border-radius: 4px; margin: 20px 0; border-left: 4px solid #2196F3; }"
"</style></head><body>"
"<div class='container'>"
"<h1>ESP32 Firmware Update</h1>"
"<div class='info'>"
"<strong>Device:</strong> Watering System<br>"
"<strong>Platform:</strong> ESP32-S3<br>"
"<strong>Endpoint:</strong> /firmware"
"</div>"
"<form method='POST' action='/firmware' enctype='multipart/form-data'>"
"<input type='file' name='update' accept='.bin' required>"
"<input type='submit' value='Update Firmware'>"
"</form>"
"</div></body></html>";

// Success page
const char* successPage = 
"<html><head><title>Update Success</title>"
"<meta http-equiv='refresh' content='10;url=/'>"
"<style>"
"body { font-family: Arial, sans-serif; margin: 40px; background: #f0f0f0; text-align: center; }"
".container { background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); max-width: 500px; margin: 0 auto; }"
".success { color: #4CAF50; font-size: 48px; }"
"</style></head><body>"
"<div class='container'>"
"<div class='success'>✓</div>"
"<h1>Update Successful!</h1>"
"<p>Device is rebooting...</p>"
"<p>This page will redirect in 10 seconds.</p>"
"</div></body></html>";

// Authentication check
bool checkAuth() {
  if (!httpServer.authenticate(update_username, update_password)) {
    httpServer.requestAuthentication();
    return false;
  }
  return true;
}

void setupOta() {
  Serial.println();
  Serial.println("Setting up OTA...");

  // Start mDNS
  if (MDNS.begin(host)) {
    Serial.println("mDNS responder started");
  } else {
    Serial.println("Error setting up mDNS responder!");
  }

  // Serve the upload form
  httpServer.on("/", HTTP_GET, []() {
    if (!checkAuth()) return;
    httpServer.sendHeader("Connection", "close");
    httpServer.send(200, "text/html", serverIndex);
  });

  // Handle firmware upload
  httpServer.on("/firmware", HTTP_POST, []() {
    if (!checkAuth()) return;
    httpServer.sendHeader("Connection", "close");
    httpServer.send(200, "text/html", successPage);
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = httpServer.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      
      // Start update process
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      // Write firmware data
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      } else {
        Serial.printf("Progress: %d%%\r", (Update.progress() * 100) / Update.size());
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      // Finish update
      if (Update.end(true)) {
        Serial.printf("\nUpdate Success: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  // Status endpoint
  httpServer.on("/status", HTTP_GET, []() {
    if (!checkAuth()) return;
    
    String json = "{";
    json += "\"device\":\"watering_system\",";
    json += "\"platform\":\"ESP32-S3\",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"chip_model\":\"" + String(ESP.getChipModel()) + "\",";
    json += "\"chip_revision\":" + String(ESP.getChipRevision()) + ",";
    json += "\"cpu_freq\":" + String(ESP.getCpuFreqMHz()) + ",";
    json += "\"flash_size\":" + String(ESP.getFlashChipSize()) + ",";
    json += "\"sketch_size\":" + String(ESP.getSketchSize()) + ",";
    json += "\"free_sketch_space\":" + String(ESP.getFreeSketchSpace());
    json += "}";
    
    httpServer.send(200, "application/json", json);
  });

  httpServer.begin();

  // Add HTTP service to mDNS
  MDNS.addService("http", "tcp", 80);
  
  Serial.println("=================================");
  Serial.println("OTA Update Server Ready!");
  Serial.printf("URL: http://%s.local%s\n", host, update_path);
  Serial.printf("Or: http://", host);
  Serial.print(WiFi.localIP());
  Serial.printf("%s\n", update_path);
  Serial.printf("Username: %s\n", update_username);
  Serial.printf("Password: %s\n", update_password);
  Serial.println("Status: http://%s.local/status", host);
  Serial.println("=================================");
  Serial.println("Setting up OTA... Done!");
}

void loopOta() {
  httpServer.handleClient();
}

#endif // OTA_H