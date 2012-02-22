/*
 * Copyright (c) 2011 Conformal Systems LLC <info@conformal.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <libgen.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <regex.h>
#include <errno.h>

#include <assl.h>
#include <clog.h>
#include <exude.h>
#include <xmlsd.h>

#include <ctutil.h>
#include "ct_xml.h"

#include "ct.h"
#include "ct_crypto.h"

struct ctfile_list_file {
	union {
		RB_ENTRY(ctfile_list_file)	nxt;
		SLIST_ENTRY(ctfile_list_file)	lnk;
	}					mlf_entries;
#define mlf_next	mlf_entries.nxt
#define mlf_link	mlf_entries.lnk
	char					mlf_name[CT_CTFILE_MAXLEN];
	off_t					mlf_size;
	time_t					mlf_mtime;
	int					mlf_keep;
};

SLIST_HEAD(ctfile_list, ctfile_list_file);
RB_HEAD(ctfile_list_tree, ctfile_list_file);
RB_PROTOTYPE(ctfile_list_tree, ctfile_list_file, next, ct_cmp_ctfile);

struct ctfile_list			ctfile_list_files =
				     SLIST_HEAD_INITIALIZER(&ctfile_list_files);
FILE				*ctfile_handle = NULL;
int				ctfile_block_no = 0;
int				ctfile_is_open = 0;
int				ctfile_open_inflight = 0;
off_t				ctfile_size, ctfile_offset;

int			strcompare(const void *, const void *);

void	ctfile_list_complete(int, char **, char **, struct ctfile_list_tree *);

void ct_cull_send_shas(struct ct_op *);
void ct_cull_setup(struct ct_op *);
void ct_cull_start_shas(struct ct_op *);
void ct_cull_start_complete(struct ct_op *op);
void ct_cull_send_complete(struct ct_op *op);
void ct_cull_complete(struct ct_op *op);
void ct_cull_collect_ctfiles(struct ct_op *op);
void ct_cull_fetch_all_ctfiles(struct ct_op *op);

struct xmlsd_v_elements ct_xml_cmds[] = {
	{ "ct_md_list", xe_ct_md_list },
	{ "ct_md_open_read", xe_ct_md_open_read },
	{ "ct_md_open_create", xe_ct_md_open_create },
	{ "ct_md_delete", xe_ct_md_delete },
	{ "ct_md_close", xe_ct_md_close },
	{ "ct_cull_setup", xe_ct_cull_setup },
	{ "ct_cull_shas", xe_ct_cull_shas },
	{ "ct_cull_complete", xe_ct_cull_complete },
	{ "ct_cull_setup_reply", xe_ct_cull_setup_reply },
	{ "ct_cull_shas_reply", xe_ct_cull_shas_reply },
	{ "ct_cull_complete_reply", xe_ct_cull_complete_reply },
	{ NULL, NULL }
};

char *
ctfile_cook_name(const char *path)
{
	char	*bname;

	bname = basename((char *)path);
	if (bname == NULL)
		CFATAL("can't basename metadata path");
	if (bname[0] == '/')
		CFATALX("invalid metadata filename");

	return (e_strdup(bname));
}

struct fnode	ctfile_node;
void
ctfile_archive(struct ct_op *op)
{
	const char		*ctfile = op->op_local_fname;
	const char		*rname = op->op_remote_fname;
	struct stat		sb;
	ssize_t			rsz, rlen;
	struct ct_trans		*ct_trans;
	int			error;

	CNDBG(CT_LOG_FILE, "entered for block %d", ctfile_block_no);
	ct_set_file_state(CT_S_RUNNING);
loop:
	ct_trans = ct_trans_alloc();
	if (ct_trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(CT_S_WAITING_TRANS);
		return;
	}

	if (ctfile_is_open == 0) {
		if (ctfile_open_inflight) {
			CNDBG(CT_LOG_FILE, "waiting on remote open");
			ct_trans_free(ct_trans);
			ct_set_file_state(CT_S_WAITING_TRANS);
			return;
		}

		CNDBG(CT_LOG_FILE, "opening ctfile for archive %s", ctfile);
		ctfile_handle = fopen(ctfile, "rb");
		if (ctfile_handle == NULL)
			CFATAL("can't open %s for reading", ctfile);

		ctfile_offset = 0;
		ctfile_block_no = 0;

		error = fstat(fileno(ctfile_handle), &sb);
		if (error) {
			CFATAL("can't stat backup file %s", ctfile);
		} else {
			ctfile_size = sb.st_size;
		}

		if (rname == NULL) {
			rname = ctfile_cook_name(ctfile);
			op->op_remote_fname = (char *)rname;
		}
		ct_xml_file_open(ct_trans, rname, MD_O_WRITE, 0);
		ctfile_open_inflight = 1;
		return;
	}

	/* Are we done here? */
	if (ctfile_size == ctfile_offset) {
		ct_set_file_state(CT_S_FINISHED);
		ct_trans->tr_fl_node = NULL;
		ct_trans->tr_state = TR_S_XML_CLOSE;
		ct_trans->tr_eof = 1;
		ct_trans->tr_trans_id = ct_trans_id++;
		CNDBG(CT_LOG_FILE, "setting eof on trans %" PRIu64,
		    ct_trans->tr_trans_id);
		ct_trans->hdr.c_flags = C_HDR_F_METADATA;
		ct_trans->tr_ctfile_name = rname;
		ct_stats->st_bytes_tot += ctfile_size;
		ct_queue_transfer(ct_trans);
		return;
	}
	/* perform read */
	rsz = ctfile_size - ctfile_offset;

	CNDBG(CT_LOG_FILE, "rsz %ld max %d", (long) rsz, ct_max_block_size);
	if (rsz > ct_max_block_size) {
		rsz = ct_max_block_size;
	}

	ct_trans->tr_dataslot = 0;
	rlen = fread(ct_trans->tr_data[0], sizeof(char), rsz, ctfile_handle);

	CNDBG(CT_LOG_FILE, "read %ld", (long) rlen);

	ct_stats->st_bytes_read += rlen;

	ct_trans->tr_fl_node = &ctfile_node;
	ct_trans->tr_chsize = ct_trans->tr_size[0] = rlen;
	ct_trans->tr_state = TR_S_READ;
	ct_trans->tr_type = TR_T_WRITE_CHUNK;
	ct_trans->tr_trans_id = ct_trans_id++;
	ct_trans->tr_eof = 0;
	ct_trans->hdr.c_flags = C_HDR_F_METADATA;
	ct_trans->hdr.c_ex_status = 2; /* we handle new metadata protocol */
	ct_trans->tr_ctfile_chunkno = ctfile_block_no;
	ct_trans->tr_ctfile_name = rname;

	CNDBG(CT_LOG_FILE, " trans %"PRId64", read size %ld, into %p rlen %ld",
	    ct_trans->tr_trans_id, (long) rsz, ct_trans->tr_data[0],
	    (long) rlen);

	/*
	 * init iv to something that can be recreated, used if hdr->c_flags
	 * has C_HDR_F_METADATA set.
	 */
	bzero(ct_trans->tr_iv, sizeof(ct_trans->tr_iv));
	ct_trans->tr_iv[0] = (ctfile_block_no >>  0) & 0xff;
	ct_trans->tr_iv[1] = (ctfile_block_no >>  8) & 0xff;
	ct_trans->tr_iv[2] = (ctfile_block_no >> 16) & 0xff;
	ct_trans->tr_iv[3] = (ctfile_block_no >> 24) & 0xff;
	ct_trans->tr_iv[4] = (ctfile_block_no >>  0) & 0xff;
	ct_trans->tr_iv[5] = (ctfile_block_no >>  8) & 0xff;
	ct_trans->tr_iv[6] = (ctfile_block_no >> 16) & 0xff;
	ct_trans->tr_iv[7] = (ctfile_block_no >> 24) & 0xff;
	/* XXX - leaves the rest of the iv with 0 */

	ctfile_block_no++;

	CNDBG(CT_LOG_FILE, "sizes rlen %ld offset %ld size %ld", (long) rlen,
	    (long) ctfile_offset, (long) ctfile_size);

	if (rsz != rlen || (rlen + ctfile_offset) == ctfile_size) {
		/* short read, file truncated or EOF */
		CNDBG(CT_LOG_FILE, "DONE");
		error = fstat(fileno(ctfile_handle), &sb);
		if (error) {
			CWARNX("file stat error %s %d %s",
			    ctfile, errno, strerror(errno));
		} else if (sb.st_size != ctfile_size) {
			CWARNX("file truncated during backup %s",
			    ctfile);
			/*
			 * may need to perform special nop processing
			 * to pad archive file to right number of chunks
			 */
		}
		/*
		 * we don't set eof here because the next go round
		 * will hit the state done case above
		 */
		ctfile_offset = ctfile_size;
	} else {
		ctfile_offset += rlen;
	}
	ct_queue_transfer(ct_trans);
	goto loop;
}

