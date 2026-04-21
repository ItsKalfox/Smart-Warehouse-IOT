/*
 * ESP32 Robot — WiFi Control + Serial Monitor Logging
 * Fork uses ONLY Servo 2 (pin 19)
 * Fork DOWN: Servo2=0
 * Fork UP:   Servo2=180
 */

#include <ESP32Servo.h>
#include <WiFi.h>
#include <WebServer.h>

// ========== WIFI CREDENTIALS ==========
const char* ssid     = "D's";
const char* password = "00000000";

WebServer server(80);

// ========== PIN DEFINITIONS ==========
#define MOTOR_RIGHT_IN1   25
#define MOTOR_RIGHT_IN2   26
#define MOTOR_LEFT_IN3    27
#define MOTOR_LEFT_IN4    14
#define MOTOR_RIGHT_ENA   32
#define MOTOR_LEFT_ENB    33

#define SERVO_MG996_2     19
#define SERVO_SG90_STEER  23

#define ENCODER_RIGHT     34
#define ENCODER_LEFT      35

// ========== PWM CONFIG ==========
#define PWM_FREQ   1000
#define PWM_RES    8

// ========== ENCODER + ROBOT CONFIG ==========
#define TICKS_PER_REV   20
#define WHEEL_DIAMETER  65.0
#define WHEEL_BASE      120.0

float wheelCircumference = PI * WHEEL_DIAMETER;

// ========== SERVOS ==========
Servo servoMG996_2;
Servo servoSG90_Steer;

int forkAngle = 0;   // starts DOWN

// ========== ENCODERS ==========
volatile long encoderRightCount = 0;
volatile long encoderLeftCount  = 0;

// ========== POSITION ==========
float posX    = 0.0;
float posY    = 0.0;
float heading = 0.0;

long lastLeftTicks  = 0;
long lastRightTicks = 0;

unsigned long logIndex = 0;

// ========== INTERRUPTS ==========
void IRAM_ATTR encoderRightISR() { encoderRightCount++; }
void IRAM_ATTR encoderLeftISR()  { encoderLeftCount++;  }

// ========================================================
// POSITION UPDATE
// ========================================================

void updatePosition() {
  portDISABLE_INTERRUPTS();
  long currentLeft  = encoderLeftCount;
  long currentRight = encoderRightCount;
  portENABLE_INTERRUPTS();

  long dLeft  = currentLeft  - lastLeftTicks;
  long dRight = currentRight - lastRightTicks;

  lastLeftTicks  = currentLeft;
  lastRightTicks = currentRight;

  float distLeft  = (dLeft  / (float)TICKS_PER_REV) * wheelCircumference;
  float distRight = (dRight / (float)TICKS_PER_REV) * wheelCircumference;

  float distance = (distLeft + distRight) / 2.0;
  float dTheta   = (distRight - distLeft) / WHEEL_BASE;

  heading += dTheta;
  posX    += distance * cos(heading);
  posY    += distance * sin(heading);
}

// ========================================================
// FORK CONTROL — Servo 2 only
// ========================================================

void moveForkTo(int target, int stepDelay = 15) {
  target = constrain(target, 0, 180);
  int step = (target > forkAngle) ? 1 : -1;

  while (forkAngle != target) {
    forkAngle += step;
    servoMG996_2.write(forkAngle);
    delay(stepDelay);
  }

  Serial.print(">>> Fork done → Servo2: ");
  Serial.print(forkAngle);
  Serial.println("°");
}

void forkDown() { moveForkTo(0);   }   // Servo2=0
void forkUp()   { moveForkTo(180); }   // Servo2=180

// ========================================================
// MOTOR CONTROL
// ========================================================

void setMotorSpeed(int rightSpeed, int leftSpeed) {
  digitalWrite(MOTOR_RIGHT_IN1, rightSpeed >= 0 ? LOW  : HIGH);
  digitalWrite(MOTOR_RIGHT_IN2, rightSpeed >= 0 ? HIGH : LOW);
  digitalWrite(MOTOR_LEFT_IN3,  leftSpeed  >= 0 ? HIGH : LOW);
  digitalWrite(MOTOR_LEFT_IN4,  leftSpeed  >= 0 ? LOW  : HIGH);
  ledcWrite(MOTOR_RIGHT_ENA, abs(rightSpeed));
  ledcWrite(MOTOR_LEFT_ENB,  abs(leftSpeed));
}

void stopMotors() { setMotorSpeed(0, 0); }

// ========================================================
// LOGGING
// ========================================================

