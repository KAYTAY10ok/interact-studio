#include "languages.h"

#include <QFileInfo>
#include <QHash>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSet>
#include <QSyntaxHighlighter>
#include <QVector>

namespace {

enum class Token { Comment, String, Number, Keyword, Name, Key, Punctuation };
enum class Layout { Code, Markup, Markdown, Data, Style };

struct Region {
    QString begin;
    QString end;
    Token token;
    bool nested = false;
};

struct Language {
    QString name;
    QStringList keywords;
    QSet<QString> wordSet;
    QStringList lineComments;
    QString quotes;
    QVector<Region> regions;
    Layout layout;
    bool insensitive;
};

const Region cComment{"/*", "*/", Token::Comment};
const Region nestedCComment{"/*", "*/", Token::Comment, true};
const Region markupComment{"<!--", "-->", Token::Comment};
const Region backticks{"`", "`", Token::String};
const QVector<Region> tripleQuotes{
    {QString(3, '"'), QString(3, '"'), Token::String},
    {QString(3, '\''), QString(3, '\''), Token::String}
};
const char javascriptKeywords[] =
    "async await break case catch class const continue debugger default delete do else export extends false finally for function if import in instanceof let new null of return static super switch this throw true try typeof undefined var void while yield";
const char typescriptKeywords[] =
    "abstract any as async await boolean break case catch class const continue declare default delete do else enum export extends false finally for function if implements import in infer instanceof interface keyof let namespace never new null number of private protected public readonly return satisfies static string super switch this throw true try type typeof undefined unknown var void while yield";

class LanguageCatalog {
public:
    LanguageCatalog()
    {
        add("C", "c",
            "alignas alignof asm auto bool break case char const constexpr continue default do double else enum extern false float for goto if inline int long nullptr register restrict return short signed sizeof static struct switch thread_local true typedef typeof union unsigned void volatile while",
            {"//"}, "\"'", {cComment});
        add("C++", "h hh hpp hxx cpp cc cxx inl ipp tpp",
            "alignas alignof asm auto bool break case catch char class concept const consteval constexpr constinit continue decltype default delete do double else enum explicit export extern false final float for friend goto if import inline int long module mutable namespace new noexcept nullptr operator override private protected public requires return short signed sizeof static struct switch template this throw true try typedef typename union unsigned using virtual void volatile while",
            {"//"}, "\"'", {cComment});
        add("Rust", "rs",
            "as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type unsafe use where while",
            {"//"}, "\"", {nestedCComment});
        add("Python", "py pyi pyw",
            "and as assert async await break class continue def del elif else except False finally for from global if import in is lambda match case None nonlocal not or pass raise return True try while with yield",
            {"#"}, "\"'", tripleQuotes);
        add("JavaScript", "js mjs cjs", javascriptKeywords, {"//"}, "\"'", {cComment, backticks});
        add("TypeScript", "ts mts cts", typescriptKeywords, {"//"}, "\"'", {cComment, backticks});
        add("JSX", "jsx", javascriptKeywords, {"//"}, "\"'", {cComment, backticks});
        add("TSX", "tsx", typescriptKeywords, {"//"}, "\"'", {cComment, backticks});
        add("HTML", "htm html xhtml shtml",
            "a article aside body button canvas code div footer form h1 h2 h3 head header html iframe img input label li link main meta nav ol option p script section select span style table textarea ul",
            {}, "\"'", {markupComment}, Layout::Markup, true);
        add("Vue", "vue",
            "component script slot style template transition v-bind v-else v-for v-if v-model v-on v-show",
            {}, "\"'", {markupComment}, Layout::Markup, true);
        add("Svelte", "svelte",
            "class let on script slot style svelte:component svelte:head svelte:window",
            {}, "\"'", {markupComment}, Layout::Markup, true);
        add("XML", "xml svg xsd xsl xslt rss atom plist ui qrc",
            "", {}, "\"'", {markupComment}, Layout::Markup);
        add("CSS", "css scss sass less",
            "auto block border color display flex font grid inherit initial inline inline-block margin none padding position relative static unset var",
            {}, "\"'", {cComment}, Layout::Style, true);
        add("JSON", "json webmanifest geojson ipynb",
            "false null true", {}, "\"", {}, Layout::Data);
        add("JSON with comments", "jsonc",
            "false null true", {"//"}, "\"", {cComment}, Layout::Data);
        add("YAML", "yaml yml",
            "false null true yes no on off", {"#"}, "\"'", {}, Layout::Data, true);
        add("TOML", "toml",
            "false true", {"#"}, "\"'", tripleQuotes, Layout::Data);
        add("Markdown", "md markdown mdown mdx",
            "", {}, "", {markupComment}, Layout::Markdown);
        add("Lua", "lua",
            "and break do else elseif end false for function goto if in local nil not or repeat return then true until while",
            {"--"}, "\"'", {{"--[[", "]]", Token::Comment}, {"[[", "]]", Token::String}});
        add("PHP", "php phtml",
            "abstract array as break case catch class const continue declare default die do echo else elseif empty enum extends false final finally fn for foreach function global if implements include interface isset match namespace new null print private protected public readonly require return static switch throw trait true try unset use while yield",
            {"//", "#"}, "\"'", {cComment}, Layout::Code, true);
        add("Java", "java",
            "abstract assert boolean break byte case catch char class continue default do double else enum extends false final finally float for if implements import instanceof int interface long native new non-sealed null package permits private protected public record return sealed short static super switch synchronized this throw throws transient true try var void volatile while",
            {"//"}, "\"'", {cComment, {QString(3, '"'), QString(3, '"'), Token::String}});
        add("Kotlin", "kt kts",
            "abstract as break by catch class companion const constructor continue data do else enum false finally for fun if import in init interface internal is lateinit null object open out override package private protected public return sealed super suspend this throw true try typealias val var when while",
            {"//"}, "\"'", {nestedCComment, {QString(3, '"'), QString(3, '"'), Token::String}});
        add("C#", "cs",
            "abstract as async await base bool break byte case catch class const continue decimal default delegate do double else enum event explicit extern false finally fixed float for foreach if implicit in int interface internal is lock long namespace new null object operator out override params private protected public readonly record ref return sbyte sealed short sizeof stackalloc static string struct switch this throw true try typeof uint ulong unchecked unsafe ushort using var virtual void volatile while yield",
            {"//"}, "\"'", {cComment, {QString(3, '"'), QString(3, '"'), Token::String}});
        add("Go", "go",
            "any bool break byte case chan comparable const continue default defer else error fallthrough false float32 float64 for func go goto if import int interface map nil package range return rune select string struct switch true type uint var",
            {"//"}, "\"'", {cComment, backticks});
        add("Ruby", "rb rake gemspec",
            "alias and begin break case class def defined do else elsif end ensure false for if in module next nil not or redo rescue retry return self super then true undef unless until when while yield",
            {"#"}, "\"'`", {});
        add("Swift", "swift",
            "actor any as async await break case catch class defer deinit do else enum extension false fileprivate for func guard if import in init inout internal is let nil nonisolated open operator override private protocol public repeat return self Self some static struct subscript super switch throw throws true try typealias var where while",
            {"//"}, "\"'", {nestedCComment, {QString(3, '"'), QString(3, '"'), Token::String}});
        add("Shell", "sh bash zsh fish zshrc bashrc",
            "break case cd continue declare do done echo elif else esac eval exec exit export false fi for function if in local printf read readonly return select set shift source test then trap true unset until wait while",
            {"#"}, "\"'`", {});
        add("PowerShell", "ps1 psm1 psd1",
            "begin break catch class continue data do dynamicparam else elseif end enum exit filter finally for foreach function if in param process return switch throw trap try until using while workflow",
            {"#"}, "\"'", {{"<#", "#>", Token::Comment}}, Layout::Code, true);
        add("SQL", "sql ddl dml psql",
            "add all alter and as asc begin between by case check column commit create cross default delete desc distinct drop else end except exists false foreign from full group having in index inner insert intersect into is join key left like limit not null offset on or order outer primary references returning right rollback select set table then transaction true union unique update using values view when where with",
            {"--"}, "\"'`", {cComment}, Layout::Code, true);
        add("Dockerfile", "",
            "ADD ARG CMD COPY ENTRYPOINT ENV EXPOSE FROM HEALTHCHECK LABEL MAINTAINER ONBUILD RUN SHELL STOPSIGNAL USER VOLUME WORKDIR",
            {"#"}, "\"'", {}, Layout::Code, true);
        add("Makefile", "mk mak",
            "call define else endef endif export foreach ifdef ifeq ifndef ifneq include override patsubst private shell subst undefine unexport wildcard",
            {"#"}, "\"'", {});
        add("CMake", "cmake",
            "add_custom_command add_custom_target add_executable add_library break cmake_minimum_required configure_file else elseif endforeach endfunction endif endwhile execute_process file find_package foreach function if include include_directories install list message option project return set string target_compile_definitions target_compile_options target_include_directories target_link_libraries unset while",
            {"#"}, "\"", {}, Layout::Code, true);
        add("Lisp", "el lisp lsp scm ss clj cljs cljc edn rkt",
            "car cdr cond cons def defmacro defn defun defvar do doseq fn if lambda let loop nil progn quote require setq when",
            {";"}, "\"", {{"#|", "|#", Token::Comment, true}});
        add("Dart", "dart",
            "abstract as assert async await break case catch class const continue default do dynamic else enum export extends extension external false final finally for Function get if implements import in interface is late library mixin new null on operator part required return sealed set static super switch this throw true try typedef var void while with yield",
            {"//"}, "\"'", {nestedCComment, tripleQuotes[0], tripleQuotes[1]});
        add("Scala", "scala sc",
            "abstract as case catch class def do else enum export extends false final finally for given if implicit import inline lazy match new null object opaque override package private protected return sealed super then this throw trait true try type using val var while with yield",
            {"//"}, "\"'", {nestedCComment, tripleQuotes[0]});
        add("Haskell", "hs lhs",
            "as case class data default deriving do else hiding if import in infix infixl infixr instance let module newtype of qualified then type where",
            {"--"}, "\"", {{"{-", "-}", Token::Comment, true}});
        add("OCaml", "ml mli",
            "and as assert begin class constraint do done downto else end exception external false for fun function functor if in include inherit lazy let match method module mutable new object of open or private rec sig struct then to true try type val virtual when while with",
            {}, "\"", {{"(*", "*)", Token::Comment, true}});
        add("Elixir", "ex exs",
            "after alias and case catch cond def defdelegate defmacro defmodule defp defprotocol defstruct do else end false fn for if import in nil not or quote raise receive require rescue true try unless use when with",
            {"#"}, "\"'", tripleQuotes);
        add("R", "r rprofile",
            "break else FALSE for function if Inf NA NaN next NULL repeat TRUE while",
            {"#"}, "\"'`", {});
        add("Nix", "nix",
            "assert else false if in inherit let null or rec then true with",
            {"#"}, "\"", {cComment, {"''", "''", Token::String}});
        add("INI / configuration", "ini cfg conf properties desktop env gitignore gitattributes editorconfig",
            "false no off on true yes", {"#", ";"}, "\"'", {}, Layout::Data, true);
        add("Diff", "diff patch",
            "", {}, "", {}, Layout::Markdown);

        names.insert("cmakelists.txt", index("CMake"));
        names.insert("makefile", index("Makefile"));
        names.insert("gnumakefile", index("Makefile"));
        names.insert("dockerfile", index("Dockerfile"));
        names.insert("containerfile", index("Dockerfile"));
        names.insert(".bashrc", index("Shell"));
        names.insert(".zshrc", index("Shell"));
        names.insert(".editorconfig", index("INI / configuration"));
        names.insert(".gitignore", index("INI / configuration"));
        names.insert(".gitattributes", index("INI / configuration"));
        names.insert("rakefile", index("Ruby"));
        names.insert("gemfile", index("Ruby"));
    }

