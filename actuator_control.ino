// Actuator Controller
// Copyright (c) 2026 Mackenzie Glenfadden
// Licensed under CC BY-NC 4.0
// Free for personal/non-commercial use.
// Commercial licensing: kenzieglen@gmail.com
// https://creativecommons.org/licenses/by-nc/4.0/

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>

// ------------------------------------------------------------
//  PWM config
// ------------------------------------------------------------
#define RPWM_PIN       18
#define PWM_FREQ       12000
#define PWM_RESOLUTION 8

// ------------------------------------------------------------
// Pin assignments
// ------------------------------------------------------------
// DRV8871:
//   VM → 24V supply positive (power in from wall)
//   GND → 24V supply negative
//   OUT1 → Actuator black wire (out to machine)
//   OUT2 → Actuator red wire
//   IN1 → GPIO18 (ESP32)
//   IN2 → GND (tie to ground)
//   VM → IN+ (5V buck converter)
//   GND → IN- (5V buck converter)
// 5V buck converter:
//   OUT+ → VIN (ESP32)
//   OUT- → GND (ESP32)
// ------------------------------------------------------------

// ------------------------------------------------------------
//  Globals
// ------------------------------------------------------------
WebServer     server(80);
Preferences   prefs;
WiFiClientSecure secured_client;
UniversalTelegramBot* bot = nullptr;

String telegramToken;

bool          isRunning    = false;
int           currentSpeed = 0;
unsigned long stopAt       = 0;

#define BOT_MTBS 1000
unsigned long bot_lasttime = 0;

// ------------------------------------------------------------
//  Motor helpers
// ------------------------------------------------------------
void motorSetSpeed(int percent) {
  if (percent == 0) {
    ledcWrite(RPWM_PIN, 0);
    return;
  }
  int duty = map(percent, 1, 100, 153, 255);
  ledcWrite(RPWM_PIN, duty);
}

void motorStart(int speedPercent, int durationSeconds) {
  isRunning    = true;
  currentSpeed = speedPercent;
  stopAt       = millis() + ((unsigned long)durationSeconds * 1000UL);
  motorSetSpeed(speedPercent);
  Serial.printf("START speed=%d%% duration=%ds\n", speedPercent, durationSeconds);
}

void motorStop() {
  isRunning    = false;
  currentSpeed = 0;
  stopAt       = 0;
  motorSetSpeed(0);
  Serial.println("STOP");
}

// ------------------------------------------------------------
//  Web UI
// ------------------------------------------------------------
void handleRoot() {
  String html = R"rawhtml(
<!DOCTYPE html><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>RoboBoink</title>
  <style>
    body { font-family: sans-serif; max-width: 400px; margin: 40px auto; padding: 0 16px; }
    h1 { font-size: 1.4em; }
    label { display: block; margin-top: 16px; font-weight: bold; }
    input[type=number] { width: 100%; padding: 8px; font-size: 1em; box-sizing: border-box; }
    button { margin-top: 16px; width: 100%; padding: 12px; font-size: 1em; cursor: pointer; }
    #runBtn  { background: #2a9d2a; color: white; border: none; border-radius: 4px; }
    #stopBtn { background: #c0392b; color: white; border: none; border-radius: 4px; }
    #status  { margin-top: 24px; padding: 12px; background: #f0f0f0; border-radius: 4px; font-family: monospace; }
  </style>
</head>
<body>
  <h1>RoboBoink</h1>
  <h3>Remote-controlled sex machine</h3>

  <label>Speed (1-100 %)</label>
  <input type="number" id="speed" min="1" max="100" value="80">

  <label>Duration (seconds)</label>
  <input type="number" id="duration" min="1" value="30">

  <button id="runBtn"  onclick="runMotor()">Run</button>
  <button id="stopBtn" onclick="stopMotor()">Stop</button>

  <div id="status">Status: unknown</div>

  <script>
    function setStatus(obj) {
      document.getElementById('status').textContent = JSON.stringify(obj, null, 2);
    }

    function runMotor() {
      const speed    = document.getElementById('speed').value;
      const duration = document.getElementById('duration').value;
      fetch(`/run?speed=${speed}&duration=${duration}`)
        .then(r => r.json()).then(setStatus).catch(e => setStatus({ error: String(e) }));
    }

    function stopMotor() {
      fetch('/stop')
        .then(r => r.json()).then(setStatus).catch(e => setStatus({ error: String(e) }));
    }

    function pollStatus() {
      fetch('/status')
        .then(r => r.json()).then(setStatus).catch(() => {});
    }

    setInterval(pollStatus, 2000);
    pollStatus();
  </script>
</body>
</html>
)rawhtml";
  server.send(200, "text/html", html);
}

