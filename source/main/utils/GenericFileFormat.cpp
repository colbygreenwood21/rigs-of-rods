/*
    This source file is part of Rigs of Rods
    Copyright 2022 Petr Ohlidal

    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.

    Rigs of Rods is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Rigs of Rods. If not, see <http://www.gnu.org/licenses/>.
*/

#include "GenericFileFormat.h"

#include "Application.h"
#include "Console.h"

#include <algorithm>

using namespace RoR;
using namespace Ogre;

enum class PartialToken
{
    NONE,
    COMMENT_SEMICOLON, // Comment starting with ';'
    COMMENT_SLASH,     // Comment starting with '//'
    STRING_QUOTED,     // String starting/ending with '"'
    STRING_NAKED,      // String without '"' on either end
    TITLE_STRING,      // A whole-line string, with spaces
    NUMBER,            // Number with digits and optionally leading '-'
    NUMBER_DOT,        // Like NUMBER but already containing '.'
    KEYWORD,           // Unqoted string at the start of line
    BOOL_TRUE,         // Partial 'true'
    BOOL_FALSE,        // Partial 'false'
    GARBAGE,           // Text not fitting any above category, will be discarded
};

struct DocumentParser
{
    DocumentParser(GenericDocument& d, const BitMask_t opt, Ogre::DataStreamPtr ds)
        : doc(d), options(opt), datastream(ds) {}

    // Config
    GenericDocument& doc;
    const BitMask_t options;
    Ogre::DataStreamPtr datastream;

    // State
    std::vector<char> tok;
    size_t line_num = 0;
    size_t line_pos = 0;
    PartialToken partial_tok_type = PartialToken::NONE;
    bool title_found = false; // Only for OPTION_FIRST_LINE_IS_TITLE

    void BeginToken(const char c);
    void UpdateComment(const char c);
    void UpdateString(const char c);
    void UpdateNumber(const char c);
    void UpdateBool(const char c);
    void UpdateKeyword(const char c);
    void UpdateTitle(const char c); // Only for OPTION_FIRST_LINE_IS_TITLE
    void UpdateGarbage(const char c);
};

void DocumentParser::BeginToken(const char c)
{
    switch (c)
    {
    case '\r':
        break;

    case ' ':
    case ',':
    case '\t':
        line_pos++;
        break;

    case '\n':
        doc.tokens.push_back({ TokenType::LINEBREAK, 0.f });
        line_num++;
        line_pos = 0;
        break;

    case ';':
        partial_tok_type = PartialToken::COMMENT_SEMICOLON;
        line_pos++;
        break;

    case '/':
        if (options & GenericDocument::OPTION_ALLOW_SLASH_COMMENTS)
        {
            partial_tok_type = PartialToken::COMMENT_SLASH;
        }
        else if (options & GenericDocument::OPTION_ALLOW_NAKED_STRINGS &&
            (doc.tokens.size() != 0 && doc.tokens.back().type != TokenType::LINEBREAK)) // not first on line?
        {
            tok.push_back(c);
            partial_tok_type = PartialToken::STRING_NAKED;
        }
        else
        {
            partial_tok_type = PartialToken::GARBAGE;
            tok.push_back(c);
        }
        line_pos++;
        break;

    case '"':
        partial_tok_type = PartialToken::STRING_QUOTED;
        line_pos++;
        break;

    case '.':
        tok.push_back(c);
        partial_tok_type = PartialToken::NUMBER_DOT;
        line_pos++;
        break;

    case '-':
        tok.push_back(c);
        partial_tok_type = PartialToken::NUMBER;
        line_pos++;
        break;

    case 't':
        tok.push_back(c);
        partial_tok_type = PartialToken::BOOL_TRUE;
        line_pos++;
        break;

    case 'f':
        tok.push_back(c);
        partial_tok_type = PartialToken::BOOL_FALSE;
        line_pos++;
        break;

    default:
        if (isdigit(c))
        {
            tok.push_back(c);
            partial_tok_type = PartialToken::NUMBER;
        }
        else if (isalpha(c) &&
            (doc.tokens.size() == 0 || doc.tokens.back().type == TokenType::LINEBREAK)) // on line start?
        {
            tok.push_back(c);
            partial_tok_type = PartialToken::KEYWORD;
        }
        else if (options & GenericDocument::OPTION_ALLOW_NAKED_STRINGS)
        {
            tok.push_back(c);
            partial_tok_type = PartialToken::STRING_NAKED;
        }
        else
        {
            partial_tok_type = PartialToken::GARBAGE;
            tok.push_back(c);
        }
        line_pos++;
        break;
    }

    if (options & GenericDocument::OPTION_FIRST_LINE_IS_TITLE
        && !title_found
        && (doc.tokens.size() == 0 || doc.tokens.back().type == TokenType::LINEBREAK)
        && partial_tok_type != PartialToken::NONE
        && partial_tok_type != PartialToken::COMMENT_SEMICOLON
        && partial_tok_type != PartialToken::COMMENT_SLASH)
    {
        title_found = true;
        partial_tok_type = PartialToken::TITLE_STRING;
    }

    if (partial_tok_type == PartialToken::GARBAGE)
    {
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_WARNING,
            fmt::format("{}, line {}, pos {}: stray character '{}'", datastream->getName(), line_num, line_pos, c));
    }
}

