#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "raylib.h"
#include <ghostty/vt.h>

// Embed the font file directly into the binary at compile time using C23
// #embed so we don't need to locate it at runtime.
static const unsigned char font_jetbrains_mono[] = {
    #embed "fonts/JetBrainsMono-Regular.ttf"
};

// ---------------------------------------------------------------------------
// PTY helpers
// ---------------------------------------------------------------------------

// Spawn /bin/sh in a new pseudo-terminal.
//
// Creates a pty pair via forkpty(), sets the initial window size, execs the
// shell in the child, and puts the master fd into non-blocking mode so we
// can poll it each frame without stalling the render loop.
//
// Returns the master fd on success (>= 0) and stores the child pid in
// *child_out.  Returns -1 on failure.
static int pty_spawn(pid_t *child_out, uint16_t cols, uint16_t rows)
{
    int pty_fd;
    struct winsize ws = { .ws_row = rows, .ws_col = cols };

    // forkpty() combines openpty + fork + login_tty into one call.
    // In the child it sets up the slave side as stdin/stdout/stderr.
    pid_t child = forkpty(&pty_fd, NULL, NULL, &ws);
    if (child < 0) {
        perror("forkpty");
        return -1;
    }
    if (child == 0) {
        // Child process — replace ourselves with the shell.
        // TERM tells programs what escape sequences we understand.
        setenv("TERM", "xterm-256color", 1);
        execl("/bin/sh", "sh", NULL);
        _exit(127); // execl only returns on error
    }

    // Parent — make the master fd non-blocking so read() returns EAGAIN
    // instead of blocking when there's no data, letting us poll each frame.
    int flags = fcntl(pty_fd, F_GETFL);
    fcntl(pty_fd, F_SETFL, flags | O_NONBLOCK);

    *child_out = child;
    return pty_fd;
}

// Drain all available output from the pty master and feed it into the
// ghostty terminal.  The terminal's VT parser will process any escape
// sequences and update its internal screen/cursor/style state.
//
// Because the fd is non-blocking, read() returns -1 with EAGAIN once
// the kernel buffer is empty, at which point we stop.
static void pty_read(int pty_fd, GhosttyTerminal terminal)
{
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = read(pty_fd, buf, sizeof(buf));
        if (n > 0)
            ghostty_terminal_vt_write(terminal, buf, (size_t)n);
        else
            break;
    }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

// Encode a single Unicode codepoint into UTF-8 and write it to the pty.
// This is used for printable characters that raylib delivers as codepoints
// via GetCharPressed().
static void pty_write_codepoint(int pty_fd, int cp)
{
    char utf8[4];
    int len;

    // Standard UTF-8 encoding: 1–4 bytes depending on codepoint range.
    if (cp < 0x80) {
        utf8[0] = (char)cp;
        len = 1;
    } else if (cp < 0x800) {
        utf8[0] = (char)(0xC0 | (cp >> 6));
        utf8[1] = (char)(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        utf8[0] = (char)(0xE0 | (cp >> 12));
        utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        utf8[0] = (char)(0xF0 | (cp >> 18));
        utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (cp & 0x3F));
        len = 4;
    }
    write(pty_fd, utf8, len);
}

