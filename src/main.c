#define _POSIX_C_SOURCE 2
#include <errno.h>
#include <ftw.h>
#include <iconv.h>
#include <id3tag.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

const char *ENCODINGS[] = {
    "SHIFT-JIS",
    "EUC-JP",
    "SJIS-OPEN",
    "UTF8",
    NULL
};

void fix(const char *file);

static size_t id3_latin1len(const id3_latin1_t *text) {
    size_t len = 0;
    while (*text++) len++;
    return len;
}

static size_t id3_stringlen(const id3_ucs4_t *text) {
    size_t len = 0;
    while (*text++) len++;
    return len;
}

int walkfn(const char *fpath, const struct stat *sb, int typeflag) {
    if (typeflag == FTW_F) {
        size_t len = strlen(fpath);
        if (len > 4 && strncmp(fpath + (len - 4), ".mp3", 4) == 0) {
            pid_t child = fork();
            if (child > 0) {
                int status = 0;
                do {
                    pid_t terminated = waitpid(child, &status, WUNTRACED | WCONTINUED);
                    if (terminated == -1) {
                        // something went really really wrong
                        kill(child, 15);
                        exit(1);
                    }
                } while (!WIFEXITED(status) && !WIFSIGNALED(status));
                if (WIFSIGNALED(status)) {
                    fprintf(stderr, "Child died with signal %d\n", (int)(WTERMSIG(status)));
                    exit(1);
                }
            } else if (child == 0) {
                fix(fpath);
                exit(0);
            } else { //child < 0
                fprintf(stderr, "Could not fork: %s\n", strerror(errno));
                exit(1);
            }
        }
    }
    return 0;
}

int check_latin1(const id3_latin1_t *text, size_t len);
int check_string(const id3_ucs4_t *text, size_t len);
int guess_enc_latin1(const id3_latin1_t *text, size_t len, id3_ucs4_t **otext, size_t *olen, int enchint);
union id3_field *fix_latin1(union id3_field *fld);
union id3_field *fix_latin1full(union id3_field *fld);
union id3_field *fix_latin1list(union id3_field *fld);
union id3_field *fix_string(union id3_field *fld);
union id3_field *fix_stringfull(union id3_field *fld);
union id3_field *fix_stringlist(union id3_field *fld);

void fix(const char *file) {
    struct id3_file *id3f = id3_file_open(file, ID3_FILE_MODE_READWRITE);
    if (id3f == NULL) {
        return;
    }
    struct id3_tag *tag = id3_file_tag(id3f);
    fprintf(stderr, "%s tags version %u\n", file, tag->version);
    int replacements = 0;
    for (int i = 0; i < tag->nframes; i++) {
        struct id3_frame *frm = tag->frames[i];
        if (frm->id[0] != 'T' || (frm->encoded && frm->encoded_length)) {
            continue;
        }
        fprintf(stderr, "  frame %.4s; nfields:%3u\n", frm->id, frm->nfields);
        if (frm->nfields < 2) {
            fprintf(stderr, "%s", "ERROR: expected at least 2 fields\n");
            continue;
        }
        if (frm->fields[0].type != ID3_FIELD_TYPE_TEXTENCODING) {
            fprintf(stderr, "%s", "ERROR: expected first field encoding\n");
        }
        int text_enc = frm->fields[0].number.value;
        if (text_enc != ID3_FIELD_TEXTENCODING_ISO_8859_1) {
            fprintf(stderr, "Text encoding not latin-1 (%d), leaving alone\n", text_enc);
            continue;
        }
        int all_latin1 = 1;
        for (int j = 1; j < frm->nfields && all_latin1; j++) {
            union id3_field *fld = frm->fields + j;
            switch (fld->type) {
            case ID3_FIELD_TYPE_LATIN1:
                all_latin1 = all_latin1 && check_latin1(fld->latin1.ptr, id3_latin1len(fld->latin1.ptr));
                break;
            case ID3_FIELD_TYPE_LATIN1FULL:
                all_latin1 = all_latin1 && check_latin1(fld->latin1.ptr, id3_latin1len(fld->latin1.ptr));
                break;
            case ID3_FIELD_TYPE_LATIN1LIST:
                for (size_t k = 0; k < fld->latin1list.nstrings && !all_latin1; k++) {
                    all_latin1 = all_latin1 && check_latin1(fld->latin1list.strings[k],
                            id3_latin1len(fld->latin1list.strings[k]));
                }
                break;
            case ID3_FIELD_TYPE_STRING:
                all_latin1 = all_latin1 && check_string(fld->string.ptr, id3_stringlen(fld->string.ptr));
                break;
            case ID3_FIELD_TYPE_STRINGFULL:
                all_latin1 = all_latin1 && check_string(fld->string.ptr, id3_stringlen(fld->string.ptr));
                break;
            case ID3_FIELD_TYPE_STRINGLIST:
                for (size_t k = 0; k < fld->stringlist.nstrings && all_latin1; k++) {
                    all_latin1 = all_latin1 && check_string(fld->stringlist.strings[k],
                            id3_stringlen(fld->stringlist.strings[k]));
                }
                break;
            default:
                break;
            }
        }
        if (all_latin1) {
            fprintf(stderr, "Actually latin1\n");
            continue;
        }
        replacements = 1;
        frm->fields[0].number.value = ID3_FIELD_TEXTENCODING_UTF_8;
        for (int j = 1; j < frm->nfields; j++) {
            union id3_field *fld = frm->fields + j;
            fprintf(stderr, "    field: %d\n", (int)fld->type);
            switch (fld->type) {
            case ID3_FIELD_TYPE_LATIN1:
                fix_latin1(fld);
                break;
            case ID3_FIELD_TYPE_LATIN1FULL:
                fix_latin1full(fld);
                break;
            case ID3_FIELD_TYPE_LATIN1LIST:
                fix_latin1list(fld);
                break;
            case ID3_FIELD_TYPE_STRING:
                fix_string(fld);
                break;
            case ID3_FIELD_TYPE_STRINGFULL:
                fix_stringfull(fld);
                break;
            case ID3_FIELD_TYPE_STRINGLIST:
                fix_stringlist(fld);
                break;
            default:
                break;
            }
        }
    }
    if (replacements) {
        id3_file_update(id3f);
    }
    id3_file_close(id3f);
}