    const Language *lookup(const QString &path) const
    {
        const QFileInfo file(path);
        // .C is conventionally C++ while .c is C.
        if (file.suffix() == "C") return &languages[index("C++")];
        const QString filename = file.fileName().toLower();
        int selected = names.value(filename, -1);
        if (selected < 0 && (filename.startsWith("dockerfile.") || filename.startsWith("containerfile.")))
            selected = index("Dockerfile");
        if (selected < 0 && (filename == ".env" || filename.startsWith(".env.")))
            selected = index("INI / configuration");
        if (selected < 0) selected = extensions.value(file.suffix().toLower(), -1);
        return selected < 0 ? nullptr : &languages[selected];
    }

private:
    void add(const char *name, const char *suffixes, const char *words, QStringList lineComments,
             const char *quotes, QVector<Region> regions, Layout layout = Layout::Code, bool insensitive = false)
    {
        Language entry{QString::fromLatin1(name), QString::fromLatin1(words).split(' ', Qt::SkipEmptyParts),
                       {}, std::move(lineComments), QString::fromLatin1(quotes), std::move(regions), layout, insensitive};
        for (const auto &word : entry.keywords)
            entry.wordSet.insert(insensitive ? word.toCaseFolded() : word);
        for (const auto &extension : QString::fromLatin1(suffixes).split(' ', Qt::SkipEmptyParts))
            extensions.insert(extension, languages.size());
        byName.insert(entry.name, languages.size());
        languages.append(std::move(entry));
    }

