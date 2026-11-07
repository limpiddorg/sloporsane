"""Shared syntax palette, independent of diff addition/removal surfaces."""
from functools import lru_cache

from pygments import lex
from pygments.lexers import get_lexer_by_name, get_lexer_for_filename, TextLexer
from pygments.token import Token
from pygments.util import ClassNotFound
from rich.text import Text

from .config import ALGAE_MD, BOLD_MD, DEPTH_MD, MISC_MD, MUTED_COLOR, TEXT_COLOR, TITLE_MD

ADDED_BACKGROUND = "#20382B"
REMOVED_BACKGROUND = "#3C242B"
TOKEN_COLORS = {
    Token.Comment: MUTED_COLOR,
    Token.Keyword: MISC_MD,
    Token.Name.Function: TITLE_MD,
    Token.Name.Class: TITLE_MD,
    Token.Name.Decorator: DEPTH_MD,
    Token.Name.Builtin: BOLD_MD,
    Token.Literal.String: ALGAE_MD,
    Token.Literal.Number: BOLD_MD,
    Token.Operator: MISC_MD,
    Token.Punctuation: TEXT_COLOR,
    Token.Generic.Heading: TITLE_MD,
}


def token_color(token):
    while token not in TOKEN_COLORS and token.parent is not None:
        token = token.parent
    return TOKEN_COLORS.get(token, TEXT_COLOR)


def highlighted_code(source, *, path=None, language=None):
    try:
        if language:
            lexer = get_lexer_by_name(language, stripnl=False, ensurenl=False)
        elif path:
            lexer = get_lexer_for_filename(path, source, stripnl=False, ensurenl=False)
        else:
            lexer = TextLexer(stripnl=False, ensurenl=False)
    except ClassNotFound:
        lexer = TextLexer(stripnl=False, ensurenl=False)
    text = Text()
    for token, value in lex(source, lexer):
        text.append(value, style=token_color(token))
    return text


@lru_cache(maxsize=12)
def highlighted_lines(path, source):
    return highlighted_code(source, path=path).split("\n", allow_blank=True)
