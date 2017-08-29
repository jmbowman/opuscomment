#include <ogg/ogg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include "opuscomment.h"
#include "global.h"

static size_t seeked_len;
static char *outtmp;
static bool remove_tmp;
static uint32_t opus_sno;
static FILE *fpout;

void move_file(void) {
	if (fclose(fpout) == EOF) {
		fileerror(O.out ? O.out : outtmp);
	}
	if (O.out) {
	}
	else {
		int fd = fileno(fpopus);
		struct stat st;
		fstat(fd, &st);
		if (!rename(outtmp, O.in)) {
			// rename(2)は移動先が存在していてもunlink(2)の必要はない
			remove_tmp = false;
			if (!chmod(O.in, st.st_mode)) {
				return;
			}
		}
		oserror();
	}
}

static uint8_t *gbuf;
static size_t const gbuflen = 65307; // oggページの最大長
static void prepare_gbuf(void) {
	if (gbuf) return;
	gbuf = malloc(gbuflen);
}

static void put_left(void) {
	size_t l;
	prepare_gbuf();
	
	clearerr(fpopus);
	if (fseek(fpopus, seeked_len, SEEK_SET)) {
		oserror();
	}
	
	while (l = fread(gbuf, 1, gbuflen, fpopus)) {
		size_t ret = fwrite(gbuf, 1, l, fpout);
		if (ret != l) {
			oserror();
		}
	}
	if (ferror(fpopus)) {
		oserror();
	}
	free(gbuf);
	move_file();
	exit(0);
}

static void write_page(ogg_page *og) {
	size_t ret;
	ret = fwrite(og->header, 1, og->header_len, fpout);
	if (ret != og->header_len) {
		oserror();
	}
	ret = fwrite(og->body, 1, og->body_len, fpout);
	if (ret != og->body_len) {
		oserror();
	}
}

