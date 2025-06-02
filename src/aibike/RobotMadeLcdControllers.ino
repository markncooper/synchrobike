// ***************************************************************************
// MESH CONFIGURATION
// ***************************************************************************
#include <painlessMesh.h>
#include <ArduinoJson.h> // For message formatting

// #define IS_EXPLICIT_MASTER true // <<< UNCOMMENT AND SET TO 'true' FOR ONE DEVICE TO BE MASTER
                               // If commented out, the mesh root node will act as master.

#define MESH_PREFIX "LED_Sync_Mesh" // Must be unique for your mesh network
#define MESH_PASSWORD "SomethingSecure" // Change this!
#define MESH_PORT 5555

Scheduler userScheduler; // to control your own tasks
painlessMesh mesh;

// ***************************************************************************
// LED STRIP CONFIGURATION
// ***************************************************************************
#include <FastLED.h>
#define LED_PIN 13 // WeMos D1 Mini D4 pin (GPIO2) - GPIO2
#define NUM_LEDS 150
#define BRIGHTNESS 64 // Max 255. Start low.
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
CRGB leds[NUM_LEDS];

// ***************************************************************************
// PATTERN & SYNC CONFIGURATION
// ***************************************************************************
#define PATTERN_DURATION_S (15) // 15 seconds
#define MASTER_ANNOUNCE_INTERVAL_S (2) // Master sends sync info every 2 secs

byte currentPatternIndex = 0;
uint32_t patternStartTimeMeshMs = 0; // Mesh-synchronized time (in ms) when current pattern started
uint32_t lastMasterAnnounceMeshMs = 0;

bool amIMaster = false;

// ***************************************************************************
// PATTERN DEFINITIONS
// ***************************************************************************
typedef void (*PatternFunction)(); // Function pointer type for patterns

void patternSolidRed() {
  fill_solid(leds, NUM_LEDS, CRGB::Red);
}

void patternSolidGreen() {
  fill_solid(leds, NUM_LEDS, CRGB::Green);
}

void patternSolidBlue() {
  fill_solid(leds, NUM_LEDS, CRGB::Blue);
}

void patternRainbow() {
  static uint8_t hue = 0;
  fill_rainbow(leds, NUM_LEDS, hue++, 7);
}

void patternCylon() {
  static uint8_t hue = 0;
  static int pos = 0;
  static bool movingForward = true;
  fadeToBlackBy(leds, NUM_LEDS, 20);
  if (movingForward) {
    leds[pos] = CHSV(hue, 255, 255);
    pos++;
    if (pos >= NUM_LEDS) {
      pos = NUM_LEDS - 2;
      movingForward = false;
      hue += 32;
    }
  } else {
    leds[pos] = CHSV(hue, 255, 255);
    pos--;
    if (pos < 0) {
      pos = 1;
      movingForward = true;
      hue += 32;
    }
  }
}

// NEW PATTERN: Sparkle
#define SPARKLE_DENSITY 80 // Lower is more sparkles. Chance = 1/SPARKLE_DENSITY
#define SPARKLE_FADE_SPEED 40 // How fast sparkles fade. Higher is faster.
void patternSparkle() {
  fadeToBlackBy(leds, NUM_LEDS, SPARKLE_FADE_SPEED);
  if (random8() < SPARKLE_DENSITY) {
    leds[random16(NUM_LEDS)] = CHSV(random8(), 200, 255);
  }
}

// NEW PATTERN: Theater Chase
#define THEATER_CHASE_SPACING 3 // LEDs per group (e.g., 3 ON, 3 OFF, 3 ON...)
#define THEATER_CHASE_SPEED 100 // Milliseconds between steps
void patternTheaterChase() {
  static uint8_t j = 0;
  static unsigned long lastChaseUpdateTime = 0;
  static uint8_t currentHue = 0;

  // Note: We use millis() here because this pattern has its own internal timing
  // for the animation speed, independent of the global pattern change timer.
  if (millis() - lastChaseUpdateTime > THEATER_CHASE_SPEED) {
    lastChaseUpdateTime = millis();
    j++;
    if (j >= THEATER_CHASE_SPACING * 2) {
        j = 0;
        currentHue += 15; // Shift hue for next cycle for a rainbow effect
    }
    for (int i = 0; i < NUM_LEDS; i++) {
      if (((i + j) % (THEATER_CHASE_SPACING * 2)) < THEATER_CHASE_SPACING) {
        leds[i] = CHSV(currentHue + (i * 3), 255, 255); // Rainbow effect within the chase
      } else {
        leds[i] = CRGB::Black;
      }
    }
  }
}

