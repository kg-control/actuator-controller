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
#include <queue>

// ============================================================
//  BLE ADDITIONS — includes
// ============================================================
#include <NimBLEDevice.h>

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
//  Queue item
// ------------------------------------------------------------
struct QueueItem {
  int speed;
  int duration;
  String source;   // BLE ADDITION: cosmetic label only, e.g. "Alex's phone". Empty for HTTP/Telegram-originated items.
};

// ------------------------------------------------------------
//  Globals
// ------------------------------------------------------------
WebServer     server(80);
Preferences   prefs;
WiFiClientSecure secured_client;
UniversalTelegramBot* bot = nullptr;

String telegramToken;
String adminChatId;

// Motor / queue state
std::queue<QueueItem> motorQueue;
bool          isRunning    = false;
int           currentSpeed = 0;
unsigned long stopAt       = 0;
String        currentSource = "";   // BLE ADDITION: label of whatever item is currently running

// Web key state (memory only — clears on reboot)
String        webKey       = "";   // empty = unlocked

#define BOT_MTBS 1000
unsigned long bot_lasttime = 0;

// ============================================================
//  BLE ADDITIONS — globals, UUIDs, config
// ============================================================
// "Party mode" BLE control: no auth, no key mechanic. Anyone with the app
// in range can connect and push onto the shared queue. Bounded connection
// count with advertising stopped once full (see below).

#define BLE_MAX_CONNECTIONS 3   // NimBLE default max is usually higher; we cap intentionally

#define BLE_SERVICE_UUID        "6f3f0001-6d65-4b6f-696e-6b656e7a6965"
#define BLE_CMD_CHAR_UUID       "6f3f0002-6d65-4b6f-696e-6b656e7a6965"  // write:  commands
#define BLE_STATUS_CHAR_UUID    "6f3f0003-6d65-4b6f-696e-6b656e7a6965"  // notify: status JSON
#define BLE_CONFIG_CHAR_UUID    "6f3f0004-6d65-4b6f-696e-6b656e7a6965"  // write:  hidden config menu

NimBLEServer*         bleServer        = nullptr;
NimBLECharacteristic*  bleCmdChar      = nullptr;
NimBLECharacteristic*  bleStatusChar   = nullptr;
NimBLECharacteristic*  bleConfigChar   = nullptr;
int bleConnCount = 0;

const int  BLE_SOURCE_MAX_LEN = 20;   // hard cap on the cosmetic name field
unsigned long bleLastNotify = 0;
#define BLE_NOTIFY_INTERVAL_MS 2000

// ------------------------------------------------------------
//  Key helpers
// ------------------------------------------------------------
String generateKey() {
  const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  String key = "";
  for (int i = 0; i < 4; i++) {
    key += charset[esp_random() % 26];
  }
  return key;
}

bool isLocked() {
  return webKey.length() > 0;
}

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

void motorStop() {
  while (!motorQueue.empty()) motorQueue.pop();
  isRunning     = false;
  currentSpeed  = 0;
  stopAt        = 0;
  currentSource = "";
  motorSetSpeed(0);
  Serial.println("STOP — queue cleared");
}

// Original signature kept identical so existing HTTP/Telegram call sites
// don't need to change; source defaults to "" for those callers.
void queueAdd(int speedPercent, int durationSeconds, String source = "") {
  QueueItem item = { speedPercent, durationSeconds, source };
  motorQueue.push(item);
  Serial.printf("QUEUED speed=%d%% duration=%ds source=\"%s\" (queue depth: %d)\n",
                speedPercent, durationSeconds, source.c_str(), (int)motorQueue.size());

  // If nothing is running, start immediately
  if (!isRunning) {
    queueAdvance();
  }
}

void queueAdvance() {
  if (motorQueue.empty()) {
    isRunning     = false;
    currentSpeed  = 0;
    stopAt        = 0;
    currentSource = "";
    motorSetSpeed(0);
    Serial.println("Queue empty — stopped");
    return;
  }

  QueueItem item = motorQueue.front();
  motorQueue.pop();

  isRunning     = true;
  currentSpeed  = item.speed;
  stopAt        = millis() + ((unsigned long)item.duration * 1000UL);
  currentSource = item.source;
  motorSetSpeed(item.speed);
  Serial.printf("NEXT speed=%d%% duration=%ds source=\"%s\" (remaining in queue: %d)\n",
                item.speed, item.duration, item.source.c_str(), (int)motorQueue.size());
}

