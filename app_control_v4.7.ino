/** 
 * @file app_control.ino 
 * @brief APP遥控 + 高级自定义指令巡线 (V4.7 Hardware PWM + Dual Task Tables + I2C Command Polling)
 * @version V4.7 
 */

#include <Wire.h>
#include <Arduino.h>
#include "FastLED.h"
#include "Ultrasound.h"

#define SENSOR_DIR_MODE 0
float g_tracking_speed_ratio = 0.8;
unsigned long g_return_time = 300;
unsigned long g_turn_time = 700;
unsigned long g_u_turn_time = 2500;
unsigned long g_u_turn_back_time = 1500;
int g_brake_time = 80;

#define ACT_FWD 0x00
#define ACT_BWD 0x01
#define ACT_TL 0x02
#define ACT_TR 0x03
#define ACT_SPIN_L 0x04
#define ACT_SPIN_R 0x05
#define ACT_STOP_WAIT 0x06
#define ACT_ALIGN_L 0x0C
#define ACT_ALIGN_R 0x0D
#define ACT_SHIFT_L 0x0E
#define ACT_SHIFT_R 0x0F
#define ACT_PLAY_SOUND 0x10
#define ACT_MODE_0 0x07

#define SEN_NONE 0x00
#define SEN_S0 0x01
#define SEN_S1 0x02
#define SEN_S2 0x04
#define SEN_S3 0x08
#define SEN_ANY 0x0F
#define SEN_LEFT 0x03
#define SEN_RIGHT 0x0C
#define SEN_CENTER 0x06

#define TASK(T, A, P) (((unsigned long)(T) << 16) | ((P) << 8) | (A))
#define SKIP_TRACKING 0x8000
#define FIND_ALIGN_SPEED 0.45

const unsigned long task_actions1[] = {
  0,
  TASK(17500 | SKIP_TRACKING, ACT_PLAY_SOUND, 1),
  TASK(300, ACT_FWD, SEN_NONE),
  TASK(0, ACT_MODE_0, SEN_NONE),
  TASK(4800, ACT_TR, SEN_LEFT),
  TASK(300 | SKIP_TRACKING, ACT_TR, SEN_NONE),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(11500 | SKIP_TRACKING, ACT_PLAY_SOUND, 2),
  TASK(4500 | SKIP_TRACKING, ACT_BWD, SEN_S0),
  TASK(2700, ACT_SPIN_L, SEN_CENTER),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(300 | SKIP_TRACKING, ACT_TR, SEN_NONE),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(13500 | SKIP_TRACKING, ACT_PLAY_SOUND, 3),
  TASK(4500 | SKIP_TRACKING, ACT_BWD, SEN_S0),
  TASK(2700, ACT_SPIN_L, SEN_CENTER),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(300 | SKIP_TRACKING, ACT_TR, SEN_NONE),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(13500 | SKIP_TRACKING, ACT_PLAY_SOUND, 4),
  TASK(4500 | SKIP_TRACKING, ACT_BWD, SEN_S0),
  TASK(2700, ACT_SPIN_L, SEN_CENTER),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(300 | SKIP_TRACKING, ACT_TR, SEN_NONE),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(13500 | SKIP_TRACKING, ACT_PLAY_SOUND, 5),
  TASK(4500 | SKIP_TRACKING, ACT_BWD, SEN_S0),
  TASK(2700, ACT_SPIN_L, SEN_CENTER),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(300 | SKIP_TRACKING, ACT_TR, SEN_NONE),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(13500 | SKIP_TRACKING, ACT_PLAY_SOUND, 6),
  TASK(4500 | SKIP_TRACKING, ACT_BWD, SEN_S0),
  TASK(2700, ACT_SPIN_L, SEN_CENTER),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(300 | SKIP_TRACKING, ACT_TR, SEN_NONE),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(13500 | SKIP_TRACKING, ACT_PLAY_SOUND, 7),
  TASK(4500 | SKIP_TRACKING, ACT_BWD, SEN_S0),
  TASK(2700, ACT_SPIN_L, SEN_CENTER),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(300 | SKIP_TRACKING, ACT_TR, SEN_NONE),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(13500 | SKIP_TRACKING, ACT_PLAY_SOUND, 8),
  TASK(4500 | SKIP_TRACKING, ACT_BWD, SEN_S0),
  TASK(2700, ACT_SPIN_L, SEN_CENTER),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(300 | SKIP_TRACKING, ACT_TR, SEN_NONE),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(16500 | SKIP_TRACKING, ACT_PLAY_SOUND, 9),
  TASK(4500 | SKIP_TRACKING, ACT_BWD, SEN_S0),
  TASK(2700, ACT_SPIN_L, SEN_CENTER),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(2800, ACT_TR, SEN_LEFT),
  TASK(100, ACT_FWD, SEN_NONE),
};
const int g_total_tasks1 = sizeof(task_actions1) / sizeof(task_actions1[0]) - 1;

