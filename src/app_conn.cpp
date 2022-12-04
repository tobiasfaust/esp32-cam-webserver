#include "app_conn.h"

CLAppConn::CLAppConn() {
    setTag("conn");
}

int CLAppConn::start() {

    Serial.println("Starting WiFi");
    WiFi.mode(WIFI_STA); 
    
    if(loadPrefs() != OK) {
        return WiFi.status();
    }

    // Disable power saving on WiFi to improve responsiveness
    // (https://github.com/espressif/arduino-esp32/issues/1484)
    WiFi.setSleep(false);
    
    byte mac[6] = {0,0,0,0,0,0};
    WiFi.macAddress(mac);
    Serial.printf("MAC address: %02X:%02X:%02X:%02X:%02X:%02X\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    int bestStation = -1;
    long bestRSSI = -1024;
    char bestSSID[65] = "";
    uint8_t bestBSSID[6];
    if (stationCount > 0) {
        // We have a list to scan
        Serial.printf("Scanning local Wifi Networks\r\n");
        int stationsFound = WiFi.scanNetworks();
        Serial.printf("%i networks found\r\n", stationsFound);
        if (stationsFound > 0) {
            for (int i = 0; i < stationsFound; ++i) {
                // Print SSID and RSSI for each network found
                String thisSSID = WiFi.SSID(i);
                int thisRSSI = WiFi.RSSI(i);
                String thisBSSID = WiFi.BSSIDstr(i);
                Serial.printf("%3i : [%s] %s (%i)", i + 1, thisBSSID.c_str(), thisSSID.c_str(), thisRSSI);
                // Scan our list of known external stations
                for (int sta = 0; sta < stationCount; sta++) {
                    if ((strcmp(stationList[sta]->ssid, thisSSID.c_str()) == 0) ||
                    (strcmp(stationList[sta]->ssid, thisBSSID.c_str()) == 0)) {
                        Serial.print("  -  Known!");
                        // Chose the strongest RSSI seen
                        if (thisRSSI > bestRSSI) {
                            bestStation = sta;
                            strncpy(bestSSID, thisSSID.c_str(), 64);
                            // Convert char bssid[] to a byte array
                            parseBytes(thisBSSID.c_str(), ':', bestBSSID, 6, 16);
                            bestRSSI = thisRSSI;
                        }
                    }
                }
                Serial.println();
            }
        }
    } else {
        // No list to scan, therefore we are an accesspoint
        accesspoint = true;
    }

    if (bestStation == -1 ) {
        if (!accesspoint) {
            Serial.println("No known networks found, entering AccessPoint fallback mode");
            accesspoint = true;
        }

    } else {
        Serial.printf("Connecting to Wifi Network %d: [%02X:%02X:%02X:%02X:%02X:%02X] %s \r\n",
                       bestStation, bestBSSID[0], bestBSSID[1], bestBSSID[2], bestBSSID[3],
                       bestBSSID[4], bestBSSID[5], bestSSID);
        // Apply static settings if necesscary
        if (stationList[bestStation]->dhcp == false) {

            if(staticIP.ip && staticIP.gateway  && staticIP.netmask) {
                Serial.println("Applying static IP settings");
                WiFi.config(*staticIP.ip, *staticIP.gateway, *staticIP.netmask, *staticIP.dns1, *staticIP.dns2);
            }
            else
                Serial.println("Static IP settings requested but not defined properly in config, falling back to dhcp");
        }

        WiFi.setHostname(mdnsName);

        // Initiate network connection request (3rd argument, channel = 0 is 'auto')
        WiFi.begin(bestSSID, stationList[bestStation]->password, 0, bestBSSID);

        // Wait to connect, or timeout
        unsigned long start = millis();
        while ((millis() - start <= WIFI_WATCHDOG) && (WiFi.status() != WL_CONNECTED)) {
            delay(500);
            Serial.print('.');
        }
        // If we have connected, inform user
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("Client connection succeeded");
            accesspoint = false;
            // Print IP details
            Serial.printf("IP address: %s\r\n",WiFi.localIP().toString());
            Serial.printf("Netmask   : %s\r\n",WiFi.subnetMask().toString());
            Serial.printf("Gateway   : %s\r\n",WiFi.gatewayIP().toString());

        } else {
            Serial.println("Client connection Failed");
            WiFi.disconnect();   // (resets the WiFi scan)
            return wifiStatus();
        }
    }

    if (accesspoint && (WiFi.status() != WL_CONNECTED)) {
        // The accesspoint has been enabled, and we have not connected to any existing networks
        WiFi.mode(WIFI_AP);
        // reset ap_status
        ap_status = WL_DISCONNECTED;

        Serial.printf("Setting up Access Point (channel=%d)\r\n", ap_channel);
        Serial.print("  SSID     : ");
        Serial.println(apName);
        Serial.print("  Password : ");
        Serial.println(apPass);

        // User has specified the AP details; apply them after a short delay
        // (https://github.com/espressif/arduino-esp32/issues/985#issuecomment-359157428)
        if(WiFi.softAPConfig(*apIP.ip, *apIP.ip, *apIP.netmask)) {
            Serial.printf("IP address: %s\r\n",WiFi.softAPIP().toString());
        }
        else {
            Serial.println("softAPConfig failed");
            ap_status = WL_CONNECT_FAILED;
            return wifiStatus();
        }

        WiFi.softAPsetHostname(mdnsName);

        if(!WiFi.softAP(apName, apPass, ap_channel)) {
            Serial.println("Access Point init failed!");
            ap_status = WL_CONNECT_FAILED;
            return wifiStatus();
        }       
        
        ap_status = WL_CONNECTED;
        Serial.println("Access Point init successfull");

        // Start the DNS captive portal if requested
        // if (ap_dhcp) {
        //     Serial.println("Starting Captive Portal");
        //     dnsServer.start(DNS_PORT, "*", *apIP.ip);
        //     captivePortal = true;
        // }
    }

    calcURLs();

    return wifiStatus();
}