void DocumentParser::UpdateComment(const char c)
{
    switch (c)
    {
    case '\r':
        break;

    case '\n':
        // Flush comment
        doc.tokens.push_back({ TokenType::COMMENT, (float)doc.string_pool.size() });
        tok.push_back('\0');
        std::copy(tok.begin(), tok.end(), std::back_inserter(doc.string_pool));
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        // Break line
        doc.tokens.push_back({ TokenType::LINEBREAK, 0.f });
        line_num++;
        line_pos = 0;
        break;

    case '/':
        if (partial_tok_type != PartialToken::COMMENT_SLASH || tok.size() > 0) // With COMMENT_SLASH, skip any number of leading '/'
        {
            tok.push_back(c);
        }
        line_pos++;
        break;

    default:
        tok.push_back(c);
        line_pos++;
        break;
    }
}

void DocumentParser::UpdateString(const char c)
{
    switch (c)
    {
    case '\r':
        break;

    case ' ':
    case ',':
    case '\t':
        if (partial_tok_type == PartialToken::STRING_QUOTED)
        {
            tok.push_back('\0');
        }
        else // (partial_tok_type == PartialToken::STRING_NAKED)
        {
            // Flush string
            doc.tokens.push_back({ TokenType::STRING, (float)doc.string_pool.size() });
            tok.push_back('\0');
            std::copy(tok.begin(), tok.end(), std::back_inserter(doc.string_pool));
            tok.clear();
            partial_tok_type = PartialToken::NONE;
        }
        line_pos++;
        break;

    case '\n':
        if (partial_tok_type == PartialToken::STRING_QUOTED)
        {
            App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_WARNING,
                fmt::format("{}, line {}, pos {}: quoted string interrupted by newline", datastream->getName(), line_num, line_pos));
        }
        // Flush string
        doc.tokens.push_back({ TokenType::STRING, (float)doc.string_pool.size() });
        tok.push_back('\0');
        std::copy(tok.begin(), tok.end(), std::back_inserter(doc.string_pool));
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        // Break line
        doc.tokens.push_back({ TokenType::LINEBREAK, 0.f });
        line_num++;
        line_pos = 0;
        break;

    case '"':
        if (partial_tok_type == PartialToken::STRING_QUOTED)
        {
            // Flush string
            doc.tokens.push_back({ TokenType::STRING, (float)doc.string_pool.size() });
            tok.push_back('\0');
            std::copy(tok.begin(), tok.end(), std::back_inserter(doc.string_pool));
            tok.clear();
            partial_tok_type = PartialToken::NONE;
        }
        else // (partial_tok_type == PartialToken::STRING_NAKED)
        {
            partial_tok_type = PartialToken::GARBAGE;
            tok.push_back(c);
        }
        line_pos++;
        break;

    default:
        tok.push_back(c);
        line_pos++;
        break;
    }

    if (partial_tok_type == PartialToken::GARBAGE)
    {
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_WARNING,
            fmt::format("{}, line {}, pos {}: stray character '{}' in string", datastream->getName(), line_num, line_pos, c));
    }
}