int check_latin1(const id3_latin1_t *text, size_t len) {
    for (size_t i = 0; ; i++) {
        if (i >= len) {
            return 1;
        } else if (text[i] < 0x20 || (text[i] > 0x7E && text[i] < 0xA1)) {
            return 0;
        }
    }
}

int check_string(const id3_ucs4_t *text, size_t len) {
    for (size_t i = 0; ; i++) {
        if (i >= len) {
            return 1;
        } else if (text[i] < 0x20 || (text[i] > 0x7E && text[i] < 0xA0)) {
            return 0;
        }
    }
}

int guess_enc_latin1(const id3_latin1_t *text, size_t len, id3_ucs4_t **otext, size_t *olen, int enchint) {
    size_t inbytesleft;
    size_t outbytesleft;
    size_t tmpbufsz = len * 4;
    char *tmpbuf = malloc(tmpbufsz);
    char *textin = malloc(len + 1);
    memcpy(textin, text, len);
    textin[len] = 0;
    int result = 0;
    if (!tmpbuf || !textin) {
        puts("malloc fail!");
        exit(1);
    }
    for (int ienc = enchint ? enchint - 1 : 0; !result && ENCODINGS[ienc];) {
        iconv_t cd = iconv_open("UCS-4BE", ENCODINGS[ienc]);
        iconv(cd, 0, 0, 0, 0);
        memset(tmpbuf, 0, tmpbufsz);
        inbytesleft = len;
        outbytesleft = tmpbufsz;
        char *textin2 = textin;
        char *tmpbuf2 = tmpbuf;
        size_t converted = iconv(cd, &textin2, &inbytesleft, &tmpbuf2, &outbytesleft);
        if (converted != -1) {
            *olen = (tmpbufsz - outbytesleft) / 4;
            fprintf(stderr, "Guessed encoding: %s\n", ENCODINGS[ienc]);
            result = ienc + 1;
        } else if (errno == E2BIG) {
            free(tmpbuf);
            tmpbufsz *= 2;
            tmpbuf = malloc(tmpbufsz);
            if (!tmpbuf) {
                fprintf(stderr, "%s", "malloc fail!");
                exit(1);
            }
        } else {
            ienc++;
        }
        iconv_close(cd);
    }
    if (result) {
        *otext = (id3_ucs4_t *)malloc((*olen + 1) * sizeof(id3_ucs4_t));
        for (size_t i = 0; i < *olen; i++) {
            (*otext)[i] = (id3_ucs4_t) (
                ((id3_ucs4_t) tmpbuf[4 * i + 0] & 0xFF) << 24 |
                ((id3_ucs4_t) tmpbuf[4 * i + 1] & 0xFF) << 16 |
                ((id3_ucs4_t) tmpbuf[4 * i + 2] & 0xFF) << 8 |
                ((id3_ucs4_t) tmpbuf[4 * i + 3] & 0xFF));
        }
        (*otext)[*olen] = 0;
    } else {
        fprintf(stderr, "%s\n", "Failed to guess encoding");
    }
    free(textin);
    free(tmpbuf);
    return result;
}

