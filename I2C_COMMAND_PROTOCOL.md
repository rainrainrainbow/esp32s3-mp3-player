# I2C命令通道协议文档

## 概述

本系统扩展了I2C通信协议，在原有的MP3播放控制基础上，新增了一个命令通道寄存器，用于ESP32-S3触控屏向Arduino Uno发送任务执行指令。

## 硬件拓扑

```
Arduino Uno (I2C Master, 5V逻辑)
├── SDA/SCL ── 巡线模块 (地址0x78)
├── SDA/SCL ── 超声波模块 (I2C版本)
└── SDA/SCL ── ESP32-S3 (地址0x52, 3.3V逻辑, 需电平转换)
```

## 寄存器映射

| 地址 | 方向 | 功能 | 说明 |
|------|------|------|------|
| 0x01 | Uno→S3 | MP3播放 | 写入曲目号(1-255)触发播放 |
| 0x02 | S3→Uno | 播放状态 | 读取状态: 0=停止, 1=播放, 2=暂停 |
| 0x03 | S3→Uno | 当前曲目 | 读取当前播放的曲目号 |
| 0x04 | 双向 | 音量 | 0-100 |
| 0x05 | 双向 | 亮度 | 0-100 |
| **0x10** | **S3→Uno** | **命令通道** | **新增：任务控制命令** |

## 命令码定义

| 命令码 | 含义 | 动作 |
|--------|------|------|
| 0x00 | 无命令 | 空闲状态 |
| 0xA1 | 执行任务表1 | Uno启动task_actions1序列 |
| 0xA2 | 执行任务表2 | Uno启动task_actions2序列 |
| 0xAB | 中止任务 | 停止当前任务，返回APP模式 |

## 通信流程

### 触控按钮触发任务

```
1. 用户点击ESP32-S3屏幕上的"TASK 1"按钮
2. ESP32-S3设置命令寄存器: g_cmd_to_uno = 0xA1
3. Arduino Uno每20ms轮询一次命令寄存器
4. Uno读取到0xA1，调用StartTable(1)
5. ESP32-S3在500ms后自动清除命令（防止重复触发）
```

### I2C读取时序

```cpp
// Arduino Uno端轮询代码
void PollSlaveCommand(void) {
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < 20) return;  // 20ms间隔
  lastPoll = millis();
  
  Wire.beginTransmission(0x52);
  Wire.write(0x10);              // 指向命令寄存器
  byte err = Wire.endTransmission(false);
  if (err != 0) return;          // 设备未响应
  
  Wire.requestFrom(0x52, 1);
  if (Wire.available()) {
    uint8_t cmd = Wire.read();
    // 处理命令...
  }
}
```

## 任务表结构

### 任务表1 (task_actions1)

原有的完整巡线任务序列，包含9个音乐播放点，总时长约2-3分钟。

```cpp
const unsigned long task_actions1[] = {
  0,                              // 索引0保留
  TASK(17500 | SKIP_TRACKING, ACT_PLAY_SOUND, 1),  // 播放第1首
  TASK(300, ACT_FWD, SEN_NONE),  // 前进300ms
  // ... 更多任务
};
```

### 任务表2 (task_actions2)

新增的简化演示序列：

```cpp
const unsigned long task_actions2[] = {
  0,
  TASK(500, ACT_FWD, SEN_NONE),           // 前进500ms
  TASK(200, ACT_STOP_WAIT, SEN_NONE),     // 停止200ms
  TASK(300, ACT_TL, SEN_NONE),            // 左转300ms
  TASK(1000, ACT_FWD, SEN_NONE),          // 前进1000ms
  TASK(300, ACT_TR, SEN_NONE),            // 右转300ms
  TASK(100, ACT_STOP_WAIT, SEN_NONE),     // 停止100ms
  TASK(500 | SKIP_TRACKING, ACT_PLAY_SOUND, 3),  // 播放第3首
};
```

## 状态机

### Arduino Uno运行模式

```
APP_MODE (空闲)
  ├─ 收到0xA1 → StartTable(1) → TRACKING_MODE
  ├─ 收到0xA2 → StartTable(2) → TRACKING_MODE
  └─ A3按键短按 → StartTable(1) → TRACKING_MODE

TRACKING_MODE (执行任务)
  ├─ 任务完成 → 自动返回APP_MODE
  ├─ 收到0xAB → StopToApp() → APP_MODE
  └─ A3按键短按 → StopToApp() → APP_MODE
```

### LED状态指示

- **绿色** (0,255,0): APP_MODE空闲
- **蓝色** (0,0,255): TRACKING_MODE执行任务
- **红色** (255,0,0): 任务完成/错误

## 边界情况处理

### 1. 任务执行中收到新命令

直接重置状态机，从头执行新任务表：

```cpp
void StartTable(int n) {
  // 重置所有状态
  g_runMode = TRACKING_MODE;
  cross_count = 0;
  substate = SUB_STOP;
  modestate = STATE_TASK_HANDLE;
  // 切换到新任务表...
}
```

### 2. I2C总线冲突

- 巡线模块读取频率：≥20ms
- 命令轮询频率：20ms
- MP3播放命令：按需触发
- 总线上设备：3个（巡线0x78、超声波、S3 0x52）

### 3. 命令丢失

命令寄存器采用边沿触发+自动清除机制：
- 命令写入后立即生效
- 500ms后自动清零
- 防止重复触发

## 文件修改清单

### ESP32-S3端

| 文件 | 修改内容 |
|------|----------|
| `main/i2c_slave.h` | 新增REG_COMMAND(0x10)和命令码定义 |
| `main/i2c_slave.c` | 添加命令寄存器读写逻辑、自动清除机制 |
| `main/ui_main.h` | 新增任务按钮回调接口 |
| `main/ui_main.c` | 添加TASK1/TASK2按钮UI |
| `main/main.c` | 注册任务按钮回调 |

### Arduino Uno端

| 文件 | 修改内容 |
|------|----------|
| `app_control_v4.7.ino` | 完整改造：双任务表、I2C轮询、状态机重构 |

## 编译与烧录

### ESP32-S3

```bash
# GitHub Actions自动编译
# 产物：esp32s3_mp3_player.bin

# 本地编译
cd esp32s3-mp3-player
idf.py build

# 烧录
esptool.py --chip esp32s3 write_flash -z 0x10000 build/esp32s3_mp3_player.bin
```

### Arduino Uno

使用Arduino IDE打开`app_control_v4.7.ino`，直接编译上传。

## 测试方法

1. **触控按钮测试**
   - 点击屏幕"TASK 1"按钮
   - 观察Arduino串口输出：`CMD: Execute Table 1`
   - 观察LED变为蓝色

2. **中止测试**
   - 任务执行中点击屏幕任意位置返回菜单
   - 或按A3物理按键
   - 观察LED变为绿色

3. **I2C总线测试**
   - 使用逻辑分析仪抓取SDA/SCL波形
   - 验证轮询间隔和命令传输时序

## 版本历史

- **V4.7** (2026-08-27): 新增I2C命令通道，支持双任务表
- **V4.6**: 硬件PWM + MP3播放器集成
- **V4.5**: 基础巡线功能

## 许可证

MIT License