const unsigned long task_actions2[] = {
  0,
  TASK(500, ACT_FWD, SEN_NONE),
  TASK(200, ACT_STOP_WAIT, SEN_NONE),
  TASK(300, ACT_TL, SEN_NONE),
  TASK(1000, ACT_FWD, SEN_NONE),
  TASK(300, ACT_TR, SEN_NONE),
  TASK(100, ACT_STOP_WAIT, SEN_NONE),
  TASK(500 | SKIP_TRACKING, ACT_PLAY_SOUND, 3),
};
const int g_total_tasks2 = sizeof(task_actions2) / sizeof(task_actions2[0]) - 1;

const unsigned long* active_table = task_actions1;
int active_table_id = 1;
int active_total_tasks = g_total_tasks1;

int g_turn_kp_small = 50;
int g_turn_kp_big = 65;

#define NOTONE 0
typedef enum { MODE_NONE, MODE_ROCKERANDGRAVITY, MODE_RGB_ADJUST, MODE_SPEED_CONTROL, MODE_ULTRASOUND_SEND, MODE_VOLTAGE_SEND, MODE_AVOID } CarMode;
typedef enum { APP_MODE, TRACKING_MODE } RunMode;
typedef enum { WARNING_OFF, WARNING_BEEP, WARNING_RGB } VoltageWarning;
typedef enum { STATE_TRACKING, STATE_CROSS_OVER, STATE_TASK_HANDLE, STATE_CORNER_TURNING } TrackingState;
typedef enum { SUB_STOP, SUB_TURNING, SUB_BRAKE } SubTaskState;

Ultrasound ultrasound;
static VoltageWarning g_warning = WARNING_OFF;
static CarMode g_mode = MODE_NONE;
static RunMode g_runMode = APP_MODE;
static uint8_t g_state = 8;
static uint8_t avoid_flag = 0;
static uint8_t rot_flag = 0;
static int8_t car_rot = 0;
static uint8_t speed_update = 55;
static float voltage;
static int voltage_send, last_voltage_send = 8000, real_voltage_send = 8000, error_voltage;
static CRGB rgbs[1];
String rec_data[4];
const static uint8_t ledPin = 2;
const static uint8_t buzzerPin = 3;
const static uint8_t keyPin = A3;
const static uint8_t motorpwmPin[4] = { 10, 9, 6, 11 };
const static uint8_t motordirectionPin[4] = { 12, 8, 7, 13 };
static uint16_t distance = 0;
static TrackingState modestate = STATE_TASK_HANDLE;
static SubTaskState substate = SUB_STOP;
static uint8_t tracking_data;
static uint8_t tracking_rec_data[5];
static unsigned long stateStartTime = 0;
static int cross_count = 0;
static bool lastKeyState = false;
static unsigned long lastKeyDebounceTime = 0;
static int currentA3Value = 0;
static uint32_t prevVoltageTime = 0;
static uint16_t target_angle = 0;
static uint8_t target_velocity = 0;
static int8_t target_rot = 0;

#define LINE_FOLLOWER_I2C_ADDR 0x78
#define MP3_PLAYER_I2C_ADDR 0x52
#define CMD_TASK1 0xA1
#define CMD_TASK2 0xA2
#define CMD_STOP  0xAB

