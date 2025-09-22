#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** Section 2: Defines ***/

#define WRITEAM_VERSION "0.0.1"
#define WRITEAM_TAB_STOP 8
#define WRITEAM_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

enum editorHighlight {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_MLCOMMENT,
  HL_KEYWORD1,
  HL_KEYWORD2,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** Section 3: Data ***/

struct editorSyntax {
  char *fileType;
  char **fileMatch;
  char **keywords;
  char * singlelineCommentStart;
  char *multilineCommentStart;
  char *multilineCommentEnd;
  int flags;
};

typedef struct erow {
  int idx;
  int size;
  int rsize;
  char *chars;
  char *render;
  unsigned char *hl;
  int hlOpenComment;
} erow;

struct editorConfig {
  int cursorX, cursorY;
  int rx;
  int rowoff;
  int coloff;
  int screenRows;
  int screenCols;
  int numRows;
  erow *row;
  int dirty;
  char *fileName;
  char statusmsg[80];
  time_t statusmsg_time;
  struct editorSyntax *syntax;
  struct termios orig_termios;
};

struct editorConfig E;

/*** Section 4: FileTypes ***/

char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};

char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else", "struct", "union", "typedef", "static", "enum", "class", "case", "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|", "void|", NULL
};

struct editorSyntax HLDB[] = {
  {
    "c",
    C_HL_extensions,
    C_HL_keywords,
    "//", "/*", "*/",
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
  },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** Section 5: Prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** Section 6: Terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
    die("tcgetattr");
  }

  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {     
    die("tcsetattr");
  }
}

int editorReadKey() {
  int nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) {
      return '\x1b';
    }

    if (read(STDIN_FILENO, &seq[1], 1) != 1) {
      return '\x1b';
    }

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) {
          return '\x1b';
        }

        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1':
              return HOME_KEY;
            case '3':
              return DEL_KEY;
            case '4':
              return END_KEY;
            case '5':
              return PAGE_UP;
            case '6':
              return PAGE_DOWN;
            case '7':
              return HOME_KEY;
            case '8':
              return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A':
            return ARROW_UP;
          case 'B':
            return ARROW_DOWN;
          case 'C':
            return ARROW_RIGHT;
          case 'D':
            return ARROW_LEFT;
          case 'H':
            return HOME_KEY;
          case 'F':
            return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }

    if (buf[i] == 'R') {
      break;
    }

    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }

  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }

    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;

    return 0;
  }
}

/*** Section 7: Syntax Highlighting ***/

int isSeparator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
  row->hl = realloc(row->hl, row->rsize);
  memset(row->hl, HL_NORMAL, row->rsize);

  if (E.syntax == NULL) {
    return;
  }

  char **keywords = E.syntax->keywords;

  char *scs = E.syntax->singlelineCommentStart;
  char *mcs = E.syntax->multilineCommentStart;
  char *mce = E.syntax->multilineCommentEnd;

  int scsLength = scs ? strlen(scs) : 0;
  int mcsLength = mcs ? strlen(mcs) : 0;
  int mceLength = mce ? strlen(mce) : 0;

  int previousSep = 1;
  int inString = 0;
  int inComment = (row->idx > 0 && E.row[row->idx - 1].hlOpenComment);

  int i = 0;
  while (i < row->rsize) {
    char c = row->render[i];
    unsigned char previousHl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

    if (scsLength && !inString && !inComment) {
      if (!strncmp(&row->render[i], scs, scsLength)) {
        memset(&row->hl[i], HL_COMMENT, row->rsize - i);
        break;
      }
    }

    if (mcsLength && mceLength && !inString) {
      if (inComment) {
        row->hl[i] = HL_MLCOMMENT;

        if (!strncmp(&row->render[i], mce, mceLength)) {
          memset(&row->hl[i], HL_MLCOMMENT, mceLength);
          i += mceLength;
          inComment = 0;
          previousSep = 1;
          continue;
        } else {
          i++;
          continue;
        }
      } else if (!strncmp(&row->render[i], mcs, mcsLength)) {
        memset(&row->hl[i], HL_MLCOMMENT, mcsLength);
        i += mcsLength;
        inComment = 1;
        continue;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (inString) {
        row->hl[i] = HL_STRING;

        if (c == '\\' && i + 1 < row->rsize) {
          row->hl[i + 1] = HL_STRING;
          i += 2;
          continue;
        }

        if (c == inString) {
          inString = 0;
        }

        i++;
        previousSep = 1;
        continue;
      } else {
        if (c == '"' || c == '\'') {
          inString = c;
          row->hl[i] = HL_STRING;
          i++;
          continue;
        }
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if ((isdigit(c) && (previousSep || previousHl == HL_NORMAL)) || (c == '.' && previousHl == HL_NUMBER)) {
        row->hl[i] = HL_NUMBER;
        i++;
        previousSep = 0;
        continue;
      }
  }

  if (previousSep) {
    int j;
    for (j = 0; keywords[j]; j++) {
      int kLength = strlen(keywords[j]);
      int kw2 = keywords[j][kLength - 1] == '|';

      if (kw2) {
        kLength--;
      }

      if (!strncmp(&row->render[i], keywords[j], kLength) && isSeparator(row->render[i + kLength])) {
        memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, kLength);
        i+= kLength;
        break;
      }
    }

    if (keywords[j] != NULL) {
      previousSep = 0;
      continue;
    }
  }

    previousSep = isSeparator(c);
    i++;
  }

  int changed = (row->hlOpenComment != inComment);
  row->hlOpenComment = inComment;

  if (changed && row->idx + 1 < E.numRows) {
    editorUpdateSyntax(&E.row[row->idx + 1]);
  }
}

