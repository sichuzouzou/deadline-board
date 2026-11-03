/*
 * ============================================================
 *  工作 deadline 提示板 · 主固件 v3.1
 * ============================================================
 *
 *  v3.0 → v3.1 的改动（2026-10-27，龙哥选定直立版）：
 *   1. 天数数字换成 DSEG7 直立七段数码管（自建位图字模 dseg_digits.h）
 *      34×56px、段厚 8、缝 1px，段厚/字高 0.143（对齐他家里墨水屏日历的比例）
 *      替代原 logisoso34 圆润手写体；直写画布缓冲区，渲染更快
 *   2. 「天」下移到数字右下角（top+38），底部与数字齐平，像 25.7°C 的 C
 *   3. 「逾期」不再挤在数字左侧（56px 大数字占满右侧，那里只剩十几 px）
 *      改为次行左侧写「逾期 M/D」替代「截止 M/D」——语义一样清楚
 *   4. 数字右边界 358、「天」362..388，两者留 4px 不重叠；
 *      标题区右边界仍是 272，两位数字左沿 287，间距 15px
 *
 *  v2 → v3 的改动（2026-10-27）：
 *   1. 版式：大字天数从「跨两行」改成「与标题并排」，不再压标题；
 *      去掉冗余的「还剩」二字；底栏全角括号改半角
 *   2. 六态逻辑：清空 / 逾期(最久反白) / 待确认(3px粗框+还要吗) /
 *      只剩1天(左竖条) / 正常 / 未对时。反白全屏最多 1 条。
 *   3. 待确认按「工作日」算（周末不计），≥2 个工作日没同步才提示
 *   4. 模块化刷屏接口 + FULL 强制全刷命令（局刷实测不省时间，已锁定全刷）
 *
 *  v1 → v2 的改动：
 *   1. HAS_RTC = 1：真读 DS3231（Wire 直读寄存器，不装 RTClib）
 *      → 断电后时间不再丢，跨天判断也真了
 *   2. drawText2x 改为直写画布缓冲区（原来是 getPixel/fillRect 虚函数逐点）
 *   3. 刷新过程加分段计时，串口打印 渲染/传输 各多少毫秒
 *
 * 硬件：AirM2M CORE ESP32-C3 一体机 + GDEH042Z96 (4.2" 400x300 黑白)
 * 编译：esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,FlashFreq=80,
 *       FlashSize=4M,PartitionScheme=no_ota
 *
 * ⚠️ 铁律（踩坑换来的，勿改）：
 *   1. FlashMode 必须 = dio。板子实际是 DIO，用默认 QIO 编出的 bootloader
 *      读 flash 会错位 → boot loop（partition invalid magic number）
 *   2. SPI.begin(SCLK, -1, MOSI, CS) —— MISO 传 -1 是设计如此，
 *      串口那条 spiAttachMISO 报错是无害噪音
 *   3. setRotation(2) + mirror(true) —— 这块屏必须旋转+镜像，照抄厂家
 *   4. 三色面板被 GxEPD2_BW 包装使用（只用黑白层），红色留后续
 *   5. RTC 接线：SDA=GPIO1(脚3左) SCL=GPIO8(脚29右) VCC=3.3V(脚26) GND=脚25
 *      GPIO8 是 C3 的 strapping 脚，模块上拉正常拉高；若 SCL 虚焊 → 板上不了电
 *   6. 屏上所有文字必须是 wqy 字体里有的字：全角括号（）和全角空格会显示不出来，
 *      一律用半角 ( ) 和普通空格
 *
 * USB 更新协议（115200，行文本，UTF-8）：
 *    PING                            -> OK PONG
 *    TIME|2026-10-26 16:03:00        -> OK TIME     同时写入 DS3231
 *    ITEM|1|业务连续性演练|2026-11-05  -> OK ITEM 1   （标题超过 10 字自动截断）
 *    CLR|1                           -> OK CLR 1
 *    CLEARALL                        -> OK CLEARALL
 *    SHOW                            -> OK SHOW
 *    FULL                            -> OK FULL（强制全刷，清残影用）
 *    STATUS                          -> OK STATUS ...  含 wd= 工作日数、stale= 是否待确认
 *    RTC                             -> OK RTC ...（诊断用，读一次 DS3231）
 *    DUMP                            -> 把画布按 hex 输出，电脑端还原 PNG 自查版式
 *
 * 六态显示逻辑：
 *   清空    无有效条目              → 「清空」+「手上没有在跑的活」
 *   逾期    dueDay < 今天           → 最久那条整行反白，其余黑字「逾期 N 天」
 *   待确认  ≥2 个工作日没同步       → 3px 粗框 + 次行右「还要吗?」
 *   只剩1天 剩余 = 1 天             → 行左 5px 竖条
 *   正常    其余                    → 白底黑字 + 右侧大字天数
 *   未对时  RTC 无效                → 顶部「未对时」，不显示天数
 *   优先级：反白独占，全屏最多 1 条，优先给逾期最久的；待确认只描边不反白
 *
 * 持久化：SPIFFS /d.txt
 * 时间源：DS3231 优先；RTC 无效/掉电(OSF=1) 才回退 SPIFFS 里的旧值
 * ============================================================
 */

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <GxEPD2_420c_Z96.h>
#include <GxEPD2_BW.h>
#include "dseg_digits.h"     // 56px 大天数七段字模（gen_dseg_final.py 生成）
#include "dseg_small.h"      // 18/26px 小字号七段字模（gen_seg_small_h.py 生成）

// ============ 屏幕引脚（厂家定义，勿改） ============
#define EPD_CS    7
#define EPD_DC    6
#define EPD_RST   10
#define EPD_BUSY  0
#define EPD_SCLK  2
#define EPD_MOSI  3

// ============ RTC（已焊通，2026-10-26 实测 0x68 在线 + 断电走时 OK） ============
#define HAS_RTC       1
#define I2C_SDA_PIN   1
#define I2C_SCL_PIN   8
#define RTC_ADDR      0x68

// ============ 版式常量 ============
#define SCR_W 400
#define SCR_H 300
#define TZ_OFFSET (8 * 3600)   // 中国 UTC+8

// ============ 小字号七段字体句柄 ============
// ⚠️ 必须定义在文件靠前处：Arduino 会把所有函数原型自动插到前面，
//    如果 SegFont 定义在函数之后，原型里的 SegFont* 参数就找不到类型（实测报错）
struct SegFont {
  const uint8_t  *w;      // 10 个字宽
  const uint16_t *bits;   // 10*H 行掩码，bit15 = 该行最左像素
  uint8_t         h;
  uint8_t         gap;    // 字间距：不随字号统一。26px 用 5，18px 用 4。
                          // 同样 4px，在 26px 字高上只占 15%（顶栏「27」挤成一坨），
                          // 在 18px 上占 22% 正好 —— 间距要按字高比例给，不能一刀切。
};
static const SegFont SEGF18 = { SEG18_W, (const uint16_t *)SEG18_BITS, SEG18_H, 4 };
static const SegFont SEGF26 = { SEG26_W, (const uint16_t *)SEG26_BITS, SEG26_H, 5 };

// ============ 数据模型 ============
#define ITEM_COUNT 3
#define TITLE_MAX  40          // 字节数（UTF-8，约 13 个汉字）

struct Item {
  bool     used;
  int32_t  dueDay;             // 截止日：1970-01-01 起的天数
  char     title[TITLE_MAX];
};