    int index(const QString &name) const { return byName.value(name, -1); }

    QVector<Language> languages;
    QHash<QString, int> names;
    QHash<QString, int> extensions;
    QHash<QString, int> byName;
};

const Language *languageFor(const QString &path)
{
    static const LanguageCatalog catalog;
    return catalog.lookup(path);
}

class LanguageHighlighter final : public QSyntaxHighlighter {
public:
    LanguageHighlighter(QPlainTextEdit *editor, const Language &language)
        : QSyntaxHighlighter(editor->document()), editor(editor), language(language) {}

private:
    const QPlainTextEdit *editor;
    const Language &language;
    bool light = false;

    void color(int from, int to, Token token)
    {
        if (to <= from) return;
        // Separate palettes keep comments and syntax accents legible in light and dark themes.
        static const QColor dark[] = {
            QColor("#85938d"), QColor("#b1cb91"), QColor("#e4b878"), QColor("#ce9fce"),
            QColor("#88c9cb"), QColor("#9eb8e3"), QColor("#abb5c5")
        };
        static const QColor pale[] = {
            QColor("#506555"), QColor("#397248"), QColor("#965020"), QColor("#824876"),
            QColor("#226879"), QColor("#325887"), QColor("#536175")
        };
        setFormat(from, to - from, (light ? pale : dark)[static_cast<int>(token)]);
    }

