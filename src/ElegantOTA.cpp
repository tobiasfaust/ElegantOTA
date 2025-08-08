#include "ElegantOTA.h"

ElegantOTAClass::ElegantOTAClass(){}

void ElegantOTAClass::begin(ELEGANTOTA_WEBSERVER *server, const char * username, const char * password) {
  _server = server;
  this->setAuth(username, password);

  // determine chip family
  #ifdef ESP32
    String variantString = ARDUINO_VARIANT;
  #else
    String variantString = "esp8266";
  #endif

  if (variantString == "esp32s3") {
      this->ChipFamily = "ESP32-S3";
  } else if (variantString == "esp32c3") {
      this->ChipFamily = "ESP32-C3";
  } else if (variantString == "esp32s2") {
      this->ChipFamily = "ESP32-S2";
  } else if (variantString == "esp32c6") {
      this->ChipFamily = "ESP32-C6";
  } else if (variantString == "esp32h2") {
      this->ChipFamily = "ESP32-H2";
  } else if (variantString == "esp8266") {
      this->ChipFamily = "ESP8266";
  } else {
      this->ChipFamily = "ESP32";
  }

 #ifdef CORS_DEBUG
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
 #endif
 
  _server->on("/update", HTTP_GET, [&](AsyncWebServerRequest *request){
      if(_authenticate && !request->authenticate(_username.c_str(), _password.c_str())){
        return request->requestAuthentication();
      }
      #if defined(ASYNCWEBSERVER_VERSION) && ASYNCWEBSERVER_VERSION_MAJOR > 2  // This means we are using recommended fork of AsyncWebServer
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", include_ElegantOTA_html_gz, include_ElegantOTA_html_gz_len);
      #else
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", include_ElegantOTA_html_gz, include_ElegantOTA_html_gz_len);
      #endif
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
  });
  
  _server->on("/getdeviceinfo", HTTP_GET, [&](AsyncWebServerRequest *request){
      if(_authenticate && !request->authenticate(_username.c_str(), _password.c_str())){
        return request->requestAuthentication();
      }
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
      response->addHeader("Pragma", "no-cache");
      response->addHeader("Expires", "-1");

      JsonDocument doc;
      this->getDeviceInfo(doc);
      String ret("");
      ArduinoJson::serializeJson(doc, ret);

      response->print(ret);
      request->send(response);

  });
  
  _server->on("/ota/start", HTTP_GET, [&](AsyncWebServerRequest *request) {
      if (_authenticate && !request->authenticate(_username.c_str(), _password.c_str())) {
        return request->requestAuthentication();
      }

      // Get header x-ota-mode value, if present
      OTA_Mode mode = OTA_MODE_FIRMWARE;
      // Get mode from arg
      if (request->hasParam("mode")) {
        String argValue = request->getParam("mode")->value();
        if (argValue == "fs") {
          this->logf("OTA Mode: Filesystem");
          mode = OTA_MODE_FILESYSTEM;
        } else {
          this->logf("OTA Mode: Firmware");
          mode = OTA_MODE_FIRMWARE;
        }
        this->_currentOtaMode = mode;
      }

      // Get file MD5 hash from arg
      if (request->hasParam("hash")) {
        String hash = request->getParam("hash")->value();
        if (!Update.setMD5(hash.c_str())) {
          this->logf("ERROR: MD5 hash not valid: %s", hash.c_str());
          return request->send(400, "text/plain", "MD5 parameter invalid");
        }
      }

      #if DEBUGMODE >= 1
        // Serial output must be active to see the callback serial prints
        Serial.setDebugOutput(true);
      #endif

      // Pre-OTA update callback
      if (preUpdateCallback != NULL) preUpdateCallback();

      // Start update process
      #if defined(ESP8266)
        uint32_t update_size = mode == OTA_MODE_FILESYSTEM ? ((size_t)FS_end - (size_t)FS_start) : ((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
        if (mode == OTA_MODE_FILESYSTEM) {
          close_all_fs();
        }
        Update.runAsync(true);
        if (!Update.begin(update_size, mode == OTA_MODE_FILESYSTEM ? U_FS : U_FLASH)) {
          this->logf("Failed to start update process");
          // Save error to string
          StreamString str;
          Update.printError(str);
          _update_error_str = str.c_str();
          _update_error_str.concat("\n");
          this->logf(_update_error_str.c_str());
        }
      #elif defined(ESP32)  
        
        //if (!Update.begin(UPDATE_SIZE_UNKNOWN, mode == OTA_MODE_FILESYSTEM ? U_SPIFFS : U_FLASH)) {
        if (this->FsPartitionLabel!="") {
          this->logf("Starting update on partition: %s", this->FsPartitionLabel.c_str());
        }

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, (mode == OTA_MODE_FILESYSTEM ? U_SPIFFS : U_FLASH), -1, LOW, (this->FsPartitionLabel=="" ? NULL : this->FsPartitionLabel.c_str()))) {
          this->logf("Failed to start update process");
          // Save error to string
          StreamString str;
          Update.printError(str);
          _update_error_str = str.c_str();
          _update_error_str.concat("\n");
          this->logf(_update_error_str.c_str());
        }        
      #endif

      return request->send((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
  });

  _server->on("/ota/upload", HTTP_POST, [&](AsyncWebServerRequest *request) {
        if(_authenticate && !request->authenticate(_username.c_str(), _password.c_str())){
          return request->requestAuthentication();
        }
        
        if (Update.hasError()) {
          if (postUpdateCallback != NULL) postUpdateCallback(!Update.hasError());
        }

        AsyncWebServerResponse *response = request->beginResponse((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
        response->addHeader("Connection", "close");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);

  }, [&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        //Upload handler chunks in data
        if(_authenticate){
            if(!request->authenticate(_username.c_str(), _password.c_str())){
                return request->requestAuthentication();
            }
        }

        if (!index) {
          // Reset progress size on first frame
          _current_progress_size = 0;
        }

        // Write chunked data to the free sketch space
        if(len){
            if (Update.write(data, len) != len) {
                return request->send(400, "text/plain", "Failed to write chunked data to free space");
            }
            _current_progress_size += len;
            // Progress update callback
            if (progressUpdateCallback != NULL) progressUpdateCallback(_current_progress_size, request->contentLength());
        }
            
        if (final) { // if the final flag is set then this is the last frame of data
            if (postUpdateCallback != NULL) postUpdateCallback(!Update.hasError());
            if (!Update.end(true)) { //true to set the size to the current progress
                this->logf("Error Occurred. Error #%s: ", String(Update.getError()));
                // Save error to string
                StreamString str;
                Update.printError(str);
                _update_error_str = str.c_str();
                _update_error_str.concat("\n");
                this->logf(_update_error_str.c_str());
            } else {
              this->logf("Update of %s complete", filename.c_str());
              // Set reboot flag now, no Restore needed
              if (_auto_reboot) {
                _reboot_request_millis = millis();
                 _reboot = true;
              }
            }

        } else {
            return;
        }
  });
}
void ElegantOTAClass::setFWVariant(String variant) {
  this->FWVariant = variant;
}

void ElegantOTAClass::setFWVersion(String version) {
  this->FWVersion = version;
}

void ElegantOTAClass::setID(String id) {
  this->id = id;
}

void ElegantOTAClass::setGitEnv(String owner, String repo, String branch) {
  this->setGitEnv(owner, repo, branch, 0);
}

void ElegantOTAClass::setGitEnv(String owner, String repo, String branch, uint16_t build) {
  this->gitOwner = owner;
  this->gitRepo = repo;
  this->gitBranch = branch;
  this->gitBuild = build;
}

void ElegantOTAClass::setTargetPartition(String FsPartitionLabel) {
  this->FsPartitionLabel = FsPartitionLabel;
}

void ElegantOTAClass::logf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buffer[256];
  vsnprintf(buffer, sizeof(buffer), format, args);
  Serial.print("[ElegantOTA] ");
  Serial.println(buffer);
  va_end(args);
}

void ElegantOTAClass::getDeviceInfo(JsonDocument& doc) {
  JsonObject jsonRoot = doc.to<JsonObject>();
      
  jsonRoot["owner"] = this->gitOwner.c_str();
  jsonRoot["repository"] = this->gitRepo.c_str();
  jsonRoot["chipfamily"] = this->getChipFamily().c_str();
  jsonRoot["branch"] = this->gitBranch.c_str();
  jsonRoot["build"] = this->gitBuild;
  jsonRoot["FWVersion"] = this->FWVersion.c_str();
  jsonRoot["HwId"] = this->id.c_str();
  jsonRoot["FWVariant"] = this->FWVariant.c_str();

}

void ElegantOTAClass::setAuth(const char * username, const char * password){
  this->_username = username;
  this->_password = password;
  this->_authenticate = _username.length() && _password.length();
}

void ElegantOTAClass::clearAuth(){
  this->_authenticate = false;
}

void ElegantOTAClass::setAutoReboot(bool enable){
  this->_auto_reboot = enable;
}

void ElegantOTAClass::loop() {
  // Check if 2 seconds have passed since _reboot_request_millis was set
  if (this->_reboot && millis() - this->_reboot_request_millis > 2000) {
    this->logf("Rebooting...");
    #if defined(ESP8266) || defined(ESP32)
      ESP.restart();
    #elif defined(TARGET_RP2040)
      rp2040.reboot();
    #endif
    this->_reboot = false;
  }
}

void ElegantOTAClass::onStart(std::function<void()> callable){
    preUpdateCallback = callable;
}

void ElegantOTAClass::onProgress(std::function<void(size_t current, size_t final)> callable){
    progressUpdateCallback= callable;
}

void ElegantOTAClass::onEnd(std::function<void(bool success)> callable){
    postUpdateCallback = callable;
}

ElegantOTAClass ElegantOTA;