int editorSyntaxToColor(int hl) {
  switch (hl) {
    case HL_COMMENT:
      return 36;
    case HL_MLCOMMENT:
      return 36;
    case HL_KEYWORD1:
      return 33;
    case HL_KEYWORD2:
      return 32;
    case HL_STRING:
      return 35;
    case HL_NUMBER:
      return 31;
    case HL_MATCH:
      return 34;
    default:
      return 37;
  }
}

void editorSelectSyntaxHighlight() {
  E.syntax = NULL;

  if (E.fileName == NULL) {
    return;
  }

  char *ext = strrchr(E.fileName, '.');

  for (unsigned int i = 0; i < HLDB_ENTRIES; i++) {
    struct editorSyntax *s = &HLDB[i];
    unsigned int j = 0;

    while (s->fileMatch[j]) {
      int isExt = (s->fileMatch[j][0] == '.');

      if ((isExt && ext && !strcmp(ext, s->fileMatch[j])) || (!isExt && strstr(E.fileName, s->fileMatch[j]))) {
        E.syntax = s;

        for (int fileRow = 0; fileRow < E.numRows; fileRow++) {
          editorUpdateSyntax(&E.row[fileRow]);
        }

        return;
      }

      j++;
    }
  }
}

/*** Section 8: Row Operations ***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;

  for (int i = 0; i < cx; i++) {
    if (row->chars[i] == '\t') {
      rx += (WRITEAM_TAB_STOP - 1) - (rx % WRITEAM_TAB_STOP);
    }

    rx++;
  }

  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;

  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t') {
      cur_rx += (WRITEAM_TAB_STOP - 1) - (cur_rx % WRITEAM_TAB_STOP);
    }

    cur_rx++;

    if (cur_rx > rx) {
      return cx;
    }
  }

  return cx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  
  for (int i = 0; i < row->size; i++) {
    if (row->chars[i] == '\t') {
      tabs++;
    }
  }

  free(row->render);
  row->render = malloc(row->size + tabs * (WRITEAM_TAB_STOP - 1) + 1);

  int idx = 0;

  for (int i = 0; i < row->size; i++) {
    if (row->chars[i] == '\t') {
      row->render[idx++] = ' ';
      while (idx % WRITEAM_TAB_STOP != 0) {
        row->render[idx++] = ' ';
      }
    } else {
      row->render[idx++] = row->chars[i];
    }
  }

  row->render[idx] = '\0';
  row->rsize = idx;

  editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numRows) {
    return;
  }

  E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numRows - at));

  for (int i = at + 1; i <= E.numRows; i++) {
    E.row[i].idx++;
  }

  E.row[at].idx = at;

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  E.row[at].hlOpenComment = 0;
  editorUpdateRow(&E.row[at]);

  E.numRows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numRows) {
    return;
  }

  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numRows - at - 1));

  for (int i = at; i < E.numRows - 1; i++) {
    E.row[i].idx--;
  }

  E.numRows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) {
    at = row->size;
  }

  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);

  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) {
    return;
  }

  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** Section 9: Editor Operations ***/

void editorInsertChar(int c) {
  if (E.cursorY == E.numRows) {
    editorInsertRow(E.numRows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cursorY], E.cursorX
, c);
  E.cursorX++;
}

