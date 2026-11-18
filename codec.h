// =====================================================================
//  codec.h — 编解码层(纯函数,无业务状态)
//  · UIOT 报文编解码:AES-256-ECB/PKCS7 → hex → base64,及其逆
//  · MD5 签名、毫秒时间戳
//  · 轻量 JSON 字段提取(避免引入 JSON 库)
//  线程安全:每次调用独立 malloc/free,可同时被主线程(MQTT 回调)
//  与后台核(云任务)调用而不互相干扰。
// =====================================================================
#pragma once
#include <Arduino.h>
#include <time.h>
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "config.h"   // APP_SECRET

static String toHex(const uint8_t* buf, size_t len) {
  static const char* h = "0123456789abcdef";
  String s; s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) { s += h[buf[i] >> 4]; s += h[buf[i] & 0xF]; }
  return s;
}

static String md5Hex(const String& in) {
  uint8_t out[16];
  const mbedtls_md_info_t* mi = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
  mbedtls_md(mi, (const unsigned char*)in.c_str(), in.length(), out);
  return toHex(out, 16);
}

static String nowMs() {                      // 毫秒级 epoch(服务器校验时效)
  return String((uint64_t)time(nullptr) * 1000ULL);
}

// UIOT encrypt1(): AES-256-ECB + PKCS7 → hex → base64(hex)
static String aesEcbB64(const String& plain) {
  size_t L = plain.length(), pad = 16 - (L % 16), total = L + pad;
  uint8_t* buf = (uint8_t*)malloc(total);
  uint8_t* enc = (uint8_t*)malloc(total);
  memcpy(buf, plain.c_str(), L);
  for (size_t i = L; i < total; i++) buf[i] = (uint8_t)pad;        // PKCS7
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, (const unsigned char*)APP_SECRET, 256);
  for (size_t off = 0; off < total; off += 16)
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, buf + off, enc + off);
  mbedtls_aes_free(&aes);
  String hex = toHex(enc, total);
  free(buf); free(enc);
  size_t cap = ((hex.length() + 2) / 3) * 4 + 4, olen = 0;
  unsigned char* b64 = (unsigned char*)malloc(cap);
  mbedtls_base64_encode(b64, cap, &olen, (const unsigned char*)hex.c_str(), hex.length());
  b64[olen] = 0;
  String out = String((char*)b64);
  free(b64);
  return out;
}

// UIOT decrypt1(): base64 → hex → AES-256-ECB 解密 → 去 PKCS7
static String aesEcbDecrypt(const String& b64) {
  size_t cap = (b64.length() / 4) * 3 + 8, hexlen = 0;
  unsigned char* hexbuf = (unsigned char*)malloc(cap);
  if (mbedtls_base64_decode(hexbuf, cap, &hexlen,
        (const unsigned char*)b64.c_str(), b64.length()) != 0) { free(hexbuf); return ""; }
  auto hv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  size_t n = hexlen / 2;
  if (n == 0 || n % 16) { free(hexbuf); return ""; }
  uint8_t* enc = (uint8_t*)malloc(n);
  for (size_t i = 0; i < n; i++) enc[i] = (hv(hexbuf[2*i]) << 4) | hv(hexbuf[2*i+1]);
  free(hexbuf);
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, (const unsigned char*)APP_SECRET, 256);
  uint8_t* dec = (uint8_t*)malloc(n);
  for (size_t off = 0; off < n; off += 16)
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, enc + off, dec + off);
  mbedtls_aes_free(&aes);
  free(enc);
  size_t out_len = n;                          // 去 PKCS7
  uint8_t p = dec[n - 1];
  if (p >= 1 && p <= 16 && p <= n) out_len = n - p;
  String out; out.reserve(out_len);
  for (size_t i = 0; i < out_len; i++) out += (char)dec[i];
  free(dec);
  return out;
}

// -------- 轻量 JSON 字段提取 --------
// 抠 "key":123 或 "key":"123" 的数字串
static String jsonNum(const String& s, const char* key, bool* ok) {
  int p = s.indexOf(String("\"") + key + "\":");
  if (p < 0) { *ok = false; return ""; }
  p += strlen(key) + 3;
  while (p < (int)s.length() && (s[p] == '"' || s[p] == ' ')) p++;
  int e = p;
  while (e < (int)s.length() && s[e] >= '0' && s[e] <= '9') e++;
  *ok = (e > p);
  return s.substring(p, e);
}
// 抠 "key":"value" 的字符串值
static String jsonStr(const String& s, const char* key) {
  int p = s.indexOf(String("\"") + key + "\":\"");
  if (p < 0) return "";
  p += strlen(key) + 4;
  int e = s.indexOf('"', p);
  return e < 0 ? String("") : s.substring(p, e);
}
// 精确定位 "deviceId":<id>(其后须非数字,避免 24 命中 247)
static int findDevicePos(const String& s, const String& devId) {
  String key = "\"deviceId\":";
  int from = 0;
  while (true) {
    int p = s.indexOf(key, from);
    if (p < 0) return -1;
    int q = p + key.length();
    while (q < (int)s.length() && s[q] == '"') q++;
    int e = q;
    while (e < (int)s.length() && s[e] >= '0' && s[e] <= '9') e++;
    if (s.substring(q, e) == devId) return p;
    from = p + key.length();
  }
}
