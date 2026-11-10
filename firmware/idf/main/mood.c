#include "mood.h"

#include <string.h>

static const char *const MOOD_NAMES[EXPR_COUNT] = {
    "neutral", "happy", "sleepy", "sleeping", "tsundere", "surprised", "worried", "excited", "rice",
};

const char *mood_name(int expr)
{
    if (expr < 0 || expr >= EXPR_COUNT)
        return "neutral";
    return MOOD_NAMES[expr];
}

int mood_parse(const char *tag)
{
    if (!tag || !tag[0])
        return -1;
    for (int i = 0; i < EXPR_COUNT; ++i)
        if (!strcmp(tag, MOOD_NAMES[i]))
            return i;
    return -1;
}

int mood_from_text(const char *text)
{
    if (!text)
        return EXPR_NEUTRAL;
    if (strstr(text, "打盹") || strstr(text, "眼罩"))
        return EXPR_SLEEPING;
    if (strstr(text, "困") || strstr(text, "睡") || strstr(text, "晚安"))
        return EXPR_SLEEPY;
    if (strstr(text, "哼") || strstr(text, "才没有") || strstr(text, "才不"))
        return EXPR_TSUNDERE;
    if (strstr(text, "饭") || strstr(text, "饿") || strstr(text, "吃"))
        return EXPR_RICE;
    if (strstr(text, "哇") || strstr(text, "诶"))
        return EXPR_SURPRISED;
    if (strstr(text, "开心") || strstr(text, "好耶") || strstr(text, "快乐") || strstr(text, "＞ω＜"))
        return EXPR_HAPPY;
    if (strstr(text, "伞") || strstr(text, "冷") || strstr(text, "小心") || strstr(text, "别"))
        return EXPR_WORRIED;
    return EXPR_NEUTRAL;
}