void
ct_xml_file_open(struct ct_trans *trans, const char *file, int mode,
    uint32_t chunkno)
{
	struct xmlsd_element_list	 xl;
	struct xmlsd_element		*xe;
	char				 b64[CT_MAX_MD_FILENAME];
	size_t				 sz;

	trans->tr_trans_id = ct_trans_id++;
	trans->tr_state = TR_S_XML_OPEN;

	CNDBG(CT_LOG_XML, "setting up XML");

	if (ct_base64_encode(CT_B64_M_ENCODE, (uint8_t *)file, strlen(file),
	    (uint8_t *)b64, sizeof(b64)))
		CFATALX("cant base64 encode %s", file);

	if (mode == MD_O_WRITE || mode == MD_O_APPEND) {
		xe = xmlsd_create(&xl, "ct_md_open_create");
		xmlsd_set_attr(xe, "version", CT_MD_OPEN_CREATE_VERSION);
	} else {	/* mode == MD_O_READ */
		xe = xmlsd_create(&xl, "ct_md_open_read");
		xmlsd_set_attr(xe, "version", CT_MD_OPEN_READ_VERSION);
	}

	xe = xmlsd_add_element(&xl, xe, "file");
	xmlsd_set_attr(xe, "name", b64);

	if (mode == MD_O_APPEND || chunkno) {
		xmlsd_set_attr_uint32(xe, "chunkno", chunkno);
	}

	if ((trans->tr_data[2] = (uint8_t *)xmlsd_generate(&xl,
	    ct_body_alloc_xml, &sz, 1)) == NULL)
		CFATALX("%s: Could not allocate xml body", __func__);
	xmlsd_unwind(&xl);
	trans->tr_dataslot = 2;
	trans->tr_size[2] = sz;

	CNDBG(CT_LOG_XML, "open trans %"PRIu64, trans->tr_trans_id);
	ct_queue_transfer(trans);

}

int
ct_xml_file_open_polled(struct ct_assl_io_ctx *asslctx,
    const char *file, int mode, uint32_t chunkno)
{
#define ASSL_TIMEOUT 20
	extern uint64_t		 ct_packet_id;
	struct ct_header	 hdr;

	struct xmlsd_element_list xl;
	struct xmlsd_element	*xe;
	char			*body = NULL;
	char			 b64[CT_MAX_MD_FILENAME];
	size_t			 sz;
	int			 rv = 1;

	CNDBG(CT_LOG_XML, "setting up XML");

	if (ct_base64_encode(CT_B64_M_ENCODE, (uint8_t *)file, strlen(file),
	    (uint8_t *)b64, sizeof(b64)))
		CFATALX("cant base64 encode %s", file);

	if (mode == MD_O_WRITE || mode == MD_O_APPEND) {
		xe = xmlsd_create(&xl, "ct_md_open_create");
		xmlsd_set_attr(xe, "version", CT_MD_OPEN_CREATE_VERSION);
	} else {	/* mode == MD_O_READ */
		xe = xmlsd_create(&xl, "ct_md_open_read");
		xmlsd_set_attr(xe, "version", CT_MD_OPEN_READ_VERSION);
	}

	xe = xmlsd_add_element(&xl, xe, "file");
	xmlsd_set_attr(xe, "name", b64);

	if (mode == MD_O_APPEND || chunkno) {
		xmlsd_set_attr_uint32(xe, "chunkno", chunkno);
	}

	body = xmlsd_generate(&xl, malloc, &sz, 1);
	xmlsd_unwind(&xl);

	hdr.c_version = C_HDR_VERSION;
	hdr.c_opcode = C_HDR_O_XML;
	hdr.c_flags = C_HDR_F_METADATA;
	/* use previous packet id so it'll fit with the state machine */
	hdr.c_tag = ct_packet_id - 1;
	hdr.c_size = sz;

	ct_wire_header(&hdr);
	if (ct_assl_io_write_poll(asslctx, &hdr, sizeof hdr, ASSL_TIMEOUT)
	    != sizeof hdr) {
		CWARNX("could not write header");
		goto done;
	}
	if (ct_assl_io_write_poll(asslctx, body, sz,  ASSL_TIMEOUT) != sz) {
		CWARNX("could not write body");
		goto done;
	}
	free(body);

	/* get server reply */
	if (ct_assl_io_read_poll(asslctx, &hdr, sizeof hdr, ASSL_TIMEOUT)
	    != sizeof hdr) {
		CWARNX("invalid header size");
		goto done;
	}
	ct_unwire_header(&hdr);

	if (hdr.c_status == C_HDR_S_OK && hdr.c_opcode == C_HDR_O_XML_REPLY)
		rv = 0;

	/* we know the open was ok or bad, just read the body and dump it */
	body = e_calloc(1, hdr.c_size);
	if (ct_assl_io_read_poll(asslctx, body, hdr.c_size, ASSL_TIMEOUT)
	    != hdr.c_size) {
		rv = 1;
	}
	e_free(&body);

done:
	if (body)
		e_free(&body);
	return (rv);
#undef ASSL_TIMEOUT
}

void
ct_xml_file_close(void)
{
	struct xmlsd_element_list	 xl;
	struct xmlsd_element		*xe;
	struct ct_trans			*trans;
	size_t				 sz;

	trans = ct_trans_alloc();
	if (trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(CT_S_WAITING_TRANS);
		return;
	}

	trans->tr_trans_id = ct_trans_id++;
	trans->tr_state = TR_S_XML_CLOSING;

	CNDBG(CT_LOG_XML, "setting up XML");

	xe = xmlsd_create(&xl, "ct_md_close");
	xmlsd_set_attr(xe, "version", CT_MD_CLOSE_VERSION);

	if ((trans->tr_data[2] = (uint8_t *)xmlsd_generate(&xl,
	    ct_body_alloc_xml, &sz, 1)) == NULL)
		CFATALX("%s: Could not allocate xml body", __func__);
	xmlsd_unwind(&xl);
	trans->tr_dataslot = 2;
	trans->tr_size[2] = sz;

	ct_queue_transfer(trans);
}