// ------------------------------------------------------------
//  API routes
// ------------------------------------------------------------
void handleRun() {
  if (!server.hasArg("speed") || !server.hasArg("duration")) {
    server.send(400, "application/json",
      "{\"error\":\"Required params: speed (1-100), duration (seconds)\"}");
    return;
  }

  int speed    = server.arg("speed").toInt();
  int duration = server.arg("duration").toInt();

  if (speed < 1 || speed > 100) {
    server.send(400, "application/json", "{\"error\":\"speed must be 1-100\"}");
    return;
  }
  if (duration < 1) {
    server.send(400, "application/json", "{\"error\":\"duration must be >= 1\"}");
    return;
  }

  motorStart(speed, duration);

  StaticJsonDocument<128> doc;
  doc["status"]   = "running";
  doc["speed"]    = speed;
  doc["duration"] = duration;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleStop() {
  motorStop();
  server.send(200, "application/json", "{\"status\":\"stopped\"}");
}

void handleStatus() {
  StaticJsonDocument<128> doc;
  if (isRunning) {
    long msLeft   = (long)(stopAt - millis());
    int  secsLeft = (int)(msLeft > 0 ? msLeft / 1000 : 0);
    doc["status"]         = "running";
    doc["speed"]          = currentSpeed;
    doc["time_remaining"] = secsLeft;
  } else {
    doc["status"] = "stopped";
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ------------------------------------------------------------
//  Telegram command handler
// ------------------------------------------------------------
void handleTelegramMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot->messages[i].chat_id);
    String text    = bot->messages[i].text;
    text.trim();

    Serial.printf("Telegram [%s]: %s\n", chat_id.c_str(), text.c_str());

    if (text == "/stop") {
      motorStop();
      bot->sendMessage(chat_id, "Stopped.", "");

    } else if (text == "/status") {
      String reply;
      if (isRunning) {
        long msLeft   = (long)(stopAt - millis());
        int  secsLeft = (int)(msLeft > 0 ? msLeft / 1000 : 0);
        reply = "Running at " + String(currentSpeed) + "% — " + String(secsLeft) + "s remaining.";
      } else {
        reply = "Stopped.";
      }
      bot->sendMessage(chat_id, reply, "");

    } else if (text.startsWith("/run")) {
      int speed = 80, duration = 30;
      sscanf(text.c_str(), "/run %d %d", &speed, &duration);

      if (speed < 1 || speed > 100) {
        bot->sendMessage(chat_id, "Speed must be 1-100.", "");
      } else if (duration < 1) {
        bot->sendMessage(chat_id, "Duration must be >= 1 second.", "");
      } else {
        motorStart(speed, duration);
        bot->sendMessage(chat_id,
          "Running at " + String(speed) + "% for " + String(duration) + "s.", "");
      }

    } else if (text == "/help") {
      bot->sendMessage(chat_id,
        "Commands:\n"
        "/run [speed] [duration] — start motor (e.g. /run 80 30)\n"
        "/stop — stop immediately\n"
        "/status — current state\n"
        "/help — this message", "");

    } else {
      bot->sendMessage(chat_id, "Unknown command. Send /help for options.", "");
    }
  }
}

// ------------------------------------------------------------
//  Setup
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\nActuator Controller starting...");

  // PWM
  ledcAttach(RPWM_PIN, PWM_FREQ, PWM_RESOLUTION);
  motorSetSpeed(0);

  // Load saved prefs
  prefs.begin("actuator", false);
  telegramToken = prefs.getString("tg_token", "");
  prefs.end();

  // WiFiManager
  WiFiManager wm;
  WiFiManagerParameter param_tg_token("tg_token", "Telegram Bot Token",
                                       telegramToken.c_str(), 60);
  wm.addParameter(&param_tg_token);

  wm.setSaveParamsCallback([&]() {
    telegramToken = param_tg_token.getValue();
    prefs.begin("actuator", false);
    prefs.putString("tg_token", telegramToken);
    prefs.end();
    Serial.println("Config saved.");
  });

  wm.autoConnect("Actuator-Setup");
  Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());

  // Telegram
  if (telegramToken.length() > 0) {
    secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    bot = new UniversalTelegramBot(telegramToken, secured_client);
    Serial.println("Telegram bot initialized.");
  } else {
    Serial.println("No Telegram token — bot disabled.");
  }

  // Web server
  server.on("/",       HTTP_GET, handleRoot);
  server.on("/run",    HTTP_GET, handleRun);
  server.on("/stop",   HTTP_GET, handleStop);
  server.on("/status", HTTP_GET, handleStatus);
  server.begin();
  Serial.println("HTTP server started.");
}

// ------------------------------------------------------------
//  Loop
// ------------------------------------------------------------
void loop() {
  server.handleClient();

  if (isRunning && millis() >= stopAt) {
    Serial.println("Duration expired — auto-stopping.");
    motorStop();
  }

  if (bot && millis() - bot_lasttime > BOT_MTBS) {
    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    while (numNewMessages) {
      handleTelegramMessages(numNewMessages);
      numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    }
    bot_lasttime = millis();
  }
}
