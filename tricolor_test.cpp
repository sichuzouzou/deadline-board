/*
 * tricolor_test.cpp —— 三色红层「互补双带」极性判定（一次性测试）
 *
 * 为什么要这么测：
 *   昨天试了三个版本（0xC7 / 0xF7 / 加指针复位）都不出红，属于"改一个命令试一次"，
 *   变量没分离，等于在猜。本测试把「红层数据极性」这个唯一变量做成二分：
 *
 *   屏幕从上到下：
 *     y   0..19    黑条（标记"上"边缘）
 *     y  20..149   红带 A：redPlane = 0xFF  （假设"1=红"）
 *     y 150..279   红带 B：redPlane = 0x00  （假设"0=红"）
 *     y 280..299   黑条（标记"下"边缘）
 *     四周          3px 黑边框
 *
 *   驱动对红层做 _transfer(~data)，所以实际发到芯片的是：
 *     红带 A → 发 0x00
 *     红带 B → 发 0xFF
 *
 *   判读（三种结果都有明确结论，不会白烧）：
 *     靠上黑条那段红  → 芯片 0x26 里 0 = 红
 *     靠下黑条那段红  → 芯片 0x26 里 1 = 红
 *     两段都白        → 极性问题不成立，转查 LUT / Panel Setting
 *
 * 依据：厂家 Basic demo 里 R_Buffer「置 1 = 红」，发送时 ~R_datas（因此发 0=红）；
 *       但它的清屏函数又给 0x26 发 0x00，按"0=红"清屏会整屏变红，自相矛盾。
 *       → 该 demo 用的是 2.9 寸图（Ap_29demo），疑为小屏代码套用，不可作为权威。
 *       → 因此只能实测。
 */

#include <Arduino.h>
#include <string.h>
#include <GxEPD2_420c_Z96.h>
#include <GxEPD2_BW.h>

extern GxEPD2_BW<GxEPD2_420c_Z96, GxEPD2_420c_Z96::HEIGHT> display;

#define TR_W   400
#define TR_H   300
#define TR_WB  (TR_W / 8)

static uint8_t blackPlane[TR_H * TR_WB];
static uint8_t redPlane[TR_H * TR_WB];

void runTricolorTest() {
  Serial.println();
  Serial.println(F("=== 三色红层极性判定（互补双带） ==="));
  Serial.println(F("屏面从上到下：黑条 / 红带A(源0xFF) / 红带B(源0x00) / 黑条"));

  // 全白底、无红
  memset(blackPlane, 0xFF, sizeof(blackPlane));
  memset(redPlane, 0x00, sizeof(redPlane));

  // 上下黑条（方向锚点）
  for (int y = 0; y < 20; y++)        memset(&blackPlane[y * TR_WB], 0x00, TR_WB);
  for (int y = 280; y < TR_H; y++)    memset(&blackPlane[y * TR_WB], 0x00, TR_WB);

  // 红带 A：源数据 0xFF（驱动取反后发 0x00）
  for (int y = 20; y < 150; y++)      memset(&redPlane[y * TR_WB], 0xFF, TR_WB);
  // 红带 B：源数据保持 0x00（驱动取反后发 0xFF）

  // 左右黑边框
  for (int y = 0; y < TR_H; y++) {
    blackPlane[y * TR_WB] = 0x00;
    blackPlane[y * TR_WB + TR_WB - 1] = 0x00;
    redPlane[y * TR_WB] = 0x00;
    redPlane[y * TR_WB + TR_WB - 1] = 0x00;
  }
  Serial.println(F("图案已生成"));

  display.setFullWindow();
  uint32_t t0 = millis();
  display.epd2.writeImage(blackPlane, redPlane, 0, 0, TR_W, TR_H, false, false, false);
  Serial.printf("双平面写入 %lu ms\n", (unsigned long)(millis() - t0));

  uint32_t t1 = millis();
  display.epd2.refresh(false);
  Serial.printf("刷新完成 %lu ms\n", (unsigned long)(millis() - t1));

  Serial.println(F("--- 请判读屏幕 ---"));
  Serial.println(F("  靠【上】黑条那段红 -> 芯片 0x26 的 0 是红"));
  Serial.println(F("  靠【下】黑条那段红 -> 芯片 0x26 的 1 是红"));
  Serial.println(F("  两段都白          -> 极性问题不成立，查 LUT/初始化"));
}