void
ctfile_extract(struct ct_op *op)
{
	const char		*ctfile = op->op_local_fname;
	const char		*rname = op->op_remote_fname;
	struct ct_trans		*trans;
	struct ct_header	*hdr;

	ct_set_file_state(CT_S_RUNNING);

	trans = ct_trans_alloc();
	if (trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(CT_S_WAITING_TRANS);
		return;
	}
	if (ctfile_is_open == 0) {
		if (ctfile_open_inflight) {
			CNDBG(CT_LOG_FILE, "waiting on remote open");
			ct_trans_free(trans);
			ct_set_file_state(CT_S_WAITING_TRANS);
			return;
		}

		/* XXX -chmod when done */
		if (ctfile_handle == NULL) { /* may have been opened for us */
			if ((ctfile_handle = fopen(ctfile, "wb")) == NULL)
				CFATALX("unable to open file %s", ctfile);
		}
		ctfile_block_no = 0;

		if (rname == NULL) {
			rname = ctfile_cook_name(ctfile);
			op->op_remote_fname = (char *)rname;
		}
		ct_xml_file_open(trans, rname, MD_O_READ, 0);
		ctfile_open_inflight = 1;
		return;
	}

	trans->tr_fl_node = &ctfile_node;
	trans->tr_state = TR_S_EX_SHA;
	trans->tr_type = TR_T_READ_CHUNK;
	trans->tr_trans_id = ct_trans_id++;
	trans->tr_eof = 0;
	trans->tr_ctfile_chunkno = ctfile_block_no;
	trans->tr_ctfile_name = rname;

	hdr = &trans->hdr;
	hdr->c_ex_status = 2;
	hdr->c_flags |= C_HDR_F_METADATA;

	bzero(trans->tr_sha, sizeof(trans->tr_sha));
	trans->tr_sha[0] = (ctfile_block_no >>  0) & 0xff;
	trans->tr_sha[1] = (ctfile_block_no >>  8) & 0xff;
	trans->tr_sha[2] = (ctfile_block_no >> 16) & 0xff;
	trans->tr_sha[3] = (ctfile_block_no >> 24) & 0xff;
	bzero(trans->tr_iv, sizeof(trans->tr_iv));
	trans->tr_iv[0] = (ctfile_block_no >>  0) & 0xff;
	trans->tr_iv[1] = (ctfile_block_no >>  8) & 0xff;
	trans->tr_iv[2] = (ctfile_block_no >> 16) & 0xff;
	trans->tr_iv[3] = (ctfile_block_no >> 24) & 0xff;
	trans->tr_iv[4] = (ctfile_block_no >>  0) & 0xff;
	trans->tr_iv[5] = (ctfile_block_no >>  8) & 0xff;
	trans->tr_iv[6] = (ctfile_block_no >> 16) & 0xff;
	trans->tr_iv[7] = (ctfile_block_no >> 24) & 0xff;

	ctfile_block_no++; /* next chunk on next pass */

	ct_queue_transfer(trans);
}

void
ct_complete_metadata(struct ct_trans *trans)
{
	ssize_t			wlen;
	int			slot, done = 0;

	switch(trans->tr_state) {
	case TR_S_EX_READ:
	case TR_S_EX_DECRYPTED:
	case TR_S_EX_UNCOMPRESSED:
		if (trans->hdr.c_status == C_HDR_S_OK) {
			slot = trans->tr_dataslot;
			CNDBG(CT_LOG_FILE, "writing packet sz %d",
			    trans->tr_size[slot]);
			wlen = fwrite(trans->tr_data[slot], sizeof(char),
			    trans->tr_size[slot], ctfile_handle);
			if (wlen != trans->tr_size[slot])
				CWARN("unable to write to ctfile");
		} else {
			ct_state->ct_file_state = CT_S_FINISHED;
		}
		break;

	case TR_S_DONE:
		/* More operations to be done? */
		if (ct_op_complete())
			done = 1;

		/* Clean up reconnect name, shared between all trans */
		if (trans->tr_ctfile_name != NULL)
			e_free(&trans->tr_ctfile_name);

		if (!done)
			return;
		if (ct_verbose_ratios)
			ct_dump_stats(stdout);

		ct_file_extract_fixup();
		ct_shutdown();
		break;
	case TR_S_WMD_READY:
	case TR_S_XML_OPEN:
	case TR_S_XML_CLOSING:
	case TR_S_XML_CLOSED:
	case TR_S_XML_OPENED:
	case TR_S_READ:
		break;
	case TR_S_XML_CLOSE:
		CNDBG(CT_LOG_FILE, "eof reached, closing file");
		if (ctfile_handle != NULL) {
			fclose(ctfile_handle);
			ctfile_handle = NULL;
		}
		ct_xml_file_close();
		break;

	case TR_S_XML_CULL_REPLIED:
		ct_wakeup_file();
		break;
	default:
		CFATALX("unexpected tr state in %s %d", __func__,
		    trans->tr_state);
	}
}

void
ctfile_list_start(struct ct_op *op)
{
	struct xmlsd_element_list	 xl;
	struct xmlsd_element		*xe;
	struct ct_trans			*trans;
	size_t				 sz;

	ct_set_file_state(CT_S_FINISHED);

	trans = ct_trans_alloc();

	trans->tr_trans_id = ct_trans_id++;
	trans->tr_state = TR_S_XML_LIST;

	CNDBG(CT_LOG_XML, "setting up XML");

	xe = xmlsd_create(&xl, "ct_md_list");
	xmlsd_set_attr(xe, "version", CT_MD_LIST_VERSION);

	if ((trans->tr_data[2] = (uint8_t *)xmlsd_generate(&xl,
	    ct_body_alloc_xml, &sz, 1)) == NULL)
		CFATALX("%s: Could not allocate xml body", __func__);
	xmlsd_unwind(&xl);
	trans->tr_dataslot = 2;
	trans->tr_size[2] = sz;

	ct_queue_transfer(trans);
}

void
ctfile_list_complete(int matchmode, char **flist, char **excludelist,
    struct ctfile_list_tree *results)
{
	struct ct_match		*match, *ex_match = NULL;
	struct ctfile_list_file	*file;

	if (SLIST_EMPTY(&ctfile_list_files))
		return;

	match = ct_match_compile(matchmode, flist);
	if (excludelist)
		ex_match = ct_match_compile(matchmode, excludelist);
	while ((file = SLIST_FIRST(&ctfile_list_files)) != NULL) {
		SLIST_REMOVE_HEAD(&ctfile_list_files, mlf_link);
		if (ct_match(match, file->mlf_name) == 0 && (ex_match == NULL ||
		    ct_match(ex_match, file->mlf_name) == 1)) {
			RB_INSERT(ctfile_list_tree, results, file);
		} else {
			e_free(&file);
		}
	}
	if (ex_match != NULL)
		ct_match_unwind(ex_match);
	ct_match_unwind(match);
}

int
ct_cmp_ctfile(struct ctfile_list_file *f1, struct ctfile_list_file *f2)
{
	return (strcmp(f1->mlf_name, f2->mlf_name));
}

RB_GENERATE(ctfile_list_tree, ctfile_list_file, mlf_next, ct_cmp_ctfile);

void
ctfile_delete(struct ct_op *op)
{
	struct xmlsd_element_list	 xl;
	struct xmlsd_element		*xe;
	const char			*rname = op->op_remote_fname;
	struct ct_trans			*trans;
	char				 b64[CT_MAX_MD_FILENAME * 2];
	size_t				 sz;

	rname = ctfile_cook_name(rname);

	if (ct_base64_encode(CT_B64_M_ENCODE, (uint8_t *)rname, strlen(rname),
	    (uint8_t *)b64, sizeof(b64)))
		CFATALX("cant base64 encode %s", rname);

	xe = xmlsd_create(&xl, "ct_md_delete");
	xmlsd_set_attr(xe, "version", CT_MD_DELETE_VERSION);
	xe = xmlsd_add_element(&xl, xe, "file");
	xmlsd_set_attr(xe, "name", b64);

	e_free(&rname);

	trans = ct_trans_alloc();
	trans->tr_trans_id = ct_trans_id++;
	trans->tr_state = TR_S_XML_DELETE;

	if ((trans->tr_data[2] = (uint8_t *)xmlsd_generate(&xl,
	    ct_body_alloc_xml, &sz, 1)) == NULL)
		CFATALX("%s: Could not allocate xml body", __func__);
	xmlsd_unwind(&xl);
	trans->tr_dataslot = 2;
	trans->tr_size[2] = sz;

	ct_queue_transfer(trans);
}