void Key_Control_Task(void);
void Voltage_Detection(void);
void Parse_Serial_NonBlocking(void);
void Execute_Current_Mode(void);
void Sensor_Receive(void);
void Tracking_Line_Task(void);
void Cross_Over_Task(void);
void Task_Handle_Task(void);
void Corner_Turning_Task(void);
int GetCurrentSpeed(void);
void Motor_Init(void);
void Velocity_Controller(uint16_t angle, uint8_t velocity, int8_t rot);
void Motors_Set(int8_t Motor_0, int8_t Motor_1, int8_t Motor_2, int8_t Motor_3);
void PWM_Out_Hardware(uint8_t PWM_Pin, int8_t DutyCycle);
void Rgb_Show(uint8_t rValue, uint8_t gValue, uint8_t bValue);
bool WireWriteByte(uint8_t val);
bool WireReadDataByte(uint8_t reg, uint8_t &val);
void MP3_Play(uint8_t track);
void Rockerandgravity_Task(void);
void Speed_Task(void);
void Avoid_Obstacle(void);
void Rgb_Task(void);
void StartTable(int n);
void StopToApp(void);
void PollSlaveCommand(void);

void setup() {
  Serial.begin(9600);
  FastLED.addLeds<WS2812, ledPin, RGB>(rgbs, 1);
  Wire.begin();
  Motor_Init();
#if NOTONE
  tone(buzzerPin, 1200); delay(100); noTone(buzzerPin);
#endif
  Serial.println("System Ready V4.7");
}

void loop() {
  currentA3Value = analogRead(A3);
  Key_Control_Task();
  Voltage_Detection();
  Parse_Serial_NonBlocking();
  Execute_Current_Mode();
  PollSlaveCommand();
  if (g_runMode == APP_MODE) {
    Velocity_Controller(target_angle, target_velocity, target_rot);
    if (avoid_flag == 1) Avoid_Obstacle();
  } else if (g_runMode == TRACKING_MODE) {
    Sensor_Receive();
    switch (modestate) {
      case STATE_TRACKING: Tracking_Line_Task(); break;
      case STATE_CROSS_OVER: Cross_Over_Task(); break;
      case STATE_TASK_HANDLE: Task_Handle_Task(); break;
      case STATE_CORNER_TURNING: Corner_Turning_Task(); break;
    }
  }
}

void StartTable(int n) {
  if (n == 1) {
    active_table = task_actions1;
    active_total_tasks = g_total_tasks1;
    active_table_id = 1;
    Serial.println("Start Task Table 1");
  } else if (n == 2) {
    active_table = task_actions2;
    active_total_tasks = g_total_tasks2;
    active_table_id = 2;
    Serial.println("Start Task Table 2");
  }
  g_runMode = TRACKING_MODE;
  target_angle = 0; target_velocity = 0; target_rot = 0;
  Velocity_Controller(0, 0, 0);
  cross_count = 0;
  substate = SUB_STOP;
  modestate = STATE_TASK_HANDLE;
  stateStartTime = millis();
  Rgb_Show(0, 0, 255);
}

void StopToApp(void) {
  Serial.println("Stop Task - Return to APP Mode");
  g_runMode = APP_MODE;
  target_angle = 0; target_velocity = 0; target_rot = 0;
  Velocity_Controller(0, 0, 0);
  Rgb_Show(0, 255, 0);
}

void PollSlaveCommand(void) {
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < 20) return;
  lastPoll = millis();
  Wire.requestFrom(MP3_PLAYER_I2C_ADDR, 1);
  if (Wire.available()) {
    uint8_t cmd = Wire.read();
    switch (cmd) {
      case CMD_TASK1:
        Serial.println("CMD: Execute Table 1");
        StartTable(1);
        break;
      case CMD_TASK2:
        Serial.println("CMD: Execute Table 2");
        StartTable(2);
        break;
      case CMD_STOP:
        Serial.println("CMD: Stop Task");
        StopToApp();
        break;
      default:
        break;
    }
  }
}

