// Scintilla source code edit control
/** @file LexAHK2.cxx
 ** Lexer for AutoHotkey v2
 ** Written by fincs
 **/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "CharacterCategory.h"
#include "LexerModule.h"
#include "OptionSet.h"
#include "SubStyles.h"
#include "DefaultLexer.h"

using namespace Scintilla;
using namespace Lexilla;

namespace {

	struct OptionsAHK2 {
		bool Fold;
		bool FoldComment;
		bool FoldCompact;

		OptionsAHK2() { }
	};

	static const char* const ahk2WordListDesc[] = {
		"Directives (Expression)",
		"Directives (String)",
		"Control Flow",
		"Reserved Words",
		"Named Keys",
		nullptr
	};

	static const char ahk2StyleSubable[] = {
		SCE_AHK2_ID_TOP_LEVEL,
		SCE_AHK2_ID_OBJECT,
		0
	};

	struct OptionSetAHK2 final : public OptionSet<OptionsAHK2> {

		OptionSetAHK2() {
			DefineWordListSets(ahk2WordListDesc);

			DefineProperty("fold", &OptionsAHK2::Fold);

			DefineProperty("fold.compact", &OptionsAHK2::FoldCompact);

			DefineProperty("fold.comment", &OptionsAHK2::FoldComment,
				"This option enables folding multi-line comments and explicit fold points when using the AutoHotkey v2 lexer."
				" Explicit fold points allows adding extra folding by placing a ;{ comment at the start and a ;}"
				" at the end of a section that should fold.");
		}

	};

	enum {
		TokenFlag_IsLoop      = 1U << 8,
		TokenFlag_IsClass     = 1U << 9,
		TokenFlag_IsClassName = 1U << 10,
		TokenFlag_TakesLabel  = 1U << 11,
	};

	enum {
		StringState_EndCharMask = 0x7f,
		StringState_NoEndChar   = 1U << 8,
		StringState_DoubleColon = 1U << 9,
		StringState_HotstringX  = 1U << 10,
		// Future: More state flags
	};

	enum {
		ContState_Inside   = 1U << 0,
		ContState_String   = 1U << 1,
		ContState_Comments = 1U << 2,
		ContState_NoEscape = 1U << 3,
	};

	struct ParserStateAHK2 final {
		int finalToken = SCE_AHK2_DEFAULT;
		unsigned stringState = 0;
		unsigned contState = 0;

		constexpr bool InContSect() const {
			return (contState & ContState_Inside) != 0;
		}

		constexpr bool InStringContSect() const {
			return (contState & ContState_String) != 0;
		}

		constexpr bool AllowLineComments() const {
			return !InContSect() || (contState & ContState_Comments) != 0;
		}

		constexpr bool AllowStringEscape() const {
			return (contState & ContState_NoEscape) == 0;
		}
	};

	class CharBackup {
		char* ptr;
		char backup;
	public:
		CharBackup(char* ptr) : ptr{ptr} {
			backup = *ptr;
			*ptr = 0;
		}

		~CharBackup() {
			*ptr = backup;
		}
	};

	inline int toLower(int c)
	{
		if (c >= 'A' && c <= 'Z') {
			c += 'a' - 'A';
		}

		return c;
	}

	inline bool isWhitespace(int c)
	{
		return c == ' ' || c == '\t';
	}

	inline bool isWhitespaceOrCR(int c)
	{
		return isWhitespace(c) || c == '\r';
	}

	inline bool isNumeric(int c, bool allowHex = false)
	{
		return (c >= '0' && c <= '9') || (allowHex && ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')));
	}

	inline bool isHexNumeric(int c)
	{
		return isNumeric(c, true);
	}

	inline bool isIdChar(int c, bool allowNumeric = true)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (allowNumeric && isNumeric(c)) || c == '_' || c > 0x7F;
	}

	inline bool isIdChar(char c, bool allowNumeric = true)
	{
		// XX: This ugly overload is needed in order to enforce correct behaviour on
		// wacko architectures that define 'char' as a signed type, thus making non-ASCII
		// characters become negative. (One such architecture is x86_32/x86_64)
		return isIdChar((int)(unsigned char)c, allowNumeric);
	}

	inline bool isExprOp(int c)
	{
		return
			c == '+' || c == '-' || c == '*' || c == '/' || c == '.' || c == '=' || c == '!' || c == '<' ||
			c == '>' || c == '&' || c == '|' || c == '^' || c == '~' || c == '?' || c == ':' || c == ',';
	}

	inline bool isOpeningBrace(int c)
	{
		return c == '(' || c == '[' || c == '{';
	}

	inline bool isClosingBrace(int c)
	{
		return c == ')' || c == ']' || c == '}';
	}

	inline bool isExprOpOrBrace(int c)
	{
		return isExprOp(c) || isOpeningBrace(c) || isClosingBrace(c);
	}

	inline bool isSameLineComment(StyleContext &sc)
	{
		return sc.state != SCE_AHK2_COMMENT_BLOCK && sc.ch == ';' && isWhitespace(sc.chPrev);
	}