void DocumentParser::UpdateNumber(const char c)
{
    switch (c)
    {
    case '\r':
        break;

    case ' ':
    case ',':
    case '\t':
        // Flush number
        tok.push_back('\0');
        doc.tokens.push_back({ TokenType::NUMBER, (float)Ogre::StringConverter::parseReal(tok.data()) });
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        line_pos++;
        break;

    case '\n':
        // Flush number
        tok.push_back('\0');
        doc.tokens.push_back({ TokenType::NUMBER, (float)Ogre::StringConverter::parseReal(tok.data()) });
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        // Break line
        doc.tokens.push_back({ TokenType::LINEBREAK, 0.f });
        line_num++;
        line_pos = 0;
        break;

    case '-':
        partial_tok_type = PartialToken::GARBAGE;
        tok.push_back(c);
        line_pos++;
        break;

    case '.':
        if (partial_tok_type == PartialToken::NUMBER)
        {
            tok.push_back(c);
            partial_tok_type = PartialToken::NUMBER_DOT;
        }
        else // (partial_tok_type == PartialToken::NUMBER_DOT)
        {
            partial_tok_type = PartialToken::GARBAGE;
            tok.push_back(c);
        }
        line_pos++;
        break;
    }

    if (partial_tok_type == PartialToken::GARBAGE)
    {
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_WARNING,
            fmt::format("{}, line {}, pos {}: stray character '{}' in number", datastream->getName(), line_num, line_pos, c));
    }
}

void DocumentParser::UpdateBool(const char c)
{
    switch (c)
    {
    case '\r':
        break;

    case ' ':
    case ',':
    case '\t':
        // Discard token
        tok.push_back('\0');
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_WARNING,
            fmt::format("{}, line {}, pos {}: discarding incomplete boolean token '{}'", datastream->getName(), line_num, line_pos, tok.data()));
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        line_pos++;
        break;

    case '\n':
        // Discard token
        tok.push_back('\0');
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_WARNING,
            fmt::format("{}, line {}, pos {}: discarding incomplete boolean token '{}'", datastream->getName(), line_num, line_pos, tok.data()));
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        // Break line
        doc.tokens.push_back({ TokenType::LINEBREAK, 0.f });
        line_num++;
        line_pos = 0;
        break;

    case 'r':
        if (partial_tok_type != PartialToken::BOOL_TRUE || tok.size() != 1)
        {
            if (options & GenericDocument::OPTION_ALLOW_NAKED_STRINGS)
                partial_tok_type = PartialToken::STRING_NAKED;
            else
                partial_tok_type = PartialToken::GARBAGE;
        }
        tok.push_back(c);
        line_pos++;
        break;

    case 'u':
        if (partial_tok_type != PartialToken::BOOL_TRUE || tok.size() != 2)
        {
            if (options & GenericDocument::OPTION_ALLOW_NAKED_STRINGS)
                partial_tok_type = PartialToken::STRING_NAKED;
            else
                partial_tok_type = PartialToken::GARBAGE;
        }
        tok.push_back(c);
        line_pos++;
        break;

    case 'a':
        if (partial_tok_type != PartialToken::BOOL_FALSE || tok.size() != 1)
        {
            if (options & GenericDocument::OPTION_ALLOW_NAKED_STRINGS)
                partial_tok_type = PartialToken::STRING_NAKED;
            else
                partial_tok_type = PartialToken::GARBAGE;
        }
        tok.push_back(c);
        line_pos++;
        break;

    case 'l':
        if (partial_tok_type != PartialToken::BOOL_FALSE || tok.size() != 2)
        {
            if (options & GenericDocument::OPTION_ALLOW_NAKED_STRINGS)
                partial_tok_type = PartialToken::STRING_NAKED;
            else
                partial_tok_type = PartialToken::GARBAGE;
        }
        tok.push_back(c);
        line_pos++;
        break;

    case 's':
        if (partial_tok_type != PartialToken::BOOL_FALSE || tok.size() != 3)
        {
            if (options & GenericDocument::OPTION_ALLOW_NAKED_STRINGS)
                partial_tok_type = PartialToken::STRING_NAKED;
            else
                partial_tok_type = PartialToken::GARBAGE;
        }
        tok.push_back(c);
        line_pos++;
        break;

    case 'e':
        if (partial_tok_type == PartialToken::BOOL_TRUE && tok.size() == 3)
        {
            doc.tokens.push_back({ TokenType::BOOL, 1.f });
            tok.clear();
            partial_tok_type = PartialToken::NONE;
        }
        else if (partial_tok_type == PartialToken::BOOL_FALSE && tok.size() == 4)
        {
            doc.tokens.push_back({ TokenType::BOOL, 0.f });
            tok.clear();
            partial_tok_type = PartialToken::NONE;
        }
        else
        {
            if (options & GenericDocument::OPTION_ALLOW_NAKED_STRINGS)
                partial_tok_type = PartialToken::STRING_NAKED;
            else
                partial_tok_type = PartialToken::GARBAGE;
            tok.push_back(c);
        }
        line_pos++;
        break;

    default:
        if (options & GenericDocument::OPTION_ALLOW_NAKED_STRINGS)
            partial_tok_type = PartialToken::STRING_NAKED;
        else
            partial_tok_type = PartialToken::GARBAGE;
        tok.push_back(c);
        break;
    }

    if (partial_tok_type == PartialToken::GARBAGE)
    {
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_WARNING,
            fmt::format("{}, line {}, pos {}: stray character '{}' in boolean", datastream->getName(), line_num, line_pos, c));
    }
}