// NEW PATTERN: Theater Chase Rainbow (variation, continuously shifting rainbow)
#define THEATER_CHASE_RAINBOW_SPEED 50 // Milliseconds between steps
void patternTheaterChaseRainbow() {
    static uint8_t j = 0; // position offset
    static uint8_t q = 0; // overall hue offset
    static unsigned long lastChaseUpdateTime = 0;

    if (millis() - lastChaseUpdateTime > THEATER_CHASE_RAINBOW_SPEED) {
        lastChaseUpdateTime = millis();
        j++; // Shift the "on" block
        q++; // Shift the base color for the rainbow

        for (int i = 0; i < NUM_LEDS; i++) {
            // if ( (i+j) modulo (group_size*2) is less than group_size ) then light it up
            if (((i + j) % (THEATER_CHASE_SPACING * 2)) < THEATER_CHASE_SPACING) {
                // CHSV(hue, saturation, value)
                // (i * (255 / NUM_LEDS)) creates a rainbow across the strip
                // q shifts this rainbow along
                leds[i] = CHSV( ((i * (255 / NUM_LEDS))/2 + q) % 255 , 255, 255);
            } else {
                leds[i] = CRGB::Black;
            }
        }
    }
}


PatternFunction patterns[] = {
  patternSolidRed,
  patternSolidGreen,
  patternSolidBlue,
  patternRainbow,
  patternCylon,
  patternSparkle,
  patternTheaterChase,
  patternTheaterChaseRainbow
};
const byte numPatterns = sizeof(patterns) / sizeof(patterns[0]);

// ***************************************************************************
// MESH CALLBACKS & HELPERS
// ***************************************************************************
void receivedCallback(uint32_t from, String &msg);
void newConnectionCallback(uint32_t nodeId);
void changedConnectionCallback();
void nodeTimeAdjustedCallback(int32_t offset);
void determineMasterRole();

void sendSyncPacket() {
  if (!amIMaster) return;
  StaticJsonDocument<200> doc;
  doc["p"] = currentPatternIndex;
  doc["t"] = patternStartTimeMeshMs; // Mesh time in MICROseconds
  String str;
  serializeJson(doc, str);
  mesh.sendBroadcast(str);
}

Task taskSendSync(MASTER_ANNOUNCE_INTERVAL_S * 1000, TASK_FOREVER, &sendSyncPacket);

// ***************************************************************************
// SETUP
// ***************************************************************************
void setup() {
  Serial.begin(115200);
  Serial.println();

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  delay(100);

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  determineMasterRole();

  userScheduler.addTask(taskSendSync);
  // taskSendSync will be enabled only by the master

  // Initialize with current mesh time (microseconds)
  // but convert to ms for the first patternStartTimeMeshMs if needed, or keep as us consistently
  patternStartTimeMeshMs = mesh.getNodeTime();
  Serial.printf("Initial patternStartTimeMeshMs (us): %u (Node time (us): %u)\n", patternStartTimeMeshMs, mesh.getNodeTime());
}

// ***************************************************************************
// LOOP
// ***************************************************************************
void loop() {
  userScheduler.execute();
  mesh.update();

  uint32_t currentTimeMeshUs = mesh.getNodeTime(); // painlessMesh time is in MICROseconds

  if (amIMaster) {
    // Check if it's time to change pattern (compare microseconds)
    if (currentTimeMeshUs - patternStartTimeMeshMs >= (unsigned long)PATTERN_DURATION_S * 1000000) {
      currentPatternIndex = (currentPatternIndex + 1) % numPatterns;
      patternStartTimeMeshMs = currentTimeMeshUs; // Record new start time in microseconds
      Serial.printf("MASTER: New pattern: %d, Start time (us): %u\n", currentPatternIndex, patternStartTimeMeshMs);
      sendSyncPacket(); // Send immediately on change
      // lastMasterAnnounceMeshMs is not strictly needed if using task for periodic sync
    }
  } else { // Slave logic
    // Optimistic change if master message missed
    if (patternStartTimeMeshMs != 0 && (currentTimeMeshUs - patternStartTimeMeshMs >= (unsigned long)PATTERN_DURATION_S * 1000000)) {
        currentPatternIndex = (currentPatternIndex + 1) % numPatterns;
        patternStartTimeMeshMs += (unsigned long)PATTERN_DURATION_S * 1000000; // Project next start time in us
        Serial.printf("SLAVE %u: Optimistic pattern change to %d. Next projected start (us): %u\n", mesh.getNodeId(), currentPatternIndex, patternStartTimeMeshMs);
    }
  }

  if (currentPatternIndex < numPatterns) {
    patterns[currentPatternIndex]();
  }
  FastLED.show();
  FastLED.delay(16); // Aim for ~60 FPS
}

