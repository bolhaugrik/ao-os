/* CMOS RTC a 0x70/0x71 portokon. BCD vagy binaris, 12 vagy 24 oras mod a B statusz szerint.
 * Az ora helyi ido: a netbookon a BIOS es a korabbi rendszerek is igy hasznaltak. */
#include "rtc.h"
#include "../arch/io.h"

#define REG_SEC 0x00
#define REG_MIN 0x02
#define REG_HOUR 0x04
#define REG_DAY 0x07
#define REG_MON 0x08
#define REG_YEAR 0x09
#define REG_A 0x0A
#define REG_B 0x0B
#define REG_CENTURY 0x32

static u8 cmos_read(u8 reg)
{
    outb(0x70, (u8)(reg | 0x80));       /* NMI tiltva a hozzaferes alatt */
    return inb(0x71);
}

static void cmos_write(u8 reg, u8 v)
{
    outb(0x70, (u8)(reg | 0x80));
    outb(0x71, v);
}

static u8 bcd2bin(u8 v) { return (u8)((v & 15) + (v >> 4) * 10); }
static u8 bin2bcd(u8 v) { return (u8)(((v / 10) << 4) | (v % 10)); }

static void read_raw(struct rtc_time *t, bool *bcd, bool *h24)
{
    u8 b = cmos_read(REG_B);
    *bcd = !(b & 4);
    *h24 = (b & 2) != 0;
    u8 sec = cmos_read(REG_SEC), min = cmos_read(REG_MIN), hour = cmos_read(REG_HOUR);
    u8 day = cmos_read(REG_DAY), mon = cmos_read(REG_MON), year = cmos_read(REG_YEAR);
    u8 cent = cmos_read(REG_CENTURY);
    bool pm = !*h24 && (hour & 0x80);
    hour &= 0x7F;
    if (*bcd) { sec = bcd2bin(sec); min = bcd2bin(min); hour = bcd2bin(hour); day = bcd2bin(day);
                mon = bcd2bin(mon); year = bcd2bin(year); cent = bcd2bin(cent); }
    if (!*h24) { hour %= 12; if (pm) hour = (u8)(hour + 12); }
    if (cent < 19 || cent > 21) cent = 20;
    t->sec = sec; t->min = min; t->hour = hour; t->day = day; t->mon = mon;
    t->year = (u16)(cent * 100 + year);
}

bool rtc_read(struct rtc_time *t)
{
    for (int i = 0; i < 100000 && (cmos_read(REG_A) & 0x80); i++) ;   /* frissites folyamatban */
    struct rtc_time a, b;
    bool bcd, h24;
    read_raw(&a, &bcd, &h24);
    read_raw(&b, &bcd, &h24);
    if (a.sec != b.sec || a.min != b.min) read_raw(&b, &bcd, &h24);
    *t = b;
    return t->mon >= 1 && t->mon <= 12 && t->day >= 1 && t->day <= 31 && t->hour < 24 && t->min < 60 && t->sec < 60;
}

void rtc_write(const struct rtc_time *t)
{
    u8 b = cmos_read(REG_B);
    bool bcd = !(b & 4), h24 = (b & 2) != 0;
    cmos_write(REG_B, (u8)(b | 0x80));                 /* SET: az ora megall a frissites alatt */
    u8 hour = t->hour;
    if (!h24) { bool pm = hour >= 12; hour = (u8)(hour % 12); if (hour == 0) hour = 12; if (bcd) hour = bin2bcd(hour); if (pm) hour |= 0x80; }
    else if (bcd) hour = bin2bcd(hour);
    u8 cent = (u8)(t->year / 100), year = (u8)(t->year % 100);
    cmos_write(REG_SEC, bcd ? bin2bcd(t->sec) : t->sec);
    cmos_write(REG_MIN, bcd ? bin2bcd(t->min) : t->min);
    cmos_write(REG_HOUR, hour);
    cmos_write(REG_DAY, bcd ? bin2bcd(t->day) : t->day);
    cmos_write(REG_MON, bcd ? bin2bcd(t->mon) : t->mon);
    cmos_write(REG_YEAR, bcd ? bin2bcd(year) : year);
    cmos_write(REG_CENTURY, bcd ? bin2bcd(cent) : cent);
    cmos_write(REG_B, (u8)(b & ~0x80));
}

u8 rtc_weekday(const struct rtc_time *t)
{
    /* Sakamoto */
    static const u8 off[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    u32 y = t->year;
    if (t->mon < 3) y--;
    return (u8)((y + y / 4 - y / 100 + y / 400 + off[t->mon - 1] + t->day) % 7);
}

const char *rtc_weekday_name(u8 wd)
{
    static const char *names[7] = { "vasarnap", "hetfo", "kedd", "szerda", "csutortok", "pentek", "szombat" };
    return names[wd % 7];
}