void
ct_handle_xml_reply(struct ct_trans *trans, struct ct_header *hdr,
    void *vbody)
{
	struct xmlsd_element_list xl;
	struct xmlsd_attribute *xa;
	struct xmlsd_element *xe;
	char *body = vbody;
	char *filename;
	char b64[CT_MAX_MD_FILENAME * 2];
	int r;

	CNDBG(CT_LOG_XML, "xml [%s]", (char *)vbody);

	/* Dispose of last parsed command. */
	TAILQ_INIT(&xl);

	r = xmlsd_parse_mem(body, hdr->c_size - 1, &xl);
	if (r)
		CFATALX("XML parse failed! (%d)", r);

	TAILQ_FOREACH(xe, &xl, entry) {
		CNDBG(CT_LOG_XML, "%d %s = %s (parent = %s)",
		    xe->depth, xe->name, xe->value ? xe->value : "NOVAL",
		    xe->parent ? xe->parent->name : "NOPARENT");
		TAILQ_FOREACH(xa, &xe->attr_list, entry)
			CNDBG(CT_LOG_XML, "\t%s = %s", xa->name, xa->value);
	}

	r = xmlsd_validate(&xl, ct_xml_cmds);
	if (r)
		CFATALX("XML validate of '%s' failed! (%d)", body, r);

	if (TAILQ_EMPTY(&xl))
		CFATALX("parse command: No XML");

	xe = TAILQ_FIRST(&xl);
	if (strncmp(xe->name, "ct_md_open", strlen("ct_md_open")) == 0) {
		int die = 1;

		TAILQ_FOREACH(xe, &xl, entry) {
			if (strcmp(xe->name, "file") == 0) {
				filename = xmlsd_get_attr(xe, "name");
				if (filename && filename[0] != '\0') {
					CNDBG(CT_LOG_FILE, "%s opened\n",
					    filename);
					die = 0;
					ctfile_open_inflight = 0;
					ctfile_is_open = 1;
					ct_wakeup_file();
				}
			}
		}
		if (die)
			CFATALX("couldn't open remote file");
		trans->tr_state = TR_S_XML_OPENED;
	} else if (strcmp(xe->name, "ct_md_close") == 0) {
		ctfile_is_open = 0;
		trans->tr_state = TR_S_DONE;
	} else if (strcmp(xe->name, "ct_md_list") == 0) {
		struct ctfile_list_file	*file;
		const char		*errstr;
		char			*tmp;

		TAILQ_FOREACH(xe, &xl, entry) {
			if (strcmp(xe->name, "file") == 0) {
				file = e_malloc(sizeof(*file));
				tmp = xmlsd_get_attr(xe, "name");
				if (tmp == NULL) {
					e_free(&file);
					continue;
				}

				if (ct_base64_encode(CT_B64_M_DECODE,
				    (uint8_t *)tmp, strlen(tmp),
				    (uint8_t *)file->mlf_name,
				    sizeof(file->mlf_name))) {
					    e_free(&file);
					    continue;
				}

				tmp = xmlsd_get_attr(xe, "size");
				file->mlf_size = strtonum(tmp, 0, LLONG_MAX,
				    &errstr);
				if (errstr != NULL)
					CFATAL("can't parse file size %s",
					    errstr);

				tmp = xmlsd_get_attr(xe, "mtime");
				file->mlf_mtime = strtonum(tmp, 0, LLONG_MAX,
				    &errstr);
				if (errstr != NULL)
					CFATAL("can't parse mtime: %s", errstr);
				SLIST_INSERT_HEAD(&ctfile_list_files, file,
				    mlf_link);
			}
		}
		trans->tr_state = TR_S_DONE;
	} else  if (strcmp(xe->name, "ct_md_delete") == 0) {
		TAILQ_FOREACH(xe, &xl, entry) {
			if (strcmp(xe->name, "file") == 0) {
				filename = xmlsd_get_attr(xe, "name");
				if (filename == NULL || filename[0] == '\0')
					printf("specified archive does not "
					    "exist\n");
				else {
					if (ct_base64_encode(CT_B64_M_DECODE,
					    (uint8_t *)filename, strlen(filename),
					    (uint8_t *)b64, sizeof(b64))) {
						CFATALX("cant base64 encode %s",
						    filename);
					}
					printf("%s deleted\n", b64);
				}
			}
		}
		trans->tr_state = TR_S_DONE;
	} else  if (strcmp(xe->name, "ct_cull_setup_reply") == 0) {
		CNDBG(CT_LOG_XML, "cull_setup_reply");
		trans->tr_state = TR_S_DONE;
	} else  if (strcmp(xe->name, "ct_cull_shas_reply") == 0) {
		CNDBG(CT_LOG_XML, "cull_shas_reply");
		if (trans->tr_eof == 1)
			trans->tr_state = TR_S_DONE;
		else
			trans->tr_state = TR_S_XML_CULL_REPLIED;
	} else  if (strcmp(xe->name, "ct_cull_complete_reply") == 0) {
		CNDBG(CT_LOG_XML, "cull_complete_reply");
		trans->tr_state = TR_S_DONE;
	} else {
		CABORTX("unexpected XML returned [%s]", (char *)vbody);
	}

	ct_queue_transfer(trans);
	ct_body_free(NULL, vbody, hdr);
	ct_header_free(NULL, hdr);
	xmlsd_unwind(&xl);
}

void
ctfile_mode_setup(const char *mode)
{
	CNDBG(CT_LOG_CTFILE, "%s", mode ? mode : "");
	if (mode == NULL)
		return;

	if (strcmp(mode, "remote") == 0)
		ctfile_mode = CT_MDMODE_REMOTE;
	else if (strcmp(mode, "local") == 0)
		ctfile_mode = CT_MDMODE_LOCAL;
	else
		CFATALX("invalid ctfile mode specified");
}

int
ctfile_in_cache(const char *ctfile)
{
	struct dirent	*dp;
	DIR		*dirp;
	int		 found = 0;

	if ((dirp = opendir(ctfile_cachedir)) == NULL)
		CFATALX("can't open metadata cache dir");
	while ((dp = readdir(dirp)) != NULL) {
		if (strcmp(dp->d_name, ctfile) == 0) {
			CNDBG(CT_LOG_CTFILE, "found in cachedir");
			found = 1;
			break;
		}
	}
	closedir(dirp);

	return (found);
}

/*
 * return the filename in the cache directory that a ctfile would have
 * if it extisted.
 */
char *
ctfile_get_cachename(const char *ctfile)
{
	char	*cachename;

	/* cachedir was made sure to terminate with / earlier */
	e_asprintf(&cachename, "%s%s", ctfile_cachedir, ctfile);
	return cachename;
}

/*
 * returns boolean 1/0 whether or not the ctfile in question is the full tag
 * with date/time or not.
 */
int
ctfile_is_fullname(const char *ctfile)
{
	char			*pattern = "^[[:digit:]]{8}-[[:digit:]]{6}-";
	char			error[1024];
	regex_t			re;
	int			match = 0, rv;
	if ((rv = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB)) != 0) {
		regerror(rv, &re, error, sizeof(error) - 1);
		CFATALX("%s: regcomp failed: %s", __func__, error);
	}
	if (regexec(&re, ctfile, 0, NULL, 0) == 0)
		match = 1;

	regfree(&re);
	return match;
}


#define			TIMEDATA_LEN	17	/* including NUL */
int
strcompare(const void *a, const void *b)
{
	/* sort purely based on date */
	return (strncmp(*(char **)b, *(char **)a, TIMEDATA_LEN - 1));
}