union id3_field *fix_latin1(union id3_field *fld) {
    id3_ucs4_t *text;
    size_t textlen;
    union id3_field fix;
    if (guess_enc_latin1(fld->latin1.ptr, id3_latin1len(fld->latin1.ptr), &text, &textlen, 0)) {
        fix.type = ID3_FIELD_TYPE_STRING;
        fix.string.ptr = text;
        memcpy(fld, &fix, sizeof(union id3_field));
    }
    return fld;
}
union id3_field *fix_latin1full(union id3_field *fld) {
    id3_ucs4_t *text;
    size_t textlen;
    union id3_field fix;
    if (guess_enc_latin1(fld->latin1.ptr, id3_latin1len(fld->latin1.ptr), &text, &textlen, 0)) {
        fix.type = ID3_FIELD_TYPE_STRINGFULL;
        fix.string.ptr = text;
        memcpy(fld, &fix, sizeof(union id3_field));
    }
    return fld;
}
union id3_field *fix_latin1list(union id3_field *fld) {
    id3_ucs4_t *text;
    size_t textlen;
    union id3_field fix;
    fix.type = ID3_FIELD_TYPE_STRINGLIST;
    fix.stringlist.nstrings = fld->latin1list.nstrings;
    fix.stringlist.strings = (id3_ucs4_t **)malloc(fix.stringlist.nstrings * sizeof(id3_ucs4_t *));
    for (size_t i = 0; i < fld->latin1list.nstrings; i++) {
        if (!guess_enc_latin1(fld->latin1list.strings[i], id3_latin1len(fld->latin1list.strings[i]),
                &text, &textlen, 0)) {
            return fld;
        }
        fix.stringlist.strings[i] = text;
    }
    memcpy(fld, &fix, sizeof(union id3_field));
    return fld;
}

static void ucs4_to_latin1(const id3_ucs4_t *text, size_t textsz,
        id3_latin1_t **latintext, size_t *latintextsz) {
    *latintext = (id3_latin1_t *)malloc((textsz + 1) * sizeof(id3_latin1_t));
    for (size_t i = 0; i < textsz; i++) {
        (*latintext)[i] = (id3_latin1_t)(text[i] & 0xFF);
    }
    (*latintext)[textsz] = 0;
    *latintextsz = textsz;
}

union id3_field *fix_string(union id3_field *fld) {
    id3_latin1_t *latintext;
    size_t latintextsz;
    id3_ucs4_t *text;
    size_t textlen;
    union id3_field fix;
    ucs4_to_latin1(fld->string.ptr, id3_stringlen(fld->string.ptr), &latintext, &latintextsz);
    if (guess_enc_latin1(latintext, latintextsz, &text, &textlen, 0)) {
        fix.type = ID3_FIELD_TYPE_STRING;
        fix.string.ptr = text;
        memcpy(fld, &fix, sizeof(union id3_field));
    }
    free(latintext);
    return fld;
}
union id3_field *fix_stringfull(union id3_field *fld) {
    id3_latin1_t *latintext;
    size_t latintextsz;
    id3_ucs4_t *text;
    size_t textlen;
    union id3_field fix;
    ucs4_to_latin1(fld->string.ptr, id3_stringlen(fld->string.ptr), &latintext, &latintextsz);
    if (guess_enc_latin1(latintext, latintextsz, &text, &textlen, 0)) {
        fix.type = ID3_FIELD_TYPE_STRINGFULL;
        fix.string.ptr = text;
        memcpy(fld, &fix, sizeof(union id3_field));
    }
    free(latintext);
    return fld;
}
union id3_field *fix_stringlist(union id3_field *fld) {
    id3_ucs4_t *text;
    size_t textlen;
    union id3_field fix;
    fix.type = ID3_FIELD_TYPE_STRINGLIST;
    fix.stringlist.nstrings = fld->latin1list.nstrings;
    fix.stringlist.strings = (id3_ucs4_t **)malloc(fix.stringlist.nstrings * sizeof(id3_ucs4_t *));
    for (size_t i = 0; i < fld->stringlist.nstrings; i++) {
        id3_latin1_t *latintext;
        size_t latintextsz;
        ucs4_to_latin1(fld->stringlist.strings[i], id3_stringlen(fld->stringlist.strings[i]),
                &latintext, &latintextsz);
        if (!guess_enc_latin1(latintext, latintextsz, &text, &textlen, 0)) {
            return fld;
        }
        free(latintext);
        fix.stringlist.strings[i] = text;
    }
    memcpy(fld, &fix, sizeof(union id3_field));
    return fld;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("Usage:\n", stdout);
        fputs(argv[0], stdout);
        fputs(" dir\n", stdout);
        fputs("  scan and fix id3tags to utf-8 encoding in dir\n", stdout);
        return 1;
    }
    long openmax = sysconf(_SC_OPEN_MAX);
    ftw(argv[1], walkfn, openmax - 8);
    return 0;
}