    static bool begins(const QString &line, int at, int end, const QString &text)
    {
        return at + text.size() <= end && line.midRef(at, text.size()) == text;
    }

    static int skipSpace(const QString &line, int i, int limit)
    {
        while (i < limit && line.at(i).isSpace()) ++i;
        return i;
    }

    int scanRegion(const QString &line, int from, int end, const Region &region, int &depth) const
    {
        if (!region.nested && region.token != Token::String) {
            const int close = line.indexOf(region.end, from);
            if (close >= 0 && close + region.end.size() <= end) {
                depth = 0;
                return close + region.end.size();
            }
            return end;
        }
        for (int i = from; i < end;) {
            if (region.token == Token::String && line.at(i) == '\\') i += 2;
            else if (begins(line, i, end, region.end)) {
                i += region.end.size();
                if (--depth == 0) return i;
            } else if (region.nested && begins(line, i, end, region.begin)) {
                depth = qMin(depth + 1, 1 << 20);
                i += region.begin.size();
            } else ++i;
        }
        return end;
    }

    bool highlightMarkdown(const QString &line, int end)
    {
        int from = skipSpace(line, 0, end);
        if (language.name == "Diff") {
            if (line.startsWith("@@")) color(0, end, Token::Name);
            else if (line.startsWith('+')) color(0, end, Token::String);
            else if (line.startsWith('-')) color(0, end, Token::Keyword);
            else if (line.startsWith("diff ") || line.startsWith("index ")) color(0, end, Token::Number);
            return true;
        }
        const int previous = previousBlockState();
        constexpr int ticks = 1 << 28;
        constexpr int tildes = ticks + 1;
        const bool firstTicks = begins(line, from, end, QStringLiteral("```"));
        const bool firstTildes = begins(line, from, end, QStringLiteral("~~~"));
        if (previous == ticks || previous == tildes) {
            const bool close = previous == ticks ? firstTicks : firstTildes;
            color(0, end, close ? Token::Punctuation : Token::String);
            setCurrentBlockState(close ? 0 : previous);
            return true;
        }
        if (firstTicks || firstTildes) {
            color(from, end, Token::Punctuation);
            setCurrentBlockState(firstTicks ? ticks : tildes);
            return true;
        }
        if (from < end && line.at(from) == '#') {
            color(from, end, Token::Name);
            return true;
        }
        if (from < end && line.at(from) == '>') color(from, end, Token::Comment);
        static const QRegularExpression spans(QStringLiteral(R"(`[^`]+?`|\[[^\]]+\]\([^)]*\)|\*\*[^*]+\*\*|__[^_]+__)"));
        auto match = spans.globalMatch(line.left(end));
        while (match.hasNext()) {
            const auto token = match.next();
            color(token.capturedStart(), token.capturedEnd(),
                  line.at(token.capturedStart()) == '`' ? Token::String : Token::Keyword);
        }
        return false;
    }