	inline bool isValidPointDecimal(StyleContext &sc)
	{
		return sc.ch == '.' && (isWhitespace(sc.chPrev) || isExprOp(sc.chPrev) || isOpeningBrace(sc.chPrev)) && isNumeric(sc.chNext);
	}

	inline bool isHotstringOptionChar(int c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isNumeric(c) || c == '*' || c == '?';
	}

	inline bool isHotkeyModifier(int c)
	{
		return c == '#' || c == '!' || c == '^' || c == '+' || c == '<' || c == '>' || c == '*' || c == '~' || c == '$';
	}

	inline bool isSpecialLoopType(const char* str)
	{
		return strcmp(str,"files") == 0 || strcmp(str,"parse") == 0 || strcmp(str,"read") == 0 || strcmp(str,"reg") == 0;
	}

	inline bool isContSectCommentFlag(const char* str)
	{
		static const char comment_flag[9] = "comments";

		Sci_PositionU pos = 0;
		for (pos = 0; pos < (sizeof(comment_flag)-1); pos ++) {
			if (str[pos] != comment_flag[pos]) {
				break;
			}
		}

		return pos && str[pos]==0;
	}

	inline bool isEtterCompatible(const char* str)
	{
		// get/set on its own (case insensitive), or followed by { or =>

		int c = *str++;
		bool valid = c == 'g' || c == 's';
		if (valid) {
			valid = *str++ == 'e';
		}
		if (valid) {
			valid = *str++ == 't';
		}

		if (valid && *str) {
			valid = !isIdChar(*str);
			if (valid) {
				for (; isWhitespace(*str); str ++);
				valid = *str == '{' || (str[0] == '=' && str[1] == '>');
			}
		}

		return valid;
	}

	inline bool isLabelCompatible(const char* str, Sci_PositionU len)
	{
		// In RegEx this is ^[a-zA-Z_][a-zA-Z0-9_]*:$

		bool valid = len >= 2 && str[len-1] == ':';
		for (Sci_PositionU i = 0; valid && i < (len-1); i ++) {
			valid = isIdChar(str[i], i > 0);
		}

		return valid;
	}

	inline bool isHotstringCompatible(const char* str, bool &isX)
	{
		// In RegEx this is ^:\s*[a-zA-Z0-9\*\?]*\s*:[^:]?
		// XX: AutoHotkey is more lenient and allows for any character other than colon
		// as a hotstring option (ignoring all non-recognized characters). Moreover, it
		// will "cancel" the hotstring parsing if it ends up not finding a terminating
		// double colon - continuation sections are also allowed everywhere throughout.
		// We try to be more reasonable and only handle typical, non-pathological cases.

		bool valid = str[0] == ':';
		if (valid) {
			str ++;
			isX = false;
			for (; isWhitespace(*str); str ++);
			for (; isHotstringOptionChar(*str); str ++) {
				// Check for 'X' option, which causes the hotstring to be parsed as
				// a one-liner function instead of a simple string replacement.
				isX = isX || *str == 'x' || *str == 'X';
			}
			for (; isWhitespace(*str); str ++);
			valid = str[0] == ':' && str[1] != ':';
		}

		return valid;
	}

	inline char* skipHotkeyModifiers(char* str)
	{
		// See hotkey.cpp Hotkey::TextToModifiers() for more details.
		for (; isHotkeyModifier(str[0]) && str[1] && str[1] != ' '; str++);
		return str;
	}

	inline bool isValidKey(const char* str, WordList *namedKeys = nullptr)
	{
		// See keyboard_mouse.cpp TextToVK() for more details.

		// Empty string is not a valid key
		if (!*str) return false;

		// Any single character is valid, and parsed by CharToVKAndModifiers()
		if (!str[1]) return true;

		// XX: If we aren't passed a namedKeys wordlist, we are validating a hotkey label.
		// For simplicity, and because AutoHotkey parses this situation as a hotkey regardless
		// of whether the named key is actually recognised or not (displaying an error message
		// if it's not), allow any combination of identifier characters as a valid key specification.
		if (!namedKeys) {
			for (; *str; str++) {
				if (!isIdChar(*str)) {
					return false;
				}
			}
		}

		// Otherwise, we are checking the target of a potential remap hotkey. In this case,
		// AutoHotkey only parses the situation as a remap if the named key is in fact
		// recognised, otherwise falling back as a normal action.
		else {
			bool isVKorSC = false;

			// vkNN - skip over hex digits
			if (str[0] == 'v' && str[1] == 'k' && isHexNumeric(str[2])) {
				isVKorSC = true;
				for (str += 3; isHexNumeric(*str); str++);
			}

			// scNNN - skip over hex digits
			if (str[0] == 's' && str[1] == 'c' && isHexNumeric(str[2])) {
				isVKorSC = true;
				for (str += 3; isHexNumeric(*str); str++);
			}

			// If either of above matched, ensure there are no trailing characters
			if (isVKorSC) {
				return !*str;
			}

			// Otherwise check the list of named keys
			return namedKeys->InList(str);
		}

		return true;
	}

