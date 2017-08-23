#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <locale.h>
#include <langinfo.h>
#include <iconv.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include "opuscomment.h"
#include "global.h"

static bool from_file;
static void readerror(void) {
	if (from_file) {
		fileerror(O.tag_filename);
	}
	else {
		oserror();
	}
}

static void tagerror(char *e) {
	errorprefix();
	fprintf(stderr, catgets(catd, 1, 6, "editing input #%zu: "), tagnum_edit + 1);
	fputs(e, stderr);
	fputc('\n', stderr);
	exit(1);
}

static void err_nosep(void) {
	tagerror(catgets(catd, 5, 1, "no field separator"));
}
static void err_name(void) {
	tagerror(catgets(catd, 5, 2, "invalid field name"));
}
static void err_empty(void) {
	tagerror(catgets(catd, 5, 3, "empty field name"));
}
static void err_bin(void) {
	tagerror(catgets(catd, 5, 4, "binary file"));
}
static void err_esc(void) {
	tagerror(catgets(catd, 5, 5, "invalid escape sequence"));
}
static void err_utf8(void) {
	tagerror(catgets(catd, 5, 6, "invalid UTF-8 sequence"));
}

static void toutf8(int fdu8) {
	size_t const buflen = 512;
	char ubuf[buflen];
	char lbuf[buflen];
	
	iconv_t cd = iconv_new("UTF-8", O.tag_raw ? "UTF-8" : nl_langinfo(CODESET));
	size_t readlen, remain, total;
	remain = 0; total = 0;
	while ((readlen = fread(&lbuf[remain], 1, buflen - remain, stdin)) != 0) {
		total += readlen;
		if (total > TAG_LENGTH_LIMIT__INPUT) {
			mainerror(catgets(catd, 2, 11, "too long editing input. Haven't you executed odd command?"));
		}
		if (strnlen(&lbuf[remain], readlen) != readlen) {
			err_bin();
		}
		size_t llen = readlen + remain;
		remain = llen;
		for (;;) {
			size_t ulen = buflen;
			char *lp = lbuf, *up = ubuf;
			size_t iconvret = iconv(cd, &lp, &remain, &up, &ulen);
			int ie = errno;
			if (iconvret == (size_t)-1) {
				switch (ie) {
				case EILSEQ:
					oserror();
					break;
				case EINVAL:
				case E2BIG:
					break;
				}
				memcpy(lbuf, lp, remain);
			}
			write(fdu8, ubuf, up - ubuf);
			if (iconvret != (size_t)-1 || ie == EINVAL) break;
		}
	}
	if (fclose(stdin) == EOF) {
		readerror();
	}
	if (remain) {
		errno = EILSEQ;
		readerror();
	}
	char *up = ubuf; readlen = buflen;
	remain = iconv(cd, NULL, NULL, &up, &readlen);
	if (remain == (size_t)-1) oserror();
	iconv_close(cd);
	write(fdu8, ubuf, up - ubuf);
	close(fdu8);
}

static FILE *record;
static int recordfd;
static void blank_record() {
	rewind(record);
	ftruncate(recordfd, 0);
}

static size_t editlen;
static void write_record(void) {
	uint32_t end = ftell(record);
	rewind(record);
	uint8_t buf[512];
	size_t n;
	
	// 全部空白かどうか
	bool blank = true;
	while ((n = fread(buf, 1, 512, record))) {
		for (size_t i = 0; i < n; i++) {
			if (!strchr("\x9\xa\xd\x20", buf[i])) {
				blank = false;
				goto END_BLANK_TEST;
			}
		}
	}
END_BLANK_TEST:
	if (blank) {
		blank_record();
		return;
	}
	// 空白じゃなかったら編集に採用
	editlen += 4 + end;
	if (editlen > TAG_LENGTH_LIMIT__OUTPUT) {
		mainerror(catgets(catd, 2, 10, "tag length exceeded the limit of storing (up to %u MiB)"), TAG_LENGTH_LIMIT__OUTPUT >> 20);
	}
	end = oi32(end);
	fwrite(&end, 4, 1, fpedit);
	rewind(record);
	
	// 最初が = か
	fread(buf, 1, 1, record);
	if (*buf == 0x3d) err_empty();
	rewind(record);
	
	// 項目名がPCS印字文字範囲内か
	while ((n = fread(buf, 1, 512, record))) {
		size_t i;
		for (i = 0; i < n && buf[i] != 0x3d; i++) {
			if (!(buf[i] >= 0x20 && buf[i] <= 0x7e)) {
				err_name();
			}
			if (buf[i] >= 0x61 && buf[i] <= 0x7a) {
				buf[i] -= 32;
			}
		}
		fwrite(buf, 1, n, fpedit);
		if (i < n) break;
	}
	while ((n = fread(buf, 1, 512, record))) {
		fwrite(buf, 1, n, fpedit);
	}
	blank_record();
	tagnum_edit++;
}