    void highlightBlock(const QString &block) override
    {
        // A minified file can be one huge physical line. Cap colored work per block.
        const QString line = block.left(16384);
        const int end = line.size();
        light = editor->palette().base().color().lightness() > 145;
        const int previous = previousBlockState();
        setCurrentBlockState(0);
        if (language.layout == Layout::Markdown &&
                (previous <= 0 || (previous & 15) == 0 || previous >= (1 << 28)) &&
                highlightMarkdown(line, end)) return;

        constexpr int tagState = 1 << 27;
        constexpr int tagNameState = 1 << 26;
        const bool markup = language.layout == Layout::Markup;
        const bool react = language.name == "JSX" || language.name == "TSX";
        int regionIndex = previous > 0 ? (previous & 15) - 1 : -1;
        int depth = regionIndex >= 0 ? (previous & ~(tagState | tagNameState)) >> 4 : 0;
        if (regionIndex >= language.regions.size()) regionIndex = -1;
        bool inTag = previous > 0 && (previous & tagState) != 0;
        bool tagName = previous > 0 && (previous & tagNameState) != 0;

        for (int i = 0; i < end || regionIndex >= 0;) {
            const int start = i;
            if (regionIndex < 0) {
                for (int candidate = 0; candidate < language.regions.size(); ++candidate) {
                    if (begins(line, i, end, language.regions[candidate].begin)) {
                        regionIndex = candidate;
                        depth = 1;
                        i += language.regions[candidate].begin.size();
                        break;
                    }
                }
            }
            if (regionIndex >= 0) {
                const auto &region = language.regions[regionIndex];
                i = scanRegion(line, i, end, region, depth);
                color(start, i, region.token);
                if (depth > 0) break;
                regionIndex = -1;
                continue;
            }

            bool comment = false;
            for (const auto &prefix : language.lineComments) {
                if (begins(line, i, end, prefix)) { comment = true; break; }
            }
            if (comment) {
                color(i, end, Token::Comment);
                break;
            }

            const QChar ch = line.at(i++);
            if (markup || react) {
                if (ch == '<') {
                    int before = start - 1;
                    while (before >= 0 && line.at(before).isSpace()) --before;
                    const bool jsxContext = before < 0 || QStringLiteral("=(,>{?:[").contains(line.at(before)) ||
                            (before >= 5 && line.midRef(before - 5, 6) == "return") ||
                            (i < end && line.at(i) == '/');
                    inTag = i < end && (markup || jsxContext) &&
                            (line.at(i).isLetter() || line.at(i) == '/' || line.at(i) == '?' || line.at(i) == '!');
                    tagName = inTag;
                    if (inTag) color(start, i, Token::Punctuation);
                    continue;
                }
                if (markup && !inTag) continue;
                if (inTag && ch == '>') {
                    inTag = tagName = false;
                    color(start, i, Token::Punctuation);
                    continue;
                }
            }

            if (language.quotes.contains(ch)) {
                while (i < end) {
                    const QChar current = line.at(i++);
                    if (current == ch) break;
                    if (current == '\\' && i < end) ++i;
                }
                const int next = skipSpace(line, i, end);
                color(start, i, language.layout == Layout::Data && next < end && line.at(next) == ':'
                      ? Token::Key : Token::String);
                continue;
            }

            if (ch.isDigit()) {
                while (i < end && (line.at(i).isLetterOrNumber() || line.at(i) == '_' || line.at(i) == '.')) ++i;
                color(start, i, Token::Number);
                continue;
            }

            if (!ch.isLetter() && ch != '_' && ch != '$') continue;
            const bool dashIdentifier = inTag || language.layout == Layout::Data || language.layout == Layout::Style;
            while (i < end && (line.at(i).isLetterOrNumber() || line.at(i) == '_' ||
                              (dashIdentifier && (line.at(i) == '-' || (inTag && line.at(i) == ':'))))) ++i;
            const int next = skipSpace(line, i, end);
            const QChar after = next < end ? line.at(next) : QChar{};
            const QString word = line.mid(start, i - start);
            if (inTag) {
                color(start, i, tagName ? Token::Keyword : Token::Key);
                tagName = false;
            } else if ((language.layout == Layout::Data || language.layout == Layout::Style) &&
                       (after == ':' || after == '=')) {
                color(start, i, Token::Key);
            } else if (language.wordSet.contains(language.insensitive ? word.toCaseFolded() : word)) {
                color(start, i, Token::Keyword);
            } else if (after == '(') {
                color(start, i, Token::Name);
            }
        }
        int state = regionIndex >= 0 ? (depth << 4) | (regionIndex + 1) : 0;
        if (inTag) state |= tagState;
        if (tagName) state |= tagNameState;
        setCurrentBlockState(state);
    }
};

}

QString editorLanguage(const QString &path)
{
    const auto *language = languageFor(path);
    return language ? language->name : QStringLiteral("Plain text");
}

QStringList languageCompletions(const QString &path)
{
    const auto *language = languageFor(path);
    return language ? language->keywords : QStringList{};
}

QSyntaxHighlighter *createLanguageHighlighter(QPlainTextEdit *editor, const QString &path)
{
    const auto *language = languageFor(path);
    return language ? new LanguageHighlighter(editor, *language) : nullptr;
}