	inline bool isHotkeyCompatible(char* str, WordList &namedKeys, bool &isRemap)
	{
		// Assumptions:
		// - strlen(str) >= 1
		// - No leading or trailing whitespace
		// Refer to hotkey.cpp Hotkey::TextInterpret() for more details.
		isRemap = false;

		char* sep = strstr(str+1, "::"); // +1 so that we can detect ::: as colon-hotkey
		if (!sep) return false;

		// Isolate hotkey/remap target and remove leading whitespace
		char* target = sep+2;
		for (; isWhitespace(*target); target++);

		// Isolate hotkey definition and remove trailing whitespace
		for (; sep > str && isWhitespace(sep[-1]); sep--);

		// Check and remove "up" modifier along with even more trailing whitespace
		if (sep >= (str + 3) && isWhitespace(sep[-2]) && sep[-1] == 'u' && sep[0] == 'p') {
			for (sep -= 3; sep > str && isWhitespace(sep[-1]); sep--);
		}

		// Temporarily introduce a NUL terminator
		CharBackup chBackup1{sep};

		// Check for single or composite hotkeys
		bool valid = false;
		sep = strstr(str, " & "); // AutoHotkey only allows spaces
		if (sep) {
			// Isolate second key
			char* str2 = sep+3;
			for (; isWhitespace(*str2); str2++);

			// Remove trailing whitespace
			for (; sep > str && isWhitespace(sep[-1]); sep--);

			// Temporarily introduce a NUL terminator
			CharBackup chBackup2{sep};

			// Skip the only allowed modifier (and leading whitespace)
			if (*str == '~') {
				for (str++; isWhitespace(*str); str++);
				if (!*str) return false; // This is technically an error
			}

			// Validate the two keys
			valid = isValidKey(str) && isValidKey(str2);
		} else {
			// Skip modifiers
			str = skipHotkeyModifiers(str);

			// Validate the key
			valid = isValidKey(str);
		}

		// If above successfully validated the hotkey - check if this is a remap
		if (valid && *target && *target != '{') {
			// As per AHK source: "To use '{' as remap_dest, escape it!"
			if (target[0] == '`' && target[1] == '{') {
				target++;
			}

			// Exempt 'Pause' (a valid built-in command) from being considered as a key name
			if (strcmp(target, "pause") != 0) {
				// Skip modifiers
				target = skipHotkeyModifiers(target);

				// Validate the key
				isRemap = isValidKey(target, &namedKeys);
			}
		}

		return valid;
	}

	Sci_PositionU extractLineRTrim(StyleContext &sc, LexAccessor &styler, char* buf, Sci_PositionU bufSize)
	{
		// Retrieve remaining line length
		Sci_PositionU lineLen = sc.lineEnd - sc.currentPos;
		if (lineLen > bufSize-1) {
			lineLen = bufSize-1;
		}

		// Retrieve character range, lowering the case of all letters
		if (styler.Encoding() == EncodingType::eightBit) {
			// Fast path for fixed size 8-bit encodings (e.g. legacy Western codepages)
			styler.GetRangeLowered(sc.currentPos, sc.currentPos + lineLen, buf, bufSize);
		} else {
			// Properly handle variable-length encodings (not just UTF-8), replacing
			// all non-ASCII characters with a placeholder value (0x80)
			IDocument* multiByteAccess = styler.MultiByteAccess();
			Sci_PositionU inPos = sc.currentPos;
			Sci_PositionU outPos = 0;
			while (inPos < sc.currentPos + lineLen && outPos < bufSize - 1) {
				Sci_Position width;
				int c = multiByteAccess->GetCharacterAndWidth(inPos, &width);
				inPos += width;
				buf[outPos++] = c < 0x80 ? toLower(c) : 0x80;
			}
			buf[outPos] = 0;
		}

		// Remove same-line comment if present
		for (char* cmtPos = buf; cmtPos;) {
			cmtPos = strchr(cmtPos, ';');
			if (cmtPos) {
				if (cmtPos == buf || isWhitespace(cmtPos[-1])) {
					*cmtPos = 0;
					lineLen = cmtPos - buf;
					break;
				} else {
					cmtPos ++;
				}
			}
		}

		// Remove trailing whitespace
		while (lineLen && isWhitespaceOrCR(buf[lineLen-1])) {
			lineLen --;
			buf[lineLen] = 0;
		}

		return lineLen;
	}

}

class LexerAHK2 final : public DefaultLexer {

	OptionsAHK2 options;
	OptionSetAHK2 opSet;
	SubStyles subStyles;
	WordList directivesExpr, directivesStr, controlFlow, reservedWords, namedKeys;
	std::map<Sci_Position, ParserStateAHK2> parserStates;

	explicit LexerAHK2() :
		DefaultLexer("ahk2", SCLEX_AHK2),
		subStyles(ahk2StyleSubable, 0x80, 0x40, 0) { }
	~LexerAHK2() override { }