void editorInsertNewLine() {
  if (E.cursorX
 == 0) {
    editorInsertRow(E.cursorY, "", 0);
  } else {
    erow *row = &E.row[E.cursorY];
    editorInsertRow(E.cursorY + 1, &row->chars[E.cursorX
], row->size - E.cursorX
);
    row = &E.row[E.cursorY];
    row ->size = E.cursorX
;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cursorY++;
  E.cursorX = 0;
}

void editorDelChar() {
  if (E.cursorY == E.numRows) {
    return;
  }

  if (E.cursorX
 == 0 && E.cursorY == 0) {
    return;
  }

  erow *row = &E.row[E.cursorY];

  if (E.cursorX
 > 0) {
    editorRowDelChar(row, E.cursorX
   - 1);
    E.cursorX
--;
  } else {
    E.cursorX
 = E.row[E.cursorY - 1].size;
    editorRowAppendString(&E.row[E.cursorY - 1], row->chars, row->size);
    editorDelRow(E.cursorY);
    E.cursorY--;
  }
}

/*** Section 10: File I/O ***/

char *editorRowsToString(int *buflen) {
  int totalLength = 0;

  for (int i = 0; i < E.numRows; i++) {
    totalLength += E.row[i].size + 1;
  }

  *buflen = totalLength;

  char *buf = malloc(totalLength);
  char *p = buf;

  for (int i = 0; i < E.numRows; i++) {
    memcpy(p, E.row[i].chars, E.row[i].size);
    p += E.row[i].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorOpen(char *fileName) {
  free(E.fileName);
  E.fileName = strdup(fileName);

  editorSelectSyntaxHighlight();

  FILE *fp = fopen(fileName, "r");
  if (!fp) {
    die("fopen");
  }

  char *line = NULL;
  size_t lineCap = 0;
  ssize_t lineLength;

  while ((lineLength = getline(&line, &lineCap, fp)) != -1) {
    while (lineLength > 0 && (line[lineLength - 1] == '\n' || line[lineLength - 1] == '\r')) {
      lineLength--;
    }
    editorInsertRow(E.numRows, line, lineLength);
  }
  
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.fileName == NULL) {
    E.fileName = editorPrompt("Save As: %s (ESC to cancel)", NULL);
    if (E.fileName == NULL) {
      editorSetStatusMessage("Save Aborted");
      return;
    }

    editorSelectSyntaxHighlight();
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.fileName, O_RDWR | O_CREAT, 0644);

  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage("Save Failed! I/O Error: %s", strerror(errno));
}

/*** Section 11: Find ***/

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  static int saved_hl_line;
  static char *saved_hl = NULL;

  if (saved_hl) {
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl);
    saved_hl = NULL;
  }

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1) {
    direction = 1;
  }

  int current = last_match;

  for (int i = 0; i < E.numRows; i++) {
    current += direction;

    if (current == -1) {
      current = E.numRows - 1;
    } else if (current == E.numRows) {
      current = 0;
    }

    erow *row = &E.row[current];
    char *match = strstr(row->render, query);

    if (match) {
      last_match = current;
      E.cursorY = current;
      E.cursorX
   = editorRowRxToCx(row, match - row->render);
      E.rowoff = E.numRows;

      saved_hl_line = current;
      saved_hl = malloc(row->rsize);
      memcpy(saved_hl, row->hl, row->rsize);
      memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
      break;
    }
  }
}