static void r_line(uint8_t *line, size_t n) {
	static bool afterlf = false;
	
	if (!line) {
		write_record();
		afterlf = false;
		return;
	}
	
	bool lf = line[n - 1] == 0xa;
	if (afterlf) {
		if (*line == 0x9) {
			*line = 0xa;
		}
		else {
			write_record();
		}
		afterlf = false;
	}
	fwrite(line, 1, n - lf, record);
	if (lf) {
		afterlf = true;
	}
	else {
		afterlf = false;
	}
}

static int esctab(int c) {
	switch (c) {
	case 0x30: // '0'
		c = '\0';
		break;
	case 0x5c: // '\'
		c = 0x5c;
		break;
	case 0x6e: // 'n'
		c = 0x0a;
		break;
	case 0x72: // 'r'
		c = 0x0d;
		break;
	default:
		err_esc();
		break;
	}
	return c;
}
static void r_line_e(uint8_t *line, size_t n) {
	static bool tagcont = false;
	static bool escape = false;
	
	if (!line) {
		if (escape) err_esc();
		if (tagcont) write_record();
		return;
	}
	
	if (escape) {
		*line = esctab(*line);
		fwrite(line, 1, 1, record);
		escape = false;
		line++;
		n--;
	}
	
	uint8_t *p = line;
	bool lf = n && line[n - 1] == 0x0a;
	uint8_t *endp = line + n - lf;
	for (; p < endp && *p != 0x5c; p++) {}
	fwrite(line, 1, p - line, record);
	if (p < endp) {
		// '\\'があった時
		uint8_t *escbegin = p;
		uint8_t *advp = p;
		for (; advp < endp; p++, advp++) {
			char c;
			if ((c = *advp) == 0x5c) {
				if (++advp == endp) {
					escape = true;
					tagcont = true;
					break;
				}
				c = esctab(*advp);
			}
			*p = c;
		}
		fwrite(escbegin, 1, p - escbegin, record);
	}
	if (lf) {
		write_record();
	}
	tagcont = !lf;
}

void prepare_record(void) {
	if (record) {
		return;
	}
	record = tmpfile();
	recordfd = fileno(record);
}

void *split(void *fp_) {
	FILE *fp = fp_;
	prepare_record();
	fpedit = fpedit ? fpedit : tmpfile();
	void (*line)(uint8_t *, size_t) = O.tag_escape ? r_line_e : r_line;
	
	uint8_t tagbuf[512];
	size_t tagbuflen, readlen;
	tagbuflen = 512;
	while ((readlen = fread(tagbuf, 1, tagbuflen, fp)) != 0) {
		uint8_t *p1, *p2;
		if (memchr(tagbuf, 0, readlen) != NULL) {
			err_bin();
		}
		p1 = tagbuf;
		while ((p2 = memchr(p1, 0x0a, readlen - (p1 - tagbuf))) != NULL) {
			p2++;
			line(p1, p2 - p1);
			p1 = p2;
		}
		size_t left = readlen - (p1 - tagbuf);
		if (left) line(p1, left);
	}
	fclose(fp);
	error_on_thread = false;
	line(NULL, 0);
	fclose(record);
}

void parse_tags(void) {
	from_file = O.tag_filename && strcmp(O.tag_filename, "-") != 0;
	if (from_file) {
		FILE *tmp = freopen(O.tag_filename, "r", stdin);
		if (!tmp) {
			fileerror(O.tag_filename);
		}
	}
	
	// UTF-8化された文字列をチャンク化する処理をスレッド化(化が多い)
	int pfd[2];
	pipe(pfd);
	FILE *fpu8 = fdopen(pfd[0], "r");
	error_on_thread = true;
	pthread_t split_thread;
	pthread_create(&split_thread, NULL, split, fpu8);
	
	// 本スレッドはstdinをUTF-8化する
	toutf8(pfd[1]);
	pthread_join(split_thread, NULL);
}

void add_tag_from_opt(char const *arg) {
	static iconv_t cd = (iconv_t)-1;
	
	if (!arg) {
		if (cd == (iconv_t)-1) return;
		iconv_close(cd);
		cd = (iconv_t)-1;
		return;
	}
	
	char *ls = (char*)arg;
	size_t l = strlen(ls);
	
	if (cd == (iconv_t)-1) {
		cd = iconv_new("UTF-8", nl_langinfo(CODESET));
	}
	fpedit = fpedit ? fpedit : tmpfile();
	prepare_record();
	char u8buf[512];
	size_t u8left;
	char *u8;
	while (l) {
		u8left = 512;
		u8 = u8buf;
		size_t ret = iconv(cd, &ls, &l, &u8, &u8left);
		
		if (ret == (size_t)-1) {
			// 引数処理なのでEINVAL時のバッファ持ち越しは考慮しない
			if (errno != E2BIG) {
				oserror();
			}
		}
		r_line(u8buf, u8 - u8buf);
	}
	u8 = u8buf;
	u8left = 512;
	if (iconv(cd, NULL, NULL, &u8, &u8left) == (size_t)-1) {
		oserror();
	}
	u8[0] = 0xa;
	r_line(u8buf, u8 - u8buf + 1);
	r_line(NULL, 0);
}
