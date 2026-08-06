#ifndef LTP_BOOT_H
#define LTP_BOOT_H
/* ---------------------------------------------------------------------------
 * ltp_boot.h — ВТОРАЯ копия протокола LTP, для загрузчика (.bootsec).
 *
 * ЗАЧЕМ КОПИЯ. Обычный ltp.c живёт в области приложения и во время заливки
 * стирается — звать его оттуда нельзя (правило 1 в boot.h). Просто «положить
 * ltp.c в .bootsec» тоже не годится: тогда приложение вызывало бы его по
 * адресу СВОЕЙ сборки, а во флеше лежала бы секция от предыдущей прошивки по
 * SWD — адреса разъехались бы молча (правило 3).
 *
 * Поэтому две копии, но ОДИН исходник: ltp_boot.c переименовывает символы
 * этими макросами и включает ltp.c целиком. Реализация физически одна —
 * разъехаться нечему.
 * ------------------------------------------------------------------------- */

/* Переименование внешних символов: у загрузчика — свои. */
#define ltp_crc16        boot_ltp_crc16
#define ltp_build        boot_ltp_build
#define ltp_parser_init  boot_ltp_parser_init
#define ltp_parser_feed  boot_ltp_parser_feed

/* ⚠ libc здесь недоступна (лежит в стираемой области), поэтому ltp.c не должен
 * её звать вообще. Единственное обращение (memset в ltp_parser_init) убрано в
 * самом ltp.c — ручным циклом. Появится новое — оно молча утянет в .bootsec
 * ссылку на область приложения, и загрузчик упадёт посреди заливки. */

#include "ltp.h"

/* Типы у копии те же — они существуют только на этапе компиляции. Отдельные
 * имена нужны лишь для читаемости кода загрузчика. */
typedef LtpParser BootLtpParser;
typedef LtpPacket BootLtpPacket;

#define BOOT_LTP_FLAG_DIR         LTP_FLAG_DIR
#define BOOT_LTP_FLAG_ERR         LTP_FLAG_ERR
#define BOOT_LTP_ERR_UNKNOWN_CMD  LTP_ERR_UNKNOWN_CMD
#define BOOT_LTP_MAX_FRAME(n)     LTP_MAX_FRAME(n)

#endif /* LTP_BOOT_H */
