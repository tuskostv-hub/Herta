#include "herta/lexer/token.hpp"

#include <sstream>

namespace herta::lexer {

std::string_view to_string(TokenKind k) noexcept {
    switch (k) {
        case TokenKind::Eof: return "Eof";
        case TokenKind::Invalid: return "Invalid";
        case TokenKind::IntLiteral: return "IntLiteral";
        case TokenKind::FloatLiteral: return "FloatLiteral";
        case TokenKind::StringLiteral: return "StringLiteral";
        case TokenKind::Identifier: return "Identifier";
        case TokenKind::KwFn: return "KwFn";
        case TokenKind::KwLet: return "KwLet";
        case TokenKind::KwVar: return "KwVar";
        case TokenKind::KwReturn: return "KwReturn";
        case TokenKind::KwIf: return "KwIf";
        case TokenKind::KwElse: return "KwElse";
        case TokenKind::KwWhile: return "KwWhile";
        case TokenKind::KwBreak: return "KwBreak";
        case TokenKind::KwContinue: return "KwContinue";
        case TokenKind::KwStruct: return "KwStruct";
        case TokenKind::KwType: return "KwType";
        case TokenKind::KwNamespace: return "KwNamespace";
        case TokenKind::KwImpl: return "KwImpl";
        case TokenKind::KwModule: return "KwModule";
        case TokenKind::KwImport: return "KwImport";
        case TokenKind::KwPub: return "KwPub";
        case TokenKind::KwTrue: return "KwTrue";
        case TokenKind::KwFalse: return "KwFalse";
        case TokenKind::Plus: return "Plus";
        case TokenKind::Minus: return "Minus";
        case TokenKind::Star: return "Star";
        case TokenKind::Slash: return "Slash";
        case TokenKind::Percent: return "Percent";
        case TokenKind::EqEq: return "EqEq";
        case TokenKind::BangEq: return "BangEq";
        case TokenKind::Lt: return "Lt";
        case TokenKind::Gt: return "Gt";
        case TokenKind::LtEq: return "LtEq";
        case TokenKind::GtEq: return "GtEq";
        case TokenKind::AmpAmp: return "AmpAmp";
        case TokenKind::PipePipe: return "PipePipe";
        case TokenKind::Bang: return "Bang";
        case TokenKind::Eq: return "Eq";
        case TokenKind::ColonEq: return "ColonEq";
        case TokenKind::Arrow: return "Arrow";
        case TokenKind::Colon: return "Colon";
        case TokenKind::Comma: return "Comma";
        case TokenKind::Semicolon: return "Semicolon";
        case TokenKind::Dot: return "Dot";
        case TokenKind::LParen: return "LParen";
        case TokenKind::RParen: return "RParen";
        case TokenKind::LBrace: return "LBrace";
        case TokenKind::RBrace: return "RBrace";
        case TokenKind::LBracket: return "LBracket";
        case TokenKind::RBracket: return "RBracket";
    }
    return "?";
}

std::string to_string(const Token& t) {
    std::ostringstream os;
    os << t.loc.line << ':' << t.loc.column << "  "
       << to_string(t.kind) << "  '" << t.lexeme << '\'';
    return os.str();
}

}  // namespace herta::lexer