// ***************************************************************************
// MESH CALLBACK IMPLEMENTATIONS (Same as before)
// ***************************************************************************
void determineMasterRole() {
  bool oldMasterState = amIMaster;
  #ifdef IS_EXPLICIT_MASTER
    amIMaster = IS_EXPLICIT_MASTER;
  #else
    amIMaster = mesh.isRoot();
  #endif

  if (amIMaster && !oldMasterState) {
    Serial.printf("Node %u taking over as MASTER.\n", mesh.getNodeId());
    taskSendSync.enableIfNot();
    patternStartTimeMeshMs = mesh.getNodeTime();
    currentPatternIndex = 0; // Optionally reset pattern when becoming master
    sendSyncPacket();
  } else if (!amIMaster && oldMasterState) {
    Serial.printf("Node %u relinquishing MASTER role.\n", mesh.getNodeId());
    taskSendSync.disable();
  } else if (amIMaster && oldMasterState) {
     Serial.printf("Node %u confirmed as MASTER.\n", mesh.getNodeId());
  }
}

void receivedCallback(uint32_t from, String &msg) {
  if (amIMaster) return;

  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, msg);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  byte receivedPatternIndex = doc["p"];
  uint32_t receivedPatternStartTimeMeshUs = doc["t"]; // This is in MICROseconds

  if (receivedPatternIndex != currentPatternIndex || receivedPatternStartTimeMeshUs != patternStartTimeMeshMs) {
      currentPatternIndex = receivedPatternIndex;
      patternStartTimeMeshMs = receivedPatternStartTimeMeshUs;
      uint32_t timeSinceMasterStartMs = (mesh.getNodeTime() - patternStartTimeMeshMs) / 1000;

      Serial.printf("SLAVE %u: Synced. Pattern: %d, Master StartTime (us): %u. (Master started %u ms ago)\n",
                    mesh.getNodeId(), currentPatternIndex, patternStartTimeMeshMs, timeSinceMasterStartMs);
  }
}

void newConnectionCallback(uint32_t nodeId) {
  Serial.printf("--> New Connection, nodeId = %u\n", nodeId);
  Serial.printf("Node list: %s\n", mesh.subConnectionJson().c_str());
  determineMasterRole();
  if (amIMaster) {
    sendSyncPacket();
  }
}

void changedConnectionCallback() {
  Serial.printf("Changed connections %s\n", mesh.subConnectionJson().c_str());
  determineMasterRole();
  if(amIMaster && mesh.isRoot()) { // Check if still root if that's the criteria
    Serial.println("Connections changed, I am master, sending sync packet.");
    // Consider if patternStartTimeMeshMs needs reset. If it's based on its own clock, it's fine.
    // If it just became master, determineMasterRole would have reset it.
    sendSyncPacket();
  }
}

void nodeTimeAdjustedCallback(int32_t offset) { // Offset is in microseconds
  Serial.printf("Time Adjusted by offset %dus. Current Node Time (us): %u\n", offset, mesh.getNodeTime());
  if (amIMaster) {
      // Adjust patternStartTimeMeshMs to keep the pattern phase consistent with the new time
      // This is important because patternStartTimeMeshMs is based on the master's mesh.getNodeTime()
      patternStartTimeMeshMs = patternStartTimeMeshMs - offset; // If clock moved forward (offset>0), start time was effectively earlier
      Serial.printf("Master: Adjusted patternStartTimeMeshUs to %u due to time sync.\n", patternStartTimeMeshMs);

      if (abs(offset) > 500000) { // If adjustment is more than 0.5s (500,000 us)
         Serial.println("Master: Large time adjustment, resending sync packet with new base time.");
         sendSyncPacket();
      }
  }
  // For slaves, their patternStartTimeMeshMs is the master's time, so it's already on the mesh timeline.
  // Their local mesh.getNodeTime() is now corrected, so comparisons will be more accurate.
}