/*
 * filenames passed in remote mode are opaque tags for the backup.
 * the are stored on the server and in remote mode in the form
 * YYYYMMDD-HHMMSS-<strnvis(mname)>
 */
void
ctfile_find_for_extract(struct ct_op *op)
{
	const char	*ctfile = op->op_local_fname;
	struct ct_op	*list_fakeop;
	char	 	**bufp;
	int		 matchmode;

	/* cook the ctfile so we only search for the actual tag */
	ctfile = ctfile_cook_name(ctfile);

	list_fakeop = e_calloc(1, sizeof(*list_fakeop));
	bufp = e_calloc(2, sizeof(char **));
	if (ctfile_is_fullname(ctfile)) {
		/* use list as stat() for now */
		*bufp = e_strdup(ctfile);
		matchmode = CT_MATCH_GLOB;
	} else {
		e_asprintf(bufp, "^[[:digit:]]{8}-[[:digit:]]{6}-%s$", ctfile);

		matchmode = CT_MATCH_REGEX;
		/*
		 * get the list of files matching this tag from the server.
		 * list returns an empty list if it found
		 * nothing and NULL upon failure.
		 */
	}
	e_free(&ctfile);

	CNDBG(CT_LOG_CTFILE, "looking for %s", bufp[0]);

	list_fakeop->op_filelist = bufp;
	list_fakeop->op_matchmode = matchmode;

	op->op_priv = list_fakeop;
	ctfile_list_start(list_fakeop);
}

void
ct_free_localname(struct ct_op *op)
{

	if (op->op_local_fname != NULL)
		e_free(&op->op_local_fname);
}

void
ct_free_remotename(struct ct_op *op)
{
	if (op->op_remote_fname != NULL)
		e_free(&op->op_remote_fname);
}

void
ct_free_localname_and_remote(struct ct_op *op)
{
	ct_free_localname(op);
	ct_free_remotename(op);
}

void ctfile_download_next(struct ct_op *op);
void
ctfile_download_next(struct ct_op *op)
{
	const char		*ctfile = op->op_local_fname;
	const char		*rfile = op->op_remote_fname;
	char			*prevfile;
	char			*cachename;
	char			*cookedname;

	ctfile_is_open = 0;	/* prevent trying to close upon next download */
again:
	CNDBG(CT_LOG_CTFILE, "ctfile %s", ctfile);

	/* this will provide us the path that we need to use */
	prevfile = ctfile_get_previous(ctfile);
	if (prevfile == NULL)
		goto out;

	if (prevfile[0] != '\0') {
		cookedname = ctfile_cook_name(prevfile);
		cachename = ctfile_get_cachename(cookedname);
		CNDBG(CT_LOG_CTFILE, "prev file %s cachename %s", prevfile,
		    cachename);
		if (!ctfile_in_cache(cachename)) {
			e_free(&cachename);
			ct_add_operation_after(op, ctfile_extract,
			    ctfile_download_next, (char *)prevfile, cookedname,
				NULL, NULL, NULL, 0, 0);
		} else {
			if (ctfile)
				e_free(&ctfile);
			if (rfile)
				e_free(&rfile);
			e_free(&cookedname);
			e_free(&cachename);
			ctfile = prevfile;
			goto again;
		}
	} else
		e_free(&prevfile);

out:
	if (ctfile)
		e_free(&ctfile);
	if (rfile)
		e_free(&rfile);

}

/*
 * now the operation has completed we can kick off the next operation knowing
 * that everything has been set up for it.
 */
void
ctfile_extract_nextop(struct ct_op *op)
{
	char	*ctfile, *tctfile, *trfile;

	ctfile_is_open = 0;
	/*
	 * need to determine if this is a layered backup, if so, we need to
	 * queue download of that file
	 */
	if (op->op_action == CT_A_EXTRACT || op->op_action == CT_A_LIST ||
	    op->op_action == CT_A_JUSTDL) {
		/*
		 * we need to keep these files, but download_next normally
		 * needs to free them, make a temporary copy.
		 */
		tctfile = op->op_local_fname;
		trfile = op->op_remote_fname;
		if (tctfile)
			op->op_local_fname = e_strdup(tctfile);
		if (trfile)
			op->op_remote_fname = e_strdup(trfile);
		ctfile_download_next(op);
		op->op_local_fname = tctfile;
		op->op_remote_fname = trfile;
	}

	/*
	 * Any recursive download after here will be placed after the
	 * current operation in the queue of ops. So we can now add the final
	 * operation to the end of the queue without difficulty.
	 */
	switch (op->op_action) {
	case CT_A_EXTRACT:
		ct_add_operation(ct_extract, ct_free_localname_and_remote,
		    op->op_local_fname, op->op_remote_fname, op->op_filelist,
		    op->op_excludelist, NULL, op->op_matchmode, 0);
		break;
	case CT_A_LIST:
		ct_add_operation(ct_list_op, ct_free_localname_and_remote,
		    op->op_local_fname, op->op_remote_fname, op->op_filelist,
		    op->op_excludelist, NULL, op->op_matchmode, 0);
		break;
	case CT_A_ARCHIVE:
		if (op->op_remote_fname)
			e_free(&op->op_remote_fname);
		/*
		 * Since we were searching for previous, original ctfile
		 * is stored in basis. Swap them.
		 */
		ctfile = ctfile_find_for_archive(op->op_basis);
		CNDBG(CT_LOG_CTFILE, "setting basisname %s",
		    op->op_local_fname);
		/* XXX does this leak cachename? */
		ct_add_operation(ct_archive, NULL, ctfile, NULL,
		    op->op_filelist, op->op_excludelist, op->op_local_fname,
		    op->op_matchmode, 0);
		ct_add_operation(ctfile_archive, ct_free_localname_and_remote,
		    ctfile, NULL, NULL, NULL, NULL, 0, 0);
		break;
	case CT_A_JUSTDL:
		{
		extern char * ct_fb_filename; 
		ct_fb_filename = op->op_local_fname; /* XXX ick */
		ct_add_operation(ct_shutdown_op, NULL, NULL, NULL, NULL, NULL,
		    NULL, 0, 0);
		}
		break;
	default:
		CFATALX("invalid action");
	}
}

void
ctfile_find_for_extract_complete(struct ct_op *op)
{
	struct ct_op		*list_fakeop = op->op_priv;
	struct ctfile_list_tree	 result;
	struct ctfile_list_file	*tmp;
	char	 		*best, *cachename = NULL;

	RB_INIT(&result);
	ctfile_list_complete(list_fakeop->op_matchmode,
	    list_fakeop->op_filelist, list_fakeop->op_excludelist, &result);
	e_free(list_fakeop->op_filelist);
	e_free(&list_fakeop->op_filelist);
	e_free(&list_fakeop);

	/* grab the newest one */
	if ((tmp = RB_MAX(ctfile_list_tree, &result)) == NULL) {
		if (op->op_action == CT_A_ARCHIVE) {
			goto do_operation;
		} else  {
			CFATALX("unable to find metadata tagged %s",
			    op->op_local_fname);
		}
	}

	/* pick the newest one */
	best = e_strdup(tmp->mlf_name);
	CNDBG(CT_LOG_CTFILE, "backup file is %s", best);

	while((tmp = RB_ROOT(&result)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &result, tmp);
		e_free(&tmp);
	}

	/*
	 * if the metadata file is not in the cache directory then we
	 * need to download it first. if we need to recursively download
	 * a differential chain then that code will handle scheduling
	 * those operations too. If we have it, we still need to check
	 * that all others in the chain exist, however.
	 */
	cachename = ctfile_get_cachename(best);
	if (!ctfile_in_cache(best)) {
		/*
		 * since archive needs the original metadata name still
		 * and is searching for a prior archive for differentials
		 * we put local_fname (the original) in the basis slot here.
		 * nextop will fix it for us.
		 */
		ct_add_operation(ctfile_extract, ctfile_extract_nextop,
		    cachename, best, op->op_filelist, op->op_excludelist,
		    op->op_local_fname, op->op_matchmode, op->op_action);
	} else {
		e_free(&best);
do_operation:
		/*
		 * Don't need to grab this ctfile, but may need one later in
		 * the differential chain, recurse. When we know more we can
		 * prepare the final operation
		 */
		op->op_basis = op->op_local_fname;
		op->op_local_fname = cachename;
		ctfile_extract_nextop(op);
	}

}

