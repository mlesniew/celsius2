#include <mutex>
#include <map>

#include <Arduino.h>

#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <uri/UriRegex.h>

#include <ArduinoJson.h>
#include <PicoMQTT.h>

String hostname = "kelvin";

WebServer server;
PicoMQTT::Client mqtt("calor.local");

bool scanning = false;
std::mutex mutex;

struct Reading {
    unsigned long timestamp;
    float temperature;
    float humidity;
    unsigned int battery;
};

std::map<BLEAddress, std::string> discovered;
std::map<BLEAddress, std::string> subscribed;
std::map<BLEAddress, Reading> readings;

void report_device(BLEAdvertisedDevice & device) {

    static const char ADDRESS_PREFIX[] = {0xa4, 0xc1, 0x38};
    static const BLEUUID THERMOMETHER_UUID{(uint16_t) 0x181a};

    auto address = device.getAddress();
    if (memcmp(address.getNative(), ADDRESS_PREFIX, 3) != 0) {
        return;
    }

    {
        // add device to discovered device list
        std::string & stored_name = discovered[address];

        // store name if it was captured
        const auto name = device.getName();
        if (name.size()) {
            stored_name = name;
        }
    }

    {
        // see if we're subscribed to the address
        auto it = subscribed.find(address);
        if (it == subscribed.end()) {
            return;
        }
    }

    Reading & reading = readings[address];

    for (int i = 0; i < device.getServiceDataUUIDCount(); ++i) {
        if (!device.getServiceDataUUID(i).equals(THERMOMETHER_UUID)) {
            continue;
        }

        const auto raw_data = device.getServiceData();

        struct {
            // uint8_t     size;   // = 18
            // uint8_t     uid;    // = 0x16, 16-bit UUID
            // uint16_t    UUID;   // = 0x181A, GATT Service 0x181A Environmental Sensing
            uint8_t     MAC[6]; // [0] - lo, .. [6] - hi digits
            int16_t     temperature;    // x 0.01 degree
            uint16_t    humidity;       // x 0.01 %
            uint16_t    battery_mv;     // mV
            uint8_t     battery_level;  // 0..100 %
            uint8_t     counter;        // measurement count
            uint8_t     flags;  // GPIO_TRG pin (marking "reset" on circuit board) flags:
            // bit0: Reed Switch, input
            // bit1: GPIO_TRG pin output value (pull Up/Down)
            // bit2: Output GPIO_TRG pin is controlled according to the set parameters
            // bit3: Temperature trigger event
            // bit4: Humidity trigger event
        } __attribute__((packed)) data;

        if (sizeof(data) != raw_data.size()) {
            continue;
        }

        memcpy(&data, raw_data.c_str(), sizeof(data));

        reading.temperature = 0.01 * (float) data.temperature;
        reading.humidity = 0.01 * (float) data.humidity;
        reading.battery = data.battery_level;
        reading.timestamp = millis();
    }
}

void scan_complete(BLEScanResults results) {
    std::lock_guard<std::mutex> guard(mutex);
    for (int i = 0; i < results.getCount(); ++i) {
        auto device = results.getDevice(i);
        report_device(device);
    }
    scanning = false;
}

void setup() {
    Serial.begin(115200);

    WiFi.hostname(hostname);
    WiFi.setAutoReconnect(true);

    if (false) {
        // use smart config
        Serial.println("Beginning smart config...");
        WiFi.beginSmartConfig();

        while (!WiFi.smartConfigDone()) {
            delay(500);
            Serial.print(".");
        }
        Serial.println("Smart config complete");
    } else {
        // use stored credentials
        WiFi.softAPdisconnect(true);
        WiFi.begin();
    }

    if (!MDNS.begin(hostname.c_str())) {
        Serial.println(F("MDNS init failed"));
    }

    BLEDevice::init("");
    auto * scan = BLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);

    subscribed[BLEAddress("a4:c1:38:16:ee:b7")] = "Foo";
    subscribed[BLEAddress("a4:c1:38:2e:e5:cf")] = "Bar";
    subscribed[BLEAddress("a4:c1:38:6a:85:d7")] = "Baz";

    server.on("/readings", HTTP_GET, []{
        std::lock_guard<std::mutex> guard(mutex);

        StaticJsonDocument<1024> json;

        for (const auto & kv : readings) {
            auto address = kv.first;
            const auto & reading = kv.second;

            const auto it = subscribed.find(address);
            if (it == subscribed.end())
                continue;

            const auto name = it->second;

            auto json_element = json[name];
            json_element["temperature"] = reading.temperature;
            json_element["humidity"] = reading.humidity;
            json_element["battery"] = reading.battery;
            json_element["age"] = (millis() - reading.timestamp) / 1000;
        }

        String output;
        serializeJson(json, output);
        server.send(200, F("application/json"), output);
    });

    server.on("/discovered", HTTP_GET, []{
        std::lock_guard<std::mutex> guard(mutex);
        StaticJsonDocument<1024> json;
        for (const auto & kv : discovered) {
            auto address = kv.first;
            const auto & name = kv.second;
            json[address.toString()] = name;
        }
        String output;
        serializeJson(json, output);
        server.send(200, F("application/json"), output);
    });

    server.on("/subscriptions", [] {
        std::lock_guard<std::mutex> guard(mutex);
        StaticJsonDocument<1024> json;
        for (const auto & kv : subscribed) {
            auto address = kv.first;
            const auto & name = kv.second;
            json[address.toString()] = name;
        }
        String output;
        serializeJson(json, output);
        server.send(200, F("application/json"), output);
    });

    server.on(UriRegex("/subscriptions/((?:[a-fA-F0-9]{2}:){5}[a-fA-F0-9]{2})"), [] {
        std::lock_guard<std::mutex> guard(mutex);

        const std::string addrstr = server.pathArg(0).c_str();
        BLEAddress address{addrstr};

        switch (server.method()) {
            case HTTP_POST:
            case HTTP_PUT:
            case HTTP_PATCH:
                subscribed[address] = server.arg("plain").c_str();
            case HTTP_GET:
                {
                    auto it = subscribed.find(address);
                    if (it == subscribed.end())
                        server.send(404);
                    else
                        server.send(200, F("text/plain"), it->second.c_str());
                }
                return;
            case HTTP_DELETE:
                subscribed.erase(address);
                server.send(200);
                return;
            default:
                server.send(405);
                return;
        }
    });

    server.begin();
    mqtt.begin();
}

void publish_readings() {
    static unsigned long last_update = 0;

    std::lock_guard<std::mutex> guard(mutex);

    for (const auto & kv : readings) {
        auto address = kv.first;
        const auto & reading = kv.second;

        const auto it = subscribed.find(address);
        if (it == subscribed.end()) {
            // not subscribed (anymore...)
            continue;
        }

        if (reading.timestamp < last_update) {
            // already published
            continue;
        }

        const auto name = it->second;

        Serial.printf("%s  %-16s  %6.2f  %6.2f  %3u\n",
                      address.toString().c_str(),
                      name.c_str(),
                      reading.temperature,
                      reading.humidity,
                      reading.battery);

        const String topic = "calor/" + hostname + "/" + name.c_str();
        mqtt.publish(topic + "/temperature", String(reading.temperature));
        mqtt.publish(topic + "/humidity", String(reading.humidity));
        mqtt.publish(topic + "/battery_level", String(reading.battery));
    }

    last_update = millis();
}

void loop() {
    server.handleClient();
    mqtt.loop();

    if (!scanning) {
        scanning = BLEDevice::getScan()->start(10, scan_complete, false);
    }

    publish_readings();
}