void queueNext() {
  motorSetSpeed(0);
  isRunning = false;
  queueAdvance();
}

// Queue helpers for status
int queueTotalSeconds() {
  // Count remaining time on current item plus all queued items
  int total = 0;
  if (isRunning) {
    long msLeft = (long)(stopAt - millis());
    total += (int)(msLeft > 0 ? msLeft / 1000 : 0);
  }
  // We can't iterate std::queue directly, so we track total separately
  // via queueTotalDuration global below
  return total;
}

// ------------------------------------------------------------
//  Web UI
// ------------------------------------------------------------
void handleRoot() {
  // Inject lock state as a JS variable so one HTML file serves all roles
  String lockState = isLocked() ? "true" : "false";

  String html = R"rawhtml(
<!DOCTYPE html><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>RoboBoink</title>
  <style>
    * { box-sizing: border-box; }
    body {
      font-family: sans-serif;
      max-width: 420px;
      margin: 40px auto;
      padding: 0 16px;
      background: #1a1a1a;
      color: #f0f0f0;
    }
    img {
      max-width: 400px;
      height: auto;
      display: block; /* Prevents unwanted bottom margin spacing */
    }
    h1 { font-size: 1.4em; margin-bottom: 2px; }
    h3 { margin-top: 0; color: #aaa; font-weight: normal; font-size: 0.95em; }
    label { display: block; margin-top: 16px; font-weight: bold; font-size: 0.85em; text-transform: uppercase; letter-spacing: 0.05em; color: #aaa; }
    input[type=number] { width: 100%; padding: 10px; font-size: 1em; background: #2a2a2a; color: #f0f0f0; border: 1px solid #444; border-radius: 4px; margin-top: 4px; }
    .btn-row { display: flex; gap: 8px; margin-top: 16px; }
    button { flex: 1; padding: 12px; font-size: 1em; cursor: pointer; border: none; border-radius: 4px; font-weight: bold; }
    #runBtn   { background: #2a9d2a; color: white; }
    #startBtn { background: #2a9d2a; color: white; }
    #stopBtn  { background: #c0392b; color: white; }
    #nextBtn  { background: #e67e22; color: white; }
    #keyBtn   { background: #2980b9; color: white; width: 100%; margin-top: 8px; }
    #releaseBtn { background: #7f8c8d; color: white; width: 100%; margin-top: 8px; }
    button:disabled { opacity: 0.35; cursor: not-allowed; }
    #status {
      margin-top: 24px;
      padding: 14px;
      background: #2a2a2a;
      border-radius: 4px;
      font-family: monospace;
      font-size: 0.9em;
      line-height: 1.6;
    }
    #keyModal {
      display: none;
      position: fixed; top: 0; left: 0; right: 0; bottom: 0;
      background: rgba(0,0,0,0.85);
      align-items: center;
      justify-content: center;
    }
    #keyModal.show { display: flex; }
    .modal-box {
      background: #2a2a2a;
      border-radius: 8px;
      padding: 28px 24px;
      max-width: 320px;
      width: 90%;
      text-align: center;
    }
    .modal-box h2 { margin-top: 0; }
    .modal-box input {
      width: 100%;
      padding: 10px;
      font-size: 1.4em;
      text-align: center;
      letter-spacing: 0.2em;
      text-transform: uppercase;
      background: #1a1a1a;
      color: #f0f0f0;
      border: 1px solid #555;
      border-radius: 4px;
      margin: 12px 0;
    }
    .modal-btn-row { display: flex; gap: 8px; margin-top: 8px; }
    .modal-btn-row button { flex: 1; padding: 10px; }
    #newKeyDisplay {
      display: none;
      font-size: 2em;
      letter-spacing: 0.3em;
      font-weight: bold;
      color: #2ecc71;
      margin: 16px 0;
    }
    .readonly-notice {
      margin-top: 12px;
      padding: 8px 12px;
      background: #3a2a00;
      border-left: 3px solid #e67e22;
      border-radius: 2px;
      font-size: 0.85em;
      color: #f0c060;
    }
    .slider-group {
      background: #2a2a2a;
      border-radius: 8px;
      padding: 14px 16px;
      margin-top: 12px;
    }
    .slider-row {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      margin-bottom: 8px;
    }
    .slider-row label {
      margin: 0;
      font-weight: bold;
      font-size: 0.85em;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      color: #aaa;
    }
    .slider-value {
      font-weight: bold;
      color: #f0f0f0;
      font-size: 0.95em;
    }
    input[type=range] {
      width: 100%;
      -webkit-appearance: none;
      appearance: none;
      height: 4px;
      background: #444;
      border-radius: 2px;
      outline: none;
    }
    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 20px;
      height: 20px;
      background: #2980b9;
      border-radius: 50%;
      cursor: pointer;
      border: none;
    }
    input[type=range]::-moz-range-thumb {
      width: 20px;
      height: 20px;
      background: #2980b9;
      border-radius: 50%;
      cursor: pointer;
      border: none;
    }
  </style>