static int opus_idx_diff;
static void store_tags(size_t lastpagelen, struct rettag_st *rst, struct edit_st *est) {
	prepare_gbuf();
	
	FILE *fptag = rst->tag;
	// タグ個数書き込み
	fseek(fptag, rst->tagbegin, SEEK_SET);
	*(uint32_t*)gbuf = oi32(rst->num + est->num);
	fwrite(gbuf, 4, 1, fptag);
	fseek(fptag, 0, SEEK_END);
	
	// 編集入力書き込み
	rewind(est->str); rewind(est->len);
	while (fread(gbuf, 4, 1, est->len)) {
		size_t reclen = *(uint32_t*)gbuf;
		*(uint32_t*)gbuf = oi32(reclen);
		fwrite(gbuf, 4, 1, fptag);
		while (reclen) {
			size_t readlen = fill_buffer(gbuf, reclen, gbuflen, est->str);
			fwrite(gbuf, 1, readlen, fptag);
			reclen -= readlen;
		}
	}
	fclose(est->str); fclose(est->len);
	
	if (rst->padding) {
		// パディングがある時は先頭をfptagの後ろに移す(Vorbis対策)
		rewind(rst->padding);
		fread(gbuf, 1, 1, rst->padding);
		fwrite(gbuf, 1, 1, fptag);
	}
	long commentlen = ftell(fptag);
	if (commentlen > TAG_LENGTH_LIMIT__OUTPUT) {
		exceed_output_limit();
	}
	
	rewind(fptag); 
	ogg_page og;
	og.header_len = 282;
	og.header = gbuf;
	og.body_len = 255 * 255;
	og.body = gbuf + 282;
	memcpy(og.header,
		"\x4f\x67\x67\x53"
		"\0"
		"\0"
		"\0\0\0\0\0\0\0\0", 14);
// 		"S.NO"
// 		"SEQ "
// 		"CRC ", 26);
	*(uint32_t*)&og.header[14] = oi32(opus_sno);
	memset(og.header + 26, 0xff, 256);
	
	uint32_t idx = 1;
	while (commentlen >= 255 * 255) {
		og.header[5] = idx != 1;
		fread(og.body, 1, 255 * 255, fptag);
		*(uint32_t*)&og.header[18] = oi32(idx);
		ogg_page_checksum_set(&og);
		write_page(&og);
		commentlen -= 255 * 255;
		idx++;
	}
	
	og.header[5] = idx != 1;
	*(uint32_t*)&og.header[18] = oi32(idx++);
	og.header[26] = commentlen / 255 + 1;
	og.header[26 + og.header[26]] = commentlen % 255;
	fread(og.body, 1, commentlen, fptag);
	fclose(fptag);
	og.header_len = 27 + og.header[26];
	og.body_len = commentlen;
	if (og.header[26] != 255 && rst->padding) {
		// vorbis commentの最後のページにパディングを付け足す処理
		// vorbisはタグとパディングの間でlacing valueを分けないとおかしくなるっぽい
		size_t len;
		size_t laceleft = (255 - og.header[26]) * 255;
		len = fread(og.body + og.body_len, 1, laceleft, rst->padding);
		if (laceleft == len) {
			og.header_len = 282;
			og.header[26] = 255;
			og.body_len += len;
		}
		else {
			fclose(rst->padding);
			rst->padding = NULL;
			int seg = len / 255 + 1;
			int lastseg = len % 255;
			og.header_len += seg;
			og.header[26] += seg;
			og.header[26 + og.header[26]] = lastseg;
			og.body_len += len;
		}
	}
	ogg_page_checksum_set(&og);
	write_page(&og);
	
	if (rst->padding) {
		memset(og.header + 26, 0xff, 256);
		og.header[5] = 1;
		size_t readlen;
		while ((readlen = fread(og.body, 1, 255 * 255, rst->padding))) {
			*(uint32_t*)&og.header[18] = oi32(idx++);
			if (readlen != 255 * 255) break;
			ogg_page_checksum_set(&og);
			write_page(&og);
		}
		fclose(rst->padding);
		
		og.header[26] = readlen / 255 + 1;
		og.header[26 + og.header[26]] = readlen % 255;
		og.header_len = 27 + og.header[26];
		og.body_len = readlen;
		ogg_page_checksum_set(&og);
		write_page(&og);
	}
	
	if (idx < opus_idx) {
		// 出力するタグ部分のページ番号が入力の音声開始部分のページ番号に満たない場合、
		// 空のページを生成して開始ページ番号を合わせる
		og.header[5] = 1;
		og.header[26] = 0;
		og.header_len = 27;
		og.body_len = 0;
		while (idx < opus_idx) {
			*(uint32_t*)&og.header[18] = oi32(idx++);
			ogg_page_checksum_set(&og);
			write_page(&og);
		}
		seeked_len -= lastpagelen;
		put_left();
		/* NOTREACHED */
	}
	else {
		if (idx == opus_idx) {
			seeked_len -= lastpagelen;
			put_left();
			/* NOTREACHED */
		}
	}
	opus_idx_diff = idx - opus_idx;
}

static void cleanup(void) {
	if (remove_tmp) {
		unlink(outtmp);
	}
}

void open_output_file(void) {
	if (O.out) {
		if (strcmp(O.out, "-") == 0) {
			fpout = stdout;
		}
		else {
			fpout = fopen(O.out, "w");
			if (!fpout) {
				fileerror(O.out);
			}
		}
		remove_tmp = false;
	}
	else {
		char const *tmpl = "opuscomment.XXXXXX";
		outtmp = calloc(strlen(O.in) + strlen(tmpl) + 1, 1);
		char *p = strrchr(O.in, '/');
		if (p) {
			p++;
			strncpy(outtmp, O.in, p - O.in);
			strcpy(outtmp + (p - O.in), tmpl);
		}
		else {
			strcpy(outtmp, tmpl);
		}
		int fd = mkstemp(outtmp);
		if (fd == -1) {
			oserror();
		}
		remove_tmp = true;
		atexit(cleanup);
		fpout = fdopen(fd, "w+");
	}
}

static void exit_without_sigpipe(void) {
	if (!error_on_thread) {
		return;
	}
	signal(SIGPIPE, SIG_IGN);
}

void discontinuous_page(int page) {
	opuserror(err_opus_discontinuous, page, opus_idx);
}

