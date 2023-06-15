// Copyright (C) 2023  ilobilo

#include <fmt/color.h>

#include <string_view>
#include <string>

#include <optional>
#include <utility>
#include <vector>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <csignal>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

constexpr auto ui_colours = fmt::bg(fmt::terminal_color::white) | fmt::fg(fmt::terminal_color::black);
constexpr std::size_t tab_size = 4;

constexpr auto status_help = "Ctrl-Q - Quit | Ctrl-S - Save";
std::string status_message = status_help;

std::string filename;
bool modified = false;

std::size_t width;
std::size_t height;

struct {
    std::size_t r;
    std::size_t x;
    std::size_t y;
} cursor { 0, 0, 1 };

using row_t = std::pair<std::string, std::string>;
std::vector<row_t> rows;

std::size_t row_offset = 0;
std::size_t col_offset = 0;

termios og_tios;

template<std::size_t limit = 128>
class printer
{
    private:
    std::string _buffer;

    public:
    printer()
    {
        this->_buffer.reserve(limit);
    }

    printer &operator()(char c, std::size_t count = 1)
    {
        if (this->_buffer.length() + 1 > limit)
            this->operator()();

        _buffer.append(count, c);
        return *this;
    }

    printer &operator()(std::string_view str)
    {
        if (this->_buffer.length() + str.length() > limit)
            this->operator()();

        _buffer.append(str);
        return *this;
    }

    printer &flush()
    {
        write(STDOUT_FILENO, this->_buffer.c_str(), this->_buffer.length());
        this->_buffer.clear();
        return *this;
    }

    printer &operator()()
    {
        return this->flush();
    }
};
printer print;

std::size_t intlen(auto num)
{
    std::size_t digits = 0;
    while (num)
    {
        num /= 10;
        digits++;
    }
    return digits;
}

auto max_digits()
{
    return std::max(intlen(rows.size()), std::size_t(2));
}

constexpr inline std::size_t round_down(std::size_t n, std::size_t a)
{
    return (n & ~(a - 1));
}

constexpr inline std::size_t round_up(std::size_t n, std::size_t a)
{
    return round_down(n + a - 1, a);
}

std::pair<char, int> readone()
{
    char c = 0;
    return { c, read(STDOUT_FILENO, &c, 1) };
}

std::size_t x2r(std::string_view rline, std::size_t x)
{
    auto mdigits = max_digits();
    std::size_t ret = 0;
    for (std::size_t i = 0; i < (x - (mdigits + 1)); i++)
    {
        if (rline.at(i) == '\t')
            ret += (tab_size - 1) - (ret % tab_size);
        ret++;
    }
    return ret + mdigits + 1;
}

std::string getrline(std::string rline)
{
    std::size_t pos = 0;
    while ((pos = rline.find('\t', pos)) != rline.npos)
    {
        auto numspaces = round_up((pos == 0 || (pos % tab_size) == 0) ? pos + 1 : pos, tab_size) - pos;
        rline.replace(pos, 1, numspaces, ' ');
        pos += numspaces;
    }
    return rline;
}

void insert(row_t &row, std::size_t pos, char c)
{
    row.second = getrline(row.first.insert(pos, 1, c));
    modified = true;
}

void append(row_t &row, std::string_view str)
{
    row.second = getrline(row.first.append(str));
    modified = true;
}

void append(char c)
{
    auto mdigits = max_digits();
    if (c == '\n')
    {
        if (cursor.x == mdigits + 1)
            rows.emplace(rows.begin() + (cursor.y - 1), "", "");
        else if (!(cursor.y == rows.size() && cursor.x == rows[cursor.y - 1].first.length() + (mdigits + 1)))
        {
            auto &row = rows[cursor.y - 1];
            auto it = rows.emplace(rows.begin() + cursor.y, std::string_view(row.first).substr(cursor.x - (mdigits + 1)), "");
            it->second = getrline(it->first);
            row.second = getrline(row.first.erase(cursor.x - (mdigits + 1)));
        }
        cursor.y++;
        cursor.x = mdigits + 1;
    }
    else
    {
        insert(rows[cursor.y - 1], cursor.x - (mdigits + 1), c);
        cursor.x++;
    }
}

void deleter(std::size_t idx)
{
    if (idx >= rows.size())
        return;
    rows.erase(rows.begin() + idx);
}

void deletec()
{
    auto mdigits = max_digits();
    if (cursor.y - 1 == rows.size() || (cursor.x == (mdigits + 1) && cursor.y == 1))
        return;

    if (cursor.x > (mdigits + 1))
    {
        auto &row = rows[cursor.y - 1];
        row.second = getrline(row.first.erase(--cursor.x - (mdigits + 1), 1));
    }
    else
    {
        auto &row = rows[cursor.y - 2];
        cursor.x = row.first.length() + mdigits + 1;
        append(row, rows[cursor.y - 1].first.c_str());
        deleter(--cursor.y);
    }
    modified = true;
}