</head>
<body>
  <img src="https://<insert domain name here>/video">
  <!-- see webcam.py for a simple webcam broadcaster -->

  <div id="mainControls">
    <div class="slider-group">
      <div class="slider-row">
        <label for="speed">Speed</label>
        <span class="slider-value"><span id="speedVal">50</span>%</span>
      </div>
      <input type="range" id="speed" min="1" max="100" value="50"
             oninput="document.getElementById('speedVal').textContent=this.value">
    </div>

    <div class="slider-group">
      <div class="slider-row">
        <label for="duration">Time</label>
        <span class="slider-value"><span id="durationVal">30</span>s</span>
      </div>
      <input type="range" id="duration" min="30" max="300" value="30"
             oninput="document.getElementById('durationVal').textContent=this.value">
    </div>
    <div class="btn-row">
      <button id="runBtn"  onclick="runMotor()">Add Time to Queue</button>
      <button id="nextBtn" onclick="nextItem()">Next</button>
    </div>
    <div class="btn-row">
      <button id="startBtn"  onclick="startMotor()">Start (no timer)</button>
      <button id="stopBtn" onclick="stopMotor()">Stop All</button>
    </div>

    <!--<button id="keyBtn" onclick="showTakeKeys()" style="display:none">Take Keys</button>
    <button id="releaseBtn" onclick="releaseKeys()" style="display:none">Release Keys</button>-->
  </div>

  <div id="readonlyControls" style="display:none">
    <div class="readonly-notice">Device is currently in use. View only.</div>
  </div>

  <div id="status">Connecting...</div>

  <!-- Key modal: used for both "take keys" (new key display) and "enter key" (login) -->
  <div id="keyModal">
    <div class="modal-box">
      <div id="modalTakeKeys">
        <h2>Take Keys</h2>
        <p>Taking the keys gives you exclusive control. Others can watch but not interact.</p>
        <button onclick="confirmTakeKeys()" style="background:#2980b9;color:white;width:100%;padding:12px;border:none;border-radius:4px;font-size:1em;font-weight:bold;">Generate Key</button>
        <div id="newKeyDisplay"></div>
        <p id="keyInstructions" style="display:none;font-size:0.85em;color:#aaa;">Save this code — it won't be shown again. You'll need it if you reload the page.</p>
        <button id="keyDoneBtn" onclick="closeModal()" style="display:none;background:#2a9d2a;color:white;width:100%;padding:12px;border:none;border-radius:4px;font-size:1em;font-weight:bold;margin-top:8px;">Done</button>
      </div>

      <div id="modalEnterKey" style="display:none">
        <h2>Enter Key</h2>
        <p style="font-size:0.85em;color:#aaa;">This device is locked. Enter the key to take control, or cancel to watch.</p>
        <input type="text" id="keyInput" maxlength="4" placeholder="XXXX" oninput="this.value=this.value.toUpperCase()">
        <div class="modal-btn-row">
          <button onclick="submitKey()" style="background:#2980b9;color:white;">Enter</button>
          <button onclick="dismissModal()" style="background:#444;color:white;">Watch only</button>
        </div>
      </div>
    </div>
  </div>

  <script>
    const LOCKED = )rawhtml";

  html += lockState;

  html += R"rawhtml(;
    let isKeyholder = false;
    let checkedKey  = false;

    function setStatus(obj) {
      let lines = [];
      if (obj.status === 'running') {
        lines.push('Running at ' + obj.speed + '%');
        lines.push('' + obj.time_remaining + 's remaining on current item');
        if (obj.queue_depth > 0) {
          lines.push('' + obj.queue_depth + ' item(s) queued (' + obj.queue_seconds + 's total)');
        }
      } else {
        lines.push('Stopped');
      }
      if (obj.locked) {
        lines.push('Keys are held');
      }
      document.getElementById('status').textContent = lines.join('\n');
    }

    function updateUI(locked, keyholder) {
      const controls = document.getElementById('mainControls');
      const readonly = document.getElementById('readonlyControls');
      const keyBtn     = document.getElementById('keyBtn');
      const releaseBtn = document.getElementById('releaseBtn');

      if (!locked) {
        // Open — everyone has full controls
        controls.style.display = 'block';
        readonly.style.display = 'none';
        keyBtn.style.display     = 'block';
        releaseBtn.style.display = 'none';
        setInputsDisabled(false);
      } else if (keyholder) {
        // I hold the keys
        controls.style.display = 'block';
        readonly.style.display = 'none';
        keyBtn.style.display     = 'none';
        releaseBtn.style.display = 'block';
        setInputsDisabled(false);
      } else {
        // Locked, not keyholder
        controls.style.display = 'none';
        readonly.style.display = 'block';
      }
    }

    function setInputsDisabled(disabled) {
      ['runBtn','startBtn','stopBtn','nextBtn'].forEach(id => {
        document.getElementById(id).disabled = disabled;
      });
    }

    function showTakeKeys() {
      document.getElementById('modalTakeKeys').style.display = 'block';
      document.getElementById('modalEnterKey').style.display = 'none';
      document.getElementById('newKeyDisplay').style.display = 'none';
      document.getElementById('keyInstructions').style.display = 'none';
      document.getElementById('keyDoneBtn').style.display = 'none';
      document.getElementById('keyModal').classList.add('show');
    }

    function confirmTakeKeys() {
      fetch('/takekeys')
        .then(r => r.json())
        .then(data => {
          if (data.key) {
            isKeyholder = true;
            document.getElementById('newKeyDisplay').textContent = data.key;
            document.getElementById('newKeyDisplay').style.display = 'block';
            document.getElementById('keyInstructions').style.display = 'block';
            document.getElementById('keyDoneBtn').style.display = 'block';
            updateUI(true, true);
          }
        });
    }

    function closeModal() {
      document.getElementById('keyModal').classList.remove('show');
    }

    function dismissModal() {
      // Dismissed key prompt — watch only
      isKeyholder = false;
      checkedKey  = true;
      closeModal();
      updateUI(true, false);
    }

    function showEnterKey() {
      document.getElementById('modalTakeKeys').style.display = 'none';
      document.getElementById('modalEnterKey').style.display = 'block';
      document.getElementById('keyModal').classList.add('show');
    }

    function submitKey() {
      const key = document.getElementById('keyInput').value.trim().toUpperCase();
      fetch('/enterkey?key=' + encodeURIComponent(key))
        .then(r => r.json())
        .then(data => {
          if (data.success) {
            isKeyholder = true;
            checkedKey  = true;
            closeModal();
            updateUI(true, true);
          } else {
            document.getElementById('keyInput').value = '';
            document.getElementById('keyInput').placeholder = 'Wrong key';
          }
        });
    }

    function releaseKeys() {
      fetch('/releasekeys')
        .then(r => r.json())
        .then(data => {
          if (data.success) {
            isKeyholder = false;
            updateUI(false, false);
          }
        });
    }

    function runMotor() {
      const speed    = document.getElementById('speed').value;
      const duration = document.getElementById('duration').value;
      fetch('/run?speed=' + speed + '&duration=' + duration)
        .then(r => r.json()).then(setStatus).catch(e => console.error(e));
    }

    function startMotor() {
      const speed    = document.getElementById('speed').value;
      const duration = 9999;
      fetch('/run?speed=' + speed + '&duration=' + duration)
        .then(r => r.json()).then(setStatus).catch(e => console.error(e));
    }

    function stopMotor() {
      fetch('/stop').then(r => r.json()).then(data => {
        setStatus(data);
        if (!data.locked) updateUI(false, false);
      });
    }

    function nextItem() {
      fetch('/next').then(r => r.json()).then(setStatus).catch(e => console.error(e));
    }

    function pollStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          setStatus(data);

          // On first load, if device is locked prompt for key
          if (data.locked && !checkedKey && !isKeyholder) {
            checkedKey = true;
            showEnterKey();
          } else {
            updateUI(data.locked, isKeyholder);
          }
        })
        .catch(() => {});
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
  // If locked, only the keyholder cookie/session... we have no sessions.
  // For web key enforcement on /run, we check a submitted key param.
  // If locked and no valid key submitted, reject.
  if (isLocked()) {
    if (!server.hasArg("key") || server.arg("key") != webKey) {
      server.send(403, "application/json", "{\"error\":\"Device is locked\"}");
      return;
    }
  }

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

  queueAdd(speed, duration);

  StaticJsonDocument<128> doc;
  doc["status"]      = "queued";
  doc["speed"]       = speed;
  doc["duration"]    = duration;
  doc["queue_depth"] = (int)motorQueue.size();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleStop() {
  motorStop();
  StaticJsonDocument<64> doc;
  doc["status"] = "stopped";
  doc["locked"] = isLocked();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleStatus() {
  StaticJsonDocument<200> doc;
  if (isRunning) {
    long msLeft   = (long)(stopAt - millis());
    int  secsLeft = (int)(msLeft > 0 ? msLeft / 1000 : 0);
    doc["status"]         = "running";
    doc["speed"]          = currentSpeed;
    doc["time_remaining"] = secsLeft;
    doc["queue_depth"]    = (int)motorQueue.size();

    // Approximate total queued seconds (current item only — queue isn't iterable)
    doc["queue_seconds"]  = secsLeft;
  } else {
    doc["status"]      = "stopped";
    doc["queue_depth"] = (int)motorQueue.size();
  }
  doc["locked"] = isLocked();

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleNext() {
  if (isLocked()) {
    if (!server.hasArg("key") || server.arg("key") != webKey) {
      server.send(403, "application/json", "{\"error\":\"Device is locked\"}");
      return;
    }
  }
  queueNext();
  handleStatus();
}

void handleTakeKeys() {
  if (isLocked()) {
    server.send(409, "application/json", "{\"error\":\"Already locked\"}");
    return;
  }
  webKey = generateKey();
  Serial.printf("Keys taken. Code: %s\n", webKey.c_str());

  // Clear any active queue when keys are taken
  motorStop();

  StaticJsonDocument<64> doc;
  doc["key"] = webKey;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleEnterKey() {
  if (!isLocked()) {
    // Not locked — grant access anyway
    StaticJsonDocument<32> doc;
    doc["success"] = true;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
    return;
  }

  if (!server.hasArg("key")) {
    server.send(400, "application/json", "{\"error\":\"key param required\"}");
    return;
  }

  String submitted = server.arg("key");
  submitted.toUpperCase();

  StaticJsonDocument<32> doc;
  doc["success"] = (submitted == webKey);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleReleaseKeys() {
  // In this handler we trust the client — key verification happens client-side
  // via the session. A determined attacker could call this endpoint directly,
  // but doing so only *unlocks* the device, which is not a harmful action.
  webKey = "";
  Serial.println("Keys released.");
  StaticJsonDocument<32> doc;
  doc["success"] = true;
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

    bool isAdmin = (adminChatId.length() > 0 && chat_id == adminChatId);

    if (text == "/stop") {
      motorStop();
      bot->sendMessage(chat_id, "Stopped. Queue cleared.", "");

    } else if (text == "/next") {
      queueNext();
      if (isRunning) {
        bot->sendMessage(chat_id,
          "Skipped. Now running at " + String(currentSpeed) + "%.", "");
      } else {
        bot->sendMessage(chat_id, "Queue empty — stopped.", "");
      }

    } else if (text == "/status") {
      String reply;
      if (isRunning) {
        long msLeft   = (long)(stopAt - millis());
        int  secsLeft = (int)(msLeft > 0 ? msLeft / 1000 : 0);
        reply = "Running at " + String(currentSpeed) + "% — " +
                String(secsLeft) + "s remaining.";
        if (!motorQueue.empty()) {
          reply += "\n" + String((int)motorQueue.size()) + " item(s) queued.";
        }
      } else {
        reply = "Stopped.";
      }
      if (isLocked()) reply += "\nWeb keys are held.";
      bot->sendMessage(chat_id, reply, "");

    } else if (text.startsWith("/run")) {
      int speed = 80, duration = 30;
      sscanf(text.c_str(), "/run %d %d", &speed, &duration);

      if (speed < 1 || speed > 100) {
        bot->sendMessage(chat_id, "Speed must be 1-100.", "");
      } else if (duration < 1) {
        bot->sendMessage(chat_id, "Duration must be >= 1 second.", "");
      } else {
        queueAdd(speed, duration);
        String reply = "Queued: " + String(speed) + "% for " + String(duration) + "s.";
        if (motorQueue.size() > 0) {
          reply += " (" + String((int)motorQueue.size()) + " in queue)";
        }
        bot->sendMessage(chat_id, reply, "");
      }

    } else if (text == "/unlock" && isAdmin) {
      webKey = "";
      bot->sendMessage(chat_id, "Web keys released by admin.", "");
      Serial.println("Admin unlocked web key via Telegram.");

    } else if (text == "/help") {
      String help =
        "Commands:\n"
        "/run [speed] [duration] — queue a run (e.g. /run 80 30)\n"
        "/next — skip current item\n"
        "/stop — stop and clear queue\n"
        "/status — current state\n"
        "/help — this message";
      if (isAdmin) {
        help += "\n/unlock — release web keys (admin only)";
      }
      bot->sendMessage(chat_id, help, "");

    } else {
      bot->sendMessage(chat_id, "Unknown command. Send /help for options.", "");
    }
  }
}

// ============================================================
//  BLE ADDITIONS — sanitization helper
// ============================================================
// Treat every BLE write as arbitrary bytes of arbitrary length until proven
// otherwise. This is a crash-prevention measure, not a security boundary —
// party mode has no auth by design. Goals:
//   - hard length cap (avoid heap fragmentation from unbounded String growth)
//   - printable-ASCII-only (avoid corrupting JSON/Telegram output downstream)
// This name is BLE-only cosmetic metadata; it is never sent to Telegram.
String sanitizeSourceName(const String& raw) {
  String clean = "";
  int limit = raw.length() < BLE_SOURCE_MAX_LEN ? raw.length() : BLE_SOURCE_MAX_LEN;
  for (int i = 0; i < limit; i++) {
    char c = raw[i];
    if (c >= 32 && c <= 126) {   // printable ASCII only, no control chars/newlines/etc.
      clean += c;
    }
  }
  return clean;
}

// ============================================================
//  BLE ADDITIONS — status JSON builder (shared by notify + read)
// ============================================================
String buildBleStatusJson() {
  StaticJsonDocument<256> doc;
  if (isRunning) {
    long msLeft   = (long)(stopAt - millis());
    int  secsLeft = (int)(msLeft > 0 ? msLeft / 1000 : 0);
    doc["status"]         = "running";
    doc["speed"]          = currentSpeed;
    doc["time_remaining"] = secsLeft;
    doc["queue_depth"]    = (int)motorQueue.size();
    doc["source"]         = currentSource;   // who queued the item currently running
  } else {
    doc["status"]      = "stopped";
    doc["queue_depth"] = (int)motorQueue.size();
  }
  doc["ble_connections"] = bleConnCount;
  doc["ble_max"]          = BLE_MAX_CONNECTIONS;

  String out;
  serializeJson(doc, out);
  return out;
}

// ============================================================
//  BLE ADDITIONS — server (connect/disconnect) callbacks
// ============================================================
// Advertising is stopped once BLE_MAX_CONNECTIONS is reached, and resumed
// on disconnect. This lets the app simply "not find" the device when full
// rather than attempting and failing a connection — avoids a race where
// two phones both think a slot is open.
class BleServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* srv, NimBLEConnInfo& connInfo) override {
    bleConnCount++;
    Serial.printf("BLE client connected (%d/%d)\n", bleConnCount, BLE_MAX_CONNECTIONS);
    if (bleConnCount >= BLE_MAX_CONNECTIONS) {
      NimBLEDevice::stopAdvertising();
      Serial.println("BLE: max connections reached — advertising stopped");
    }
  }

  void onDisconnect(NimBLEServer* srv, NimBLEConnInfo& connInfo, int reason) override {
    if (bleConnCount > 0) bleConnCount--;
    Serial.printf("BLE client disconnected (%d/%d)\n", bleConnCount, BLE_MAX_CONNECTIONS);
    // Resume advertising now that a slot is free (also covers the
    // never-was-full case, which is a harmless no-op if already advertising).
    NimBLEDevice::startAdvertising();
  }
};

// ============================================================
//  BLE ADDITIONS — command characteristic callback
// ============================================================
// Command format is a simple colon-delimited string, no auth:
//   "RUN:<speed>:<duration>"           e.g. RUN:80:30
//   "RUN:<speed>:<duration>:<name>"    e.g. RUN:80:30:Alex
//   "STOP"
//   "NEXT"
// Any connected client's write lands here identically — this is what makes
// "multiple people pushing onto the same queue" work with zero extra logic.
class CmdCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override {
    String value = String(chr->getValue().c_str());
    value.trim();
    if (value.length() == 0) return;

    if (value == "STOP") {
      motorStop();
      Serial.println("BLE: STOP");
      return;
    }

    if (value == "NEXT") {
      queueNext();
      Serial.println("BLE: NEXT");
      return;
    }

    if (value.startsWith("RUN:")) {
      // Split on ':' manually — avoids pulling in sscanf-with-strings edge cases
      int firstColon  = value.indexOf(':');
      int secondColon = value.indexOf(':', firstColon + 1);
      int thirdColon  = value.indexOf(':', secondColon + 1);

      if (secondColon == -1) return;  // malformed — need at least RUN:speed:duration

      String speedStr    = value.substring(firstColon + 1, secondColon);
      String durationStr = (thirdColon == -1)
                              ? value.substring(secondColon + 1)
                              : value.substring(secondColon + 1, thirdColon);
      String nameStr      = (thirdColon == -1) ? "" : value.substring(thirdColon + 1);

      int speed    = speedStr.toInt();
      int duration = durationStr.toInt();

      if (speed < 1 || speed > 100) {
        Serial.println("BLE: rejected RUN — speed out of range");
        return;
      }
      if (duration < 1) {
        Serial.println("BLE: rejected RUN — duration out of range");
        return;
      }

      String source = sanitizeSourceName(nameStr);
      queueAdd(speed, duration, source);
      Serial.printf("BLE: RUN speed=%d duration=%d source=\"%s\"\n",
                    speed, duration, source.c_str());
      return;
    }

    Serial.printf("BLE: unrecognized command \"%s\"\n", value.c_str());
  }
};