void DocumentParser::UpdateKeyword(const char c)
{
    switch (c)
    {
    case '\r':
        break;

    case ' ':
    case ',':
    case '\t':
        // Flush keyword
        doc.tokens.push_back({ TokenType::KEYWORD, (float)doc.string_pool.size() });
        tok.push_back('\0');
        std::copy(tok.begin(), tok.end(), std::back_inserter(doc.string_pool));
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        line_pos++;
        break;

    case '\n':
        // Flush keyword
        doc.tokens.push_back({ TokenType::KEYWORD, (float)doc.string_pool.size() });
        tok.push_back('\0');
        std::copy(tok.begin(), tok.end(), std::back_inserter(doc.string_pool));
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        // Break line
        doc.tokens.push_back({ TokenType::LINEBREAK, 0.f });
        line_num++;
        line_pos = 0;
        break;

    default:
        if (!isalnum(c))
        {
            partial_tok_type = PartialToken::GARBAGE;
        }
        tok.push_back(c);
        line_pos++;
        break;
    }

    if (partial_tok_type == PartialToken::GARBAGE)
    {
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_WARNING,
            fmt::format("{}, line {}, pos {}: stray character '{}' in keyword", datastream->getName(), line_num, line_pos, c));
    }
}

void DocumentParser::UpdateTitle(const char c)
{
    switch (c)
    {
    case '\r':
        break;

    case '\n':
        // Flush title string
        doc.tokens.push_back({ TokenType::STRING, (float)doc.string_pool.size() });
        tok.push_back('\0');
        std::copy(tok.begin(), tok.end(), std::back_inserter(doc.string_pool));
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        // Break line
        doc.tokens.push_back({ TokenType::LINEBREAK, 0.f });
        line_num++;
        line_pos = 0;
        break;

    default:
        tok.push_back(c);
        line_pos++;
        break;
    }
}

void DocumentParser::UpdateGarbage(const char c)
{
    switch (c)
    {
    case '\r':
        break;

    case ' ':
    case ',':
    case '\t':
    case '\n':
        tok.push_back('\0');
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_WARNING,
            fmt::format("{}, line {}, pos {}: discarding garbage token '{}'", datastream->getName(), line_num, line_pos, tok.data()));
        tok.clear();
        partial_tok_type = PartialToken::NONE;
        line_pos++;
        break;

    default:
        tok.push_back(c);
        line_pos++;
        break;
    }
}   

void GenericDocument::LoadFromDataStream(Ogre::DataStreamPtr datastream, const BitMask_t options)
{
    // Reset the document
    tokens.clear();
    string_pool.clear();

    // Prepare context
    DocumentParser parser(*this, options, datastream);
    const size_t LINE_BUF_MAX = 10 * 1024; // 10Kb
    char buf[LINE_BUF_MAX];

    // Parse the text
    while (!datastream->eof())
    {
        size_t buf_len = datastream->read(buf, LINE_BUF_MAX);
        for (size_t i = 0; i < buf_len; i++)
        {
            const char c = buf[i];

            switch (parser.partial_tok_type)
            {
            case PartialToken::NONE:
                parser.BeginToken(c);
                break;

            case PartialToken::COMMENT_SEMICOLON:
            case PartialToken::COMMENT_SLASH:
                parser.UpdateComment(c);
                break;

            case PartialToken::STRING_QUOTED:
            case PartialToken::STRING_NAKED:
                parser.UpdateString(c);
                break;

            case PartialToken::NUMBER:
            case PartialToken::NUMBER_DOT:
                parser.UpdateNumber(c);
                break;

            case PartialToken::BOOL_TRUE:
            case PartialToken::BOOL_FALSE:
                parser.UpdateBool(c);
                break;

            case PartialToken::KEYWORD:
                parser.UpdateKeyword(c);
                break;

            case PartialToken::TITLE_STRING:
                parser.UpdateTitle(c);
                break;

            case PartialToken::GARBAGE:
                parser.UpdateGarbage(c);
                break;
            }
        }
    }

    // Ensure newline at end of file
    if (tokens.size() == 0 || tokens.back().type != TokenType::LINEBREAK)
    {
        tokens.push_back({ TokenType::LINEBREAK, 0.f });
    }
}

