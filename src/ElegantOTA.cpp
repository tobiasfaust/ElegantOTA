#include "ElegantOTA.h"

ElegantOTAClass::ElegantOTAClass(){}

void ElegantOTAClass::begin(ELEGANTOTA_WEBSERVER *server, const char * username, const char * password){
  _server = server;

  #ifdef ESP8266
    LittleFS.begin();
  #elif ESP32
    LittleFS.begin(true); // true: format LittleFS/NVS if mount fails
  #endif

  setAuth(username, password);

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
  } else if (variantString == "esp8266") {
      this->ChipFamily = "ESP8266";
  } else {
      this->ChipFamily = "ESP32";
  }

  #if defined(TARGET_RP2040)
    if (!__isPicoW) {
      ELEGANTOTA_DEBUG_MSG("RP2040: Not a Pico W, skipping OTA setup\n");
      return;
    }
  #endif

 //#ifdef CORS_DEBUG
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
 //#endif
 
  _server->serveStatic("/ota/getfile", LittleFS, "/", "max-age=3600");

  _server->on("/ota/uploadfile", HTTP_POST, [](AsyncWebServerRequest *request) {},
                                    std::bind(&ElegantOTAClass::handleUpload, this, std::placeholders::_1, 
                                        std::placeholders::_2,
                                        std::placeholders::_3,
                                        std::placeholders::_4,
                                        std::placeholders::_5,
                                        std::placeholders::_6));


  #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
    _server->on("/update", HTTP_GET, [&](AsyncWebServerRequest *request){
      if(_authenticate && !request->authenticate(_username.c_str(), _password.c_str())){
        return request->requestAuthentication();
      }
      #if defined(ASYNCWEBSERVER_VERSION) && ASYNCWEBSERVER_VERSION_MAJOR > 2  // This means we are using recommended fork of AsyncWebServer
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", ELEGANT_HTML, ELEGANT_HTML_len);
      #else
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", ELEGANT_HTML, ELEGANT_HTML_len);
      #endif
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
    });
  #else
    _server->on("/update", HTTP_GET, [&](){
      if (_authenticate && !_server->authenticate(_username.c_str(), _password.c_str())) {
        return _server->requestAuthentication();
      }
      _server->sendHeader("Content-Encoding", "gzip");
      _server->send_P(200, "text/html", (const char*)ELEGANT_HTML, ELEGANT_HTML_len);
    });
  #endif

  #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
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
      serializeJson(doc, ret);

      response->print(ret);
      request->send(response);
    });
  #else
    _server->on("/getdeviceinfo", HTTP_GET, [&](){
      if (_authenticate && !_server->authenticate(_username.c_str(), _password.c_str())) {
        return _server->requestAuthentication();
      }
      _server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
      _server->sendHeader("Pragma", "no-cache");
      _server->sendHeader("Expires", "-1");

      JsonDocument doc;
      this->getDeviceInfo(doc);
      String ret("");
      serializeJson(doc, ret);

      _server->send(200, "application/json", ret);
  #endif

  #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
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
          ELEGANTOTA_DEBUG_MSG("OTA Mode: Filesystem\n");
          mode = OTA_MODE_FILESYSTEM;
        } else {
          ELEGANTOTA_DEBUG_MSG("OTA Mode: Firmware\n");
          mode = OTA_MODE_FIRMWARE;
        }
      }

      // Get file MD5 hash from arg
      if (request->hasParam("hash")) {
        String hash = request->getParam("hash")->value();
        ELEGANTOTA_DEBUG_MSG(String("MD5: "+hash+"\n").c_str());
        if (!Update.setMD5(hash.c_str())) {
          ELEGANTOTA_DEBUG_MSG("ERROR: MD5 hash not valid\n");
          return request->send(400, "text/plain", "MD5 parameter invalid");
        }
      }

      #if UPDATE_DEBUG == 1
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
          ELEGANTOTA_DEBUG_MSG("Failed to start update process\n");
          // Save error to string
          StreamString str;
          Update.printError(str);
          _update_error_str = str.c_str();
          _update_error_str.concat("\n");
          ELEGANTOTA_DEBUG_MSG(_update_error_str.c_str());
        }
      #elif defined(ESP32)  
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, mode == OTA_MODE_FILESYSTEM ? U_SPIFFS : U_FLASH)) {
          ELEGANTOTA_DEBUG_MSG("Failed to start update process\n");
          // Save error to string
          StreamString str;
          Update.printError(str);
          _update_error_str = str.c_str();
          _update_error_str.concat("\n");
          ELEGANTOTA_DEBUG_MSG(_update_error_str.c_str());
        }        
      #endif

      return request->send((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
    });
  #else
    _server->on("/ota/start", HTTP_GET, [&]() {
      if (_authenticate && !_server->authenticate(_username.c_str(), _password.c_str())) {
        return _server->requestAuthentication();
      }

      // Get header x-ota-mode value, if present
      OTA_Mode mode = OTA_MODE_FIRMWARE;
      // Get mode from arg
      if (_server->hasArg("mode")) {
        String argValue = _server->arg("mode");
        if (argValue == "fs") {
          ELEGANTOTA_DEBUG_MSG("OTA Mode: Filesystem\n");
          mode = OTA_MODE_FILESYSTEM;
        } else {
          ELEGANTOTA_DEBUG_MSG("OTA Mode: Firmware\n");
          mode = OTA_MODE_FIRMWARE;
        }
      }

      // Get file MD5 hash from arg
      if (_server->hasArg("hash")) {
        String hash = _server->arg("hash");
        ELEGANTOTA_DEBUG_MSG(String("MD5: "+hash+"\n").c_str());
        if (!Update.setMD5(hash.c_str())) {
          ELEGANTOTA_DEBUG_MSG("ERROR: MD5 hash not valid\n");
          return _server->send(400, "text/plain", "MD5 parameter invalid");
        }
      }

      #if UPDATE_DEBUG == 1
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
          ELEGANTOTA_DEBUG_MSG("Failed to start update process\n");
          // Save error to string
          StreamString str;
          Update.printError(str);
          _update_error_str = str.c_str();
          _update_error_str.concat("\n");
          ELEGANTOTA_DEBUG_MSG(_update_error_str.c_str());
        }
      #elif defined(ESP32)  
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, mode == OTA_MODE_FILESYSTEM ? U_SPIFFS : U_FLASH)) {
          ELEGANTOTA_DEBUG_MSG("Failed to start update process\n");
          // Save error to string
          StreamString str;
          Update.printError(str);
          _update_error_str = str.c_str();
          _update_error_str.concat("\n");
          ELEGANTOTA_DEBUG_MSG(_update_error_str.c_str());
        }
      #elif defined(TARGET_RP2040)
        uint32_t update_size = 0;
        // Gather FS Size
        if (mode == OTA_MODE_FILESYSTEM) {
          update_size = ((size_t)&_FS_end - (size_t)&_FS_start);
          LittleFS.end();
        } else {
          FSInfo i;
          LittleFS.begin();
          LittleFS.info(i);
          update_size = i.totalBytes - i.usedBytes;
        }
        // Start update process
        if (!Update.begin(update_size, mode == OTA_MODE_FILESYSTEM ? U_FS : U_FLASH)) {
          ELEGANTOTA_DEBUG_MSG("Failed to start update process because there is not enough space\n");
          _update_error_str = "Not enough space";
          return _server->send(400, "text/plain", _update_error_str.c_str());
        }
      #endif

      return _server->send((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
    });
  #endif

  #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
    _server->on("/ota/upload", HTTP_POST, [&](AsyncWebServerRequest *request) {
        if(_authenticate && !request->authenticate(_username.c_str(), _password.c_str())){
          return request->requestAuthentication();
        }
        // Post-OTA update callback
        if (postUpdateCallback != NULL) postUpdateCallback(!Update.hasError());
        AsyncWebServerResponse *response = request->beginResponse((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
        response->addHeader("Connection", "close");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
        // Set reboot flag
        if (!Update.hasError()) {
          if (_auto_reboot) {
            _reboot_request_millis = millis();
            _reboot = true;
          }
        }
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
            if (!Update.end(true)) { //true to set the size to the current progress
                // Save error to string
                StreamString str;
                Update.printError(str);
                _update_error_str = str.c_str();
                _update_error_str.concat("\n");
                ELEGANTOTA_DEBUG_MSG(_update_error_str.c_str());
            }
        }else{
            return;
        }
    });
  #else
    _server->on("/ota/upload", HTTP_POST, [&](){
      if (_authenticate && !_server->authenticate(_username.c_str(), _password.c_str())) {
        return _server->requestAuthentication();
      }
      // Post-OTA update callback
      if (postUpdateCallback != NULL) postUpdateCallback(!Update.hasError());
      _server->sendHeader("Connection", "close");
      _server->send((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
      // Set reboot flag
      if (!Update.hasError()) {
        if (_auto_reboot) {
          _reboot_request_millis = millis();
          _reboot = true;
        }
      }
    }, [&](){
      // Actual OTA Download
      HTTPUpload& upload = _server->upload();
      if (upload.status == UPLOAD_FILE_START) {
        // Check authentication
        if (_authenticate && !_server->authenticate(_username.c_str(), _password.c_str())) {
          ELEGANTOTA_DEBUG_MSG("Authentication Failed on UPLOAD_FILE_START\n");
          return;
        }
        Serial.printf("Update Received: %s\n", upload.filename.c_str());
        _current_progress_size = 0;
      } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            #if UPDATE_DEBUG == 1
              Update.printError(Serial);
            #endif
          }

          _current_progress_size += upload.currentSize;
          // Progress update callback
          if (progressUpdateCallback != NULL) progressUpdateCallback(_current_progress_size, upload.totalSize);
      } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) {
              ELEGANTOTA_DEBUG_MSG(String("Update Success: "+String(upload.totalSize)+"\n").c_str());
          } else {
              ELEGANTOTA_DEBUG_MSG("[!] Update Failed\n");
              // Store error to string
              StreamString str;
              Update.printError(str);
              _update_error_str = str.c_str();
              _update_error_str.concat("\n");
              ELEGANTOTA_DEBUG_MSG(_update_error_str.c_str());
          }

          #if UPDATE_DEBUG == 1
            Serial.setDebugOutput(false);
          #endif
      } else {
        ELEGANTOTA_DEBUG_MSG(String("Update Failed Unexpectedly (likely broken connection): status="+String(upload.status)+"\n").c_str());
      }
    });
  #endif
}