char *
ctfile_find_for_archive(const char *ctfile)
{
	char	 buf[TIMEDATA_LEN], *fullname, *cachename;
	time_t	 now;

	/* cook the ctfile so we only search for the actual tag */
	ctfile = ctfile_cook_name(ctfile);

	if (ctfile_is_fullname(ctfile) != 0)
		CFATALX("metadata name with date tag already filled in");

	now = time(NULL);
	if (strftime(buf, TIMEDATA_LEN, "%Y%m%d-%H%M%S",
	    localtime(&now)) == 0)
		CFATALX("can't format time");
	e_asprintf(&fullname, "%s-%s", buf, ctfile);
	CNDBG(CT_LOG_CTFILE, "backup file is %s", fullname);

	/* check it isn't already in the cache */
	cachename = ctfile_get_cachename(fullname);
	if (ctfile_in_cache(fullname))
		CFATALX("generated metadata name %s already in cache dir",
		    fullname);

	e_free(&ctfile);
	e_free(&fullname);

	return (cachename);
}

/*
 * Functions for automatic crypto secrets storage on the server.
 */

void	ct_secrets_unlock(struct ct_op *);

/*
 * List available crypto secrets files so we can see if we are ahead or behind
 */
void
ct_check_crypto_secrets_nextop(struct ct_op *op)
{
	extern char		*ct_crypto_password;
	char			*current_secrets = op->op_local_fname;
	char			*t, *remote_name = NULL;
	char			 *dirp, dir[PATH_MAX], tmp[PATH_MAX];
	const char		*errstr;
	struct ctfile_list_tree	 results;
	struct ctfile_list_file	*file = NULL;
	struct stat		 sb;
	time_t			 mtime = 0, local_mtime = 0;
	int			 crypto_fd;

	RB_INIT(&results);
	ctfile_list_complete(op->op_matchmode, op->op_filelist,
	    op->op_excludelist, &results);
	/* We're interested in the newest. */
	if ((file = RB_MAX(ctfile_list_tree, &results)) == NULL)
		goto check_local;

	CNDBG(CT_LOG_CRYPTO, "latest secrets file on server: %s",
	    file->mlf_name);
	/* parse out mtime */
	if ((t = strchr(file->mlf_name, '-')) == NULL)
		CFATALX("invalid answer from server");
	*t = '\0';
	mtime = strtonum(file->mlf_name, LLONG_MIN, LLONG_MAX, &errstr);
	if (errstr)
		CFATALX("mtime %s from secrets file invalid: %s",
		    file->mlf_name, errstr);
	*t = '-'; /* put it back */
	remote_name = e_strdup(file->mlf_name);
	while((file = RB_ROOT(&results)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &results, file);
		e_free(&file);
	}

check_local:
	/* get mtime, if any for current secrets file */
	if (stat(current_secrets, &sb) != -1)
		local_mtime = sb.st_mtime;

	/* This includes the case where both are missing */
	if (mtime == local_mtime) {
		CNDBG(CT_LOG_CRYPTO, "dates match, nothing to do");
		if (remote_name)
			e_free(&remote_name);
		if (ct_create_or_unlock_secrets(current_secrets,
		    ct_crypto_password))
			CFATALX("can't unlock secrets file");
	} else if (mtime < local_mtime) {
		/* XXX verify local file before upload? */
		CNDBG(CT_LOG_CRYPTO, "uploading local file");
		if (remote_name)
			e_free(&remote_name);
		e_asprintf(&remote_name, "%020" PRId64 "-crypto.secrets",
		    (long long)local_mtime);
		ct_add_operation_after(op, ctfile_archive, ct_secrets_unlock,
		    current_secrets, remote_name, NULL, NULL, NULL, 0, 0);
	} else { /* mtime > local_mtime */
		CNDBG(CT_LOG_CRYPTO, "downloading remote file");
		strlcpy(dir, current_secrets, sizeof(dir));
		if ((dirp = dirname(dir)) == NULL)
			CFATALX("can't get dirname of secrets file");
		strlcpy(tmp, dirp, sizeof(tmp));
		strlcat(tmp, "/.ctcrypto.XXXXXXXX", sizeof(tmp));
		if ((crypto_fd = mkstemp(tmp)) == -1)
			CFATAL("can't make temporary file");
		if ((ctfile_handle = fdopen(crypto_fd, "w+")) == NULL)
			CFATAL("can't associate stream with temporary file");
		CNDBG(CT_LOG_CRYPTO, "temp file: %s", tmp);
		/* stash current name in basis in case we need to fallback */
		ct_add_operation_after(op, ctfile_extract, ct_secrets_unlock,
		    e_strdup(tmp), remote_name, NULL, NULL,  current_secrets,
		    0, 0);
	}
}

void
ct_secrets_unlock(struct ct_op *op)
{
	extern char		*ct_crypto_password;
	char			*crypto_secrets = op->op_local_fname;
	char			*old_secrets = op->op_basis;
	char			 tmp[PATH_MAX], *t;
	const char		*errstr;
	struct timeval		 times[2] = { };
	struct stat	 	 sb;

	CNDBG(CT_LOG_CRYPTO, "operation complete, unlocking secrets file");
again:
	if (stat(crypto_secrets, &sb) == -1) {
		if (old_secrets != NULL) {
			/* unlink tmp file */
			(void)unlink(crypto_secrets);
			e_free(&crypto_secrets);
			crypto_secrets = old_secrets;
			old_secrets = NULL;
			CWARNX("can't parse new secrets file, using old one");
			goto again;
		}
		/* XXX should we *ever* hit this case? */
		fprintf(stderr, "No crypto secrets file. Creating\n");
		if (ct_create_secrets(ct_crypto_password, ct_crypto_secrets,
		    NULL, NULL))
			CFATALX("can't create secrets");
	}
	if (ct_unlock_secrets(ct_crypto_password, crypto_secrets,
	    ct_crypto_key, sizeof (ct_crypto_key), ct_iv, sizeof ct_iv)) {
		if (old_secrets != NULL) {
			/* unlink tmp file */
			(void)unlink(crypto_secrets);
			e_free(&crypto_secrets);
			crypto_secrets = old_secrets;
			old_secrets = NULL;
			CWARNX("can't parse new secrets file, using old one");
			goto again;
		}
		CFATALX("can't unlock secrets");
	}
	if (old_secrets != NULL) {
		strlcpy(tmp, old_secrets, sizeof(tmp));
		strlcat(tmp, ".bak", sizeof(tmp));

		/* parse out mtime */
		if ((t = strchr(op->op_remote_fname, '-')) == NULL)
			CFATALX("invalid answer from server");
		*t = '\0';
		times[1].tv_sec = strtonum(op->op_remote_fname, LLONG_MIN,
		    LLONG_MAX, &errstr);
		if (errstr)
			CFATALX("mtime %s from secrets file invalid: %s",
			    op->op_remote_fname, errstr);
		times[0].tv_sec = times[1].tv_sec;

		/* remove an existing backup file */
		(void)unlink(tmp);
		/* save old file allow for failure in case it exists */
		if (stat(old_secrets, &sb) == 0) {
			if (link(old_secrets, tmp) != 0)
				CWARN("unable to backup secrets file");
		}
		/* rename to ``real'' filename */
		if (rename(crypto_secrets, old_secrets) != 0)
			CFATAL("can't rename secrets file to real name");
		/*
		 * Set mtime to the mtime we downloaded.
		 * XXX futimens() would be nice here since atime doesn't matter
		 */
		times[0].tv_usec = times[1].tv_usec = 0;
		if (utimes(old_secrets, times) != 0)
			CWARN("couldn't set mtime on new secrets file");
		e_free(&crypto_secrets);
	}
	ct_encrypt_enabled = 1;
	/* Free remote name */
	if (op->op_remote_fname)
		e_free(&op->op_remote_fname);
}