	void ProcessLineEnd(LexAccessor &styler, StyleContext &sc, ParserStateAHK2 &parserState, int lastToken, unsigned stringState);

public:

	// Standard boilerplate
	const char* SCI_METHOD PropertyNames() override { return opSet.PropertyNames(); }
	int SCI_METHOD PropertyType(const char* name) override { return opSet.PropertyType(name); }
	const char* SCI_METHOD DescribeProperty(const char* name) override { return opSet.DescribeProperty(name); }
	Sci_Position SCI_METHOD PropertySet(const char* key, const char* val) override;
	const char* SCI_METHOD PropertyGet(const char *key) override { return opSet.PropertyGet(key); }
	const char* SCI_METHOD DescribeWordListSets() override { return opSet.DescribeWordListSets(); }
	Sci_Position SCI_METHOD WordListSet(int n, const char* wl) override;
	void SCI_METHOD Lex(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, IDocument* pAccess) override;
	void SCI_METHOD Fold(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, IDocument* pAccess) override;

	// Substyle boilerplate
	int SCI_METHOD AllocateSubStyles(int styleBase, int numberStyles) override { return subStyles.Allocate(styleBase, numberStyles); }
	int SCI_METHOD SubStylesStart(int styleBase) override { return subStyles.Start(styleBase); }
	int SCI_METHOD SubStylesLength(int styleBase) override { return subStyles.Length(styleBase); }
	int SCI_METHOD StyleFromSubStyle(int subStyle) override { return subStyles.BaseStyle(subStyle); }
	void SCI_METHOD FreeSubStyles() override { subStyles.Free(); }
	void SCI_METHOD SetIdentifiers(int style, const char *identifiers) override { subStyles.SetIdentifiers(style, identifiers); }
	const char *SCI_METHOD GetSubStyleBases() override { return ahk2StyleSubable; }

	// Factory
	static ILexer5* LexerFactoryAHK2() {
		return new LexerAHK2();
	}

};

Sci_Position SCI_METHOD LexerAHK2::PropertySet(const char* key, const char* val)
{
	return opSet.PropertySet(&options, key, val) ? 0 : -1;
}

Sci_Position SCI_METHOD LexerAHK2::WordListSet(int n, const char* wl)
{
	static WordList LexerAHK2::* const wordListAccessors[] = {
		&LexerAHK2::directivesExpr,
		&LexerAHK2::directivesStr,
		&LexerAHK2::controlFlow,
		&LexerAHK2::reservedWords,
		&LexerAHK2::namedKeys,
	};

	static constexpr int numWordLists = sizeof(wordListAccessors)/sizeof(wordListAccessors[0]);
	if (n < 0 || n >= numWordLists) {
		return -1;
	}

	(this->*wordListAccessors[n]).Set(wl);
	return 0;
}

void LexerAHK2::ProcessLineEnd(LexAccessor &styler, StyleContext &sc, ParserStateAHK2 &parserState, int lastToken, unsigned stringState)
{
	// Update last seen token
	if (lastToken != SCE_AHK2_DEFAULT) {
		parserState.finalToken = lastToken;
		parserState.stringState = stringState;
	}

	// Save state for the line that ends.
	// XX: For now, use contState as Lexilla line state so that it knows that
	// the following lines need to be relexed when contsect flags change.
	// XX: In the future, consider storing a hash of the entire parser state instead.
	parserStates.insert({ sc.currentLine, parserState });
	styler.SetLineState(sc.currentLine, parserState.contState);
}