void CLAppConn::calcURLs() {
    // Set the URL's

    // if host name is not defined or access point mode is activated, use local IP for url
    if(!strcmp(hostName, "")) {
        if(accesspoint)
            strcpy(hostName, WiFi.softAPIP().toString().c_str());
        else
            strcpy(hostName, WiFi.localIP().toString().c_str());
    }
    if (httpPort != 80) {
        sprintf(httpURL, "http://%s:%d/", hostName, httpPort);
        sprintf(streamURL, "ws://%s:%d/ws", hostName, httpPort);
    } else {
        sprintf(httpURL, "http://%s/", hostName);
        sprintf(streamURL, "ws://%s/ws", hostName);
    }
    

}

int CLAppConn::loadPrefs() {
    
    jparse_ctx_t jctx;
    int ret  = parsePrefs(&jctx);
    if(ret != OS_SUCCESS) {
        return ret;
    }

    ret = json_obj_get_string(&jctx, "mdns_name", mdnsName, sizeof(mdnsName));

    if(ret != OS_SUCCESS)
        Serial.println("MDNS Name is not defined!");

    if(ret == OS_SUCCESS) {
        json_obj_get_string(&jctx, "host_name", hostName, sizeof(hostName));
        json_obj_get_int(&jctx, "http_port", &httpPort);
    }

    if (ret == OS_SUCCESS && json_obj_get_array(&jctx, "stations", &stationCount) == OS_SUCCESS) {
        Serial.print("Known external SSIDs: ");
        if(stationCount>0)
            for(int i=0; i < stationCount && i < MAX_KNOWN_STATIONS; i++) {

                if(json_arr_get_object(&jctx, i) == OS_SUCCESS) {
                    Station *s = (Station*) malloc(sizeof(Station));
                    if(json_obj_get_string(&jctx, "ssid", s->ssid, sizeof(s->ssid)) == OS_SUCCESS &&
                       json_obj_get_string(&jctx, "pass", s->password, sizeof(s->password)) == OS_SUCCESS &&
                       json_obj_get_bool(&jctx, "dhcp", &s->dhcp) == OS_SUCCESS) {
                        Serial.printf("%s\r\n", s->ssid);
                        stationList[i] = s;
                    } 
                    else {
                        free(s);
                    }
                    json_arr_leave_object(&jctx);
                }
                
            }
        else
            Serial.println("None");
        json_obj_leave_array(&jctx);
    }

    char ip[16], netmask[16], gateway[16];
        
    // read static IP
    if(ret == OS_SUCCESS && json_obj_get_object(&jctx, "static_ip") == OS_SUCCESS) {
        // TODO: add reading of static address
        json_obj_leave_object(&jctx);
    }

    json_obj_get_string(&jctx, "ap_ssid", apName, sizeof(apName));
    json_obj_get_string(&jctx, "ap_pass", apPass, sizeof(apPass));
    if(json_obj_get_int(&jctx, "ap_channel", &ap_channel) != OS_SUCCESS)
        ap_channel = 1;
    if(json_obj_get_bool(&jctx, "ap_dhcp", &ap_dhcp) != OS_SUCCESS)
        ap_dhcp = true;
    
    if(ret == OS_SUCCESS && json_obj_get_object(&jctx, "ap_ip") == OS_SUCCESS) {
        if(json_obj_get_string(&jctx, "ip", ip, sizeof(ip)) == OS_SUCCESS) {
            if(!apIP.ip) apIP.ip = new IPAddress();
            if(!apIP.ip->fromString(ip))
                Serial.println("AP IP is invalid");
        }
        if(json_obj_get_string(&jctx, "netmask", netmask, sizeof(netmask))==OS_SUCCESS) {
            if(!apIP.netmask) apIP.netmask = new IPAddress();
            apIP.netmask->fromString(netmask);
        }
        json_obj_leave_object(&jctx);
    }

    // OTA
    json_obj_get_bool(&jctx, "ota_enabled", &otaEnabled);   
    json_obj_get_string(&jctx, "ota_password", otaPassword, sizeof(otaPassword)); 

    // NTP
    json_obj_get_string(&jctx, "ntp_server", ntpServer, sizeof(ntpServer)); 
    int64_t gmtOffset;
    if(json_obj_get_int64(&jctx, "gmt_offset", &gmtOffset) == OS_SUCCESS) {
        gmtOffset_sec = (long) gmtOffset;
    }
    json_obj_get_int(&jctx, "dst_offset", &daylightOffset_sec);

    bool dbg;
    if(json_obj_get_bool(&jctx, "debug_mode", &dbg) == OS_SUCCESS)
        setDebugMode(dbg);    

    // close the file
    json_parse_end(&jctx);
    return ret;
}