// Poll raylib for keyboard events and write the corresponding byte
// sequences to the pty.
//
// Three categories of input:
//   1. Printable characters — delivered by GetCharPressed() as Unicode
//      codepoints; we UTF-8 encode them and send them straight through.
//   2. Special / function keys — mapped to the VT escape sequences that
//      programs on the other side of the pty expect (e.g. arrow keys
//      send ESC [ A/B/C/D).
//   3. Ctrl+letter combos — produce the traditional control characters
//      (ctrl+a = 0x01, ctrl+c = 0x03, ctrl+d = 0x04, etc.).
static void handle_input(int pty_fd)
{
    // --- 1. Printable characters ---
    int ch;
    while ((ch = GetCharPressed()) != 0)
        pty_write_codepoint(pty_fd, ch);

    // --- 2. Special keys → VT escape sequences ---
    // Each entry maps a raylib key constant to the byte sequence a
    // traditional terminal would emit for that key.
    static const struct { int rl_key; const char *seq; } key_map[] = {
        { KEY_ENTER,     "\r" },       // carriage return
        { KEY_BACKSPACE, "\177" },     // DEL (ASCII 127)
        { KEY_TAB,       "\t" },       // horizontal tab
        { KEY_ESCAPE,    "\033" },     // ESC
        { KEY_UP,        "\033[A" },   // cursor up
        { KEY_DOWN,      "\033[B" },   // cursor down
        { KEY_RIGHT,     "\033[C" },   // cursor right
        { KEY_LEFT,      "\033[D" },   // cursor left
        { KEY_HOME,      "\033[H" },   // home
        { KEY_END,       "\033[F" },   // end
        { KEY_DELETE,    "\033[3~" },  // delete forward
        { KEY_PAGE_UP,   "\033[5~" },  // page up
        { KEY_PAGE_DOWN, "\033[6~" },  // page down
    };
    for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
        if (IsKeyPressed(key_map[i].rl_key) || IsKeyPressedRepeat(key_map[i].rl_key))
            write(pty_fd, key_map[i].seq, strlen(key_map[i].seq));
    }

    // --- 3. Ctrl+letter combos ---
    // Control characters are 1–26 (ctrl+a through ctrl+z).  Raylib's
    // KEY_A..KEY_Z constants are contiguous, so we can loop over them.
    for (int k = KEY_A; k <= KEY_Z; k++) {
        if ((IsKeyPressed(k) || IsKeyPressedRepeat(k)) && IsKeyDown(KEY_LEFT_CONTROL)) {
            char ctrl = (char)(k - KEY_A + 1);
            write(pty_fd, &ctrl, 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Render the current terminal screen contents into the raylib window.
//
// Uses the ghostty formatter API to extract the visible screen as plain
// text (with trailing whitespace trimmed), then draws each line using
// the provided monospace font.
//
// This is intentionally simple — it ignores colors/styles and just draws
// green-on-black text.  A future renderer should use the render state API
// or iterate cells individually to support colors, bold, underline, etc.
static void render_terminal(GhosttyTerminal terminal, Font font)
{
    // Configure the formatter: plain text output with trailing blanks
    // trimmed so we don't draw unnecessary spaces.
    GhosttyFormatterTerminalOptions fmt = GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
    fmt.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    fmt.trim = true;

    GhosttyFormatter formatter;
    if (ghostty_formatter_terminal_new(NULL, &formatter, terminal, fmt) != GHOSTTY_SUCCESS)
        return;

    // Format the entire screen into a heap-allocated buffer.  The
    // formatter returns a single string with newline-separated rows.
    uint8_t *buf = NULL;
    size_t len = 0;
    if (ghostty_formatter_format_alloc(formatter, NULL, &buf, &len) == GHOSTTY_SUCCESS) {
        int y = 10; // vertical offset from top of window
        const char *line_start = (const char *)buf;

        // Walk the buffer and split on newlines, drawing each line.
        for (size_t i = 0; i <= len; i++) {
            if (i == len || buf[i] == '\n') {
                int line_len = (int)((const char *)&buf[i] - line_start);
                char line[256] = {0};
                if (line_len > 0 && line_len < 255) {
                    memcpy(line, line_start, line_len);
                    line[line_len] = '\0';
                    DrawTextEx(font, line, (Vector2){10, y}, 16, 0, GREEN);
                }
                y += 18; // line height in pixels
                line_start = (const char *)&buf[i + 1];
            }
        }
        free(buf);
    }
    ghostty_formatter_free(formatter);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(void)
{
    // Create a ghostty virtual terminal with an 80×24 grid and 1000 lines
    // of scrollback.  This holds all the parsed screen state (cells, cursor,
    // styles, modes) but knows nothing about the pty or the window.
    uint16_t term_cols = 80, term_rows = 24;
    GhosttyTerminal terminal;
    GhosttyTerminalOptions opts = { .cols = term_cols, .rows = term_rows, .max_scrollback = 1000 };
    GhosttyResult err = ghostty_terminal_new(NULL, &terminal, opts);
    assert(err == GHOSTTY_SUCCESS);

    // Spawn a child shell connected to a pseudo-terminal.  The master fd
    // is what we read/write; the child's stdin/stdout/stderr are wired to
    // the slave side.
    pid_t child;
    int pty_fd = pty_spawn(&child, term_cols, term_rows);
    if (pty_fd < 0)
        return 1;

    // Initialize window
    InitWindow(800, 600, "ghostling");
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetTargetFPS(60);

    // Load the embedded monospace font for terminal text rendering.
    Font mono_font = LoadFontFromMemory(".ttf", font_jetbrains_mono, (int)sizeof(font_jetbrains_mono), 16, NULL, 0);
    SetTextureFilter(mono_font.texture, TEXTURE_FILTER_BILINEAR);

    // Track window size so we only recalculate the grid on actual changes.
    int prev_width = GetScreenWidth();
    int prev_height = GetScreenHeight();

    // Each frame: handle resize → read pty → process input → render.
    while (!WindowShouldClose()) {
        // Recalculate grid dimensions when the window is resized.
        // We update both the ghostty terminal (so it reflows text) and the
        // pty's winsize (so the child shell knows about the new size and
        // can send SIGWINCH to its foreground process group).
        if (IsWindowResized()) {
            int w = GetScreenWidth();
            int h = GetScreenHeight();
            if (w != prev_width || h != prev_height) {
                int cols = w / 10;  // approximate cell width in pixels
                int rows = h / 18; // line height matches render_terminal()
                if (cols < 1) cols = 1;
                if (rows < 1) rows = 1;
                term_cols = (uint16_t)cols;
                term_rows = (uint16_t)rows;
                ghostty_terminal_resize(terminal, term_cols, term_rows);
                struct winsize new_ws = { .ws_row = term_rows, .ws_col = term_cols };
                ioctl(pty_fd, TIOCSWINSZ, &new_ws);
                prev_width = w;
                prev_height = h;
            }
        }

        // Drain any pending output from the shell and update terminal state.
        pty_read(pty_fd, terminal);

        // Forward keyboard input to the shell.
        handle_input(pty_fd);

        // Draw the current terminal screen.
        BeginDrawing();
        ClearBackground(BLACK);
        render_terminal(terminal, mono_font);
        EndDrawing();
    }

    // Cleanup
    UnloadFont(mono_font);
    CloseWindow();
    close(pty_fd);
    kill(child, SIGHUP);    // signal the child shell to exit
    waitpid(child, NULL, 0); // reap the child to avoid a zombie
    ghostty_terminal_free(terminal);
    return 0;
}