static bool leave_zero, non_opus_appeared;
static bool parse_header(ogg_page *og) {
	static int osidx = 0;
	if (!ogg_page_bos(og) || ogg_page_eos(og)) {
		if (ogg_page_pageno(og) == 0) {
			opuserror(err_opus_bad_stream);
		}
		
		if (osidx) {
			opuserror(err_opus_no_target, O.target_idx);
		}
		else {
			opuserror(err_opus_non_opus);
		}
	}
	if (ogg_page_pageno(og) != 0) {
		// BOSがあるのにページが0でない
		opuserror(err_opus_bad_stream);
	}
	if (ogg_page_granulepos(og) != 0) {
		// ヘッダのgranuleposは必ず0
		opuserror(err_opus_bad_stream);
	}
	if (og->header_len < codec->headmagic_len || memcmp(og->body, codec->headmagic, codec->headmagic_len) != 0) {
		non_opus_appeared = true;
		return false;
	}
	osidx++;
	if (O.target_idx && O.target_idx != osidx) {
		non_opus_appeared = true; // ゲイン上書き処理で使う
		return false;
	}
	opus_sno = ogg_page_serialno(og);
	codec->parse(og);
	if (O.edit != EDIT_LIST) {
		write_page(og);
	}
	opus_idx++;
	opst = OPUS_HEADER_BORDER;
	return true;
}

static int retriever_fd;
static void copy_tag_packet(ogg_page *og) {
	static unsigned int total = 0;
	total += og->body_len;
	if (total > TAG_LENGTH_LIMIT__INPUT) {
		opuserror(err_opus_long_tag, TAG_LENGTH_LIMIT__INPUT >> 20);
	}
	write(retriever_fd, og->body, og->body_len);
}

static bool test_non_opus(ogg_page *og) {
	if (ogg_page_serialno(og) == opus_sno) return true;
	non_opus_appeared = true;
	
	int pno = ogg_page_pageno(og);
	if (pno == 0) {
		if (leave_zero) {
			opuserror(err_opus_bad_stream);
		}
		if (!ogg_page_bos(og) || ogg_page_eos(og) || ogg_page_granulepos(og) != 0) {
			opuserror(err_opus_bad_stream);
		}
	}
	else if (pno == 1) {
		// 複数論理ストリームの先頭は全て0で始まるため、何かが1ページ目を始めたら今後0ページ目は来ないはずである。
		leave_zero = true;
	}
	else {
		if (!leave_zero) {
			// 他で1ページ目が始まってないのに2ページ目以上が来た場合
			opuserror(err_opus_bad_stream);
		}
	}
	
	if (pno && ogg_page_bos(og)) {
		// Opusが続いているストリーム途中でBOSが来るはずがない
		opuserror(err_opus_bad_stream);
	}
	return false;
}

void *retrieve_tags(void*);
void *parse_tags(void*);

static pthread_t retriever_thread, parser_thread;
static bool parse_header_border(ogg_page *og) {
	if (!test_non_opus(og)) return false;
	
	if (ogg_page_pageno(og) != 1) {
		discontinuous_page(ogg_page_pageno(og));
	}
	leave_zero = true;
	if (ogg_page_bos(og) || ogg_page_eos(og)) {
		opuserror(err_opus_bad_stream);
	}
	if (ogg_page_continued(og)) {
		opuserror(err_opus_border);
	}
	if (/*O.gain_fix && */O.edit == EDIT_NONE) {
		// 出力ゲイン編集のみの場合
		if (O.out || non_opus_appeared) {
			// 出力指定が別にあるかストリームが多重化されていたら残りをコピー
			seeked_len -= og->header_len + og->body_len;
			put_left();
		}
		else {
			// 上書きするなら最初のページのみを直接書き換えようとする
			size_t headpagelen = ftell(fpout);
			rewind(fpout);
			uint8_t *b = malloc(headpagelen);
			
			seeked_len -= og->header_len + og->body_len + headpagelen;
			fpopus = freopen(NULL, "r+", fpopus);
			if (!fpopus) {
				oserror();
			}
			if (fseek(fpopus, seeked_len, SEEK_SET)) {
				oserror();
			}
			size_t ret = fread(b, 1, headpagelen, fpout);
			if (ret != headpagelen) {
				oserror();
			}
			ret = fwrite(b, 1, headpagelen, fpopus);
			if (ret != headpagelen) {
				oserror();
			}
			exit(0);
		}
		/* NOTREACHED */
	}
	
	// スレッド間通信で使っているパイプがエラー後のexit内での始末にSIGPIPEを発するのでその対策
	atexit(exit_without_sigpipe);
	error_on_thread = true;
	
	if (O.edit != EDIT_LIST) {
		// 編集入力タグパースを別スレッド化 parse_tags.c へ
		pthread_create(&parser_thread, NULL, parse_tags, NULL);
	}
	
	// OpusTagsパケットパースを別スレッド化 retrieve_tags.c へ
	int pfd[2];
	pipe(pfd);
	retriever_fd = pfd[1];
	FILE *retriever = fdopen(pfd[0], "r");
	pthread_create(&retriever_thread, NULL, retrieve_tags, retriever);
	
	// 本スレッドはOpusTagsのパケットを構築する
	opst = OPUS_COMMENT;
	copy_tag_packet(og);
	opus_idx++;
	return true;
}

