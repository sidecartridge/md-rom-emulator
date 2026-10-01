"""The ST side and the RP side agree on what they share.

The ST's names come from target/atarist/src/main.s and its includes; the RP's
from the headers under rp/src/include. Each row of PAIRS is one shared
constant: adding a constant both sides must agree on means adding a row. A
mismatch is the "works on the RP, garbage on the ST" bug, so fix the sources,
never the table.

The self-check cartridge and the tools must agree too: the pattern the
cartridge checks is the one make_rom_images.py writes, and the reset agent's
signature is where swd.py st-reset writes it.
"""

import ast
import importlib.util
import os
import re
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ST_SRC = os.path.join(REPO, "target", "atarist", "src")
RP_SRC = os.path.join(REPO, "rp", "src")
TOOLS = os.path.join(REPO, "tools", "dev")


def evaluate(expr, known):
    """An integer expression of numbers, known names and + - * / << >> & | ^,
    or None when it uses anything else."""
    expr = expr.strip()
    for name in sorted(set(re.findall(r"\b[A-Za-z_]\w*", expr)), key=len, reverse=True):
        if name not in known:
            return None
        expr = re.sub(rf"\b{name}\b", str(known[name]), expr)
    try:
        tree = ast.parse(expr, mode="eval")
    except SyntaxError:
        return None
    allowed = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant, ast.Add,
               ast.Sub, ast.Mult, ast.Div, ast.FloorDiv, ast.LShift,
               ast.RShift, ast.BitAnd, ast.BitOr, ast.BitXor, ast.USub,
               ast.Invert)
    if not all(isinstance(n, allowed) for n in ast.walk(tree)):
        return None
    value = eval(compile(tree, "<layout>", "eval"), {"__builtins__": {}})
    return int(value) if isinstance(value, (int, float)) else None


def asm_equs(*paths):
    """NAME equ EXPR from vasm sources: $hex, %binary and decimal numbers."""
    raw = []
    for path in paths:
        with open(path, encoding="latin-1") as f:
            for line in f:
                m = re.match(r"^([A-Za-z_]\w*):?\s+equ\s+([^;]+)", line, re.I)
                if m:
                    expr = re.sub(r"\$([0-9A-Fa-f]+)", r"0x\1", m.group(2))
                    expr = re.sub(r"%([01]+)", r"0b\1", expr)
                    raw.append((m.group(1), expr))
    return resolve(raw)


def c_defines(*paths):
    """#define NAME EXPR from C sources, without casts and number suffixes."""
    raw = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text).replace("\\\n", " ")
        for name, expr in re.findall(r"^\s*#\s*define\s+([A-Za-z_]\w*)[ \t]+([^\n]+)$",
                                     text, re.M):
            expr = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", expr)
            expr = re.sub(r"\(\s*(?:u?int\d+_t|unsigned|int|long|size_t)\s*\)", " ", expr)
            raw.append((name, expr))
    return resolve(raw)


def resolve(raw):
    known = {}
    for _ in range(len(raw) + 1):  # definitions may refer to later ones
        progress = False
        for name, expr in raw:
            if name not in known:
                value = evaluate(expr, known)
                if value is not None:
                    known[name] = value
                    progress = True
        if not progress:
            break
    return known


def st_names():
    inc = os.path.join(ST_SRC, "inc")
    return asm_equs(os.path.join(ST_SRC, "main.s"),
                    *(os.path.join(inc, f) for f in sorted(os.listdir(inc))
                      if f.endswith(".s")))


def rp_names():
    inc = os.path.join(RP_SRC, "include")
    return c_defines(*(os.path.join(inc, f) for f in
                       ("constants.h", "chandler.h", "display.h", "term.h",
                        "tprotocol.h")))


def window(st, name):
    """An ST address in the cartridge window, as an offset into it."""
    return st[name] - st["ROM4_ADDR"]


