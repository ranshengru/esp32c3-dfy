#ifndef COMPANION_MOOD_H
#define COMPANION_MOOD_H

// Eight expressions. 困 (drowsy) and 打盹 (mask on) stay separate.
// No hungry / thinking / hot / cold faces: those lines reuse one of these.
enum {
    EXPR_NEUTRAL = 0,
    EXPR_HAPPY,
    EXPR_SLEEPY,
    EXPR_SLEEPING,
    EXPR_TSUNDERE,
    EXPR_SURPRISED,
    EXPR_WORRIED,
    EXPR_EXCITED,
    EXPR_COUNT
};

const char *mood_name(int expr);
int mood_parse(const char *tag);
int mood_from_text(const char *text);

#endif