// ============================================================
//  BLE ADDITIONS — status characteristic callback (on-demand read)
// ============================================================
class StatusCharCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override {
    chr->setValue(buildBleStatusJson());
  }
};

// ============================================================
//  BLE ADDITIONS — hidden config characteristic callback
// ============================================================
// Accepts a JSON payload for changing WiFi / Telegram settings without a
// hard reflash:
//   {"tg_token":"...", "admin_id":"...", "ssid":"...", "pass":"..."}
// Any subset of fields may be present; only provided fields are changed.
// On a valid write, settings are saved and the device reboots — this is a
// deliberate, simple choice over trying to hot-swap WiFi/Telegram live.
class ConfigCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override {
    String value = String(chr->getValue().c_str());
    if (value.length() == 0 || value.length() > 256) {
      Serial.println("BLE config: rejected — empty or oversized payload");
      return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, value);
    if (err) {
      Serial.printf("BLE config: rejected — malformed JSON (%s)\n", err.c_str());
      return;
    }

    bool needsRestart = false;

    if (doc.containsKey("tg_token")) {
      String tok = doc["tg_token"].as<String>();
      if (tok.length() > 0 && tok.length() < 60) {
        prefs.begin("actuator", false);
        prefs.putString("tg_token", tok);
        prefs.end();
        Serial.println("BLE config: telegram token updated");
        needsRestart = true;
      } else {
        Serial.println("BLE config: rejected tg_token — invalid length");
      }
    }