void logAll(const char* label) {
  portDISABLE_INTERRUPTS();
  long rightSnap = encoderRightCount;
  long leftSnap  = encoderLeftCount;
  portENABLE_INTERRUPTS();

  int steer = servoSG90_Steer.read();

  Serial.print(logIndex++);  Serial.print(",");
  Serial.print(label);       Serial.print(",");
  Serial.print(leftSnap);    Serial.print(",");
  Serial.print(rightSnap);   Serial.print(",");
  Serial.print(steer);       Serial.print(",");
  Serial.print(forkAngle);   Serial.print(",");
  Serial.print(posX, 1);     Serial.print(",");
  Serial.print(posY, 1);     Serial.print(",");
  Serial.println(heading * 180.0 / PI, 1);
}

void logWaypoint(const char* name) {
  Serial.print(">>> WAYPOINT: ");
  Serial.println(name);
  logAll(name);
  Serial.println("---");
}

// ========================================================
// WEB PAGE
// ========================================================

const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Robot Control</title>
<style>
  body { font-family: sans-serif; text-align: center; background: #1a1a1a; color: white; margin: 0; padding: 20px; }
  h2 { color: #4CAF50; }
  .btn {
    display: inline-block;
    padding: 20px 30px;
    margin: 8px;
    font-size: 18px;
    font-weight: bold;
    border: none;
    border-radius: 12px;
    cursor: pointer;
    width: 120px;
  }
  .drive { background: #2196F3; color: white; }
  .drive:active { background: #0b7dda; }
  .fork  { background: #FF9800; color: white; }
  .fork:active  { background: #e68a00; }
  .stop  { background: #f44336; color: white; }
  .stop:active  { background: #d32f2f; }
  .green { background: #4CAF50; color: white; }
  .green:active { background: #388E3C; }
  .row { margin: 6px 0; }
  #status {
    margin-top: 20px;
    padding: 12px;
    background: #2a2a2a;
    border-radius: 8px;
    font-size: 14px;
    text-align: left;
    line-height: 1.8;
  }
</style>
</head>
<body>
<h2>Robot Control</h2>

<div class="row">
  <button class="btn drive"
    ontouchstart="sendCmd('fwd')" ontouchend="sendCmd('stop')"
    onmousedown="sendCmd('fwd')"  onmouseup="sendCmd('stop')">FWD</button>
</div>
<div class="row">
  <button class="btn drive"
    ontouchstart="sendCmd('left')" ontouchend="sendCmd('stop')"
    onmousedown="sendCmd('left')"  onmouseup="sendCmd('stop')">LEFT</button>
  <button class="btn stop" onclick="sendCmd('stop')">STOP</button>
  <button class="btn drive"
    ontouchstart="sendCmd('right')" ontouchend="sendCmd('stop')"
    onmousedown="sendCmd('right')"  onmouseup="sendCmd('stop')">RIGHT</button>
</div>
<div class="row">
  <button class="btn drive"
    ontouchstart="sendCmd('bwd')" ontouchend="sendCmd('stop')"
    onmousedown="sendCmd('bwd')"  onmouseup="sendCmd('stop')">BWD</button>
</div>

<div class="row" style="margin-top:20px">
  <button class="btn fork" onclick="sendCmd('up')">FORK UP</button>
  <button class="btn fork" onclick="sendCmd('down')">FORK DOWN</button>
</div>

<div class="row" style="margin-top:20px">
  <button class="btn green" onclick="sendCmd('waypoint')">SAVE WAYPOINT</button>
  <button class="btn green" onclick="sendCmd('reset')">RESET POS</button>
</div>

<div id="status">Connecting...</div>

<script>
function sendCmd(cmd) {
  fetch('/cmd?v=' + cmd)
    .then(r => r.text())
    .then(t => document.getElementById('status').innerHTML = t);
}
setInterval(() => {
  fetch('/pos')
    .then(r => r.text())
    .then(t => document.getElementById('status').innerHTML = t);
}, 300);
</script>
</body>
</html>
)rawliteral";

// ========================================================
// WEB SERVER HANDLERS
// ========================================================

int driveSpeed  = 150;
bool driving    = false;
int waypointNum = 0;

void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

void handleCmd() {
  String cmd = server.arg("v");

  if (cmd == "fwd") {
    servoSG90_Steer.write(90);
    setMotorSpeed(driveSpeed, driveSpeed);
    driving = true;
    server.send(200, "text/plain", "Moving FORWARD");

  } else if (cmd == "bwd") {
    servoSG90_Steer.write(90);
    setMotorSpeed(-driveSpeed, -driveSpeed);
    driving = true;
    server.send(200, "text/plain", "Moving BACKWARD");

  } else if (cmd == "left") {
    servoSG90_Steer.write(60);
    setMotorSpeed(driveSpeed, driveSpeed);
    driving = true;
    server.send(200, "text/plain", "Turning LEFT");

  } else if (cmd == "right") {
    servoSG90_Steer.write(120);
    setMotorSpeed(driveSpeed, driveSpeed);
    driving = true;
    server.send(200, "text/plain", "Turning RIGHT");

  } else if (cmd == "stop") {
    stopMotors();
    servoSG90_Steer.write(90);
    driving = false;
    logAll("stop");
    server.send(200, "text/plain", "STOPPED");

  } else if (cmd == "up") {
    stopMotors();
    driving = false;
    forkUp();
    logWaypoint("FORK_UP");
    server.send(200, "text/plain", "Fork UP");

  } else if (cmd == "down") {
    stopMotors();
    driving = false;
    forkDown();
    logWaypoint("FORK_DOWN");
    server.send(200, "text/plain", "Fork DOWN");

  } else if (cmd == "waypoint") {
    stopMotors();
    driving = false;
    String name = "WP" + String(waypointNum++);
    logWaypoint(name.c_str());
    server.send(200, "text/plain", "Saved: " + name);

  } else if (cmd == "reset") {
    posX = 0; posY = 0; heading = 0;
    lastLeftTicks = 0; lastRightTicks = 0;
    encoderRightCount = 0; encoderLeftCount = 0;
    logIndex = 0;
    Serial.println(">>> Reset to X:0 Y:0 H:0");
    server.send(200, "text/plain", "Position reset to 0,0,0");
  }
}

void handlePos() {
  portDISABLE_INTERRUPTS();
  long r = encoderRightCount;
  long l = encoderLeftCount;
  portENABLE_INTERRUPTS();

  String pos = "X: "       + String(posX, 1)                  + " mm<br>";
  pos += "Y: "             + String(posY, 1)                  + " mm<br>";
  pos += "Heading: "       + String(heading * 180.0 / PI, 1)  + " deg<br>";
  pos += "Fork (Servo2): " + String(forkAngle)                + " deg<br>";
  pos += "Ticks L: "       + String(l) + "  R: " + String(r);
  server.send(200, "text/plain", pos);
}

// ========================================================
// SETUP
// ========================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Servos first
  servoMG996_2.attach(SERVO_MG996_2);
  servoSG90_Steer.attach(SERVO_SG90_STEER);

  forkAngle = 90;
  servoMG996_2.write(90);
  servoSG90_Steer.write(90);
  delay(500);

  forkDown();   // sweep to idle (Servo2=0)
  delay(200);

  // Motors
  pinMode(MOTOR_RIGHT_IN1, OUTPUT);
  pinMode(MOTOR_RIGHT_IN2, OUTPUT);
  pinMode(MOTOR_LEFT_IN3,  OUTPUT);
  pinMode(MOTOR_LEFT_IN4,  OUTPUT);

  ledcAttach(MOTOR_RIGHT_ENA, PWM_FREQ, PWM_RES);
  ledcAttach(MOTOR_LEFT_ENB,  PWM_FREQ, PWM_RES);

  // Encoders
  pinMode(ENCODER_RIGHT, INPUT);
  pinMode(ENCODER_LEFT,  INPUT);

  attachInterrupt(digitalPinToInterrupt(ENCODER_RIGHT), encoderRightISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(ENCODER_LEFT),  encoderLeftISR,  FALLING);

  // WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! Open browser: http://");
  Serial.println(WiFi.localIP());

  server.on("/",    handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/pos", handlePos);
  server.begin();

  Serial.println("=== WIFI ROBOT READY ===");
  Serial.println("index,label,leftTicks,rightTicks,steer,fork,X,Y,heading");
  Serial.println("========================");

  logWaypoint("START");
}

// ========================================================
// LOOP
// ========================================================

void loop() {
  server.handleClient();

  if (driving) {
    updatePosition();
  }

  // Serial commands still work too
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("w,")) {
      logWaypoint(cmd.substring(2).c_str());
    } else if (cmd == "r") {
      logAll("READ");
    } else if (cmd == "reset") {
      posX = 0; posY = 0; heading = 0;
      lastLeftTicks = 0; lastRightTicks = 0;
      encoderRightCount = 0; encoderLeftCount = 0;
      logIndex = 0;
      Serial.println(">>> Reset to X:0 Y:0 H:0");
    }
  }
}