Item     g_items[ITEM_COUNT];
int32_t  g_now = 0;            // UTC epoch 秒（0 = 未对时）
uint32_t g_nowMs = 0;          // 最近一次对时的 millis，用于漂移补偿
int32_t  g_lastSyncDay = 0;    // 最后一次收到清单的日期序号（底栏「更新 M/D」用）
bool     g_rtcOk = false;      // RTC 本次开机是否读到有效时间
// 本次开机的 millis 基准，用来算「开机多久」。断电测试时要能自证
// 「确实重启过」——否则屏上时间对，也可能只是没断电，结论就成了猜的。
uint32_t g_bootMs = 0;

// 时间是否「可信」——与 g_rtcOk 不是一回事，必须分开：
//   g_rtcOk      = 能读到 DS3231 的寄存器
//   g_timeTrusted= 这个时间的绝对值可信
// 2026-10-28 踩过的坑：RTC 掉过电(OSF=1)时，旧代码从 SPIFFS 取旧时间兜底、
// 再回写进 RTC 并置 g_rtcOk=true，屏上照常显示日期 —— 于是设备「看着正常，
// 其实慢了一整天」（龙哥早上看到 10月27日，实际已 28 日）。
// 现在：OSF=1 或时间来自 SPIFFS 兜底 → trusted=false，屏上直接写「未对时」，
//       直到收到 PC 端 TIME 命令才恢复可信。宁可明说，不许假装。
bool     g_timeTrusted = false;

// ============ 显示对象 ============
GxEPD2_BW<GxEPD2_420c_Z96, GxEPD2_420c_Z96::HEIGHT>
    display(GxEPD2_420c_Z96(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// 全屏画布（400*300/8 = 15000 字节）
GFXcanvas1 canvas(SCR_W, SCR_H);
// 2 倍放大用的临时画布（1x 渲染后放大）
GFXcanvas1 textCV(SCR_W, 18);
U8G2_FOR_ADAFRUIT_GFX tU8g2;

// ============ 状态 ============
bool     g_dirty      = false;   // 内容脏，待刷屏
uint32_t g_dirtyMs    = 0;
int32_t  g_lastDays   = -1;      // 上次渲染时的日期序号（检测跨天）
int8_t   g_lastHalf   = -1;      // 上次渲染时的半天（0=上午 1=下午，-1=未知）
// 半天标记是给龙哥看「时间源还活着吗」的心跳：每天 12:00 该翻没翻，
// 就说明 RTC 停走或设备没在跑 —— 不用等一整天才能发现。

// ============================================================
//  日期算法（Howard Hinnant 的 days_from_civil / civil_from_days）
// ============================================================
int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

void civilFromDays(int32_t z, int &y, unsigned &m, unsigned &d) {
  z += 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int yy = (int)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = yy + (m <= 2);
}

// 本地口径的"今天"
int32_t todayDays() {
  if (g_now <= 0) return 0;
  return (int32_t)((g_now + TZ_OFFSET) / 86400);
}

// 时间可信？两个条件缺一不可：有值 + 来源可信。
// 所有「要不要把日期/天数/待确认算给用户看」的判断都走这里 ——
// 时间不可信时还算「还剩几天」，只会给出一堆看着精确的错数字。
bool timeOk() { return g_now > 0 && g_timeTrusted; }

// 当前时间（含 millis 漂移补偿）
int32_t nowEpoch() {
  if (g_now <= 0) return 0;
  return g_now + (int32_t)((millis() - g_nowMs) / 1000);
}

const char *WD_NAME[] = { "日", "一", "二", "三", "四", "五", "六" };

// ---------- 工作日计算 ----------
// 「几个工作日没跟电脑同步过」——跳过周六周日 + 法定节假日
// 1970-01-01 是周四，所以 (day + 4) % 7：0=周日 … 6=周六
//
// 法定节假日表（编码 yyyymmdd）。数据：2027 年放假安排，按国务院《全国年节及
// 纪念日放假办法》调休原则整理（新华社/人民日报 2026-11 报道）：
//   元旦   1/1(五)~1/3(日)   不调休
//   春节   2/5(除夕,五)起调休8天，顺连周末连休 9 天（2/5~2/14）
//   清明   4/3(六)~4/5(一)   不调休
//   劳动节 4/30~5/4          调休1天（媒体预计，以国务院正式通知为准）
//   端午   6/9(三)           仅 1 天
//   中秋   9/15(三)          仅 1 天
//   国庆   10/1(五)~10/7(四) 调休2天
// 注：调休补班日（周末上班）以国务院正式通知为准，暂未列入；
//     表中周末重复标记无影响。2028 年起需补充新表。
const int32_t HOLIDAY_YMD[] = {
  20270101, 20270102, 20270103,
  20270205, 20270206, 20270207, 20270208, 20270209,
  20270210, 20270211, 20270212, 20270213, 20270214,
  20270403, 20270404, 20270405,
  20270430, 20270501, 20270502, 20270503, 20270504,
  20270609,
  20270915,
  20271001, 20271002, 20271003, 20271004, 20271005, 20271006, 20271007,
};
const int HOLIDAY_N = sizeof(HOLIDAY_YMD) / sizeof(HOLIDAY_YMD[0]);

// 该日是否休息（周末 或 法定假日）。调休补班日暂不处理（误差 ≤2 天/年，可接受）
bool isRestDay(int32_t day) {
  int dow = (int)((day + 4) % 7);
  if (dow == 0 || dow == 6) return true;
  int yy; unsigned mo, dd;
  civilFromDays(day, yy, mo, dd);
  int32_t ymd = (int32_t)yy * 10000 + (int32_t)mo * 100 + (int32_t)dd;
  for (int i = 0; i < HOLIDAY_N; i++)
    if (HOLIDAY_YMD[i] == ymd) return true;
  return false;
}

int32_t workdaysBetween(int32_t fromDay, int32_t toDay) {
  if (toDay <= fromDay) return 0;
  int32_t n = 0;
  for (int32_t d = fromDay + 1; d <= toDay; d++) {
    if (!isRestDay(d)) n++;
  }
  return n;
}

// 判定「待确认」的阈值：2 个工作日没同步 → 该盘一下这些活是不是早干完了
#define STALE_WORKDAYS 2

// ============================================================
//  DS3231 直读（寄存器协议，零依赖）
// ============================================================
static inline uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// 读时间。成功返回 true，epoch 为 UTC 秒
bool rtcRead(int32_t &epoch) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7) != 7) return false;

  uint8_t s   = bcd2dec(Wire.read() & 0x7F);
  uint8_t mi  = bcd2dec(Wire.read() & 0x7F);
  uint8_t hraw = Wire.read();
  uint8_t h;
  if (hraw & 0x40) {                    // 12 小时制
    h = bcd2dec(hraw & 0x1F);
    if (hraw & 0x20) { if (h != 12) h += 12; }   // PM
    else             { if (h == 12) h = 0; }     // AM 12 点 = 0 点
  } else {
    h = bcd2dec(hraw & 0x3F);
  }
  Wire.read();                          // 星期寄存器不用，星期由日期算
  uint8_t dd  = bcd2dec(Wire.read() & 0x3F);
  uint8_t mo  = bcd2dec(Wire.read() & 0x1F);
  uint8_t yy  = bcd2dec(Wire.read());

  if (mo < 1 || mo > 12 || dd < 1 || dd > 31) return false;

  epoch = daysFromCivil(2000 + yy, mo, dd) * 86400L
          + (int32_t)h * 3600 + (int32_t)mi * 60 + (int32_t)s - TZ_OFFSET;
  return true;
}