    if (doc.containsKey("admin_id")) {
      String adm = doc["admin_id"].as<String>();
      if (adm.length() > 0 && adm.length() < 20) {
        prefs.begin("actuator", false);
        prefs.putString("admin_id", adm);
        prefs.end();
        Serial.println("BLE config: admin chat id updated");
        needsRestart = true;
      } else {
        Serial.println("BLE config: rejected admin_id — invalid length");
      }
    }

    if (doc.containsKey("ssid") && doc.containsKey("pass")) {
      String ssid = doc["ssid"].as<String>();
      String pass = doc["pass"].as<String>();
      if (ssid.length() > 0 && ssid.length() < 32 && pass.length() < 64) {
        // ESP32 Arduino core persists STA credentials to flash by default
        // (WiFi.persistent(true) is the default), so this survives reboot
        // and WiFiManager's autoConnect() will use it before opening a portal.
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.println("BLE config: WiFi credentials updated");
        needsRestart = true;
      } else {
        Serial.println("BLE config: rejected ssid/pass — invalid length");
      }
    }

    if (needsRestart) {
      Serial.println("BLE config: restarting to apply changes...");
      delay(300);   // give the write response a moment to flush over BLE
      ESP.restart();
    }
  }
};

// ============================================================
//  BLE ADDITIONS — setup
// ============================================================
void setupBle() {
  NimBLEDevice::init("Actuator-Controller");

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCallbacks());

  NimBLEService* svc = bleServer->createService(BLE_SERVICE_UUID);

  bleCmdChar = svc->createCharacteristic(
    BLE_CMD_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  bleCmdChar->setCallbacks(new CmdCharCallbacks());

  bleStatusChar = svc->createCharacteristic(
    BLE_STATUS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  bleStatusChar->setCallbacks(new StatusCharCallbacks());
  bleStatusChar->setValue(buildBleStatusJson());

  bleConfigChar = svc->createCharacteristic(
    BLE_CONFIG_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  bleConfigChar->setCallbacks(new ConfigCharCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->start();

  Serial.println("BLE service started, advertising as \"Actuator-Controller\"");
}

// ------------------------------------------------------------
//  Setup
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\nStroke machine starting...");

  // PWM
  ledcAttach(RPWM_PIN, PWM_FREQ, PWM_RESOLUTION);
  motorSetSpeed(0);

  // Load saved prefs
  prefs.begin("actuator", false);
  telegramToken = prefs.getString("tg_token", "");
  adminChatId   = prefs.getString("admin_id", "");
  prefs.end();

  // WiFiManager
  WiFiManager wm;
  WiFiManagerParameter param_tg_token("tg_token", "Telegram Bot Token",
                                       telegramToken.c_str(), 60);
  WiFiManagerParameter param_admin_id("admin_id", "Admin Telegram Chat ID",
                                       adminChatId.c_str(), 20);
  wm.addParameter(&param_tg_token);
  wm.addParameter(&param_admin_id);

  wm.setSaveParamsCallback([&]() {
    telegramToken = param_tg_token.getValue();
    adminChatId   = param_admin_id.getValue();
    prefs.begin("actuator", false);
    prefs.putString("tg_token", telegramToken);
    prefs.putString("admin_id", adminChatId);
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
    if (adminChatId.length() > 0) {
      Serial.printf("Admin chat ID: %s\n", adminChatId.c_str());
    } else {
      Serial.println("No admin chat ID set.");
    }
  } else {
    Serial.println("No Telegram token — bot disabled.");
  }

  // Web server
  server.on("/",           HTTP_GET, handleRoot);
  server.on("/run",        HTTP_GET, handleRun);
  server.on("/stop",       HTTP_GET, handleStop);
  server.on("/next",       HTTP_GET, handleNext);
  server.on("/status",     HTTP_GET, handleStatus);
  server.on("/takekeys",   HTTP_GET, handleTakeKeys);
  server.on("/enterkey",   HTTP_GET, handleEnterKey);
  server.on("/releasekeys",HTTP_GET, handleReleaseKeys);
  server.begin();
  Serial.println("HTTP server started.");

  // BLE ADDITION
  setupBle();
}

// ------------------------------------------------------------
//  Loop
// ------------------------------------------------------------
void loop() {
  server.handleClient();

  if (isRunning && millis() >= stopAt) {
    Serial.println("Item complete — advancing queue.");
    queueAdvance();
  }

  if (bot && millis() - bot_lasttime > BOT_MTBS) {
    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    while (numNewMessages) {
      handleTelegramMessages(numNewMessages);
      numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    }
    bot_lasttime = millis();
  }

  // BLE ADDITION — periodic status notify to any subscribed clients
  if (bleStatusChar && millis() - bleLastNotify > BLE_NOTIFY_INTERVAL_MS) {
    bleStatusChar->setValue(buildBleStatusJson());
    bleStatusChar->notify();
    bleLastNotify = millis();
  }
}