std::stringstream mem2buffer()
{
    std::stringstream ret;
    for (const auto &row : rows)
        ret << row.first << '\n';
    return ret;
}

bool save()
{
    if (filename.empty())
        return false;

    std::ofstream file(filename, std::ios::trunc);
    if (file.fail())
        return false;

    file << mem2buffer().rdbuf();
    if (file.fail())
        return false;

    return !(modified = false);
}

enum class operation
{
    up, down, right, left,
    pg_up, pg_down,
    home, end,
    backspace, del,
};
void handle_ctrl(operation op)
{
    auto mdigits = max_digits();
    {
        auto update_row = [&]() -> std::optional<std::string_view>
        {
            if (cursor.y - 1 < rows.size())
                return rows[cursor.y - 1].first.c_str();
            return std::nullopt;
        };
        auto row = update_row();

        auto is_last_empty = [&] {
            return cursor.y == rows.size() && row->empty();
        };

        switch (op)
        {
            case operation::up:
                if (cursor.y != 1)
                    cursor.y--;
                break;
            case operation::down:
                if (row.has_value() && !is_last_empty())
                    cursor.y++;
                break;
            case operation::right:
                if (row.has_value())
                {
                    if (cursor.x < row.value().length() + mdigits + 1)
                        cursor.x++;
                    else if (cursor.x == row.value().length() + mdigits + 1 && !is_last_empty())
                    {
                        cursor.y++;
                        cursor.x = mdigits + 1;
                    }
                }
                break;
            case operation::left:
                if (cursor.x > mdigits + 1)
                    cursor.x--;
                else if (cursor.y != 1)
                {
                    cursor.y--;
                    cursor.x = rows[cursor.y - 1].first.length() + mdigits + 1;
                }
                break;
            case operation::pg_up:
                cursor.y = row_offset + 1;

                for (std::size_t i = 0; i < (height - 1); i++)
                {
                    if (cursor.y != 1)
                        cursor.y--;
                    else
                        break;
                }
                break;
            case operation::pg_down:
                cursor.y = std::min(row_offset + height - 1, rows.size());

                for (std::size_t i = 0; i < (height - 1); i++)
                {
                    if ((row = update_row()).has_value() && !is_last_empty())
                        cursor.y++;
                    else
                        break;
                }
                break;
            case operation::home:
                cursor.x = max_digits() + 1;
                break;
            case operation::end:
                if (row.has_value())
                    cursor.x = row.value().length() + mdigits + 1;
                break;
            case operation::backspace:
                deletec();
                break;
            case operation::del:
                if (cursor.y != rows.size() || cursor.x != mdigits + 1 + row->length())
                {
                    handle_ctrl(operation::right);
                    deletec();
                }
                break;
        }
    }
    {
        std::optional<std::string_view> row(std::nullopt);
        if (cursor.y - 1 < rows.size())
            row = rows[cursor.y - 1].first.c_str();
        if (auto len = row.value_or("").length(); cursor.x > len + mdigits + 1)
            cursor.x = len + mdigits + 1;
    }
}

void scroll()
{
    auto mdigits = max_digits();
    cursor.r = mdigits + 1;

    if (cursor.y <= rows.size())
        cursor.r = x2r(rows[cursor.y - 1].first, cursor.x);

    if (cursor.y <= row_offset)
        row_offset = cursor.y - 1;
    if (cursor.y >= row_offset + (height - 1))
        row_offset = cursor.y - (height - 1);

    if (cursor.r <= col_offset + mdigits)
        col_offset = cursor.r - mdigits - 1;
    if (cursor.r >= col_offset + (width - mdigits))
        col_offset = cursor.r - (width - mdigits);
}

void drawscreen()
{
    print(fmt::format(ui_colours, "{: ^{}}", filename.empty() ? "Text Editor" : (fs::path(filename).filename().string() + (modified ? " *" : "")).c_str(), width));

    auto top = std::min(height - 1, rows.size());
    auto mdigits = max_digits();

    if (cursor.y == rows.size() + 1)
        top++;

    std::size_t i = 0;
    for (; i < top; i++)
    {
        auto row = i + row_offset;
        if (i == top - 1 && rows.size() <= row)
        {
            if (row && rows[row - 1].first.empty())
                break;
            rows.emplace_back("", "");
            modified = true;
        }

        auto line = std::string_view(rows[row].second).substr(std::min(col_offset, rows[row].second.length()), (width - (mdigits + 1)));
        print(fmt::format("{: >{}} {}\r\n", fmt::styled(row + 1, ui_colours), mdigits, line));

    }

    for (; i < height - 1; i++)
        print(fmt::format("{:{}}\r\n", fmt::styled(" ", ui_colours), mdigits));

    print(fmt::format(ui_colours, "{: <{}}", status_message, width));
}

static bool save_as = false;
void refresh()
{
    scroll();
    print("\033[?25l\033[2J\033[H");
    drawscreen();
    print(save_as ? "\033[?25h" : fmt::format("\033[{};{}H\033[?25h", (cursor.y - row_offset) + 1, (cursor.r - col_offset) + 1)).flush();
}

