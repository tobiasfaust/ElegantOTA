/**
_____ _                        _    ___ _____  _    
| ____| | ___  __ _  __ _ _ __ | |_ / _ \_   _|/ \   
|  _| | |/ _ \/ _` |/ _` | '_ \| __| | | || | / _ \  
| |___| |  __/ (_| | (_| | | | | |_| |_| || |/ ___ \ 
|_____|_|\___|\__, |\__,_|_| |_|\__|\___/ |_/_/   \_\
              |___/                                  
*/

/**
 * 
 * @name ElegantOTA
 * @author Ayush Sharma (ayush@softt.io)
 * @brief 
 * @version 3.0.0
 * @date 2023-08-30
 */

#ifndef ElegantOTA_h
#define ElegantOTA_h

#include "Arduino.h"
#include "stdlib_noniso.h"
#include "ArduinoJson.h"
#include <vector>
#include "LittleFS.h"
#include "elop.h"

#ifndef CORS_DEBUG
  #define CORS_DEBUG 0
#endif

#ifndef DEBUGMODE
  #define DEBUGMODE 0
#endif

#if defined(ESP8266)
  #include <functional>
  #include "FS.h"
  #include "LittleFS.h"
  #include "Updater.h"
  #include "StreamString.h"
  #include "ESPAsyncTCP.h"
  #include "ESPAsyncWebServer.h"
  #define ELEGANTOTA_WEBSERVER AsyncWebServer
#elif defined(ESP32)
  #include <functional>
  #include "FS.h"
  #include "Update.h"
  #include "StreamString.h"
  #include "AsyncTCP.h"
  #include "ESPAsyncWebServer.h"
  #define ELEGANTOTA_WEBSERVER AsyncWebServer
#endif

enum OTA_Mode {
    OTA_MODE_FIRMWARE = 0,
    OTA_MODE_FILESYSTEM = 1
};

class ElegantOTAClass{
  public:
    ElegantOTAClass();

    void begin(ELEGANTOTA_WEBSERVER *server, const char * username = "", const char * password = "");

    void setAuth(const char * username, const char * password);
    void clearAuth();
    void setAutoReboot(bool enable);
    void loop();

    void onStart(std::function<void()> callable);
    void onProgress(std::function<void(size_t current, size_t final)> callable);
    void onEnd(std::function<void(bool success)> callable);
    
    /**
     * @brief set some git environemnts, neseccary for selecting right versions file for OAT
     * 
     * @param owner set the git Ownername of Repository
     * @param repo set the git Repository name
     * @param branch set the current git branch name
     * @param FWVersion set the current Firmware version
     */
    void setGitEnv(String owner, String repo, String branch);
    void setFWVersion(String version);
    void setID(String id);
    void setBackupRestoreFS(String rootPath);

  private:
    ELEGANTOTA_WEBSERVER *_server;

    bool      _authenticate;
    String    _username;
    String    _password;
    String    ChipFamily;
    String    gitOwner;
    String    gitRepo;
    String    gitBranch;
    String    FWVersion;
    String    id;
    String    BackupRestoreFS;
    bool      _isRestoreInProgress = false;
    std::vector<String> _restoreFiles;
    uint16_t  _restoreFileIndex = 0;
  

    bool _auto_reboot = true;
    bool _reboot = false;
    unsigned long _reboot_request_millis = 0;

    String _update_error_str = "";
    unsigned long _current_progress_size;

    std::function<void()> preUpdateCallback = NULL;
    std::function<void(size_t current, size_t final)> progressUpdateCallback = NULL;
    std::function<void(bool success)> postUpdateCallback = NULL;

    /*
    * @brief get the device info as json document
    * @param doc the json document to store the device info
    */
    void getDeviceInfo(JsonDocument& doc);

    /**
     * @brief get the chip family of the current device
     * @return the chip family
     */
    const String& getChipFamily() {return ChipFamily;}
    
    /**
     * @brief get the complete folder structure from the given Rootpath
     * @param json the json object to store the folder structure
     * @param path the root path to start the search
     */
    void getDirList(JsonArray json, String path);

    /**
    * @brief create LittleFS folders recursively
    * @param path the full path to create
    */
    void mkdir(String path);

    //###############################################################
    // store a file at Filesystem
    //###############################################################
    /**
     * @brief store a file at Filesystem
     * @param request the AsyncWebserver request object
     * @param filename the filename to store
     * @param index the current index chunk of the file
     * @param data the data to store
     * @param len the current chunk length of the data
     * @param final the final flag
     */
    void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);

    /**
    * @brief Wrapper function for logging like Serial.printf
    * @param format the format string
    * @param ... the arguments
    */
    void logf(const char* format, ...);

    
};

extern ElegantOTAClass ElegantOTA;

#endif