/*
 * Delete all metadata files that were found by the preceding list operation.
 */
void
ctfile_trigger_delete(struct ct_op *op)
{
	struct ctfile_list_tree	 results;
	struct ctfile_list_file	*file = NULL;

	RB_INIT(&results);
	ctfile_list_complete(op->op_matchmode, op->op_filelist,
	    op->op_excludelist, &results);
	while((file = RB_ROOT(&results)) != NULL) {
		CNDBG(CT_LOG_CRYPTO, "deleting remote crypto secrets file %s",
		    file->mlf_name);
		ct_add_operation_after(op, ctfile_delete, NULL, NULL,
		    e_strdup(file->mlf_name), NULL, NULL, NULL, 0, 0);
		RB_REMOVE(ctfile_list_tree, &results, file);
		e_free(&file);
	}
}

/*
 * Verify that the ctfile name is kosher.
 * - Encode the name (with a fake prefix) to make sure it fits.
 * - To help with interoperability, scan for a few special characters
 *   and punt if we find those.
 */
int
ctfile_verify_name(char *ctfile)
{
	const char	*set = CT_CTFILE_REJECTCHRS;
	char		 b[CT_CTFILE_MAXLEN], b64[CT_CTFILE_MAXLEN];
	size_t		 span, ctfilelen;
	int		 sz;

	if (ctfile == NULL)
		return 1;

	/* No processing for local mode. */
	if (ctfile_mode == CT_MDMODE_LOCAL)
		return 0;

	sz = snprintf(b, sizeof(b), "YYYYMMDD-HHMMSS-%s", ctfile);
	if (sz == -1 || sz >= sizeof(b))
		return 1;

	/* Make sure it fits. */
	sz = ct_base64_encode(CT_B64_M_ENCODE, (uint8_t *)b, strlen(b),
	    (uint8_t *)b64, sizeof(b64));
	if (sz != 0)
		return 1;

	ctfilelen = strlen(ctfile);
	span = strcspn(ctfile, set);
	return !(span == ctfilelen);
}

/*
 * Data structures to hold cull data
 *
 * Should this be stored in memory or build a temporary DB
 * to hold it due to the number of shas involved?
 */

RB_HEAD(ct_sha_lookup, sha_entry) ct_sha_rb_head =
     RB_INITIALIZER(&ct_sha_rb_head);
uint64_t shacnt;
uint64_t sha_payload_sz;

struct sha_entry {
	RB_ENTRY(sha_entry)      s_rb;
	uint8_t sha[SHA_DIGEST_LENGTH];
};

int ct_cmp_sha(struct sha_entry *, struct sha_entry *);

RB_PROTOTYPE(ct_sha_lookup, sha_entry, s_rb, ct_cmp_sha);
RB_GENERATE(ct_sha_lookup, sha_entry, s_rb, ct_cmp_sha);

int
ct_cmp_sha(struct sha_entry *d1, struct sha_entry *d2)
{
	return bcmp(d1->sha, d2->sha, sizeof (d1->sha));
}

void
ct_cull_sha_insert(const uint8_t *sha)
{
	//char			shat[SHA_DIGEST_STRING_LENGTH];
	struct sha_entry	*node, *oldnode;

	node = e_malloc(sizeof(*node));
	bcopy (sha, node->sha, sizeof(node->sha));

	//ct_sha1_encode((uint8_t *)sha, shat);
	//printf("adding sha %s\n", shat);

	oldnode = RB_INSERT(ct_sha_lookup, &ct_sha_rb_head, node);
	if (oldnode != NULL) {
		/* already present, throw away copy */
		e_free(&node);
	} else
		shacnt++;

}

void
ct_cull_kick(void)
{

	CNDBG(CT_LOG_TRANS, "add_op cull_setup");
	CNDBG(CT_LOG_SHA, "shacnt %" PRIu64 , shacnt);

	ct_add_operation(ctfile_list_start, ct_cull_fetch_all_ctfiles, 
              NULL, NULL, NULL, NULL, NULL, 0, 0);
	ct_add_operation(ct_cull_collect_ctfiles, NULL, 
              NULL, NULL, NULL, NULL, NULL, 0, 0);
	ct_add_operation(ct_cull_setup, NULL,
	    NULL, NULL, NULL, NULL, NULL, 0, 0);
	ct_add_operation(ct_cull_send_shas, NULL,
	    NULL, NULL, NULL, NULL, NULL, 0, 0);
	ct_add_operation(ct_cull_send_complete, ct_cull_complete,
	    NULL, NULL, NULL, NULL, NULL, 0, 0);
}

void
ct_cull_complete(struct ct_op *op)
{
	CNDBG(CT_LOG_SHA, "shacnt %" PRIu64 " shapayload %" PRIu64, shacnt,
	    sha_payload_sz);
}

uint64_t cull_uuid; /* set up with random number in ct_cull_setup() */
/* tune this */
int sha_per_packet = 1000;

void
ct_cull_setup(struct ct_op *op)
{
	struct xmlsd_element_list	xl;
	struct xmlsd_element		*xp, *xe;
	struct ct_trans			*trans;
	size_t				sz;

	arc4random_buf(&cull_uuid, sizeof(cull_uuid));

	CNDBG(CT_LOG_TRANS, "cull_setup");
	ct_set_file_state(CT_S_RUNNING);

	trans = ct_trans_alloc();

	if (trans == NULL) {
		ct_set_file_state(CT_S_WAITING_TRANS);
		return;
	}

	trans->tr_trans_id = ct_trans_id++;
	trans->tr_state = TR_S_XML_CULL_SEND;

	xp = xmlsd_create(&xl, "ct_cull_setup");
	xmlsd_set_attr(xp, "version", CT_CULL_SETUP_VERSION);
	xe = xmlsd_add_element(&xl, xp, "cull");
	xmlsd_set_attr(xe, "type", "precious");
	xmlsd_set_attr_uint64(xe, "uuid", cull_uuid);

	if ((trans->tr_data[2] = (uint8_t *)xmlsd_generate(&xl,
	    ct_body_alloc_xml, &sz, 1)) == NULL)
		CFATALX("%s: Could not allocate xml body", __func__);
	xmlsd_unwind(&xl);
	trans->tr_dataslot = 2;
	trans->tr_size[2] = sz;

	ct_queue_transfer(trans);
}

int sent_complete;
void
ct_cull_send_complete(struct ct_op *op)
{
	struct xmlsd_element_list	xl;
	struct xmlsd_element		*xp, *xe;
	struct ct_trans			*trans;
	size_t				sz;

	if (sent_complete) {
		return;
	}
	sent_complete = 1;

	CNDBG(CT_LOG_TRANS, "send cull_complete");
	trans = ct_trans_alloc();

	if (trans == NULL) {
		ct_set_file_state(CT_S_WAITING_TRANS);
		return;
	}

	trans->tr_trans_id = ct_trans_id++;
	trans->tr_state = TR_S_XML_CULL_SEND;

	xp = xmlsd_create(&xl, "ct_cull_complete");
	xmlsd_set_attr(xp, "version", CT_CULL_COMPLETE_VERSION);
	xe = xmlsd_add_element(&xl, xp, "cull");
	xmlsd_set_attr(xe, "type", "process");
	xmlsd_set_attr_uint64(xe, "uuid", cull_uuid);

	if ((trans->tr_data[2] = (uint8_t *)xmlsd_generate(&xl,
	    ct_body_alloc_xml, &sz, 1)) == NULL)
		CFATALX("%s: Could not allocate xml body", __func__);
	xmlsd_unwind(&xl);
	trans->tr_dataslot = 2;
	trans->tr_size[2] = sz;
	ct_set_file_state(CT_S_FINISHED);

	ct_queue_transfer(trans);
}