void resize(int = 0)
{
    std::signal(SIGWINCH, SIG_IGN);

    winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    width = w.ws_col;
    height = w.ws_row - 1;

    if (cursor.x == 0)
        cursor.x = cursor.r = max_digits() + 1;

    refresh();

    std::signal(SIGWINCH, resize);
}

void process_key()
{
    static std::size_t quit_times = 1;

    static std::string rdbuf = "";

    auto [c, ret] = readone();

    if (quit_times == 0 && c != CTRL('q'))
    {
        quit_times = 1;
        status_message = status_help;
    }

    if (c == '\033')
    {
        char ncs[3] { 0, 0, 0 };
        for (std::size_t i = 0; i < 2; i++)
        {
            int nret;
            std::tie(ncs[i], nret) = readone();
            if (nret != 1)
                goto jump;
        }
        if (ncs[0] == '[')
        {
            if (ncs[1] >= '0' && ncs[1] <= '9')
            {
                int nret;
                if (std::tie(ncs[2], nret) = readone(); nret != 1)
                    goto jump;

                if (save_as == true)
                    goto exit2refresh;

                if (ncs[2] == '~')
                {
                    switch (ncs[1])
                    {
                        case '3':
                            handle_ctrl(operation::del);
                            goto exit2refresh;
                        case '5':
                            handle_ctrl(operation::pg_up);
                            goto exit2refresh;
                        case '6':
                            handle_ctrl(operation::pg_down);
                            goto exit2refresh;
                        case '1':
                        case '7':
                            handle_ctrl(operation::home);
                            goto exit2refresh;
                        case '4':
                        case '8':
                            handle_ctrl(operation::end);
                            goto exit2refresh;
                    }
                }
            }
            else
            {
                if (save_as == true)
                    goto exit2refresh;

                switch (ncs[1])
                {
                    case 'A':
                        handle_ctrl(operation::up);
                        goto exit2refresh;
                    case 'B':
                        handle_ctrl(operation::down);
                        goto exit2refresh;
                    case 'C':
                        handle_ctrl(operation::right);
                        goto exit2refresh;
                    case 'D':
                        handle_ctrl(operation::left);
                        goto exit2refresh;
                    case 'H':
                        handle_ctrl(operation::home);
                        goto exit2refresh;
                    case 'F':
                        handle_ctrl(operation::end);
                        goto exit2refresh;
                }
            }
        }
    }

    jump:
    if (save_as == true)
    {
        if (c == CTRL('q'))
        {
            rdbuf.clear();
            save_as = false;
            status_message = status_help;
        }
        else
        {
            if ((c == '\r' || c == '\n')  && rdbuf.empty() == false)
            {
                save_as = false;
                filename = rdbuf;
                rdbuf.clear();
                status_message = status_help;

                save();
            }
            if ((c == CTRL('h') || c == 127) && rdbuf.empty() == false)
            {
                status_message.erase(status_message.length() - 1);
                rdbuf.erase(rdbuf.length() - 1);
            }
            if (!std::iscntrl(c) && c)
            {
                status_message += c;
                rdbuf += c;
            }
        }
        goto exit2refresh;
    }

    switch (c)
    {
        case CTRL('q'):
            if (modified == true && quit_times-- > 0)
            {
                status_message = "Plase press Ctrl-Q one more time to quit without saving.";
                break;
            }
            print("\033[2J\033[H").flush();
            exit(EXIT_SUCCESS);
            break;
        case CTRL('s'):
            if (filename.empty())
            {
                status_message = "Save as: ";
                save_as = true;
            }
            else save();
            break;
        case CTRL('h'):
        case 127:
            handle_ctrl(operation::backspace);
            break;
        case CTRL('A'):
        case CTRL('B'):
        case CTRL('C'):
        case CTRL('D'):
        case CTRL('l'):
        case '\033':
            break;
        case '\r':
        case '\n':
            append('\n');
            break;
        default:
            append(c);
            break;
    }

    exit2refresh:
    refresh();
}


void rawmode()
{
    tcgetattr(STDIN_FILENO, &og_tios);

    termios tios = og_tios;
    tios.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    tios.c_oflag &= ~(OPOST);
    tios.c_cflag |= (CS8);
    tios.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    tios.c_cc[VMIN] = 1;
    tios.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &tios);
}

auto main(int argc, char *argv[]) -> int
{
    rawmode();
    std::atexit([] { tcsetattr(STDIN_FILENO, TCSAFLUSH, &og_tios); });

    if (argc >= 2 && fs::exists(argv[1]))
    {
        filename = argv[1];

        std::ifstream file(filename);
        std::string line;

        if (file.fail() == false)
        {
            while (std::getline(file, line))
                rows.emplace_back(line, getrline(line));
        }
    }
    else rows.emplace_back();

    resize();
    while (true)
        process_key();

    return 0;
}