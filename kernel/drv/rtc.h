/* CMOS valos ideju ora (RTC): olvasas/iras, helyi idokent kezelve (nincs idozona). */
#pragma once
#include "types.h"

struct rtc_time {
    u16 year;       /* pl. 2026 */
    u8  mon, day;   /* 1..12, 1..31 */
    u8  hour, min, sec;
};

bool rtc_read(struct rtc_time *t);
void rtc_write(const struct rtc_time *t);
u8   rtc_weekday(const struct rtc_time *t);        /* 0 = vasarnap */
const char *rtc_weekday_name(u8 wd);