void SCI_METHOD LexerAHK2::Lex(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, IDocument* pAccess)
{
	LexAccessor styler(pAccess);

	ParserStateAHK2 parserState; {
		// Initialize parser state with that of the previous line
		Sci_Position startLine = styler.GetLine(startPos);
		auto it = parserStates.find(startLine-1);
		if (it != parserStates.end()) {
			parserState = it->second;
		}

		// Erase stale parser states from the starting line onwards
		it = parserStates.lower_bound(startLine);
		if (it != parserStates.end()) {
			parserStates.erase(it, parserStates.end());
		}
	}

	bool atLineStart = false;
	bool canStartBlockComment = false;
	bool canEndBlockComment = false;
	int lastToken = SCE_AHK2_DEFAULT;
	unsigned stringState = 0;
	bool numIsHex = false;
	int numParseState = 0;
	int prevTokenForId = 0;
	bool isRemap = false;
	bool labelCompatible = false;
	bool etterCompatible = false;

	StyleContext sc(startPos, lengthDoc, initStyle, styler);
	char buf[512];

	bool swallowForward = false;
	auto moveForward = [&] {
		if (swallowForward) {
			swallowForward = false;
		} else {
			if (sc.atLineEnd) {
				ProcessLineEnd(styler, sc, parserState, lastToken, stringState);
			}
			sc.Forward();
		}
	};

	for (; sc.More(); moveForward()) {
		if (sc.atLineStart) {
			atLineStart = true;
			canStartBlockComment = true;
			lastToken = SCE_AHK2_DEFAULT;
			stringState = 0;
			isRemap = false;
			labelCompatible = false;
			etterCompatible = false;

			if (parserState.contState & ContState_String) {
				sc.SetState(SCE_AHK2_STRING);
			} else if ((parserState.contState & ContState_Inside) && parserState.stringState) {
				sc.SetState(lastToken = SCE_AHK2_STRING);
				stringState = parserState.stringState;
			} else if (sc.state == SCE_AHK2_COMMENT_BLOCK) {
				// Check if we ended the block comment at the end of line
				if (canEndBlockComment) {
					canEndBlockComment = false;
					sc.SetState(SCE_AHK2_DEFAULT);
				}
			} else {
				sc.SetState(SCE_AHK2_DEFAULT);
			}
		}

		if (atLineStart && !isWhitespace(sc.ch)) {
			bool allowBlockComment = canStartBlockComment;
			atLineStart = false;
			canStartBlockComment = false;

			if (parserState.InContSect()) {
				// Handle continuation section end
				if (sc.ch == ')') {
					bool isString = parserState.InStringContSect();
					sc.SetState(SCE_AHK2_OPERATOR);
					sc.ForwardSetState(isString ? SCE_AHK2_STRING : SCE_AHK2_DEFAULT);
					parserState.contState = 0;
					swallowForward = true;
					if (isString) {
						lastToken = SCE_AHK2_STRING;
						stringState = parserState.stringState;
					}
					continue;
				} else if (sc.ch == ';' && parserState.AllowLineComments()) {
					sc.SetState(SCE_AHK2_COMMENT_LINE);
				}
			} else if (sc.state == SCE_AHK2_COMMENT_BLOCK) {
				// Handle comment block closure at line start + virtual line restart
				if (sc.Match('*', '/')) {
					sc.Forward(2);
					sc.SetState(SCE_AHK2_DEFAULT);
					atLineStart = true;
					swallowForward = true;
					continue;
				}
			} else if (sc.ch == ';') {
				// Single line comment
				sc.SetState(SCE_AHK2_COMMENT_LINE);
			} else if (sc.Match('/', '*')) {
				// Start of a comment block
				sc.SetState(allowBlockComment ? SCE_AHK2_COMMENT_BLOCK : SCE_AHK2_ERROR);
			} else if (Sci_PositionU lineLen = extractLineRTrim(sc, styler, buf, sizeof(buf)); lineLen) {
				// Otherwise for non-empty lines: look ahead and check for special line types.
				// Refer to script.cpp Script::LoadIncludedFile() for more details.

				// Check if this is the start of a continuation section.
				// Refer to script.cpp Script::GetLineContinuation() for more details.
				if (buf[0] == '(' && buf[1] != ':' && !strpbrk(buf+1, "()")) {
					sc.SetState(SCE_AHK2_OPERATOR);
					sc.ForwardSetState(SCE_AHK2_STRING);
					// Don't set lastToken as this line shouldn't qualify for updating state in ProcessLineEnd.
					stringState = StringState_NoEndChar;
					parserState.contState = ContState_Inside;
					swallowForward = true;

					if (parserState.stringState != 0) {
						parserState.contState |= ContState_String;
					}

					// Parse continuation section options:
					//   - Comments/Comment/Com/C: Reenable same-line comment parsing
					//   - ` (backtick): Disable escape sequences in strings
					// XX: We decide not to handle possible side effects of Join in expression
					//     contexts for the sake of retaining our sanity (and simplicity).
					char* pos = buf+1;
					while (pos && *pos) {
						// Find end of option
						char* nextPos = strpbrk(pos, " \t");
						while (nextPos && isWhitespace(*nextPos)) {
							*nextPos++ = 0;
						}

						// Handle options
						if (pos[0] == '`' && pos[1] == 0) {
							parserState.contState |= ContState_NoEscape;
						} else if (isContSectCommentFlag(pos)) {
							parserState.contState |= ContState_Comments;
						}

						// Next option
						pos = nextPos;
					}

					continue;
				}

				// Check if this line is a hotstring definition.
				// XX: We require the hotstring options not broken up by continuation section.
				// (Also see comment in isHotstringCompatible concerning the cases we handle)
				else if (bool isX; isHotstringCompatible(buf, isX)) {
					sc.SetState(SCE_AHK2_OPERATOR);
					sc.Forward();
					if (sc.ch == ':') {
						sc.Forward();
						sc.SetState(SCE_AHK2_STRING);
						stringState = ':' | StringState_DoubleColon;
					} else {
						sc.SetState(SCE_AHK2_STRING);
						stringState = ':';
					}

					if (isX) {
						// Remember the 'X' option in the string state
						stringState |= StringState_HotstringX;
					}

					lastToken = SCE_AHK2_STRING;
					swallowForward = true;
					continue;
				}

				// Check if this line is a hotkey definition (including remaps).
				else if (isHotkeyCompatible(buf, namedKeys, isRemap)) {
					sc.SetState(SCE_AHK2_LABEL);
					// We intentionally skip over the first character in order to
					// correctly highlight ":::" (i.e. colon hotkey)
				}

				// Handle other cases
				else {
					// Check if this line is a property getter/setter definition.
					// XX: For simplicity, we will detect these lines regardless of whether
					// we actually are inside a property definition block.
					etterCompatible = isEtterCompatible(buf);

					// Check if this line is a label definition.
					// XX: Label-looking lines can sometimes be part of expressions
					// (i.e. ? : ternary operator). Due to simplicity and rarity, we
					// opt to always parse label-looking lines as labels. Otherwise
					// we would need to track block state { } on top of enclosure depth [ ] ( ),
					// which is still a hard problem caused by control flow statements,
					// function definition syntax and the OTB coding style.
					if (!etterCompatible) {
						labelCompatible = isLabelCompatible(buf, lineLen);
					}
				}
			}
		}

		// Skip initial whitespace and errored-out lines
		if (atLineStart || sc.state == SCE_AHK2_ERROR) {
			continue;
		}

		// Check for same-line comment (higher precedence than tokenization)
		if (isSameLineComment(sc) && parserState.AllowLineComments()) {
			sc.SetState(SCE_AHK2_COMMENT_LINE);
			continue;
		}

		//---------------------------------------------------------------------
		// Determine if the current token ends
		//---------------------------------------------------------------------

		switch (sc.state) {
			case SCE_AHK2_LABEL: {
				// This section only handles hotkey labels.
				// Actual labels are handled later as a state change from SCE_AHK2_ID_TOP_LEVEL.

				// Check for label termination
				if (sc.Match(':', ':')) {
					sc.SetState(SCE_AHK2_OPERATOR);
					sc.Forward(2);

					if (isRemap) {
						// Style remap targets as strings, as they are effectively
						// passed down to Send (which expects a string).
						sc.SetState(SCE_AHK2_STRING);
						stringState = StringState_NoEndChar;
					} else {
						// Otherwise: regular hotkey action
						sc.SetState(SCE_AHK2_DEFAULT);
					}
				}
				break;
			}

			case SCE_AHK2_COMMENT_BLOCK: {
				// Handle comment block closure at line end
				if (sc.Match('*','/')) {
					sc.Forward();
					canEndBlockComment = true;
				} else if (!sc.atLineEnd && !isWhitespaceOrCR(sc.ch)) {
					canEndBlockComment = false;
				}
				break;
			}

			case SCE_AHK2_STRING: {
				int stringEndChar = stringState & StringState_EndCharMask;
				if (stringEndChar && sc.ch == stringEndChar) {
					if (stringEndChar != ':') {
						sc.ForwardSetState(SCE_AHK2_DEFAULT);
						stringState = 0;
					} else {
						unsigned hotstringX = stringState & StringState_HotstringX;
						bool doubleColon = (stringState & StringState_DoubleColon) != 0;
						if (doubleColon && sc.chNext != ':') {
							break;
						}

						bool terminateString = false;
						sc.SetState(SCE_AHK2_OPERATOR);
						sc.Forward(doubleColon ? 2 : 1);
						swallowForward = true;

						if (doubleColon) {
							stringState = StringState_NoEndChar;
							terminateString = hotstringX != 0;
						} else {
							stringState = ':' | StringState_DoubleColon | hotstringX;
						}

						if (terminateString) {
							sc.SetState(SCE_AHK2_DEFAULT);
							stringState = 0;
						} else {
							sc.SetState(SCE_AHK2_STRING);
						}
					}
				} else if (sc.ch == '`' && !isRemap && parserState.AllowStringEscape()) {
					if (strchr("`;:{nrbtsvaf\"'", sc.chNext)) {
						sc.SetState(SCE_AHK2_ESCAPE);
						sc.Forward();
					} else {
						sc.SetState(SCE_AHK2_ERROR);
					}
				}
				break;
			}

			case SCE_AHK2_ESCAPE:
				sc.SetState(SCE_AHK2_STRING);
				swallowForward = true;
				break;

			case SCE_AHK2_OPERATOR:
				// XX: HACK so that decimal dot can start a new number token
				if (!isExprOpOrBrace(sc.ch) || sc.ch == '.') {
					sc.SetState(SCE_AHK2_DEFAULT);
				}
				break;

			case SCE_AHK2_NUMBER: {
				bool numEnd = false, numExponent = false;
				switch (numParseState) {
					default: case 0:
						if (numIsHex) {
							numEnd = sc.LengthCurrent() >= 2 && !isNumeric(sc.ch, true);
						} else if (sc.ch == '.') {
							numParseState = 1; // Decimal part comes next
						} else if (sc.ch == 'e' || sc.ch == 'E') {
							numExponent = true; // Handle exponent in below section
						} else if (!isNumeric(sc.ch)) {
							numEnd = true;
						}
						break;
					case 1: // Decimal part
						if (sc.ch == 'e' || sc.ch == 'E') {
							numExponent = true; // Handle exponent in below section
						} else if (!isNumeric(sc.ch)) {
							numEnd = true;
						}
						break;
					case 2: case 3: // Exponent
						if (!isNumeric(sc.ch)) {
							numEnd = true;
						} else {
							numParseState = 3;
						}
						break;
				}

				if (numExponent) {
					if (sc.chNext == '+' || sc.chNext == '-') {
						// Skip exponent sign
						sc.Forward();
					}

					numParseState = 2; // Exponent comes next
				} else if (numEnd) {
					// Check for badly terminated numbers + illegal adjacent identifiers without whitespace separation
					bool invalid = false;
					if (numIsHex) {
						invalid = sc.LengthCurrent() < 3;
					} else {
						invalid = isIdChar(sc.ch, false) || numParseState==2;
					}
					sc.SetState(invalid ? SCE_AHK2_ERROR : SCE_AHK2_DEFAULT);
				}

				break;
			}

			case SCE_AHK2_ID_TOP_LEVEL:
			case SCE_AHK2_ID_OBJECT: {
				if (isIdChar(sc.ch, true)) break;

				// Retrieve the identifier, together with its appropriate substyler object
				const WordClassifier &subStyler = subStyles.Classifier(sc.state);
				sc.GetCurrentLowered(buf, sizeof(buf));

				// Handle special cases involving bare words
				if (sc.state == SCE_AHK2_ID_TOP_LEVEL) {
					if (prevTokenForId == SCE_AHK2_DEFAULT) {
						if (etterCompatible) {
							// Special treatment for property getter/setter definitions
							sc.ChangeState(lastToken = SCE_AHK2_ID_RESERVED);
						} else if (sc.ch == ':') {
							// Special treatment for "identifier:" at the beginning of a line
							if (strcmp(buf, "default") == 0) {
								// Default case of a switch
								sc.ChangeState(lastToken = SCE_AHK2_FLOW);
							} else if (labelCompatible) {
								// Label definition
								sc.ChangeState(lastToken = SCE_AHK2_LABEL);
							}
						} else if (sc.ch != '.' && sc.ch != '(' && strcmp(buf, "class") == 0) {
							// Class definition (as opposed to the "Class" class object itself)
							sc.ChangeState(lastToken = SCE_AHK2_ID_RESERVED);
							lastToken |= TokenFlag_IsClass;
						}
					} else if (prevTokenForId & TokenFlag_IsLoop) {
						if (isSpecialLoopType(buf)) {
							// Loop Parse/Loop Read/Loop Files/Loop Reg
							sc.ChangeState(lastToken = SCE_AHK2_FLOW);
						}
					} else if (prevTokenForId & TokenFlag_IsClass) {
						// This is the name of class declaration - set flag for below
						lastToken |= TokenFlag_IsClassName;
					} else if (prevTokenForId & TokenFlag_IsClassName) {
						if (strcmp(buf, "extends") == 0) {
							// class Foo extends Bar
							sc.ChangeState(lastToken = SCE_AHK2_ID_RESERVED);
						}
					} else if (prevTokenForId & TokenFlag_TakesLabel) {
						// Target label of a goto/break/continue
						sc.ChangeState(lastToken = SCE_AHK2_LABEL);
					}
				}

				// Handle special top level identifiers
				if (sc.state == SCE_AHK2_ID_TOP_LEVEL) {
					if (controlFlow.InList(buf)) {
						sc.ChangeState(lastToken = SCE_AHK2_FLOW);

						if (strcmp(buf, "loop") == 0) {
							// This is 'Loop' - set flag so that we can later highlight special Loop types
							lastToken |= TokenFlag_IsLoop;
						} else if (strcmp(buf, "goto") == 0 || strcmp(buf, "break") == 0 || strcmp(buf, "continue") == 0) {
							// These special statements can be followed by a bare label name
							lastToken |= TokenFlag_TakesLabel;
						}
					} else if (reservedWords.InList(buf)) {
						// XX: We are treating declarators and word-operators as the same thing.
						// Should they be split into two wordlists? Two different styles?
						sc.ChangeState(lastToken = SCE_AHK2_ID_RESERVED);
					}
				}

				// If none of the above applied: handle identifier substyles
				if (sc.state == subStyler.Base()) {
					int newStyle = subStyler.ValueFor(buf);
					if (newStyle >= 0) {
						sc.ChangeState(newStyle);
					}
				}

				sc.SetState(SCE_AHK2_DEFAULT);
				break;
			}

			case SCE_AHK2_DIRECTIVE: {
				if (isIdChar(sc.ch, false)) break;

				sc.GetCurrentLowered(buf, sizeof(buf));
				if (directivesExpr.InList(buf+1)) {
					// Directive taking an expression argument
					sc.SetState(SCE_AHK2_DEFAULT);
				} else if (directivesStr.InList(buf+1)) {
					// Directive taking a (quoteless) string argument
					sc.SetState(lastToken = SCE_AHK2_STRING);
					stringState = StringState_NoEndChar;
				} else {
					// Mark this as an error, but keep the directive styling.
					sc.SetState(SCE_AHK2_ERROR);
				}
				break;
			}
		}

		// If the token hasn't ended, or if we're still handling whitespace, skip below section
		if (sc.state != SCE_AHK2_DEFAULT || sc.atLineEnd || isWhitespaceOrCR(sc.ch)) {
			continue;
		}

		//---------------------------------------------------------------------
		// Determine if a new token starts
		//---------------------------------------------------------------------

		if (sc.ch == '"' || sc.ch == '\'') {
			// String.
			// XX: If F-strings are ever added to the language, check for them here using sc.Match
			lastToken = SCE_AHK2_STRING;
			stringState = sc.ch;
		} else if (isNumeric(sc.ch) || isValidPointDecimal(sc)) {
			// Number.
			lastToken = SCE_AHK2_NUMBER;
			numIsHex = sc.ch == '0' && (sc.chNext == 'x' || sc.chNext == 'X');
			numParseState = sc.ch != '.' ? 0 : 1;
		} else if (isExprOpOrBrace(sc.ch) || sc.ch == '%') {
			// Operator expression.
			// XX: We are not validating the operator at all - probably doesn't make a difference/not worth it.
			// XX: GROSS HACK. Ideally double derefs would be handled in the identifier section with some state saving
			lastToken = SCE_AHK2_OPERATOR;
		} else if (isIdChar(sc.ch, false)) {
			// Identifier (either top-level or object prop/method).
			prevTokenForId = lastToken; // Used to detect special words
			lastToken = sc.chPrev != '.' ? SCE_AHK2_ID_TOP_LEVEL : SCE_AHK2_ID_OBJECT;
		} else if (sc.ch == '#' && lastToken == SCE_AHK2_DEFAULT && !parserState.InContSect()) {
			// Directive.
			// XX: Note that Windows-key modifier hotkeys (such as #v::SomeFunc)
			// should have already been handled by the line start logic.
			lastToken = SCE_AHK2_DIRECTIVE;
		} else {
			// Unknown - enter error state
			lastToken = SCE_AHK2_ERROR;
		}

		sc.SetState(lastToken);
	}

	sc.Complete();
}

