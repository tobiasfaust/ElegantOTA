#include "ElegantOTA.h"

ElegantOTAClass::ElegantOTAClass(){}

void ElegantOTAClass::begin(ELEGANTOTA_WEBSERVER *server, const char * username, const char * password) {
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
 
  _server->serveStatic("/ota/getfile", LittleFS, "/", "max-age=3600");

  _server->on("/ota/uploadfile", HTTP_POST, [](AsyncWebServerRequest *request) {},
                                    std::bind(&ElegantOTAClass::handleUpload, this, std::placeholders::_1, 
                                        std::placeholders::_2,
                                        std::placeholders::_3,
                                        std::placeholders::_4,
                                        std::placeholders::_5,
                                        std::placeholders::_6));


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
        this->logf(String("MD5: "+hash+"").c_str());
        if (!Update.setMD5(hash.c_str())) {
          this->logf("ERROR: MD5 hash not valid");
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
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, mode == OTA_MODE_FILESYSTEM ? U_SPIFFS : U_FLASH)) {
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

        if (this->_restoreFiles.size() > 0) {
          // Restore files from FS
          this->_restoreFileIndex = 0;
          _isRestoreInProgress = true;
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
              if (this->_currentOtaMode == OTA_MODE_FIRMWARE ||  this->_restoreFiles.size() == 0 ) { 
                  this->logf("No files to restore");
                  // Set reboot flag now, no Restore needed
                  if (_auto_reboot) {
                    _reboot_request_millis = millis();
                    _reboot = true;
                  }
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

void ElegantOTAClass::setBackupRestoreFS(String rootPath) {
  this->BackupRestoreFS = rootPath;
}

void ElegantOTAClass::mkdir(String path) {
  std::vector<String> dirs;
  size_t pos = 0;
  String token;
  while ((pos = path.indexOf('/')) != -1) {
    token = path.substring(0, pos);
    if (token.length() > 0) {
      dirs.push_back(token);
    }
    path.remove(0, pos + 1);
  }
  if (path.length() > 0) {
    dirs.push_back(path);
  }

  String currentPath = "";
  for (size_t i = 0; i < dirs.size(); ++i) {
    currentPath += "/" + dirs[i];
    if (!LittleFS.exists(currentPath)) {
      LittleFS.mkdir(currentPath);
    }
  }
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

void ElegantOTAClass::handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if(_authenticate && !request->authenticate(_username.c_str(), _password.c_str())){
        return request->requestAuthentication();
  }
  
  this->logf("Client: %s %s", request->client()->remoteIP().toString().c_str(), request->url().c_str());;

  if (!index) {
    // open the file on first call and store the file handle in the request object
    
    // Extract path from filename, create the path
    String path = filename.substring(0, filename.lastIndexOf('/'));
    if (path.length() > 0 && path != "/" && !LittleFS.exists(path)) {
      this->mkdir(path);
    }

    request->_tempFile = LittleFS.open(filename, "w");

    this->logf("Upload Start: %s", filename.c_str());
  }

  if (len) {
    // stream the incoming chunk to the opened file
    request->_tempFile.write(data, len);
    this->logf("Writing file: %s ,index=%d len=%d bytes, FreeMem: %d", filename.c_str(), index, len, ESP.getFreeHeap());
  }

  if (final) {
    // close the file handle as the upload is now done
    request->_tempFile.close();
    this->logf("Upload Complete: %s ,size: %d Bytes", filename.c_str(), (index + len));
 
    if (this->_isRestoreInProgress) {
      this->logf("Restore file complete: %s (index: %d)", _restoreFiles[this->_restoreFileIndex].c_str(), this->_restoreFileIndex);
      this->_restoreFileIndex++;
    }

    if (this->_restoreFileIndex >= _restoreFiles.size()) {
      // Restore finished
      this->_isRestoreInProgress = false;
      this->_restoreFileIndex = 0;
      this->logf("Restore finished, initiate reboot");
      if (_auto_reboot) {
        _reboot_request_millis = millis();
        _reboot = true;
      }
    }
    
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
  jsonRoot["build"] = this->gitBuild;
  jsonRoot["FWVersion"] = this->FWVersion.c_str();
  jsonRoot["HwId"] = this->id.c_str();
  jsonRoot["FWVariant"] = this->FWVariant.c_str();

  if (this->BackupRestoreFS.length() > 0) {
    JsonArray content = jsonRoot["backup"].to<JsonArray>();
    this->getDirList(content, this->BackupRestoreFS);
  }
}

void ElegantOTAClass::getDirList(JsonArray json, String path) {
  // clear _restoreFiles
  this->_restoreFiles.clear();

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
      this->_restoreFiles.push_back(String(file.name()));
    }

    file.close();
    file = FSroot.openNextFile();
  }
  FSroot.close();
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