# (what it is, the ST's value, the RP's value)
PAIRS = [
    ("cartridge code size", lambda st: st["CARTRIDGE_CODE_SIZE"],
     lambda rp: rp["CHANDLER_CARTRIDGE_CODE_SIZE"]),
    ("shared block", lambda st: window(st, "SHARED_BLOCK_ADDR"),
     lambda rp: rp["CHANDLER_SHARED_BLOCK_OFFSET"]),
    ("command sentinel", lambda st: window(st, "CMD_MAGIC_SENTINEL_ADDR"),
     lambda rp: rp["CHANDLER_CMD_SENTINEL_OFFSET"]),
    ("display command word, the sentinel",
     lambda st: window(st, "CMD_MAGIC_SENTINEL_ADDR"),
     lambda rp: rp["DISPLAY_COMMAND_ADDRESS"]),
    ("random token", lambda st: window(st, "RANDOM_TOKEN_ADDR"),
     lambda rp: rp["CHANDLER_RANDOM_TOKEN_OFFSET"]),
    ("random token seed", lambda st: window(st, "RANDOM_TOKEN_SEED_ADDR"),
     lambda rp: rp["CHANDLER_RANDOM_TOKEN_SEED_OFFSET"]),
    ("reserved slot", lambda st: window(st, "RESERVED_SLOT_ADDR"),
     lambda rp: rp["CHANDLER_RESERVED_OFFSET"]),
    ("shared variables", lambda st: window(st, "SHARED_VARIABLES"),
     lambda rp: rp["CHANDLER_SHARED_VARIABLES_OFFSET"]),
    ("app buffers", lambda st: window(st, "APP_BUFFERS_ADDR"),
     lambda rp: rp["CHANDLER_APP_BUFFERS_OFFSET"]),
    ("high-resolution translation table", lambda st: window(st, "TRANSTABLE"),
     lambda rp: rp["CHANDLER_HIGHRES_TRANSTABLE_OFFSET"]),
    ("high-resolution translation table, display.h",
     lambda st: window(st, "TRANSTABLE"),
     lambda rp: rp["DISPLAY_HIGHRES_TRANSTABLE_ADDR"]),
    ("app free area", lambda st: window(st, "APP_FREE_ADDR"),
     lambda rp: rp["CHANDLER_APP_FREE_OFFSET"]),
    ("framebuffer", lambda st: window(st, "FRAMEBUFFER_ADDR"),
     lambda rp: rp["CHANDLER_FRAMEBUFFER_OFFSET"]),
    ("framebuffer, display.h", lambda st: window(st, "FRAMEBUFFER_ADDR"),
     lambda rp: rp["DISPLAY_BUFFER_OFFSET"]),
    ("framebuffer size", lambda st: st["FRAMEBUFFER_SIZE"],
     lambda rp: rp["CHANDLER_FRAMEBUFFER_SIZE"]),
    ("framebuffer size, display.h", lambda st: st["FRAMEBUFFER_SIZE"],
     lambda rp: rp["DISPLAY_BUFFER_SIZE"]),
    ("ROM3 follows ROM4", lambda st: st["ROMCMD_START_ADDR"] - st["ROM4_ADDR"],
     lambda rp: rp["ROM_SIZE_BYTES"]),
    ("command header", lambda st: st["CMD_MAGIC_NUMBER"],
     lambda rp: rp["PROTOCOL_HEADER"]),
    ("CMD_ST_HELLO", lambda st: st["CMD_ST_HELLO"],
     lambda rp: rp["CHANDLER_ST_HELLO"]),
    ("CMD_SET_SHARED_VAR", lambda st: st["CMD_SET_SHARED_VAR"],
     lambda rp: rp["CHANDLER_SET_SHARED_VAR"]),
    ("CMD_NOP", lambda st: st["CMD_NOP"], lambda rp: rp["DISPLAY_COMMAND_NOP"]),
    ("CMD_RESET", lambda st: st["CMD_RESET"], lambda rp: rp["DISPLAY_COMMAND_RESET"]),
    ("CMD_BOOT_GEM", lambda st: st["CMD_BOOT_GEM"],
     lambda rp: rp["DISPLAY_COMMAND_CONTINUE"]),
    ("CMD_TERMINAL", lambda st: st["CMD_TERMINAL"],
     lambda rp: rp["DISPLAY_COMMAND_TERM"]),
    ("CMD_START", lambda st: st["CMD_START"],
     lambda rp: rp["DISPLAY_COMMAND_START"]),
    ("APP_TERMINAL", lambda st: st["APP_TERMINAL"], lambda rp: rp["APP_TERMINAL"]),
    ("APP_TERMINAL_START", lambda st: st["APP_TERMINAL_START"],
     lambda rp: rp["APP_TERMINAL_START"]),
    ("APP_TERMINAL_KEYSTROKE", lambda st: st["APP_TERMINAL_KEYSTROKE"],
     lambda rp: rp["APP_TERMINAL_KEYSTROKE"]),
]


def image_tool():
    spec = importlib.util.spec_from_file_location(
        "make_rom_images", os.path.join(TOOLS, "make_rom_images.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Layout(unittest.TestCase):

    def test_pairs_agree(self):
        st, rp = st_names(), rp_names()
        for what, st_value, rp_value in PAIRS:
            with self.subTest(what):
                self.assertEqual(st_value(st), rp_value(rp),
                                 f"{what}: the ST side and the RP side differ")

    def test_selfcheck_pattern(self):
        cart = asm_equs(os.path.join(TOOLS, "selfcheck", "selfcheck.s"))
        tool = image_tool()
        self.assertEqual(cart["PATTERN_XOR"], tool.PATTERN_XOR)
        self.assertEqual(cart["PATTERN_FROM"], tool.SELFCHECK_PATTERN_FROM)
        self.assertEqual(cart["WINDOW_BYTES"], tool.WINDOW)

    def test_agent_signature(self):
        """swd.py st-reset writes the reset agent's signature where agent.s
        reads it."""
        agent = asm_equs(os.path.join(TOOLS, "selfcheck", "agent.s"))
        with open(os.path.join(TOOLS, "swd.py"), encoding="utf-8") as f:
            swd = f.read()
        offset = int(re.search(r"^AGENT_SIG_OFFSET = (0x[0-9A-Fa-f]+)", swd,
                               re.M).group(1), 16)
        self.assertEqual(agent["AGENT_SIG_ADDR"] - 0xFA0000, offset)


if __name__ == "__main__":
    unittest.main()
