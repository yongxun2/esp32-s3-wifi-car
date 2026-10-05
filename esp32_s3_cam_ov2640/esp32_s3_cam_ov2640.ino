/*
 * ESP32-S3-CAM OV2640 + 电机WiFi遥控
 * 核心: FreeRTOS双核并行 - 视频流跑Core0, 电机控制跑Core1, 真正互不阻塞
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

// ====================== 摄像头引脚 ======================
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_D0      11
#define CAM_PIN_D1       9
#define CAM_PIN_D2       8
#define CAM_PIN_D3      10
#define CAM_PIN_D4      12
#define CAM_PIN_D5      18
#define CAM_PIN_D6      17
#define CAM_PIN_D7      16
#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_VYSNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_XCLK    15
#define CAM_PIN_PCLK    13

// ====================== WiFi ======================
// 默认使用 SoftAP 模式: 板子自己发热点 "ESP32-S3-CAR" (无密码), 手机直连后访问 http://192.168.4.1/
// 如需 STA 模式(连家里路由器), 取消下面两行注释并填入自己的 WiFi, 然后在 setup() 里
// 用 WiFi.begin(ssid, password) 替换 WiFi.softAP(...) 那段
// const char* ssid     = "YOUR_WIFI_SSID";
// const char* password = "YOUR_WIFI_PASSWORD";

// 单端口80搞定: WebServer处理主页+电机+拍照, 视频流独立FreeRTOS任务
WebServer server(80);

// ====================== 底盘电机引脚 (TB6612) ======================
#define PWMA_LEFT      42
#define AIN1_LEFT      40
#define AIN2_LEFT      41
#define STBY_CHASSIS   39
#define PWMB_RIGHT     47
#define BIN1_RIGHT     48
#define BIN2_RIGHT     38
const int SPEED_LEFT  = 138;   // 前进后退速度(原110,提高1/4)
const int SPEED_RIGHT = 138;   // 若直行跑偏,按±5微调
const int TURN_SPEED  = 100;   // 原地转弯两轮反向电流大,过高(140)会导致电池掉压堵转哔哔响

// ====================== 云台电机引脚 (GPIO19/20/21, STBY接3.3V) ======================
#define PWMA_YT        19
#define AIN2_YT        20
#define AIN1_YT        21
const int SPEED_YT    = 60;

// ====================== LEDC PWM (电机避开TIMER_1/CHANNEL_1) ======================
#define PWM_CH_LEFT    2
#define PWM_CH_RIGHT   3
#define PWM_CH_YT      4
#define PWM_FREQ       5000
#define PWM_RES        8

void initMotors()
{
    pinMode(AIN1_LEFT,    OUTPUT);
    pinMode(AIN2_LEFT,    OUTPUT);
    pinMode(BIN1_RIGHT,   OUTPUT);
    pinMode(BIN2_RIGHT,   OUTPUT);
    pinMode(STBY_CHASSIS, OUTPUT);
    digitalWrite(STBY_CHASSIS, HIGH);
    pinMode(AIN1_YT, OUTPUT); pinMode(AIN2_YT, OUTPUT);

    ledcSetup(PWM_CH_LEFT,  PWM_FREQ, PWM_RES);
    ledcSetup(PWM_CH_RIGHT, PWM_FREQ, PWM_RES);
    ledcSetup(PWM_CH_YT,    PWM_FREQ, PWM_RES);
    ledcAttachPin(PWMA_LEFT,  PWM_CH_LEFT);
    ledcAttachPin(PWMB_RIGHT, PWM_CH_RIGHT);
    ledcAttachPin(PWMA_YT,    PWM_CH_YT);
    carStop(); ytStop();
    Serial.println("[MOTOR] OK");
}

// ====================== 底盘/云台控制 ======================
void carStop()     { digitalWrite(AIN1_LEFT,LOW);digitalWrite(AIN2_LEFT,LOW);ledcWrite(PWM_CH_LEFT,0);digitalWrite(BIN1_RIGHT,LOW);digitalWrite(BIN2_RIGHT,LOW);ledcWrite(PWM_CH_RIGHT,0);}
void carForward()  { digitalWrite(AIN1_LEFT,HIGH);digitalWrite(AIN2_LEFT,LOW);ledcWrite(PWM_CH_LEFT,SPEED_LEFT);digitalWrite(BIN1_RIGHT,LOW);digitalWrite(BIN2_RIGHT,HIGH);ledcWrite(PWM_CH_RIGHT,SPEED_RIGHT);}
void carBackward() { digitalWrite(AIN1_LEFT,LOW);digitalWrite(AIN2_LEFT,HIGH);ledcWrite(PWM_CH_LEFT,SPEED_LEFT);digitalWrite(BIN1_RIGHT,HIGH);digitalWrite(BIN2_RIGHT,LOW);ledcWrite(PWM_CH_RIGHT,SPEED_RIGHT);}
// 差速转弯: 内侧轮停转,外侧轮前进,电流只有一个电机,不会堵转哔哔响(转弯半径变大)
void carLeft()     { digitalWrite(AIN1_LEFT,LOW);digitalWrite(AIN2_LEFT,LOW);ledcWrite(PWM_CH_LEFT,0);digitalWrite(BIN1_RIGHT,LOW);digitalWrite(BIN2_RIGHT,HIGH);ledcWrite(PWM_CH_RIGHT,SPEED_RIGHT);}
void carRight()    { digitalWrite(AIN1_LEFT,HIGH);digitalWrite(AIN2_LEFT,LOW);ledcWrite(PWM_CH_LEFT,SPEED_LEFT);digitalWrite(BIN1_RIGHT,LOW);digitalWrite(BIN2_RIGHT,LOW);ledcWrite(PWM_CH_RIGHT,0);}
void ytUp()        { digitalWrite(AIN1_YT,LOW);digitalWrite(AIN2_YT,HIGH);ledcWrite(PWM_CH_YT,SPEED_YT);}
void ytDown()      { digitalWrite(AIN1_YT,HIGH);digitalWrite(AIN2_YT,LOW);ledcWrite(PWM_CH_YT,SPEED_YT);}
void ytStop()      { digitalWrite(AIN1_YT,LOW);digitalWrite(AIN2_YT,LOW);ledcWrite(PWM_CH_YT,0);}

// ====================== 摄像头初始化 ======================
bool initCamera()
{
    camera_config_t config = {};
    config.pin_pwdn       = CAM_PIN_PWDN;
    config.pin_reset      = CAM_PIN_RESET;
    config.pin_xclk       = CAM_PIN_XCLK;
    config.pin_sccb_scl   = CAM_PIN_SIOC;
    config.pin_sccb_sda   = CAM_PIN_SIOD;
    config.pin_d7         = CAM_PIN_D7;
    config.pin_d6         = CAM_PIN_D6;
    config.pin_d5         = CAM_PIN_D5;
    config.pin_d4         = CAM_PIN_D4;
    config.pin_d3         = CAM_PIN_D3;
    config.pin_d2         = CAM_PIN_D2;
    config.pin_d1         = CAM_PIN_D1;
    config.pin_d0         = CAM_PIN_D0;
    config.pin_vsync      = CAM_PIN_VYSNC;
    config.pin_href       = CAM_PIN_HREF;
    config.pin_pclk       = CAM_PIN_PCLK;

    config.ledc_timer     = LEDC_TIMER_1;
    config.ledc_channel   = LEDC_CHANNEL_1;
    config.xclk_freq_hz   = 24000000;        // OV2640官方最高外部时钟,采集更快
    config.pixel_format   = PIXFORMAT_JPEG;
    config.frame_size     = FRAMESIZE_VGA;      // 640x480高清
    config.jpeg_quality   = 12;               // 链路充足,降低压缩比换清晰画质
    config.fb_count       = 3;                // 2→3三缓冲: 采集与网络完全解耦,传感器全速跑
    config.fb_location    = CAMERA_FB_IN_PSRAM;
    config.grab_mode      = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port  = 1;

    Serial.println("[CAM] 24MHz VGA(640x480) q=12 fb=3");
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) { Serial.printf("[CAM] FAIL 0x%x\n", err); return false; }
    Serial.println("[CAM] OK");
    return true;
}

// ====================== FreeRTOS: 视频流任务跑Core0 (完全独立,不阻塞Core1) ======================
static WiFiServer streamServer(81);

void streamTask(void *pv)
{
    streamServer.begin();
    Serial.println("[STREAM] Task started on Core 0");

    while (true) {
        WiFiClient client = streamServer.available();
        if (!client) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

        Serial.println("[STREAM] Client connected");

        client.setNoDelay(true);  // 关闭Nagle算法,小包立即发送,减少TCP延迟

        // 加大TCP发送缓冲到32KB(默认只有几KB),允许JPEG大块数据连续推送
        int sndbuf = 32768;
        setsockopt(client.fd(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        // 读掉HTTP请求行
        while (client.available()) client.read();

        // MJPEG响应头合并成一次write,减少TCP分段
        static const char mjpegHdr[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "Cache-Control: no-cache\r\n\r\n";
        client.write((const uint8_t*)mjpegHdr, sizeof(mjpegHdr) - 1);

        uint32_t fc = 0;
        unsigned long lastFps = millis();
        // 诊断统计
        uint32_t totalBytes = 0, capMs = 0, sendMs = 0;
        // 帧合并缓冲: 帧头+JPEG+尾部一次拼好,消灭尾部小TCP包(延迟ACK卡顿元凶)
        uint8_t *pkt = NULL;
        size_t pktCap = 0;

        while (client.connected()) {
            // ① 测采集耗时
            unsigned long t0 = millis();
            camera_fb_t *fb = esp_camera_fb_get();
            unsigned long t1 = millis();
            if (!fb) break;

            // ② 整帧拼成一个连续缓冲: --frame头 + JPEG数据 + \r\n
            size_t need = fb->len + 128;
            if (need > pktCap) { free(pkt); pkt = (uint8_t*)malloc(need); pktCap = pkt ? need : 0; }
            if (!pkt) { esp_camera_fb_return(fb); break; }

            int hl = snprintf((char*)pkt, 128,
                "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                (unsigned)fb->len);
            memcpy(pkt + hl, fb->buf, fb->len);
            memcpy(pkt + hl + fb->len, "\r\n", 2);
            size_t total = hl + fb->len + 2;
            esp_camera_fb_return(fb);

            // ③ 阻塞整帧发送(单缓冲已消灭尾部小包); lwIP阻塞期间让出CPU,WiFi任务高效运行
            size_t off = 0;
            bool dead = false;
            while (off < total) {
                size_t w = client.write(pkt + off, total - off);
                if (w == 0) {
                    if (!client.connected()) { dead = true; break; }
                    vTaskDelay(1);
                    continue;
                }
                off += w;
            }
            unsigned long t2 = millis();
            if (dead) break;

            totalBytes += total;
            capMs  += (t1 - t0);
            sendMs += (t2 - t1);

            unsigned long now = millis();
            if (now - lastFps >= 1000) {
                // ④ 一次打印: 帧率/帧大小/采集耗时/发送耗时/WiFi信号/实际码率
                Serial.printf("[DIAG] FPS:%lu avgKB:%lu capMs:%lu sendMs:%lu cli:%d KBps:%lu\n",
                    fc,
                    fc ? (totalBytes/fc/1024) : 0,
                    fc ? (capMs/fc) : 0,
                    fc ? (sendMs/fc) : 0,
                    WiFi.softAPgetStationNum(),
                    totalBytes/1024);
                fc = 0; totalBytes = 0; capMs = 0; sendMs = 0;
                lastFps = now;
            } else {
                fc++;
            }
        }

        free(pkt); pkt = NULL; pktCap = 0;

        client.stop();
        Serial.println("[STREAM] Client disconnected");
    }
}

// ====================== 控制路由 (80端口,跑在Core1,永远秒响应) ======================
void handleForward()  { carForward();  server.send(200,"text/plain","OK"); }
void handleBackward() { carBackward(); server.send(200,"text/plain","OK"); }
void handleLeft()     { carLeft();     server.send(200,"text/plain","OK"); }
void handleRight()    { carRight();    server.send(200,"text/plain","OK"); }
void handleStop()     { carStop(); ytStop(); server.send(200,"text/plain","OK"); }
void handleYtUp()     { ytUp();        server.send(200,"text/plain","OK"); }
void handleYtDown()   { ytDown();      server.send(200,"text/plain","OK"); }
void handleYtStop()   { ytStop();      server.send(200,"text/plain","OK"); }

// ====================== 主页 ======================
void handleRoot()
{
    String html = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>ESP32-S3-CAM 遥控小车</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
body{background:#1a1a2e;color:#fff;font-family:Arial,sans-serif;text-align:center;overflow:hidden;height:100vh;}
h2{font-size:16px;padding:6px;background:#16213e;}
#wrap{display:flex;flex-direction:column;height:calc(100vh - 32px);}
#cam{flex:1;background:#000;display:flex;align-items:center;justify-content:center;min-height:0;}
#cam img{max-width:100%;max-height:100%;object-fit:contain;}
#ctrl{display:flex;justify-content:space-around;align-items:center;padding:10px;background:#16213e;}
.dpad{display:grid;grid-template-columns:repeat(3,60px);grid-template-rows:repeat(3,60px);gap:4px;}
.btn{background:#0f3460;border:none;border-radius:10px;color:#fff;font-size:20px;font-weight:bold;cursor:pointer;user-select:none;display:flex;align-items:center;justify-content:center;}
.btn:active{background:#e94560;}
#fwd{grid-column:2;grid-row:1;}
#left{grid-column:1;grid-row:2;}
#stop{grid-column:2;grid-row:2;background:#e94560;font-size:14px;}
#right{grid-column:3;grid-row:2;}
#bwd{grid-column:2;grid-row:3;}
.yt{display:flex;flex-direction:column;gap:6px;}
.yt .btn{width:60px;height:60px;}
</style>
</head>
<body>
<h2>ESP32-S3-CAM 遥控小车</h2>
<div id="wrap">
  <div id="cam"><img id="v" src=""></div>
  <div id="ctrl">
    <div class="dpad">
      <button class="btn" id="fwd">▲</button>
      <button class="btn" id="left">◀</button>
      <button class="btn" id="stop">停止</button>
      <button class="btn" id="right">▶</button>
      <button class="btn" id="bwd">▼</button>
    </div>
    <div class="yt">
      <button class="btn" id="up">▲</button>
      <button class="btn" id="ytstop" style="font-size:12px;">停</button>
      <button class="btn" id="down">▼</button>
    </div>
  </div>
</div>
<script>
var H=window.location.hostname;
document.getElementById('v').src='http://'+H+':81/stream';
function u(p){return 'http://'+H+':80/'+p;}
function s(a){var x=new XMLHttpRequest();x.open('GET',u(a),true);x.send();}
function b(id,a){
  var e=document.getElementById(id);
  e.addEventListener('mousedown',()=>s(a));
  e.addEventListener('mouseup',()=>s('stop'));
  e.addEventListener('mouseleave',()=>s('stop'));
  e.addEventListener('touchstart',ev=>{ev.preventDefault();s(a);});
  e.addEventListener('touchend',ev=>{ev.preventDefault();s('stop');});
}
b('fwd','forward');b('bwd','backward');b('left','left');b('right','right');
document.getElementById('stop').onclick=()=>s('stop');
b('up','ytup');b('down','ytdown');
document.getElementById('ytstop').onclick=()=>s('ytstop');
</script>
</body></html>
)rawliteral";
    server.send(200, "text/html", html);
}

void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.println("\n=== ESP32-S3-CAM (FreeRTOS Dual-Core) ===");
    Serial.printf("[BOOT] PSRAM: %d bytes, Core: %d\n", ESP.getPsramSize(), xPortGetCoreID());

    initMotors();
    if (!initCamera()) { while (true) delay(1000); }

    // ================= SoftAP模式: 开放热点(无密码),排除WPA2握手/PMF兼容问题 =================
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_AP);
    // 开放网络: 手机不会因加密握手问题反复掉线
    bool apOk = WiFi.softAP("ESP32-S3-CAR", NULL, 1, 0, 4);
    delay(500);
    Serial.printf("[AP] start result: %s\n", apOk ? "SUCCESS" : "FAILED");
    Serial.printf("[AP] SSID=ESP32-S3-CAR (无密码) IP=%s ch=1\n",
                  WiFi.softAPIP().toString().c_str());

    // ========== 关键: 视频流任务跑Core0, 电机WebServer跑Core1(主循环) ==========
    xTaskCreatePinnedToCore(streamTask, "stream", 8192, NULL, 2, NULL, 0);
    Serial.println("[RTOS] streamTask pinned to Core 0");

    // 80端口: 主页 + 电机控制 (跑在Core1)
    server.on("/",        HTTP_GET, handleRoot);
    server.on("/forward", HTTP_GET, handleForward);
    server.on("/backward",HTTP_GET, handleBackward);
    server.on("/left",    HTTP_GET, handleLeft);
    server.on("/right",   HTTP_GET, handleRight);
    server.on("/stop",    HTTP_GET, handleStop);
    server.on("/ytup",    HTTP_GET, handleYtUp);
    server.on("/ytdown",  HTTP_GET, handleYtDown);
    server.on("/ytstop",  HTTP_GET, handleYtStop);
    server.begin();

    Serial.println("[RTOS] WebServer on Core 1, stream on Core 0");
    Serial.println("[INFO] 手机连接开放热点 ESP32-S3-CAR (无密码) 后打开 http://192.168.4.1/");
}

// Core 1: 只跑电机WebServer,永远不被视频流阻塞
void loop()
{
    static int lastCli = -1;
    int cli = WiFi.softAPgetStationNum();
    if (cli != lastCli) {
        Serial.printf("[AP] stations: %d -> %d\n", lastCli, cli);
        lastCli = cli;
    }
    server.handleClient();
}