void Parse_Serial_NonBlocking(void) {
  static String cmdBuffer = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '$') {
      uint8_t index = 0;
      String tempStr = cmdBuffer;
      while (tempStr.indexOf('|') != -1 && index < 3) {
        rec_data[index] = tempStr.substring(0, tempStr.indexOf('|'));
        tempStr = tempStr.substring(tempStr.indexOf('|') + 1);
        index++;
      }
      rec_data[index] = tempStr;
      for(int i = index + 1; i < 4; i++) rec_data[i] = "";
      if (rec_data[0] == "A" && avoid_flag == 0) { g_runMode = APP_MODE; g_mode = MODE_ROCKERANDGRAVITY; }
      else if (rec_data[0] == "B" && avoid_flag == 0) g_mode = MODE_RGB_ADJUST;
      else if (rec_data[0] == "C" && avoid_flag == 0) g_mode = MODE_SPEED_CONTROL;
      else if (rec_data[0] == "E" && avoid_flag == 0) {
        g_runMode = TRACKING_MODE;
        target_angle = 0; target_velocity = 0; target_rot = 0;
        Velocity_Controller(0, 0, 0);
        modestate = STATE_TASK_HANDLE; cross_count = 0; substate = SUB_STOP;
        stateStartTime = millis();
        Rgb_Show(0, 0, 255);
      } else if (rec_data[0] == "D") g_mode = MODE_ULTRASOUND_SEND;
      else if (rec_data[0] == "F") { g_mode = MODE_AVOID; avoid_flag = 1; g_state = rec_data[1].toInt(); }
      cmdBuffer = "";
    } else { cmdBuffer += c; }
  }
}

void Execute_Current_Mode(void) {
  if (g_mode == MODE_ROCKERANDGRAVITY) { Rockerandgravity_Task(); g_mode = MODE_NONE; }
  else if (g_mode == MODE_RGB_ADJUST) { Rgb_Task(); g_mode = MODE_NONE; }
  else if (g_mode == MODE_SPEED_CONTROL) { Speed_Task(); g_mode = MODE_NONE; }
  else if (g_mode == MODE_ULTRASOUND_SEND) {
    distance = ultrasound.Filter();
    Serial.print("$"); Serial.print(distance); Serial.print(","); Serial.print(real_voltage_send); Serial.print("$");
    g_mode = MODE_NONE;
  }
}

void Key_Control_Task(void) {
  bool currentState = (currentA3Value < 50);
  if (currentState != lastKeyState) lastKeyDebounceTime = millis();
  if ((millis() - lastKeyDebounceTime) < 50) {
    if (currentState != lastKeyState) {
      if (currentState == true) {
#if NOTONE
        tone(buzzerPin, 1000); delay(50); noTone(buzzerPin);
#endif
        if (g_runMode == APP_MODE) { StartTable(1); }
        else { StopToApp(); }
      }
      lastKeyState = currentState;
    }
  }
}

void Voltage_Detection(void) {
  if (millis() - prevVoltageTime < 100) return;
  prevVoltageTime = millis();
  if (currentA3Value < 50) return;
  voltage = currentA3Value * 0.02989;
  voltage_send = (int)(voltage * 1000);
  if (last_voltage_send - voltage_send >= 500) error_voltage = voltage_send;
  if (voltage_send != error_voltage) real_voltage_send = voltage_send;
  last_voltage_send = voltage_send;
  if (real_voltage_send <= 7000) { if (g_warning != WARNING_RGB) g_warning = WARNING_BEEP; }
  if (g_warning == WARNING_BEEP) {
    unsigned long now = millis();
#if NOTONE
    if (now % 1000 < 500) tone(buzzerPin, 800); else noTone(buzzerPin);
#endif
  }
  if (g_warning == WARNING_RGB) Rgb_Show(0, 10, 0);
}

void Sensor_Receive(void) {
  WireReadDataByte(1, tracking_data);
  tracking_rec_data[0] = tracking_data & 0x01;
  tracking_rec_data[1] = (tracking_data >> 1) & 0x01;
  tracking_rec_data[2] = (tracking_data >> 2) & 0x01;
  tracking_rec_data[3] = (tracking_data >> 3) & 0x01;
}

int GetCurrentSpeed() { return (int)(speed_update * g_tracking_speed_ratio); }