// 读出 OSF（0x0F bit7）。1 = 掉过电，时间不可信
bool rtcOsfSet() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x0F);
  if (Wire.endTransmission() != 0) return true;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)1) != 1) return true;
  return (Wire.read() & 0x80) != 0;
}

void rtcClearOsf() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x0F);
  if (Wire.endTransmission() != 0) return;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)1) != 1) return;
  uint8_t st = Wire.read() & 0x7F;
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x0F);
  Wire.write(st);
  Wire.endTransmission();
}

// 写时间（epoch 为 UTC 秒），并清 OSF
bool rtcWrite(int32_t epoch) {
  int32_t local = epoch + TZ_OFFSET;
  int32_t days  = local / 86400;
  int32_t sod   = local % 86400;
  if (sod < 0) { sod += 86400; days -= 1; }
  int y; unsigned mo, d;
  civilFromDays(days, y, mo, d);
  int dow = (int)((days + 4) % 7) + 1;     // DS3231：1=周日
  if (dow < 1) dow = 1;

  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(dec2bcd((uint8_t)(sod % 60)));
  Wire.write(dec2bcd((uint8_t)((sod / 60) % 60)));
  Wire.write(dec2bcd((uint8_t)(sod / 3600)));   // bit6=0 → 24 小时制
  Wire.write(dec2bcd((uint8_t)dow));
  Wire.write(dec2bcd((uint8_t)d));
  Wire.write(dec2bcd((uint8_t)mo));             // 世纪位=0 → 2000+
  Wire.write(dec2bcd((uint8_t)(y % 100)));
  if (Wire.endTransmission() != 0) return false;

  rtcClearOsf();
  return true;
}

// RTC 温度（顺带确认晶体在工作）
float rtcTemp() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x11);
  if (Wire.endTransmission() != 0) return NAN;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)2) != 2) return NAN;
  int8_t msb = (int8_t)Wire.read();
  uint8_t lsb = Wire.read();
  return (float)msb + ((lsb >> 6) * 0.25f);
}

// 从 RTC 同步到 g_now。返回是否成功
bool rtcSync() {
  int32_t ep = 0;
  if (!rtcRead(ep)) return false;
  g_now   = ep;
  g_nowMs = millis();
  g_rtcOk = true;
  return true;
}

// ============================================================
//  UTF-8 安全截断（不切半个汉字）
// ============================================================
// UTF-8 字符个数（一个汉字算 1，用于字号自适应选档）
int utf8Len(const char *s) {
  int n = 0;
  while (*s) {
    unsigned char c = (unsigned char)(*s);
    if ((c & 0xC0) != 0x80) n++;
    s++;
  }
  return n;
}

void utf8CopyN(char *dst, size_t dstSize, const char *src, int maxChars) {
  if (dstSize == 0) return;
  int chars = 0;
  size_t i = 0;
  while (src[i] && chars < maxChars && i + 4 < dstSize) {
    unsigned char c = (unsigned char)src[i];
    int len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    for (int k = 0; k < len && src[i + k]; k++) dst[i + k] = src[i + k];
    i += len;
    chars++;
  }
  dst[i] = 0;
}

// ============================================================
//  绘制：中文 2 倍放大（等效 32px，笔画变 2px = 天然加粗）
//  v2 改动：直写画布缓冲区，绕过 getPixel/fillRect 虚函数（v1 的主要耗时点）
//  y 传的是放大后的左上角坐标
// ============================================================
// 反白支持：g_paint = 1 画黑（置位），= 0 画白（清位），用于整行反白时在白字
static uint8_t g_paint = 1;

static inline void rowPaint3(uint8_t *row, int px) {
  for (int k = 0; k < 3; k++) {
    int p = px + k;
    if (p < 0 || p >= SCR_W) continue;
    uint8_t m = (uint8_t)(0x80 >> (p & 7));
    if (g_paint) row[p >> 3] |= m;
    else         row[p >> 3] &= (uint8_t)~m;
  }
}

static inline void rowPaint1(uint8_t *row, int px) {
  if (px < 0 || px >= SCR_W) return;
  uint8_t m = (uint8_t)(0x80 >> (px & 7));
  if (g_paint) row[px >> 3] |= m;
  else         row[px >> 3] &= (uint8_t)~m;
}

// 中文 2 倍放大 + 加粗，字体可指定（用于标题字号自适应）
//   每个源像素铺成 3×3（步进 2px，相邻重叠 1px）→ 笔画 2px 变 3px
void drawText2xFont(int x, int y, const char *s, const uint8_t *font) {
  tU8g2.begin(textCV);
  tU8g2.setFontMode(1);
  tU8g2.setFont(font);
  tU8g2.setForegroundColor(1);
  tU8g2.setBackgroundColor(0);

  textCV.fillScreen(0);
  tU8g2.setCursor(0, 15);
  tU8g2.print(s);

  uint8_t       *db  = canvas.getBuffer();
  const uint8_t *sb  = textCV.getBuffer();
  const int      sbW = (SCR_W + 7) >> 3;
  const int      dbW = (SCR_W + 7) >> 3;

  {
    static int dbgOnce = 0;
    if (dbgOnce < 3) {
      uint32_t nz2 = 0;
      for (int k = 0; k < sbW * 18; k++) if (sb[k]) nz2++;
      Serial.printf("[DBG] drawText2xFont \"%s\" textCV非零=%lu font=%p paint=%d\n",
                    s, (unsigned long)nz2, (const void *)font, g_paint);
      dbgOnce++;
    }
  }

  for (int yy = 0; yy < 18; yy++) {
    const uint8_t *srow = sb + yy * sbW;
    int py = y + yy * 2;
    if (py + 2 < 0 || py >= SCR_H) continue;
    for (int xx = 0; xx < SCR_W; xx++) {
      if (!(srow[xx >> 3] & (uint8_t)(0x80 >> (xx & 7)))) continue;
      int px = x + xx * 2;
      if (px + 2 < 0 || px >= SCR_W) continue;
      for (int r = 0; r < 3; r++) {
        int pr = py + r;
        if (pr >= 0 && pr < SCR_H) rowPaint3(db + pr * dbW, px);
      }
    }
  }
}

void drawText2x(int x, int y, const char *s) {
  drawText2xFont(x, y, s, u8g2_font_wqy16_t_gb2312a);
}

// 标题字号自适应（借鉴 TRMNL 的 data-fit-value 思路）：
//   短标题用大字，长标题自动降一档，保证永远不溢出 400px 宽
//   返回的字体经 2x 放大后依次为 32 / 28 / 26 / 24 px
// ⚠️ 右边界必须 ≤ 282px（「逾期 12 天」那一组的左沿），否则长标题会压到天数
//    所以上限锁 11 字：24px × 11 = 264 → 12 + 264 = 276 ✓
const uint8_t *pickTitleFont(int nchars) {
  if (nchars <= 8)  return u8g2_font_wqy16_t_gb2312a;   // 32px，8 字 = 256px
  if (nchars <= 9)  return u8g2_font_wqy14_t_gb2312a;   // 28px，9 字 = 252px
  if (nchars <= 10) return u8g2_font_wqy13_t_gb2312a;   // 26px，10 字 = 260px
  return u8g2_font_wqy12_t_gb2312a;                     // 24px，11 字 = 264px
}