#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
    const char* EOL_STR = "\r\n"; // CR+LF
#else
    const char* EOL_STR = "\n"; // "LF"
#endif

void GenericDocument::SaveToDataStream(Ogre::DataStreamPtr datastream)
{
    std::string separator;
    const char* pool_str = nullptr;
    const size_t BUF_MAX = 100;
    char buf[BUF_MAX];

    for (Token& tok : tokens)
    {
        switch (tok.type)
        {
        case TokenType::LINEBREAK:
            datastream->write(EOL_STR, strlen(EOL_STR));
            separator = "";
            break;

        case TokenType::COMMENT:
            datastream->write(";", 1);
            pool_str = string_pool.data() + (size_t)tok.data;
            datastream->write(pool_str, strlen(pool_str));
            break;

        case TokenType::STRING:
            datastream->write(separator.data(), separator.size());
            pool_str = string_pool.data() + (size_t)tok.data;
            datastream->write(pool_str, strlen(pool_str));
            separator = ",";
            break;

        case TokenType::NUMBER:
            datastream->write(separator.data(), separator.size());
            snprintf(buf, BUF_MAX, "%f", tok.data);
            datastream->write(buf, strlen(buf));
            separator = ",";
            break;

        case TokenType::BOOL:
            datastream->write(separator.data(), separator.size());
            snprintf(buf, BUF_MAX, "%s", tok.data == 1.f ? "true" : "false");
            datastream->write(buf, strlen(buf));
            separator = ",";
            break;

        case TokenType::KEYWORD:
            pool_str = string_pool.data() + (size_t)tok.data;
            datastream->write(pool_str, strlen(pool_str));
            separator = " ";
            break;
        }
    }
}

bool GenericDocument::LoadFromResource(std::string resource_name, std::string resource_group_name, BitMask_t options/* = 0*/)
{
    try
    {
        Ogre::DataStreamPtr datastream = Ogre::ResourceGroupManager::getSingleton().openResource(resource_name, resource_group_name);
        this->LoadFromDataStream(datastream, options);
        return true;
    }
    catch (Ogre::Exception& eeh)
    {
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_ERROR,
            fmt::format("GenericDocument: could not load file '{}' from resource group '{}': {}", resource_name, resource_group_name, eeh.getDescription()));
        return false;
    }
}

bool GenericDocument::SaveToResource(std::string resource_name, std::string resource_group_name)
{
    try
    {
        Ogre::DataStreamPtr datastream = Ogre::ResourceGroupManager::getSingleton().createResource(resource_name, resource_group_name);
        this->SaveToDataStream(datastream);
        return true;
    }
    catch (Ogre::Exception& eeh)
    {
        App::GetConsole()->putMessage(Console::CONSOLE_MSGTYPE_INFO, Console::CONSOLE_SYSTEM_ERROR,
            fmt::format("GenericDocument: could not write file '{}' to resource group '{}': {}", resource_name, resource_group_name, eeh.getDescription()));
        return false;
    }
}

bool GenericDocReader::SeekNextLine()
{
    // Skip current line
    while (!this->EndOfFile() && this->GetTokType() != TokenType::LINEBREAK)
    {
        this->MoveNext();
    }

    // Skip comments
    while (!this->EndOfFile() && !this->IsTokString() && !this->IsTokFloat() && !this->IsTokBool() && !this->IsTokKeyword())
    {
        this->MoveNext();
    }

    return this->EndOfFile();
}

int GenericDocReader::CountLineArgs()
{
    int count = 0;
    while (!EndOfFile(count) && this->GetTokType(count) != TokenType::LINEBREAK)
        count++;
    return count;
}