void Tracking_Line_Task(void) {
  if (cross_count > active_total_tasks) {
    Velocity_Controller(0, 0, 0);
    g_runMode = APP_MODE; modestate = STATE_TASK_HANDLE; Rgb_Show(255, 0, 0);
    return;
  }
  Rgb_Show(0, 255, 255);
  int current_speed = GetCurrentSpeed();
  uint8_t sum = tracking_rec_data[0] + tracking_rec_data[1] + tracking_rec_data[2] + tracking_rec_data[3];
  int s_left_out = (SENSOR_DIR_MODE == 0) ? tracking_rec_data[0] : tracking_rec_data[3];
  int s_right_out = (SENSOR_DIR_MODE == 0) ? tracking_rec_data[3] : tracking_rec_data[0];
  if (sum >= 3 || s_left_out || s_right_out) {
    Velocity_Controller(0, 0, 0);
    modestate = STATE_CROSS_OVER; stateStartTime = millis();
    return;
  }
  int8_t rotation = 0;
  if (tracking_rec_data[1] && tracking_rec_data[2]) rotation = 0;
  else if (tracking_rec_data[1] && !tracking_rec_data[2]) rotation = g_turn_kp_small;
  else if (!tracking_rec_data[1] && tracking_rec_data[2]) rotation = -g_turn_kp_small;
  if (SENSOR_DIR_MODE == 0) {
    if (tracking_rec_data[0] && !tracking_rec_data[1]) rotation = g_turn_kp_big;
    else if (!tracking_rec_data[2] && tracking_rec_data[3]) rotation = -g_turn_kp_big;
  } else {
    if (tracking_rec_data[3] && !tracking_rec_data[1]) rotation = g_turn_kp_big;
    else if (!tracking_rec_data[2] && tracking_rec_data[0]) rotation = -g_turn_kp_big;
  }
  Velocity_Controller(0, current_speed, rotation);
}

void Cross_Over_Task(void) {
  int current_speed = GetCurrentSpeed();
  if (millis() - stateStartTime < 200) {
    Velocity_Controller(0, current_speed, 0);
  } else {
    modestate = STATE_TASK_HANDLE; substate = SUB_STOP; stateStartTime = millis();
  }
}