// 标题最大显示字数（超出截断，PC 端工具同步限制 10 字）
#define TITLE_CHARS 10

// 1x 小字，右移 1px 重绘一次做横向加粗；支持反白。y 是字顶
void drawText1xFont(int x, int y, const char *s, const uint8_t *font) {
  uint8_t *db  = canvas.getBuffer();
  const int dbW = (SCR_W + 7) >> 3;

  for (int pass = 0; pass < 2; pass++) {
    textCV.fillScreen(0);
    tU8g2.begin(textCV);
    tU8g2.setFontMode(1);
    tU8g2.setFont(font);
    tU8g2.setForegroundColor(1);
    tU8g2.setBackgroundColor(0);
    tU8g2.setCursor(0, 15);
    tU8g2.print(s);

    const uint8_t *sb  = textCV.getBuffer();
    const int      sbW = (SCR_W + 7) >> 3;
    for (int yy = 0; yy < 18; yy++) {
      const uint8_t *srow = sb + yy * sbW;
      int py = y + yy;
      if (py < 0 || py >= SCR_H) continue;
      uint8_t *row = db + py * dbW;
      for (int xx = 0; xx < SCR_W; xx++) {
        if (!(srow[xx >> 3] & (uint8_t)(0x80 >> (xx & 7)))) continue;
        rowPaint1(row, x + xx + pass);
      }
    }
  }
}

// 1x 文字宽度（用于右对齐）
int16_t textWidth1x(const char *s, const uint8_t *font) {
  tU8g2.begin(textCV);
  tU8g2.setFont(font);
  return tU8g2.getUTF8Width(s);
}

// ============================================================
//  七段数码管数字（DSEG7 直立风格）
//  2026-10-27 替换原 logisoso54：龙哥要电子管数字感，不要圆润手写体
//  直写画布缓冲区（不走 GFX 虚函数），单字符约 1900 次位操作，微秒级
//  颜色跟随 g_paint：1 = 置墨（黑字），0 = 清位（反白行上的白字）
//  x/y 是左上角；字符 DSEG_W×DSEG_H = 34×55，「天」按它的底部对齐
// ============================================================
void drawDsegNum(int16_t x, int16_t y, const char *s) {
  uint8_t      *db  = canvas.getBuffer();
  const int     dbW = (SCR_W + 7) >> 3;
  const uint8_t ink = g_paint ? 1 : 0;

  for (const char *p = s; *p; p++) {
    if (*p < '0' || *p > '9') continue;
    const uint8_t *bm = DSEG_BITS[*p - '0'];
    for (int row = 0; row < DSEG_H; row++) {
      int py = y + row;
      if (py < 0 || py >= SCR_H) continue;
      uint8_t       *drow = db + py * dbW;
      const uint8_t *brow = bm + row * DSEG_BPR;
      for (int col = 0; col < DSEG_W; col++) {
        if (!(pgm_read_byte(brow + (col >> 3)) & (uint8_t)(0x80 >> (col & 7)))) continue;
        int px = x + col;
        if (px < 0 || px >= SCR_W) continue;
        uint8_t m = (uint8_t)(0x80 >> (px & 7));
        if (ink) drow[px >> 3] |=  m;
        else     drow[px >> 3] &= (uint8_t)~m;
      }
    }
    x += DSEG_ADV;
  }
}

// 数字串实际落脚宽度（末字的字距不计）
static inline int16_t dsegWidth(const char *s) {
  int n = 0;
  for (const char *p = s; *p; p++) if (*p >= '0' && *p <= '9') n++;
  return n > 0 ? (int16_t)(DSEG_ADV * n - 3) : 0;
}

// ============================================================
//  小字号七段混排（2026-10-27 龙哥：数字全部换七段）
//  ------------------------------------------------------------
//  为什么不用 56px 大字缩一个：1bit 屏没有灰度，缩小必然丢笔画
//  （段厚掉到 1px，横段中间的缝直接消失，3/5/8/9 糊成实心块）。
//  所以另生成一套独立几何：整数坐标、不做抗锯齿 → SEG18 / SEG26
//
//  混排规则：'0'~'9' 走七段字模，其余（中文/括号/空格）走 u8g2 点阵
//  垂直对齐：让中文的视觉中心落在数字中心上
// ============================================================
#define SEG_GAP 4      // 数字与相邻字符的间距（格宽已含字形，不再留白）