void SCI_METHOD LexerAHK2::Fold(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, IDocument* pAccess)
{
	if (!options.Fold) return;

	bool bFoldComment = options.FoldComment;
	bool bFoldCompact = options.FoldCompact;

	LexAccessor styler(pAccess);
	Sci_PositionU endPos = startPos + lengthDoc;
	bool bOnlySpaces = true;

	Sci_Position lineCurrent = styler.GetLine(startPos);
	int levelCurrent = SC_FOLDLEVELBASE;
	if (lineCurrent > 0) {
		levelCurrent = styler.LevelAt(lineCurrent - 1) >> 16;
	}

	int levelNext = levelCurrent;
	char chNext = styler[startPos];
	int styleNext = styler.StyleAt(startPos);
	int style = initStyle;

	for (Sci_PositionU i = startPos; i < endPos; i ++) {
		char ch = chNext;
		chNext = styler.SafeGetCharAt(i + 1);
		int stylePrev = style;
		style = styleNext;
		styleNext = styler.StyleAt(i + 1);
		bool atEOL = (ch == '\r' && chNext != '\n') || (ch == '\n');

		if (bFoldComment && style == SCE_AHK2_COMMENT_BLOCK) {
			if (stylePrev != SCE_AHK2_COMMENT_BLOCK) {
				levelNext ++;
			} else if (styleNext != SCE_AHK2_COMMENT_BLOCK) {
				levelNext --;
			}
		}

		if (bFoldComment && style == SCE_AHK2_COMMENT_LINE) {
			if (ch == ';') {
				if (chNext == '{') {
					levelNext ++;
				} else if (chNext == '}') {
					levelNext --;
				}
			}
		}

		if (style == SCE_AHK2_OPERATOR) {
			if (isOpeningBrace(ch)) {
				levelNext ++;
			} else if (isClosingBrace(ch)) {
				levelNext --;
			}
		}

		if (atEOL || (i == endPos-1)) {
			int level = levelCurrent | (levelNext << 16);

			if (bOnlySpaces && bFoldCompact) {
				// Empty line
				level |= SC_FOLDLEVELWHITEFLAG;
			}

			if (levelCurrent < levelNext) {
				level |= SC_FOLDLEVELHEADERFLAG;
			}

			if (level != styler.LevelAt(lineCurrent)) {
				styler.SetLevel(lineCurrent, level);
			}

			lineCurrent ++;
			levelCurrent = levelNext;

			if (atEOL && (i == static_cast<unsigned int>(styler.Length()-1))) {
				// There is an empty line at end of file so give it same level and empty
				styler.SetLevel(lineCurrent, (levelCurrent | levelCurrent << 16) | SC_FOLDLEVELWHITEFLAG);
			}

			bOnlySpaces = true;
		}

		if (!isWhitespace(ch)) {
			bOnlySpaces = false;
		}
	}
}

LexerModule lmAHK2(SCLEX_AHK2, LexerAHK2::LexerFactoryAHK2, "ahk2", ahk2WordListDesc);