void Task_Handle_Task(void) {
  int current_speed = GetCurrentSpeed();
  int current_cross_index = cross_count + 1;
  if (current_cross_index >= active_total_tasks + 1) {
    Velocity_Controller(0, 0, 0);
    g_runMode = APP_MODE; modestate = STATE_TRACKING; Rgb_Show(255, 0, 0);
    return;
  }
  unsigned long command = active_table[current_cross_index];
  unsigned long time_with_flag = (command >> 16) & 0xFFFF;
  bool skip_tracking = (time_with_flag & 0x8000) != 0;
  unsigned long time_limit = time_with_flag & 0x7FFF;
  uint8_t param_P = (command >> 8) & 0xFF;
  uint8_t action_code = command & 0xFF;
  if (action_code == ACT_MODE_0) {
    cross_count++;
    modestate = STATE_TRACKING; substate = SUB_STOP;
    return;
  }
  if (substate == SUB_STOP) {
    substate = SUB_TURNING; stateStartTime = millis();
    if (action_code == ACT_PLAY_SOUND) { MP3_Play(param_P); }
  }
  unsigned long elapsedTime = millis() - stateStartTime;
  if (substate == SUB_BRAKE) {
    if (elapsedTime < g_brake_time) {
      switch (action_code) {
        case ACT_ALIGN_L: Velocity_Controller(0, 0, (int8_t)(-current_speed * FIND_ALIGN_SPEED)); break;
        case ACT_ALIGN_R: Velocity_Controller(0, 0, (int8_t)(current_speed * FIND_ALIGN_SPEED)); break;
        case ACT_SHIFT_L: Velocity_Controller(270, (int8_t)(current_speed * FIND_ALIGN_SPEED), 0); break;
        case ACT_SHIFT_R: Velocity_Controller(90, (int8_t)(current_speed * FIND_ALIGN_SPEED), 0); break;
        default: Velocity_Controller(0, 0, 0); break;
      }
    } else {
      Velocity_Controller(0, 0, 0);
      cross_count++; substate = SUB_STOP;
      if (cross_count > active_total_tasks) {
        g_runMode = APP_MODE; modestate = STATE_TRACKING; Rgb_Show(255, 0, 0);
      } else {
        modestate = skip_tracking ? STATE_TASK_HANDLE : STATE_TRACKING;
      }
    }
    return;
  }
  switch (action_code) {
    case ACT_FWD: Velocity_Controller(0, current_speed, 0); break;
    case ACT_BWD: Velocity_Controller(180, current_speed, 0); break;
    case ACT_TL: case ACT_SPIN_L: Velocity_Controller(0, 0, current_speed); break;
    case ACT_TR: case ACT_SPIN_R: Velocity_Controller(0, 0, -current_speed); break;
    case ACT_STOP_WAIT: Velocity_Controller(0, 0, 0); break;
    case ACT_ALIGN_L: Velocity_Controller(0, 0, (int8_t)(current_speed * FIND_ALIGN_SPEED)); break;
    case ACT_ALIGN_R: Velocity_Controller(0, 0, (int8_t)(-current_speed * FIND_ALIGN_SPEED)); break;
    case ACT_SHIFT_L: Velocity_Controller(90, (int8_t)(current_speed * FIND_ALIGN_SPEED), 0); break;
    case ACT_SHIFT_R: Velocity_Controller(270, (int8_t)(current_speed * FIND_ALIGN_SPEED), 0); break;
    case ACT_PLAY_SOUND:
      if (skip_tracking) Velocity_Controller(0, 0, 0);
      else Tracking_Line_Task();
      break;
  }
  bool shouldExit = false;
  bool foundLine = false;
  if (action_code == ACT_PLAY_SOUND) {
    if (elapsedTime > time_limit) shouldExit = true;
    if (!skip_tracking) {
      uint8_t sum = tracking_rec_data[0] + tracking_rec_data[1] + tracking_rec_data[2] + tracking_rec_data[3];
      int s_left_out = (SENSOR_DIR_MODE == 0) ? tracking_rec_data[0] : tracking_rec_data[3];
      int s_right_out = (SENSOR_DIR_MODE == 0) ? tracking_rec_data[3] : tracking_rec_data[0];
      if (sum >= 3 || s_left_out || s_right_out) shouldExit = true;
    }
  } else {
    if (elapsedTime > 500 && param_P != SEN_NONE && (tracking_data & param_P) != 0) {
      shouldExit = true; foundLine = true;
    }
    if (elapsedTime > time_limit) shouldExit = true;
  }
  if (shouldExit) {
    bool is_find_line_action = (action_code == ACT_ALIGN_L || action_code == ACT_ALIGN_R || action_code == ACT_SHIFT_L || action_code == ACT_SHIFT_R);
    if (foundLine && is_find_line_action) {
      substate = SUB_BRAKE; stateStartTime = millis();
    } else {
      Velocity_Controller(0, 0, 0);
      cross_count++; substate = SUB_STOP;
      if (cross_count > active_total_tasks) {
        g_runMode = APP_MODE; modestate = STATE_TRACKING; Rgb_Show(255, 0, 0);
      } else {
        modestate = skip_tracking ? STATE_TASK_HANDLE : STATE_TRACKING;
      }
    }
  }
}

void Corner_Turning_Task(void) { /* 保留 */ }

void Rockerandgravity_Task(void) {
  g_state = rec_data[1].toInt();
  switch (g_state) {
    case 0: target_angle = 90; target_velocity = speed_update; target_rot = 0; break;
    case 1: target_angle = 45; target_velocity = speed_update; target_rot = 0; break;
    case 2: target_angle = 0;  target_velocity = speed_update; target_rot = 0; break;
    case 3: target_angle = 315; target_velocity = speed_update; target_rot = 0; break;
    case 4: target_angle = 270; target_velocity = speed_update; target_rot = 0; break;
    case 5: target_angle = 225; target_velocity = speed_update; target_rot = 0; break;
    case 6: target_angle = 180; target_velocity = speed_update; target_rot = 0; break;
    case 7: target_angle = 135; target_velocity = speed_update; target_rot = 0; break;
    case 8: target_angle = 0; target_velocity = 0; target_rot = rot_flag == 1 ? speed_update : (rot_flag == 2 ? -speed_update : 0); break;
    case 9: rot_flag = 1; target_rot = (target_velocity == 0) ? speed_update : speed_update / 3; break;
    case 10: rot_flag = 2; target_rot = (target_velocity == 0) ? -speed_update : -speed_update / 3; break;
    case 11: target_angle = 0; target_rot = 0; rot_flag = 0; break;
  }
}