static inline int utf8CharLen(unsigned char c) {
  if (c < 0x80)           return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// 单个七段数字（左上角定位）；颜色跟随 g_paint，反白行上自动变白字
static void drawSegDigit(const SegFont *f, int16_t x, int16_t y, char c) {
  const int     idx = c - '0';
  const uint8_t w   = pgm_read_byte(f->w + idx);
  const uint16_t *rp = f->bits + (uint16_t)idx * f->h;

  uint8_t      *db  = canvas.getBuffer();
  const int     dbW = (SCR_W + 7) >> 3;
  const uint8_t ink = g_paint ? 1 : 0;

  for (int row = 0; row < f->h; row++) {
    int py = y + row;
    if (py < 0 || py >= SCR_H) continue;
    uint16_t m = pgm_read_word(rp + row);
    if (!m) continue;
    uint8_t *drow = db + py * dbW;
    for (int col = 0; col < w; col++) {
      if (!(m & (uint16_t)(0x8000u >> col))) continue;
      int px = x + col;
      if (px < 0 || px >= SCR_W) continue;
      uint8_t bm = (uint8_t)(0x80 >> (px & 7));
      if (ink) drow[px >> 3] |=  bm;
      else     drow[px >> 3] &= (uint8_t)~bm;
    }
  }
}

// 算中文该传的 y，使它的视觉中心对齐数字中心。
//   drawTextNxFont 的 y 是「临时画布 textCV 第 0 行」对应的画布行，
//   而 textCV 里字形是从 baseline=15 往上 ascent 行画起的，
//   所以视觉顶 = y + scale*(15 - ascent)。
static int16_t cnYForSeg(int16_t numTop, uint8_t segH, uint8_t cntScale, int16_t ascent) {
  int16_t wantTop = numTop + ((int)segH - (int)cntScale * ascent) / 2;
  return wantTop - (int)cntScale * (15 - ascent);
}

// 画一段「中文 + 七段数字」混排文字，返回结束 x
//   间距规则：数字之间靠 SEG_GAP；中文→数字也要留 SEG_GAP（否则「逾期1」的 1
//   会贴着「期」的右边缘）；但前面已经是空格就不再叠（空格本身已有 3~4px）
int16_t drawSegText(const SegFont *f, int16_t x, int16_t numTop, const char *s,
                    const uint8_t *cnFont, uint8_t cntScale) {
  tU8g2.begin(textCV);
  tU8g2.setFont(cnFont);
  const int16_t cnY   = cnYForSeg(numTop, f->h, cntScale, (int16_t)tU8g2.getFontAscent());
  const int16_t x0    = x;
  bool          prevN = false;   // 上一个字符是数字

  const char *p = s;
  while (*p) {
    int  n     = utf8CharLen((unsigned char)*p);
    bool isNum = (n == 1 && *p >= '0' && *p <= '9');

    if (isNum) {
      if (!prevN && x > x0) x += f->gap;
      drawSegDigit(f, x, numTop, *p);
      x += pgm_read_byte(f->w + (*p - '0')) + f->gap;
    } else {
      char buf[8];
      memset(buf, 0, sizeof(buf));
      memcpy(buf, p, (n < 7) ? n : 7);
      if (cntScale >= 2) drawText2xFont(x, cnY, buf, cnFont);
      else               drawText1xFont(x, cnY, buf, cnFont);
      x += textWidth1x(buf, cnFont) * cntScale;
    }
    prevN = isNum;
    p += n;
  }
  return x;
}

// 混排文字宽度（末尾字距不计，右对齐才准）
int16_t segTextWidth(const SegFont *f, const char *s, const uint8_t *cnFont, uint8_t cntScale) {
  int16_t w     = 0;
  bool    prevN = false;

  const char *p = s;
  while (*p) {
    int  n     = utf8CharLen((unsigned char)*p);
    bool isNum = (n == 1 && *p >= '0' && *p <= '9');
    if (isNum) {
      if (!prevN && w > 0) w += f->gap;
      w += pgm_read_byte(f->w + (*p - '0')) + f->gap;
    } else {
      char buf[8];
      memset(buf, 0, sizeof(buf));
      memcpy(buf, p, (n < 7) ? n : 7);
      int16_t cw = textWidth1x(buf, cnFont) * cntScale;
      if (cntScale < 2) cw += 1;   // 1x 有「右移 1px 重绘加粗」，实际墨宽 +1
      w += cw;
    }
    prevN = isNum;
    p += n;
  }
  return (w > f->gap) ? (int16_t)(w - f->gap) : w;
}

// ============================================================
//  渲染整屏
// ============================================================
// 3px 粗描边（墨水屏 1px 线发虚，粗框才看得出「这一条要你确认」）
// 注意：canvas.drawFastHLine/VLine 直接置位，不受 g_paint 影响，
//       所以只在非反白行调用
void drawBorder3(int top) {
  const int x0 = 6, x1 = SCR_W - 7;
  // 下边界收到 top+70：相邻两行都待确认时，两个框之间要留出白缝，
  // 否则上框底(top+74)和下框顶(top+77)会粘成一条 6px 粗线，看着像一条分隔线
  const int y0 = top + 2, y1 = top + 70;
  for (int k = 0; k < 3; k++) {
    canvas.drawFastHLine(x0, y0 + k, x1 - x0 + 1, 1);
    canvas.drawFastHLine(x0, y1 - k, x1 - x0 + 1, 1);
    canvas.drawFastVLine(x0 + k, y0, y1 - y0 + 1, 1);
    canvas.drawFastVLine(x1 - k, y0, y1 - y0 + 1, 1);
  }
}

void renderCanvas() {
  canvas.fillScreen(0);   // 0 = 白
  g_paint = 1;

  int32_t days = (g_now > 0) ? todayDays() : 0;

  // ---------- 待确认判定：几个工作日没跟电脑同步过 ----------
  //   g_lastSyncDay==0 表示从没同步过（刚烧录），不判陈旧，免得一上电全变待确认
  int32_t staleWd = 0;
  if (timeOk() && g_lastSyncDay > 0) staleWd = workdaysBetween(g_lastSyncDay, days);
  bool stale = (staleWd >= STALE_WORKDAYS);

  // ---------- 统计：未完成 / 逾期，并找出逾期最久的那条 ----------
  int     nUsed = 0, nLate = 0;
  int     worstIdx = -1;
  int32_t worstLeft = 0;
  for (int i = 0; i < ITEM_COUNT; i++) {
    if (!g_items[i].used) continue;
    nUsed++;
    if (g_now > 0) {
      int32_t l = g_items[i].dueDay - days;
      if (l < 0) {
        nLate++;
        if (worstIdx < 0 || l < worstLeft) { worstIdx = i; worstLeft = l; }
      }
    }
  }

  // ---------- 顶部：日期 + 上午/下午 + 统计 ----------
  // 统计块先算好宽度并右对齐，日期块才知道能给「上午/下午」留多少地方。
  char stat[32]; stat[0] = 0;
  int16_t statLeft = SCR_W;                 // 统计块左边界
  if (nUsed > 0) {
    if (nLate > 0) snprintf(stat, sizeof(stat), "逾期%d 未完成%d", nLate, nUsed);
    else           snprintf(stat, sizeof(stat), "未完成%d", nUsed);
    int16_t sw = segTextWidth(&SEGF18, stat, u8g2_font_wqy16_t_gb2312a, 1);
    statLeft = SCR_W - 10 - sw;
  }

  if (timeOk()) {
    int yy; unsigned mo, d;
    civilFromDays(days, yy, mo, d);
    char dl[40];
    snprintf(dl, sizeof(dl), "%d月%d日 周%s", (int)mo, (int)d, WD_NAME[(days + 4) % 7]);
    // 顶部是 26px 大字：数字走 SEG26，中文仍用 wqy13 的 2 倍放大（26px）
    int16_t dx = drawSegText(&SEGF26, 10, 6, dl, u8g2_font_wqy13_t_gb2312a, 2);

    // 上午 / 下午：时间源的心跳。12:00 该翻没翻 = RTC 停走或设备没在跑。
    int hh = (int)((((g_now + TZ_OFFSET) % 86400) + 86400) % 86400 / 3600);
    const char *half = (hh < 12) ? "上午" : "下午";
    // 2x（52px）放得下就与「周三」同高，放不下退 1x，绝不和统计块压字
    if (dx + 8 + 52 <= statLeft - 6) drawText2xFont(dx + 8, -2, half, u8g2_font_wqy13_t_gb2312a);
    else                             drawText1xFont(dx + 8, 14, half, u8g2_font_wqy16_t_gb2312a);
  } else {
    // 时间不可信（RTC 掉过电 / 只有 SPIFFS 旧值）：必须说出来，
    // 绝不能像 2026-10-28 那样顶着「10月27日」装作正常。
    drawText2xFont(10, 1, "未对时·插电脑", u8g2_font_wqy13_t_gb2312a);
  }

  // 空清单时不显示「未完成0」，那一行留给空白更干净
  if (nUsed > 0) {
    drawSegText(&SEGF18, statLeft, 11, stat, u8g2_font_wqy16_t_gb2312a, 1);
  }

  // 顶部双线（墨水屏 1px 细线容易发虚，两条拼成 2px）
  canvas.drawFastHLine(10, 38, SCR_W - 20, 1);
  canvas.drawFastHLine(10, 41, SCR_W - 20, 1);

  if (nUsed == 0) {
    // ---------- 空清单 ----------
    // 「清空」两字 2x = 64px，横向居中 (400-64)/2 = 168
    // 纵向让「清空 + 副标题」这一组的视觉中心落在内容区中心（约 y=168）
    drawText2xFont(168, 130, "清空", u8g2_font_wqy16_t_gb2312a);
    drawText1xFont(136, 190, "手上没有在跑的活", u8g2_font_wqy16_t_gb2312a);
  } else {
    for (int i = 0; i < ITEM_COUNT; i++) {
      int top = 44 + i * 76;

      if (!g_items[i].used) {
        drawText1xFont(12, top + 28, "(空)", u8g2_font_wqy13_t_gb2312a);
        if (i < ITEM_COUNT - 1) canvas.drawFastHLine(10, top + 74, SCR_W - 20, 1);
        continue;
      }

      int32_t left = (g_now > 0) ? (g_items[i].dueDay - days) : 0;
      bool isLate  = timeOk() && (left < 0);
      bool inv     = (i == worstIdx);           // 逾期最久 → 整行反白（全屏最多 1 条）
      // 待确认「还要吗?」只盘逾期还挂着的（2026-11-03 改）：
      //   未到期的活数据并不算可疑——还有 2 天才到期的条目被盘问、天数被藏，
      //   纯属制造困惑（2026-11-03 早实机翻车：11-05 到期的条目显示「还要吗?」）
      bool isStale = stale && isLate && !inv;   // 待确认   → 3px 粗框
      bool isOne   = timeOk() && (left == 1) && !isLate; // 只剩 1 天 → 左竖条
      // 反白独占：粗框/竖条只画在非反白行上，否则黑底上看不见、白烧一次

      if (inv) {
        canvas.fillRect(0, top, SCR_W, 76, 1);
        g_paint = 0;
      } else if (isStale) {
        drawBorder3(top);
      }
      if (isOne) canvas.fillRect(0, top + 4, 5, 68, 1);

      // 标题：字号按字数自适应，永不溢出
      char title[64];
      utf8CopyN(title, sizeof(title), g_items[i].title, TITLE_CHARS);
      drawText2xFont(12, top + 1, title, pickTitleFont(utf8Len(title)));

      // 第二行左：正常写「截止 M月DD日」，逾期写「逾期 M月DD日」
      //   日期改成「11月05日」而不是「11/05」：中文斜杠只有 3px 宽，
      //   夹在七段数字中间像个小点，「11/05」会被看成「11)05」
      //   ⚠️ 不能再把「逾期」二字塞到数字左侧——56px 大数字占满右侧后，
      //      那里只剩十几 px，必然压标题。用「逾期」替代「截止」语义同样清楚。
      int yy; unsigned mo, d;
      civilFromDays(g_items[i].dueDay, yy, mo, d);
      char db[24];
      snprintf(db, sizeof(db), "%s %d月%02d日", isLate ? "逾期" : "截止", (int)mo, (int)d);
      drawSegText(&SEGF18, 12, top + 45, db, u8g2_font_wqy14_t_gb2312a, 1);

      // 第二行右：只有待确认才问「还要吗」，其余情况留白更干净
      if (isStale) {
        const char *q = "还要吗?";
        int16_t qw = textWidth1x(q, u8g2_font_wqy16_t_gb2312a);
        drawText1xFont(SCR_W - 12 - qw, top + 46, q, u8g2_font_wqy16_t_gb2312a);
      }

      // 右侧天数：56px 七段数码管数字，右对齐到「天」字左侧
      //   数字墨迹 top+8 ~ top+63（DSEG_H=55），「天」底部与之齐平
      if (g_now <= 0 || !timeOk()) {
        // 未对时：不显示天数，避免出现假的「0 天」
      } else if (isStale) {
        // 逾期 + 长期未同步 → 盘问「还要吗?」。56px 数字 + 问句会糊成一团
        //（2026-10-27 实屏发现），盘问时天数让位。未逾期条目永远显示天数。
      } else if (left == 0) {
        drawText2xFont(SCR_W - 12 - 52, top + 20, "今天", u8g2_font_wqy13_t_gb2312a);
      } else {
        int32_t v = (left > 0) ? left : -left;
        char nb[12];
        snprintf(nb, sizeof(nb), "%d", (int)v);

        // 「天」x=362..388；数字右边界收到 354，与「天」留 8px 呼吸空间。
        // （「1」改成靠右连续竖线后占在格子右侧 x=26..33，原来 4px 间距会显得贴脸）
        drawText2xFont(SCR_W - 12 - 26, top + 38, "天", u8g2_font_wqy13_t_gb2312a);
        drawDsegNum(SCR_W - 20 - 26 - dsegWidth(nb), top + 8, nb);
      }

      if (inv) g_paint = 1;

      if (i < ITEM_COUNT - 1 && !inv)
        canvas.drawFastHLine(10, top + 74, SCR_W - 20, 1);
    }
  }

  // ---------- 底部状态栏 ----------
  //   口径用「工作日」而不是「天」：周末不算，才对得上「清单多久没盘过」
  if (timeOk() && g_lastSyncDay > 0) {
    int32_t ago = days - g_lastSyncDay;
    int yy; unsigned mo, d;
    civilFromDays(g_lastSyncDay, yy, mo, d);
    char agoStr[20];
    if (ago <= 0) {
      snprintf(agoStr, sizeof(agoStr), "今天");
    } else {
      int32_t wd = workdaysBetween(g_lastSyncDay, days);
      if (wd <= 0)      snprintf(agoStr, sizeof(agoStr), "%d天前(周末)", (int)ago);
      else if (wd == 1) snprintf(agoStr, sizeof(agoStr), "1个工作日");
      else              snprintf(agoStr, sizeof(agoStr), "%d个工作日", (int)wd);
    }
    char foot[64];
    snprintf(foot, sizeof(foot), "更新 %d月%02d日 (%s)", (int)mo, (int)d, agoStr);
    drawSegText(&SEGF18, 10, 276, foot, u8g2_font_wqy14_t_gb2312a, 1);
  }
  const char *rt = "插电脑可改";
  int16_t rw = textWidth1x(rt, u8g2_font_wqy13_t_gb2312a);
  drawText1xFont(SCR_W - 10 - rw, 277, rt, u8g2_font_wqy13_t_gb2312a);

  // ---- 调试：画布统计（内容是否正常） ----
  {
    uint8_t *pb = canvas.getBuffer();
    uint32_t nz = 0;
    int total = SCR_W * SCR_H / 8;
    for (int k = 0; k < total; k++) if (pb[k]) nz++;
    Serial.printf("[DBG] nUsed=%d worst=%d g_now=%ld canvas非零=%lu/%d\n",
                  nUsed, worstIdx, (long)g_now, (unsigned long)nz, total);
  }
}

// ============================================================
//  把画布送上屏
//  ⚠️ 两个坑（2026-10-27 两次实测推出来的）：
//   1. epd2.writeImage 是底层接口，不走 GxEPD2 的 setRotation/mirror，
//      而这块面板的物理方向 = **上下翻转（垂直镜像）**。
//      实测：不处理→上下颠倒；做 180°→左右镜像；正确解是只做垂直翻转。
//      （也正好对上昨天 drawBitmap + setRotation(2) + mirror(true) 的组合）
//   2. 面板 RAM 里 1=白、0=黑，画布里 bit=1 表示"有墨"，
//      所以必须 invert=true，否则整屏黑底白字（负片）
//  用 writeImage 自带的 mirror_y 参数即可，无需临时缓冲
//
//  全刷 / 局刷（2026-10-27 实测后锁定全刷）：
//    局刷波形 0x22=0xFF 实测 BUSY 4.03s，全刷 0x22=0xC7 是 4.02s —— 一样慢。
//    本面板 OTP 里没有 partial 波形，4.2 秒就是物理下限，局刷没意义还带重影风险。
//    所以这里固定 full=true；保留 full 参数只是为了以后换屏时能直接复用。
// ============================================================
bool     g_needFull  = true;   // 下一次刷屏强制全刷
uint16_t g_partCount = 0;      // 自上次全刷以来连刷了几次局刷（当前恒为 0）
#define  FULL_EVERY 20

void pushCanvasToScreen(bool full) {
  display.epd2.writeImage(canvas.getBuffer(), 0, 0, SCR_W, SCR_H,
                          /*invert=*/true, /*mirror_y=*/true, /*pgm=*/false);
  display.epd2.refresh(full ? false : true);
}

// ============================================================
//  刷屏（v2 加分段计时，串口打印各段耗时）
// ============================================================
void refreshScreen() {
  uint32_t t0 = millis();
  bool full = true;              // 见上面：局刷在本面板不省时间，一律全刷
  Serial.println(F("刷新中..."));

  uint32_t tA = millis();
  renderCanvas();
  uint32_t renderMs = millis() - tA;

  uint32_t tB = millis();
  pushCanvasToScreen(full);
  uint32_t transMs = millis() - tB;

  if (full) { g_needFull = false; g_partCount = 0; }
  else      g_partCount++;

  g_lastDays = (g_now > 0) ? todayDays() : -1;
  g_lastHalf = (g_now > 0)
             ? (int8_t)(((((g_now + TZ_OFFSET) % 86400) + 86400) % 86400 / 3600) < 12 ? 0 : 1)
             : -1;
  g_dirty = false;
  Serial.printf("刷新完成 %lu ms（渲染 %lu + 传屏 %lu）\n",
                (unsigned long)(millis() - t0),
                (unsigned long)renderMs, (unsigned long)transMs);
}

// ============================================================
//  持久化
// ============================================================
// 记录"今天和电脑同步过"，供底栏「更新 M/D（N 天前）」
void markSynced() {
  if (g_now > 0) g_lastSyncDay = todayDays();
}

#define SAVE_PATH "/d.txt"

void saveData() {
  File f = SPIFFS.open(SAVE_PATH, "w");
  if (!f) { Serial.println(F("ERR 存储写入失败")); return; }
  f.printf("T=%ld\n", (long)g_now);
  f.printf("S=%ld\n", (long)g_lastSyncDay);
  for (int i = 0; i < ITEM_COUNT; i++) {
    if (g_items[i].used)
      f.printf("%d=%ld|%s\n", i + 1, (long)g_items[i].dueDay, g_items[i].title);
    else
      f.printf("%d=0|\n", i + 1);
  }
  f.close();
}

void loadData() {
  for (int i = 0; i < ITEM_COUNT; i++) {
    g_items[i].used = false;
    g_items[i].dueDay = 0;
    g_items[i].title[0] = 0;
  }
  if (!SPIFFS.exists(SAVE_PATH)) return;

  File f = SPIFFS.open(SAVE_PATH, "r");
  if (!f) return;

  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (!ln.length()) continue;

    if (ln.startsWith("T=")) {
      g_now = ln.substring(2).toInt();
      g_nowMs = millis();
    } else if (ln.startsWith("S=")) {
      g_lastSyncDay = ln.substring(2).toInt();
    } else if (ln.length() > 2 && isdigit(ln[0]) && ln[1] == '=') {
      int idx = ln[0] - '0' - 1;
      if (idx < 0 || idx >= ITEM_COUNT) continue;
      String v = ln.substring(2);
      int bar = v.indexOf('|');
      if (bar < 0) continue;
      int32_t dd = v.substring(0, bar).toInt();
      String t = v.substring(bar + 1);
      if (dd != 0 && t.length()) {
        g_items[idx].used = true;
        g_items[idx].dueDay = dd;
        utf8CopyN(g_items[idx].title, TITLE_MAX, t.c_str(), TITLE_CHARS);
      }
    }
  }
  f.close();
  Serial.printf("已加载：时间=%ld，条目=%d\n", (long)g_now,
                (g_items[0].used + g_items[1].used + g_items[2].used));
}