void editorFind() {
  int saved_cx = E.cursorX;
  int saved_cy = E.cursorY;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

  if (query) {
    free(query);;
  } else {
    E.cursorX
 = saved_cx;
    E.cursorY = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

/*** Section 12: Append Buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) {
    return;
  }

  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** Section 13: Output ***/

void editorScroll() {
  E.rx = 0;

  if (E.cursorY < E.numRows) {
    E.rx = editorRowCxToRx(&E.row[E.cursorY], E.cursorX
);
  }

  if (E.cursorY < E.rowoff) {
    E.rowoff = E.cursorY;
  }

  if (E.cursorY >= E.rowoff + E.screenRows) {
    E.rowoff = E.cursorY - E.screenRows + 1;
  }

  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }

  if (E.rx >= E.coloff + E.screenCols) {
    E.coloff = E.rx - E.screenCols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  for (int i = 0; i < E.screenRows; i++) {
    int fileRow = i + E.rowoff;

    if (fileRow >= E.numRows) {
      if (E.numRows == 0 && i == E.screenRows / 3) {
        char welcome[80];
        int welcomeLength = snprintf(welcome, sizeof(welcome), "writeAM Editor -- Version %s", WRITEAM_VERSION);

        if (welcomeLength > E.screenCols) {
          welcomeLength = E.screenCols;
        }

        int padding = (E.screenCols - welcomeLength) / 2;

        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }

        while (padding--) {
          abAppend(ab, " ", 1);
        }

        abAppend(ab, welcome, welcomeLength);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[fileRow].rsize - E.coloff;

      if (len < 0) {
        len = 0;
      }

      if (len > E.screenCols) {
        len = E.screenCols;
      }

      char *c = &E.row[fileRow].render[E.coloff];
      unsigned char *hl = &E.row[fileRow].hl[E.coloff];
      int currentColor = -1;
      for (int i = 0; i < len; i++) {
        if (iscntrl(c[i])) {
          char sym = (c[i] <= 26) ? '@' + c[i] : '?';
          abAppend(ab, "\x1b[7m", 4);
          abAppend(ab, &sym, 1);
          abAppend(ab, "\x1b[m", 3);

          if (currentColor != -1) {
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", currentColor);
            abAppend(ab, buf, clen);
          }
        } else if (hl[i] == HL_NORMAL) {
          if (currentColor != -1) {
            abAppend(ab, "\x1b[39m", 5);
            currentColor = -1;
          }

          abAppend(ab, &c[i], 1);
        } else {
          int color = editorSyntaxToColor(hl[i]);

          if (color != currentColor) {
            currentColor = color;
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            abAppend(ab, buf, clen);
          }

          abAppend(ab, &c[i], 1);
        }
      }
      abAppend(ab, "\x1b[39m", 5);
    }
  

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.fileName ? E.fileName : "[No Name]", E.numRows, E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", E.syntax ? E.syntax->fileType : "No Filetype", E.cursorY + 1, E.numRows);

  if (len > E.screenCols) {
    len = E.screenCols;
  }
  
  abAppend(ab, status, len);

  while (len < E.screenCols) {
    if (E.screenCols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);

  if (msglen > E.screenCols) {
    msglen = E.screenCols;
  }

  if (msglen && time(NULL) - E.statusmsg_time < 5) {
    abAppend(ab, E.statusmsg, msglen);
  }
}

void editorRefreshScreen() {
  editorScroll();
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursorY - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);

  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** Section 14: Input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) {
        buf[--buflen] = '\0';
      }
    }else if (c == '\x1b') {
      editorSetStatusMessage("");

      if (callback) {
        callback(buf, c);
      }

      free(buf);

      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");

        if (callback) {
          callback(buf, c);
        }

        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback) {
      callback(buf, c);
    }

  }
}

void editorMoveCursor(int key) {
  erow *row = (E.cursorY >= E.numRows) ? NULL : &E.row[E.cursorY];

  switch (key) {
    case ARROW_LEFT:
      if (E.cursorX
     !=0) {
        E.cursorX
    --;
      } else if (E.cursorY > 0){
        E.cursorY--;
        E.cursorX
     = E.row[E.cursorY].size;
      }

      break;
    case ARROW_RIGHT:
      if (row && E.cursorX
     < row->size) {
        E.cursorX
    ++;
      } else if (row && E.cursorX
     == row->size) {
        E.cursorY++;
        E.cursorX
     = 0;
      }
      break;
    case ARROW_UP:
      if (E.cursorY !=0) {
        E.cursorY--;
      }

      break;
    case ARROW_DOWN:
      if (E.cursorY < E.numRows) {
        E.cursorY++;
      }

      break;
  }

  row = (E.cursorY >= E.numRows) ? NULL : &E.row[E.cursorY];
  int rowLength = row ? row->size : 0;

  if (E.cursorX
 > rowLength) {
    E.cursorX
 = rowLength;
  }
}

void editorProcessKeypress() {
  static int quit_times = WRITEAM_QUIT_TIMES;

  int c = editorReadKey();

  switch (c) {
    case '\r':
      editorInsertNewLine();
      break;

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING: File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;
        
    case HOME_KEY:
      E.cursorX
   = 0;
      break;

    case END_KEY:
      if (E.cursorY < E.numRows) {
        E.cursorX
     = E.row[E.cursorY].size;
      }
      break;
    
    case CTRL_KEY('f'):
      editorFind();
      break;
    
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) {
        editorMoveCursor(ARROW_RIGHT);
      }
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cursorY = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cursorY = E.rowoff + E.screenRows - 1;

          if (E.cursorY > E.numRows) {
            E.cursorY = E.numRows;
          }
        }
        
        int times = E.screenRows;
        while (times--) {
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
      }  
      break;

    case ARROW_UP:
      editorMoveCursor(c);
      break;
    case ARROW_DOWN:
      editorMoveCursor(c);
      break;
    case ARROW_LEFT:
      editorMoveCursor(c);
      break;
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;
    
    default:
      editorInsertChar(c);
      break;
  }

  quit_times = WRITEAM_QUIT_TIMES;
}

/*** Section 15: Init ***/

void initEditor() {
  E.cursorX = 0;
  E.cursorY = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numRows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.fileName = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.syntax = NULL;

  if (getWindowSize(&E.screenRows, &E.screenCols) == -1) {
    die("getWindowSize");
  }

  E.screenRows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();

  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-S = Save | Ctrl-Q = Quit | Ctrl-F = Find");
  
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}