int CLAppConn::savePrefs() {
    // TODO: add saving of WiFi prefs
    return OS_SUCCESS;
}

void CLAppConn::enableOTA(bool enable) {
    // Set up OTA

    if(enable) {
        Serial.println("Setting up OTA");
        // Port defaults to 3232
        // ArduinoOTA.setPort(3232);
        // Hostname defaults to esp3232-[MAC]
        ArduinoOTA.setHostname(mdnsName);

        if (strlen(otaPassword) != 0) {
            ArduinoOTA.setPassword(otaPassword);
            Serial.printf("OTA Password: %s\n\r", otaPassword);
        } 
        else {
            Serial.printf("\r\nNo OTA password has been set! (insecure)\r\n\r\n");
        }
        
        ArduinoOTA
            .onStart([]() {
                String type;
                if (ArduinoOTA.getCommand() == U_FLASH)
                    type = "sketch";
                else // U_SPIFFS
                    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
                    type = "filesystem";
                Serial.println("Start updating " + type);

                // Stop the camera since OTA will crash the module if it is running.
                // the unit will need rebooting to restart it, either by OTA on success, or manually by the user
                AppCam.stop();

                // critERR = "<h1>OTA Has been started</h1><hr><p>Camera has Halted!</p>";
                // critERR += "<p>Wait for OTA to finish and reboot, or <a href=\"control?var=reboot&val=0\" title=\"Reboot Now (may interrupt OTA)\">reboot manually</a> to recover</p>";
            })
            .onEnd([]() {
                Serial.println("End");
            })
            .onProgress([](unsigned int progress, unsigned int total) {
                Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
            })
            .onError([](ota_error_t error) {
                Serial.printf("Error[%u]: ", error);
                if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
                else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
                else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
                else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
                else if (error == OTA_END_ERROR) Serial.println("End Failed");
            });

        ArduinoOTA.begin();
        Serial.println("OTA is enabled");
        otaEnabled = true;
    }
    else {
        ArduinoOTA.end();
        Serial.println("OTA is disabled");
        otaEnabled = false;
    }

}

void CLAppConn::configMDNS() {

    if(!otaEnabled) {
        if (!MDNS.begin(mdnsName)) {
          Serial.println("Error setting up MDNS responder!");
        }
        Serial.println("mDNS responder started");
    }
    //MDNS Config -- note that if OTA is NOT enabled this needs prior steps!
    MDNS.addService("http", "tcp", httpPort);
    Serial.println("Added HTTP service to MDNS server");

}

void CLAppConn::configNTP() {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

void CLAppConn::printLocalTime(bool extraData) {
    Serial.println(getLocalTimeStr());
    if (extraData) {
        Serial.printf("NTP Server: %s, GMT Offset: %li(s), DST Offset: %i(s)\r\n", 
                      ntpServer, gmtOffset_sec , daylightOffset_sec);
    }
}

char * CLAppConn::getLocalTimeStr() {
    struct tm timeinfo;
    static char timeStringBuff[50];
    if(getLocalTime(&timeinfo)) {
        strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S, %A, %B %d %Y", &timeinfo);
    }
    
    return timeStringBuff;
}

char * CLAppConn::getUpTimeStr() {
    int64_t sec = esp_timer_get_time() / 1000000;
    int64_t upDays = int64_t(floor(sec/86400));
    int upHours = int64_t(floor(sec/3600)) % 24;
    int upMin = int64_t(floor(sec/60)) % 60;
    int upSec = sec % 60;
    static char timeStringBuff[50];
    sprintf(timeStringBuff,"%" PRId64 ":%02i:%02i:%02i (d:h:m:s)", upDays, upHours, upMin, upSec);
    return timeStringBuff;
}

CLAppConn AppConn;