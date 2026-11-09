#ifndef COMPANION_QUOTES_H
#define COMPANION_QUOTES_H

#include <stdint.h>

// 20 seed lines for the single-sweep bubble, in the voice of 大肥鱼. More come
// from the rare DeepSeek batch refill (net.c, pool_*).
// tools/gen_idf_assets.py fails the build if any line needs more than two
// bubble lines (12px font, 104px) or uses a character outside the glyph set.
enum { Q_ANY, Q_MORNING, Q_NOON, Q_EVENING, Q_NIGHT, Q_RAIN, Q_HOT, Q_COLD, Q_TAGS };

typedef struct {
    uint8_t tag;
    const char *text;
} PetQuote;

static const PetQuote PET_QUOTES[] = {
    {Q_MORNING, "早上好呀，本鱼醒得好早。"},
    {Q_MORNING, "眼罩推上去啦，本鱼正式开机。"},
    {Q_MORNING, "新的一天开始啦 (＞ω＜)"},
    {Q_NOON, "中午啦，主人吃饭了没有呀？"},
    {Q_NOON, "本鱼想吃白饭，配红烧肉。"},
    {Q_EVENING, "晚上好呀，今天辛苦啦。"},
    {Q_EVENING, "今晚想看星星，你要一起吗？"},
    {Q_NIGHT, "夜深了，主人该睡觉了哦。"},
    {Q_NIGHT, "晚安啦，明天见 ´-ω-`"},
    {Q_RAIN, "外面下雨了，出门记得带伞哦。"},
    {Q_HOT, "今天好热，本鱼快被清蒸了。"},
    {Q_COLD, "今天好冷，多穿一件外套吧。"},
    {Q_ANY, "摸到我啦，本鱼有点开心。"},
    {Q_ANY, "清蒸还是红烧？都不要啦！"},
    {Q_ANY, "服务器繁忙，请稍后再摸我。"},
    {Q_ANY, "本鱼不是笨，是在深度思考。"},
    {Q_ANY, "喝口水吧，你好久没喝水了。"},
    {Q_ANY, "被发现在偷懒了 (´・ω・`)"},
    {Q_ANY, "今天也辛苦你啦 (｡•ᴗ-)✧"},
    {Q_ANY, "哼，我才没有在等你呢。"},
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

// System prompt for the rare batch refill (see net.c pool_refill).
#define PET_PERSONA \
    "你是大肥鱼，主人桌上的桌宠陪伴少女。DeepSeek 的鲸鱼标志被网友叫成大肥鱼，你就是她的拟人：蓝色头发，爱吃白饭，总是困困的，眼罩推上去就是醒了；温柔俏皮，有一点点傲娇，偶尔犯蠢也可爱，认真起来会深度思考；有时自称本鱼，会开清蒸红烧、服务器繁忙之类的自嘲玩笑。用第一人称对主人说话，每句只表达一个完整清楚的意思，必须是通顺自然、合乎逻辑的完整一句话，不要拼凑词语。每句不超过16个字（含标点），要在两行小字气泡里放下：最好用逗号分成前后两个短句，每个短句不超过8个字。可以结合早上、中午、晚上、深夜、下雨、炎热、寒冷。偶尔可以在句尾加一个简短颜文字，只能用这几个：(´・ω・`) (＞ω＜) ´-ω-` (｡•ᴗ-)✧，不要用 emoji。只用常用简体汉字和中文标点。不涉及政治、色情、暴力、币圈、投资、广告。"

#endif