void Speed_Task(void) {
  speed_update = (uint8_t)rec_data[1].toInt();
  Serial.print("C|"); Serial.print(speed_update); Serial.print("|$");
}

void Avoid_Obstacle(void) {
  distance = ultrasound.Filter();
  if (g_state == 1) {
    if (distance < 400) { target_angle = 0; target_rot = 100; target_velocity = 0; }
    if (distance >= 500) { target_angle = 0; target_rot = 0; target_velocity = 50; }
  } else if (g_state == 0) {
    target_angle = 0; target_rot = 0; target_velocity = 0; g_mode = MODE_NONE; avoid_flag = 0;
  }
}

void Rgb_Task(void) {
  uint8_t r = (uint8_t)rec_data[1].toInt();
  uint8_t g = (uint8_t)rec_data[2].toInt();
  uint8_t b = (uint8_t)rec_data[3].toInt();
  ultrasound.Color(r, g, b, r, g, b);
}

void Motor_Init(void) {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(motordirectionPin[i], OUTPUT);
    pinMode(motorpwmPin[i], OUTPUT);
  }
  Velocity_Controller(0, 0, 0);
}

void Velocity_Controller(uint16_t angle, uint8_t velocity, int8_t rot) {
  int8_t velocity_0, velocity_1, velocity_2, velocity_3;
  angle += 90;
  float rad = angle * PI / 180;
  float trans_x = velocity * sin(rad);
  float trans_y = velocity * cos(rad);
  float rot_f = rot;
  float m0 = trans_x - trans_y + rot_f;
  float m1 = trans_x + trans_y - rot_f;
  float m2 = trans_x - trans_y - rot_f;
  float m3 = trans_x + trans_y + rot_f;
  float max_val = 0;
  if (abs(m0) > max_val) max_val = abs(m0);
  if (abs(m1) > max_val) max_val = abs(m1);
  if (abs(m2) > max_val) max_val = abs(m2);
  if (abs(m3) > max_val) max_val = abs(m3);
  if (max_val > 100) {
    float scale = 100.0 / max_val;
    m0 *= scale; m1 *= scale; m2 *= scale; m3 *= scale;
  }
  Motors_Set((int8_t)m0, (int8_t)m1, (int8_t)m2, (int8_t)m3);
}

void Motors_Set(int8_t Motor_0, int8_t Motor_1, int8_t Motor_2, int8_t Motor_3) {
  int8_t motors[4] = { Motor_0, Motor_1, Motor_2, Motor_3 };
  bool direction[4] = { 1, 0, 0, 1 };
  for (uint8_t i = 0; i < 4; ++i) {
    if (motors[i] < 0) direction[i] = !direction[i];
    digitalWrite(motordirectionPin[i], direction[i]);
    int8_t pwmVal = abs(motors[i]);
    if (pwmVal > 100) pwmVal = 100;
    PWM_Out_Hardware(motorpwmPin[i], pwmVal);
  }
}

void PWM_Out_Hardware(uint8_t PWM_Pin, int8_t DutyCycle) {
  if (DutyCycle <= 0) analogWrite(PWM_Pin, 0);
  else if (DutyCycle >= 100) analogWrite(PWM_Pin, 255);
  else analogWrite(PWM_Pin, (uint8_t)(DutyCycle * 2.55));
}

bool WireWriteByte(uint8_t val) {
  Wire.beginTransmission(LINE_FOLLOWER_I2C_ADDR);
  Wire.write(val);
  return (Wire.endTransmission() == 0);
}

bool WireReadDataByte(uint8_t reg, uint8_t &val) {
  if (!WireWriteByte(reg)) return false;
  Wire.requestFrom(LINE_FOLLOWER_I2C_ADDR, 1);
  if (Wire.available()) val = Wire.read();
  return true;
}

void MP3_Play(uint8_t track) {
  Wire.beginTransmission(MP3_PLAYER_I2C_ADDR);
  Wire.write(0x01);
  Wire.write(track);
  Wire.endTransmission();
}

void Rgb_Show(uint8_t rValue, uint8_t gValue, uint8_t bValue) {
  rgbs[0].r = rValue; rgbs[0].g = gValue; rgbs[0].b = bValue;
  FastLED.show();
}