void
ct_cull_send_shas(struct ct_op *op)
{
	struct xmlsd_element_list	xl;
	struct xmlsd_element		*xe, *xp;
	struct ct_trans			*trans;
	struct sha_entry		*node;
	size_t				sz;
	int				sha_add;
	char				shat[SHA_DIGEST_STRING_LENGTH];

	CNDBG(CT_LOG_TRANS, "cull_send_shas");
	node = RB_ROOT(&ct_sha_rb_head);
	if (shacnt == 0 || node == NULL) {
		ct_set_file_state(CT_S_FINISHED);
		return;
	}
	ct_set_file_state(CT_S_RUNNING);

	trans = ct_trans_alloc();

	if (trans == NULL) {

		ct_set_file_state(CT_S_WAITING_TRANS);
		return;
	}

	trans->tr_trans_id = ct_trans_id++;
	trans->tr_state = TR_S_XML_CULL_SEND;

	xp = xmlsd_create(&xl, "ct_cull_shas");
	xmlsd_set_attr(xp, "version", CT_CULL_SHA_VERSION);

	xe = xmlsd_add_element(&xl, xp, "uuid");
	xmlsd_set_attr_uint64(xe, "value", cull_uuid);

	sha_add = 0;

	while (node != NULL && sha_add < sha_per_packet) {
		xe = xmlsd_add_element(&xl, xp, "sha");
		ct_sha1_encode(node->sha, shat);
		//CNDBG(CT_LOG_SHA, "adding sha %s\n", shat);
		xmlsd_set_attr(xe, "sha", shat);
		shacnt--;
		sha_add++;
		RB_REMOVE(ct_sha_lookup, &ct_sha_rb_head, node);
		e_free(&node);

		node = RB_ROOT(&ct_sha_rb_head);
	}

	if ((trans->tr_data[2] = (uint8_t *)xmlsd_generate(&xl,
	    ct_body_alloc_xml, &sz, 1)) == NULL)
		CFATALX("%s: Could not allocate xml body", __func__);
	xmlsd_unwind(&xl);
	trans->tr_dataslot = 2;
	trans->tr_size[2] = sz;

	CNDBG(CT_LOG_SHA, "sending shas [%s]", (char *)trans->tr_data[2]);
	CNDBG(CT_LOG_SHA, "sending shas len %lu", (unsigned long) sz);
	sha_payload_sz += sz;
	ct_queue_transfer(trans);

	if (shacnt == 0 || node == NULL) {
		ct_set_file_state(CT_S_FINISHED);
		trans->tr_eof = 1;
		CNDBG(CT_LOG_SHA, "shacnt %" PRIu64, shacnt);
	}
}

/*
 * Code to get all metadata files on the server.
 * to be used for cull.
 */
struct ctfile_list_tree ct_cull_all_ctfiles =
    RB_INITIALIZER(&ct_cull_all_ctfiles);
char		*all_ctfiles_pattern[] = {
			"^[[:digit:]]{8}-[[:digit:]]{6}-.*",
			NULL,
		 };

void
ct_cull_fetch_all_ctfiles(struct ct_op *op)
{
	struct ctfile_list_tree	 results;
	struct ctfile_list_file	*file;
	char			*cachename;

	RB_INIT(&results);
	ctfile_list_complete(CT_MATCH_REGEX, all_ctfiles_pattern, NULL,
	    &results);
	while ((file = RB_ROOT(&results)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &results, file);
		CNDBG(CT_LOG_CTFILE, "looking for file %s ", file->mlf_name);
		if (!ctfile_in_cache(file->mlf_name)) {
			cachename = ctfile_get_cachename(file->mlf_name);
			CNDBG(CT_LOG_CTFILE, "getting %s to %s", file->mlf_name,
			    cachename);
			ct_add_operation_after(op, ctfile_extract,
			    ct_free_localname_and_remote, cachename,
			    e_strdup(file->mlf_name), NULL, NULL, NULL, 0, 0);
		} else {
			CNDBG(CT_LOG_CTFILE, "already got %s", file->mlf_name);
		}
		RB_INSERT(ctfile_list_tree, &ct_cull_all_ctfiles, file);
	}
}

void
ct_cull_collect_ctfiles(struct ct_op *op)
{
	struct ctfile_list_file	*file, *prevfile, filesearch;
	char			*prev_filename;
	int			timelen;
	char			buf[TIMEDATA_LEN];
	time_t			now;
	int			keep_files = 0;

	if (ct_ctfile_expire_day == 0)
		CFATALX("cull: ctfile_expire_day must be specified in config");

	now = time(NULL);
	now -= (24 * 60 * 60 * ct_ctfile_expire_day);
	if (strftime(buf, TIMEDATA_LEN, "%Y%m%d-%H%M%S",
	    localtime(&now)) == 0)
		CFATALX("can't format time");

	timelen = strlen(buf);

	RB_FOREACH(file, ctfile_list_tree, &ct_cull_all_ctfiles) {
		if (strncmp (file->mlf_name, buf, timelen) < 0) {
			file->mlf_keep = 0;
		} else {
			file->mlf_keep = 1;
			keep_files++;
		}
	}

	if (keep_files == 0)
		CFATALX("All ctfiles are old and would be deleted, aborting.");

	RB_FOREACH(file, ctfile_list_tree, &ct_cull_all_ctfiles) {
		if (file->mlf_keep == 0)
			continue;

		prev_filename = ctfile_get_previous(file->mlf_name);
prev_ct_file:
		if (prev_filename != NULL) {
			CINFO("prev filename %s", prev_filename);
			strncpy(filesearch.mlf_name, prev_filename,
			    sizeof(filesearch.mlf_name));
			prevfile = RB_FIND(ctfile_list_tree,
			    &ct_cull_all_ctfiles, &filesearch);
			if (prevfile == NULL) {
				CWARNX("file not found in ctfilelist [%s]",
				    prev_filename);
			} else {
				if (prevfile->mlf_keep == 0)
					CINFO("Warning, old ctfile %s still "
					    "referenced by newer backups, "
					    "keeping", prev_filename);
				prevfile->mlf_keep++;
				e_free(&prev_filename);

				prev_filename = ctfile_get_previous(
				    prevfile->mlf_name);
				goto prev_ct_file;
			}
			e_free(&prev_filename);
		}
	}
	RB_FOREACH(file, ctfile_list_tree, &ct_cull_all_ctfiles) {
		if (file->mlf_keep == 0) {
			CNDBG(CT_LOG_CTFILE, "adding %s to delete list",
			    file->mlf_name);
			ct_add_operation(ctfile_delete, NULL, NULL,
			    e_strdup(file->mlf_name), NULL, NULL, NULL, 0, 0);
		} else {
			CNDBG(CT_LOG_CTFILE, "adding %s to keep list",
			    file->mlf_name);
			ct_cull_add_shafile(file->mlf_name);
		}
	}

	/* cleanup */
	while((file = RB_ROOT(&ct_cull_all_ctfiles)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &ct_cull_all_ctfiles, file);
		e_free(&file);
		/* XXX - name  */
	}
	ct_op_complete();
}