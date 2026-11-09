#!/usr/bin/env python3
"""Copy the DeepSeek prompts from firmware/idf/main/prompts.h into
firmware/idf/README.md verbatim (between the PROMPTS markers).
Run after editing prompts.h; `--check` exits 1 if the README is stale."""
import ast, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware" / "idf" / "main" / "prompts.h"
README = ROOT / "firmware" / "idf" / "README.md"
BEGIN, END = "<!-- PROMPTS BEGIN (tools/prompts_to_readme.py) -->", "<!-- PROMPTS END -->"


def c_string(name, text):
    body = re.search(r"%s\[\] =\s*(.*?);\n" % name, text, re.S).group(1)
    return "".join(ast.literal_eval(part) for part in re.findall(r'"(?:\\.|[^"\\])*"', body))


def block():
    text = HEADER.read_text(encoding="utf-8")
    out = [BEGIN, "", "系统提示词 `PET_SYSTEM_PROMPT`（原文）：", "", "```text",
           c_string("PET_SYSTEM_PROMPT", text), "```", "",
           "用户提示词模板 `PET_USER_PROMPT`（原文，`{{…}}` 由设备在请求前填好）：", "", "```text",
           c_string("PET_USER_PROMPT", text), "```", "", END]
    return "\n".join(out)


readme = README.read_text(encoding="utf-8")
new = re.sub(re.escape(BEGIN) + ".*?" + re.escape(END), lambda _m: block(), readme, flags=re.S)
if "--check" in sys.argv:
    sys.exit(0 if new == readme else 1)
README.write_text(new, encoding="utf-8")
print("README prompts synced")
