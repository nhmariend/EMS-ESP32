#include <UploadFileService.h>

using namespace std::placeholders; // for `_1` etc

static bool is_firmware = false;

UploadFileService::UploadFileService(AsyncWebServer * server, SecurityManager * securityManager)
    : _securityManager(securityManager) {
    server->on(UPLOAD_FILE_PATH,
               HTTP_POST,
               std::bind(&UploadFileService::uploadComplete, this, _1),
               std::bind(&UploadFileService::handleUpload, this, _1, _2, _3, _4, _5, _6));
}

void UploadFileService::handleUpload(AsyncWebServerRequest * request, const String & filename, size_t index, uint8_t * data, size_t len, bool final) {
    // quit if not authorized
    Authentication authentication = _securityManager->authenticateRequest(request);
    if (!AuthenticationPredicates::IS_ADMIN(authentication)) {
        handleError(request, 403); // send the forbidden response
        return;
    }

    // at init
    if (!index) {
        // check details of the file, to see if its a valid bin or json file
        std::string fname(filename.c_str());
        auto        position  = fname.find_last_of(".");
        std::string extension = fname.substr(position + 1);
        size_t      fsize     = request->contentLength();

#if defined(EMSESP_USE_SERIAL)
        Serial.printf("Received filename: %s, len: %d, index: %d, ext: %s, fsize: %d", filename.c_str(), len, index, extension.c_str(), fsize);
        Serial.println();
#endif

        if ((extension == "bin") && (fsize > 1500000)) {
            is_firmware = true;
        } else if (extension == "json") {
            is_firmware = false;
        } else {
            is_firmware = false;
            return; // not support file type
        }

        if (is_firmware) {
            // it's firmware - initialize the ArduinoOTA updater
            if (Update.begin(fsize)) {
                request->onDisconnect(UploadFileService::handleEarlyDisconnect); // success, let's make sure we end the update if the client hangs up
            } else {
#if defined(EMSESP_USE_SERIAL)
                Update.printError(Serial);
#endif
                handleError(request, 500); // failed to begin, send an error response
            }
        } else {
            // its a normal file, open a new temp file to write the contents too
            request->_tempFile = LittleFS.open(TEMP_FILENAME_PATH, "w");
        }
    }

    if (!is_firmware) {
        if (len) {
            request->_tempFile.write(data, len); // stream the incoming chunk to the opened file
        }

    } else {
        // if we haven't delt with an error, continue with the firmware update
        if (!request->_tempObject) {
            if (Update.write(data, len) != len) {
#if defined(EMSESP_USE_SERIAL)
                Update.printError(Serial);
#endif
                handleError(request, 500);
            }
            if (final) {
                if (!Update.end(true)) {
#if defined(EMSESP_USE_SERIAL)
                    Update.printError(Serial);
#endif
                    handleError(request, 500);
                }
            }
        }
    }
}

void UploadFileService::uploadComplete(AsyncWebServerRequest * request) {
    // did we complete uploading a json file?
    if (request->_tempFile) {
        request->_tempFile.close(); // close the file handle as the upload is now done
        request->onDisconnect(RestartService::restartNow);
        AsyncWebServerResponse * response = request->beginResponse(200);
        request->send(response);
        return;
    }

    // check if it was a firmware upgrade
    // if no error, send the success response
    if (is_firmware && !request->_tempObject) {
        request->onDisconnect(RestartService::restartNow);
        AsyncWebServerResponse * response = request->beginResponse(200);
        request->send(response);
        return;
    }

    handleError(request, 403); // send the forbidden response
}

void UploadFileService::handleError(AsyncWebServerRequest * request, int code) {
    // if we have had an error already, do nothing
    if (request->_tempObject) {
        return;
    }

    // send the error code to the client and record the error code in the temp object
    request->_tempObject              = new int(code);
    AsyncWebServerResponse * response = request->beginResponse(code);
    request->send(response);
}

void UploadFileService::handleEarlyDisconnect() {
    is_firmware = false;
    Update.abort();
}
