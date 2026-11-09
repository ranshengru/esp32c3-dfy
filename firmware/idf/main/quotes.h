#ifndef COMPANION_QUOTES_H
#define COMPANION_QUOTES_H

#include <stdint.h>

// 20 seed lines for the single-sweep bubble, in the voice of 大肥鱼. More come
// from the rare DeepSeek batch refill (net.c pool_refill, prompts in prompts.h).
// Every line carries a bucket: a time of day x a condition. At display time the
// device only picks lines whose time is T_ANY or the current time bucket and
// whose condition is C_GENERAL or one that holds right now (see net.c
// context_now). {temp} and {weather} are filled at display time ("7°C", "小雨").
// tools/gen_idf_assets.py fails the build if any line (filled with the widest
// values "-10°C" / "雷阵雨") needs more than two bubble lines (12px font,
// 104px) or uses a character outside the glyph set.
enum { T_ANY, T_MORNING, T_NOON, T_AFTERNOON, T_EVENING, T_NIGHT, T_LATE, T_COUNT };
enum { C_GENERAL, C_RAIN, C_HOT, C_COLD, C_CLEAR, C_WEEKEND, C_COUNT };

// Hours of each time bucket: morning 5-10, noon 11-13, afternoon 14-16,
// evening 17-19, night 20-22, late-night 23-4.
static const char *const T_NAMES[T_COUNT] = {"any", "morning", "noon", "afternoon", "evening", "night", "late-night"};
static const char *const C_NAMES[C_COUNT] = {"general", "rain", "hot", "cold", "clear", "weekend"};

typedef struct {
    uint8_t time;
    uint8_t cond;
    const char *text;
} PetQuote;

static const PetQuote PET_QUOTES[] = {
    {T_MORNING, C_GENERAL, "早上好呀，本鱼醒得好早。"},
    {T_MORNING, C_GENERAL, "眼罩推上去啦，本鱼正式开机。"},
    {T_MORNING, C_COLD, "早上才{temp}，起床多穿点哦。"},
    {T_NOON, C_GENERAL, "中午啦，主人吃饭了没有呀？"},
    {T_NOON, C_GENERAL, "本鱼想吃白饭，配红烧肉。"},
    {T_AFTERNOON, C_GENERAL, "下午好困呀，一起摸会儿鱼？"},
    {T_EVENING, C_GENERAL, "今天也辛苦你啦 (｡•ᴗ-)✧"},
    {T_NIGHT, C_CLEAR, "今晚天气晴，想和你看星星。"},
    {T_LATE, C_GENERAL, "这么晚了，主人该睡觉了哦。"},
    {T_LATE, C_GENERAL, "本鱼困了，晚安啦 ´-ω-`"},
    {T_ANY, C_RAIN, "外面{weather}，出门记得带伞。"},
    {T_ANY, C_HOT, "现在{temp}，本鱼快被清蒸了。"},
    {T_ANY, C_COLD, "才{temp}呀，多穿一件外套吧。"},
    {T_ANY, C_WEEKEND, "周末啦，多睡一会儿吧。"},
    {T_ANY, C_GENERAL, "摸到我啦，本鱼有点开心。"},
    {T_ANY, C_GENERAL, "清蒸还是红烧？都不要啦！"},
    {T_ANY, C_GENERAL, "服务器繁忙，请稍后再摸我。"},
    {T_ANY, C_GENERAL, "本鱼不是笨，是在深度思考。"},
    {T_ANY, C_GENERAL, "被发现在偷懒了 (´・ω・`)"},
    {T_ANY, C_GENERAL, "哼，我才没有在等你呢。"},
};

#define PET_QUOTE_COUNT ((int)(sizeof(PET_QUOTES) / sizeof(PET_QUOTES[0])))

// Birthday-mode sweep lines (shuffle bag, no repeat until all 10 are shown).
// {n} is replaced by the configured birthday name; 🎂 is drawn as the v0.1
// cake icon. The build checks each one with a 3-hanzi name in <= 2 lines.
static const char *const BIRTHDAY_WISHES[] = {
    "{n}，生日快乐呀🎂",
    "祝{n}生日快乐，天天开心！",
    "今天{n}最大，本鱼都听你的。",
    "{n}生日快乐 (＞ω＜)",
    "白饭让给{n}吃，生日快乐！",
    "{n}又长大一岁，要更快乐哦。",
    "许个愿吧{n}，本鱼帮你保密。",
    "蛋糕分本鱼一口嘛，{n}🎂",
    "愿{n}新的一岁，平安喜乐。",
    "{n}今天最好看，生日快乐！",
};

#define BIRTHDAY_WISH_COUNT ((int)(sizeof(BIRTHDAY_WISHES) / sizeof(BIRTHDAY_WISHES[0])))

#endif