static bool parse_page_sound(ogg_page *og);
static bool parse_comment_term(ogg_page *og);

static bool parse_comment(ogg_page *og) {
	if (!test_non_opus(og)) return false;
	if (!ogg_page_continued(og)) {
		return parse_comment_term(og);
	}
	
	if (ogg_page_bos(og) || ogg_page_eos(og)) {
		opuserror(err_opus_bad_stream);
	}
	if (ogg_page_pageno(og) != opus_idx) {
		discontinuous_page(ogg_page_pageno(og));
	}
	copy_tag_packet(og);
	opus_idx++;
	return true;
}

static bool parse_comment_term(ogg_page *og) {
	// タグパケットのパース処理のスレッドを合流
	struct rettag_st *rst;
	struct edit_st *est;
	close(retriever_fd);
	pthread_join(retriever_thread, (void**)&rst);
	if (O.edit != EDIT_LIST) {
		// 編集入力タグパースのスレッドを合流
		pthread_join(parser_thread, (void **)&est);
	}
	error_on_thread = false;
	
	if (O.edit == EDIT_APPEND && !rst->del && !est->num && !O.out && !O.gain_fix && !rst->upcase) {
		// タグ追記モードで出力が上書き且つ
		// タグ入力、ゲイン調整、タグ削除、大文字化適用が全て無い場合はすぐ終了する
		exit(0);
	}
//	来ない
// 	else if (O.edit == EDIT_LIST) {
// 		exit(0);
// 	}
	else {
		store_tags(og->header_len + og->body_len, rst, est);
		free(rst);
		free(est);
	}
	opst = OPUS_SOUND;
	return parse_page_sound(og);
}

static bool parse_page_sound(ogg_page *og) {
	if (ogg_page_serialno(og) != opus_sno) {
		return false;
	}
	*(uint32_t*)&og->header[18] = oi32(ogg_page_pageno(og) + opus_idx_diff);
	ogg_page_checksum_set(og);
	write_page(og);
	if (ogg_page_eos(og)) {
		put_left();
		/* NOTREACHED */
	}
	return true;
}

static void parse_page(ogg_page *og) {
	bool isopus;
	switch (opst) {
	case OPUS_HEADER:
		isopus = parse_header(og);
		break;
		
	case OPUS_HEADER_BORDER:
		isopus = parse_header_border(og);
		break;
		
	case OPUS_COMMENT:
		isopus = parse_comment(og);
		break;
		
	case OPUS_SOUND:
		isopus = parse_page_sound(og);
		break;
	}
	if (!isopus) {
		write_page(og);
	}
}

void read_page(ogg_sync_state *oy) {
	int seeklen;
	ogg_page og;
	while ((seeklen = ogg_sync_pageseek(oy, &og)) != 0) {
		if (seeklen > 0) {
			seeked_len += seeklen;
			parse_page(&og);
		}
		else seeked_len += -seeklen;
	}
}