// ============================================================
//  串口协议
// ============================================================
void printStatus() {
  int32_t days = (g_now > 0) ? todayDays() : 0;
  int32_t wd = (g_now > 0 && g_lastSyncDay > 0) ? workdaysBetween(g_lastSyncDay, days) : 0;
  Serial.printf("OK STATUS time=%ld today=%ld rtc=%d trusted=%d sync=%ld wd=%ld stale=%d half=%d up=%lu\n",
                (long)g_now, (long)days, g_rtcOk ? 1 : 0, g_timeTrusted ? 1 : 0,
                (long)g_lastSyncDay, (long)wd, (wd >= STALE_WORKDAYS) ? 1 : 0,
                (g_now > 0)
                  ? (int)(((((g_now + TZ_OFFSET) % 86400) + 86400) % 86400 / 3600) < 12 ? 0 : 1)
                  : -1,
                (unsigned long)((millis() - g_bootMs) / 1000));
  for (int i = 0; i < ITEM_COUNT; i++) {
    if (g_items[i].used)
      Serial.printf("  %d|%s|%ld|left=%ld\n", i + 1, g_items[i].title,
                    (long)g_items[i].dueDay, (long)(g_items[i].dueDay - days));
    else
      Serial.printf("  %d|(empty)\n", i + 1);
  }
  Serial.println(F("OK END"));
}