void ElegantOTAClass::setFWVersion(String version) {
  this->FWVersion = version;
}

void ElegantOTAClass::setID(String id) {
  this->id = id;
}

void ElegantOTAClass::setGitEnv(String owner, String repo, String branch) {
  this->gitOwner = owner;
  this->gitRepo = repo;
  this->gitBranch = branch;
}

void ElegantOTAClass::setBackupRestoreFS(String rootPath) {
  this->BackupRestoreFS = rootPath;
}

//###############################################################
// store a file at Filesystem
//###############################################################
void ElegantOTAClass::handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  
  Serial.printf("Client: %s %s\n", request->client()->remoteIP().toString().c_str(), request->url().c_str());;

  if (!index) {
    // open the file on first call and store the file handle in the request object
    request->_tempFile = LittleFS.open(filename, "w");

    Serial.printf("Upload Start: %s\n", filename.c_str());
  }

  if (len) {
    // stream the incoming chunk to the opened file
    request->_tempFile.write(data, len);
    Serial.printf("Writing file: %s ,index=%d len=%d bytes, FreeMem: %d\n", filename.c_str(), index, len, ESP.getFreeHeap());
  }

  if (final) {
    // close the file handle as the upload is now done
    request->_tempFile.close();
    Serial.printf("Upload Complete: %s ,size: %d Bytes\n", filename.c_str(), (index + len));
 
    AsyncResponseStream *response = request->beginResponseStream("text/json");
    response->addHeader("Server","ESP Async Web Server");

    JsonDocument jsonReturn;
    String ret;

    jsonReturn["status"] = 1;
    jsonReturn["text"] = "OK";

    serializeJson(jsonReturn, ret);
    response->print(ret);
    request->send(response);

  }
}