void printRtc() {
  int32_t ep = 0;
  if (!rtcRead(ep)) { Serial.println(F("ERR RTC 读不到（检查 SDA=GPIO1 / SCL=GPIO8）")); return; }
  int32_t local = ep + TZ_OFFSET;
  int32_t days = local / 86400;
  int32_t sod  = local % 86400;
  int y; unsigned mo, d;
  civilFromDays(days, y, mo, d);
  Serial.printf("OK RTC %04d-%02u-%02u %02d:%02d:%02d dow=%d osf=%d temp=%.2fC\n",
                y, mo, d, (int)(sod / 3600), (int)((sod / 60) % 60), (int)(sod % 60),
                (int)((days + 4) % 7) + 1, rtcOsfSet() ? 1 : 0, rtcTemp());
}

void processLine(char *line) {
  char *cmd = strtok(line, "|");
  if (!cmd) return;

  if (!strcmp(cmd, "PING")) {
    Serial.println(F("OK PONG"));
    return;
  }

  if (!strcmp(cmd, "TIME")) {
    char *t = strtok(NULL, "|");
    if (!t) { Serial.println(F("ERR TIME 参数缺失")); return; }
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    // 六个字段必须全有：只发日期会把时间悄悄设成当天 00:00（2026-11-03 审查补）
    if (sscanf(t, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6) {
      Serial.println(F("ERR TIME 格式应为 2026-10-26 16:03:00"));
      return;
    }
    int32_t ep = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400L
                 + h * 3600 + mi * 60 + s - TZ_OFFSET;
    g_now = ep;
    g_nowMs = millis();
    g_timeTrusted = true;      // PC 端给的绝对时间，可信
#if HAS_RTC
    bool ok = rtcWrite(ep);
    g_rtcOk = ok;
    if (!ok) Serial.println(F("WARN RTC 写入失败，只更新了内存时间"));
#endif
    saveData();
    g_dirty = true; g_dirtyMs = millis();
    Serial.printf("OK TIME %ld\n", (long)g_now);
    return;
  }

  if (!strcmp(cmd, "ITEM")) {
    char *pi = strtok(NULL, "|");
    char *pt = strtok(NULL, "|");
    char *pd = strtok(NULL, "|");
    if (!pi || !pt || !pd) { Serial.println(F("ERR ITEM 参数缺失")); return; }
    int idx = atoi(pi) - 1;
    if (idx < 0 || idx >= ITEM_COUNT) { Serial.println(F("ERR ITEM 序号应为 1-3")); return; }

    int y = 0, mo = 0, d = 0;
    if (sscanf(pd, "%d-%d-%d", &y, &mo, &d) < 3) {
      Serial.println(F("ERR ITEM 日期格式应为 2026-11-05"));
      return;
    }
    g_items[idx].used = true;
    g_items[idx].dueDay = daysFromCivil(y, (unsigned)mo, (unsigned)d);
    utf8CopyN(g_items[idx].title, TITLE_MAX, pt, TITLE_CHARS);
    markSynced();
    saveData();
    g_dirty = true; g_dirtyMs = millis();
    Serial.printf("OK ITEM %d\n", idx + 1);
    return;
  }

  if (!strcmp(cmd, "CLR")) {
    char *pi = strtok(NULL, "|");
    int idx = pi ? atoi(pi) - 1 : -1;
    if (idx < 0 || idx >= ITEM_COUNT) { Serial.println(F("ERR CLR 序号应为 1-3")); return; }
    g_items[idx].used = false;
    g_items[idx].title[0] = 0;
    markSynced();
    saveData();
    g_dirty = true; g_dirtyMs = millis();
    Serial.printf("OK CLR %d\n", idx + 1);
    return;
  }

  if (!strcmp(cmd, "CLEARALL")) {
    for (int i = 0; i < ITEM_COUNT; i++) { g_items[i].used = false; g_items[i].title[0] = 0; }
    markSynced();
    saveData();
    g_dirty = true; g_dirtyMs = millis();
    Serial.println(F("OK CLEARALL"));
    return;
  }

  if (!strcmp(cmd, "SHOW")) {
    Serial.println(F("OK SHOW"));
    g_dirty = true;
    g_dirtyMs = millis() - 2000;   // 立即刷
    return;
  }

  // FULL：强制走全刷（清残影）。局刷模式下万一花屏，一条命令就能救回来
  if (!strcmp(cmd, "FULL")) {
    Serial.println(F("OK FULL"));
    g_needFull = true;
    g_dirty = true;
    g_dirtyMs = millis() - 2000;
    return;
  }

  if (!strcmp(cmd, "STATUS")) { printStatus(); return; }

  // DUMP：把当前画布按 hex 输出，电脑端还原成 PNG，用于"截屏自查"
  // 不刷屏，只输出数据（15000 字节 → 30000 字符，约 3 秒）
  if (!strcmp(cmd, "DUMP")) {
    renderCanvas();
    uint8_t *pb = canvas.getBuffer();
    int total = SCR_W * SCR_H / 8;
    Serial.printf("DUMP BEGIN %d %d %d\n", total, SCR_W, SCR_H);
    static const char *HX = "0123456789ABCDEF";
    char line[131];
    int p = 0;
    for (int i = 0; i < total; i++) {
      line[p++] = HX[pb[i] >> 4];
      line[p++] = HX[pb[i] & 0x0F];
      if (p >= 128) { line[p] = 0; Serial.println(line); p = 0; }
    }
    if (p > 0) { line[p] = 0; Serial.println(line); }
    Serial.println(F("DUMP END"));
    return;
  }

#if HAS_RTC
  if (!strcmp(cmd, "RTC")) { printRtc(); return; }
#endif

  Serial.print(F("ERR 未知命令: "));
  Serial.println(cmd);
}

void handleSerial() {
  static char buf[600];
  static size_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0) { buf[len] = 0; processLine(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  g_bootMs = millis();   // 开机时长基准（STATUS 的 up=）
  // ⚠️ 必须在 begin 之前：全刷一次阻塞 4.2 秒，期间来的 USB 命令全靠这个
  //    缓冲区兜着。默认只有 256 字节，一次「同步」的 6 条命令就能撑爆，
  //    丢命令后屏上没反应、又不报错 —— 2026-10-28 龙哥踩的就是这个坑。
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  delay(1500);   // 等 USB CDC 枚举

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" 工作 deadline 提示板 v2.0 （DS3231 已接入）"));
  Serial.println(F("========================================"));

  // ---- RTC 优先：先拿真时间 ----
#if HAS_RTC
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  bool osf = rtcOsfSet();
  int32_t ep = 0;
  if (rtcRead(ep) && !osf) {
    g_now = ep; g_nowMs = millis(); g_rtcOk = true;
    g_timeTrusted = true;          // 电池兜住了、没掉过电 → 时间可信
    Serial.printf("RTC DS3231 在线，时间 %ld\n", (long)g_now);
  } else if (rtcRead(ep)) {
    // OSF=1：芯片掉过电，时间不可信
    Serial.println(F("RTC 在线但 OSF=1（掉过电），暂用存储里的旧时间【标记为不可信】"));
    g_rtcOk = false;
    g_timeTrusted = false;
  } else {
    Serial.println(F("RTC 未找到（检查 SDA=GPIO1 / SCL=GPIO8 / 3.3V / GND）"));
    g_rtcOk = false;
  }
#endif

  // 存储
  if (!SPIFFS.begin(true)) {
    Serial.println(F("ERR SPIFFS 挂载失败"));
  } else {
    int32_t keep = g_now;
    loadData();
    if (g_rtcOk) {           // RTC 有效就以 RTC 为准，覆盖存储里的旧值
      g_now = keep; g_nowMs = millis();
    }
    Serial.printf("SPIFFS: %u / %u 字节\n",
                  (unsigned)(SPIFFS.usedBytes()), (unsigned)(SPIFFS.totalBytes()));
  }

#if HAS_RTC
  // 时间已经有了但 RTC 无效（新芯片/掉过电）→ 把内存时间写进 RTC
  if (!g_rtcOk && g_now > 0) {
    if (rtcWrite(g_now)) { g_rtcOk = true; Serial.println(F("已把当前时间写入 RTC")); }
  }
#endif

  // 屏幕
  SPI.begin(EPD_SCLK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(2);
  display.mirror(true);
  Serial.println(F("屏幕初始化完成"));

  Serial.println(F("--- 等待 USB 命令（PING 测通） ---"));
  printStatus();

  refreshScreen();
}

void loop() {
  handleSerial();

  // 内容变化后合并 1.5 秒再刷（避免连续命令刷多次）。
  // ⚠️ 必须等串口收干净再刷：全刷阻塞 4.2 秒，这期间 loop 读不到任何命令，
  //    2026-10-28 实测复现：改一条 → 1.5s 后自动全刷 → 此时点「同步」，
  //    后续 ITEM 全落在这 4.2 秒里被丢掉，而且设备一声不响。
  if (g_dirty && (millis() - g_dirtyMs > 1500) && !Serial.available()) {
    refreshScreen();
  }

  // 每分钟：从 RTC 校准 + 检查跨天 / 跨半天
  if (!g_dirty) {
    static uint32_t lastChk = 0;
    if (millis() - lastChk > 60000) {   // 每分钟查一次
      lastChk = millis();
#if HAS_RTC
      bool before = g_rtcOk;
      rtcSync();                       // 有 RTC 就每分钟校准，漂移归零
      if (!before && g_rtcOk) Serial.println(F("RTC 恢复在线"));
#endif
      int32_t d = todayDays();
      int8_t  half = (g_now > 0)
                   ? (int8_t)(((((g_now + TZ_OFFSET) % 86400) + 86400) % 86400 / 3600) < 12 ? 0 : 1)
                   : -1;
      bool dayChg  = (g_lastDays >= 0 && d    != g_lastDays);
      bool halfChg = (g_lastHalf >= 0 && half != g_lastHalf);
      if (dayChg || halfChg) {
        Serial.println(dayChg ? F("跨天，自动全刷") : F("跨半天，自动全刷"));
        g_needFull = true;   // 跨天/换半天都是整块文字变，残影最明显，必须全刷
        refreshScreen();
      }
    }
  }

  delay(20);
}