void ElegantOTAClass::getDeviceInfo(JsonDocument& doc) {
  JsonObject jsonRoot = doc.to<JsonObject>();
      
  jsonRoot["owner"] = this->gitOwner.c_str();
  jsonRoot["repository"] = this->gitRepo.c_str();
  jsonRoot["chipfamily"] = this->getChipFamily().c_str();
  jsonRoot["branch"] = this->gitBranch.c_str();
  jsonRoot["FWVersion"] = this->FWVersion.c_str();
  jsonRoot["HwId"] = this->id.c_str();

  if (this->BackupRestoreFS.length() > 0) {
    JsonArray content = jsonRoot["backup"].to<JsonArray>();
    this->getDirList(content, this->BackupRestoreFS);
  }
}

//###############################################################
// returns the complete folder structure from the given Rootpath
//###############################################################
void ElegantOTAClass::getDirList(JsonArray json, String path) {
  JsonObject jsonRoot = json.add<JsonObject>();

  jsonRoot["path"] = path;
  JsonArray content = jsonRoot["content"].to<JsonArray>();

  File FSroot = LittleFS.open(path, "r");
  File file = FSroot.openNextFile();

  while (file) {
    JsonObject fileObj = content.add<JsonObject>();
    fileObj["name"] = String(file.name());

    if(file.isDirectory()) {    
        fileObj["isDir"] = 1;
        String p = path + "/" + file.name();
        if (p.startsWith("//")) { p = p.substring(1); }
        this->getDirList(json, p); // recursive call
    } else {
      fileObj["isDir"] = 0;
    }

    file.close();
    file = FSroot.openNextFile();
  }
  FSroot.close();
}


void ElegantOTAClass::setAuth(const char * username, const char * password){
  _username = username;
  _password = password;
  _authenticate = _username.length() && _password.length();
}

void ElegantOTAClass::clearAuth(){
  _authenticate = false;
}

void ElegantOTAClass::setAutoReboot(bool enable){
  _auto_reboot = enable;
}

void ElegantOTAClass::loop() {
  // Check if 2 seconds have passed since _reboot_request_millis was set
  if (_reboot && millis() - _reboot_request_millis > 2000) {
    ELEGANTOTA_DEBUG_MSG("Rebooting...\n");
    #if defined(ESP8266) || defined(ESP32)
      ESP.restart();
    #elif defined(TARGET_RP2040)
      rp2040.reboot();
    #endif
    _reboot = false;
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
