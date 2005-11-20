/*
 * card-starcos.c: Support for STARCOS SPK 2.3 cards
 *
 * Copyright (C) 2003  J�rn Zukowski <zukowski@trustcenter.de> and 
 *                     Nils Larsch   <larsch@trustcenter.de>, TrustCenter AG
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "internal.h"
#include "asn1.h"
#include "cardctl.h"
#include <stdlib.h>
#include <string.h>

static struct sc_atr_table starcos_atrs[] = {
	{ "3B:B7:94:00:c0:24:31:fe:65:53:50:4b:32:33:90:00:b4", NULL, NULL, SC_CARD_TYPE_STARCOS_GENERIC, 0, NULL },
	{ "3B:B7:94:00:81:31:fe:65:53:50:4b:32:33:90:00:d1", NULL, NULL, SC_CARD_TYPE_STARCOS_GENERIC, 0, NULL },
	{ NULL, NULL, NULL, 0, 0, NULL }
};

static struct sc_card_operations starcos_ops;
static struct sc_card_operations *iso_ops = NULL;

static struct sc_card_driver starcos_drv = {
	"STARCOS SPK 2.3",
	"starcos",
	&starcos_ops,
	NULL, 0, NULL
};

static const struct sc_card_error starcos_errors[] = 
{
	{ 0x6600, SC_ERROR_INCORRECT_PARAMETERS, "Error setting the security env"},
	{ 0x66F0, SC_ERROR_INCORRECT_PARAMETERS, "No space left for padding"},
	{ 0x69F0, SC_ERROR_NOT_ALLOWED,          "Command not allowed"},
	{ 0x6A89, SC_ERROR_FILE_ALREADY_EXISTS,  "Files exists"},
	{ 0x6A8A, SC_ERROR_FILE_ALREADY_EXISTS,  "Application exists"},
	{ 0x6F01, SC_ERROR_CARD_CMD_FAILED, "public key not complete"},
	{ 0x6F02, SC_ERROR_CARD_CMD_FAILED, "data overflow"},
	{ 0x6F03, SC_ERROR_CARD_CMD_FAILED, "invalid command sequence"},
	{ 0x6F05, SC_ERROR_CARD_CMD_FAILED, "security enviroment invalid"},
	{ 0x6F07, SC_ERROR_FILE_NOT_FOUND, "key part not found"},
	{ 0x6F08, SC_ERROR_CARD_CMD_FAILED, "signature failed"},
	{ 0x6F0A, SC_ERROR_INCORRECT_PARAMETERS, "key format does not match key length"},
	{ 0x6F0B, SC_ERROR_INCORRECT_PARAMETERS, "length of key component inconsistent with algorithm"},
	{ 0x6F81, SC_ERROR_CARD_CMD_FAILED, "system error"}
};

/* internal structure to save the current security enviroment */
typedef struct starcos_ex_data_st {
	int    sec_ops;	/* the currently selected security operation,
			 * i.e. SC_SEC_OPERATION_AUTHENTICATE etc. */
	unsigned int    fix_digestInfo;
} starcos_ex_data;

/* the starcos part */
static int starcos_match_card(sc_card_t *card)
{
	int i;

	i = _sc_match_atr(card, starcos_atrs, &card->type);
	if (i < 0)
		return 0;
	return 1;
}

static int starcos_init(sc_card_t *card)
{
	unsigned int flags;
	starcos_ex_data *ex_data;

	ex_data = (starcos_ex_data *) malloc(sizeof(starcos_ex_data));
	if (!ex_data)
		return SC_ERROR_OUT_OF_MEMORY;
	memset(ex_data, 0, sizeof(starcos_ex_data));

	card->name = "STARCOS SPK 2.3";
	card->cla  = 0x00;
	card->drv_data = (void *)ex_data;

	flags = SC_ALGORITHM_RSA_PAD_PKCS1 
		| SC_ALGORITHM_ONBOARD_KEY_GEN
		| SC_CARD_FLAG_RNG
		| SC_ALGORITHM_RSA_PAD_ISO9796
		| SC_ALGORITHM_RSA_HASH_NONE
		| SC_ALGORITHM_RSA_HASH_SHA1
		| SC_ALGORITHM_RSA_HASH_MD5
		| SC_ALGORITHM_RSA_HASH_RIPEMD160
		| SC_ALGORITHM_RSA_HASH_MD5_SHA1;

	_sc_card_add_rsa_alg(card, 512, flags, 0x10001);
	_sc_card_add_rsa_alg(card, 768, flags, 0x10001);
	_sc_card_add_rsa_alg(card,1024, flags, 0x10001);

	card->caps = SC_CARD_CAP_RNG; 

	/* we need read_binary&friends with max 128 bytes per read */
	if (card->max_send_size > 128)
		card->max_send_size = 128;
	if (card->max_recv_size > 128)
		card->max_recv_size = 128;

	return 0;
}

static int starcos_finish(sc_card_t *card)
{
	if (card->drv_data)
		free((starcos_ex_data *)card->drv_data);
	return 0;
}

static int process_fci(sc_context_t *ctx, sc_file_t *file,
		       const u8 *buf, size_t buflen)
{
	/* NOTE: According to the Starcos S 2.1 manual it's possible
	 *       that a SELECT DF returns as a FCI arbitrary data which
	 *       is stored in a object file (in the corresponding DF)
	 *       with the tag 0x6f.
	 */

	size_t taglen, len = buflen;
	const u8 *tag = NULL, *p;
  
	if (ctx->debug >= 3)
		sc_debug(ctx, "processing FCI bytes\n");

	if (buflen < 2)
		return SC_ERROR_INTERNAL;
	if (buf[0] != 0x6f)
		return SC_ERROR_INVALID_DATA;
	len = (size_t)buf[1];
	if (buflen - 2 < len)
		return SC_ERROR_INVALID_DATA;
	p = buf + 2;

	/* defaults */
	file->type = SC_FILE_TYPE_WORKING_EF;
	file->ef_structure = SC_FILE_EF_UNKNOWN;
	file->shareable = 0;
	file->record_length = 0;
	file->size = 0;
  
	tag = sc_asn1_find_tag(ctx, p, len, 0x80, &taglen);
	if (tag != NULL && taglen >= 2) {
		int bytes = (tag[0] << 8) + tag[1];
		if (ctx->debug >= 3)
			sc_debug(ctx, "  bytes in file: %d\n", bytes);
		file->size = bytes;
	}

  	tag = sc_asn1_find_tag(ctx, p, len, 0x82, &taglen);
	if (tag != NULL) {
		const char *type = "unknown";
		const char *structure = "unknown";

		if (taglen == 1 && tag[0] == 0x01) {
			/* transparent EF */
			type = "working EF";
			structure = "transparent";
			file->type = SC_FILE_TYPE_WORKING_EF;
			file->ef_structure = SC_FILE_EF_TRANSPARENT;
		} else if (taglen == 1 && tag[0] == 0x11) {
			/* object EF */
			type = "working EF";
			structure = "object";
			file->type = SC_FILE_TYPE_WORKING_EF;
			file->ef_structure = SC_FILE_EF_TRANSPARENT; /* TODO */
		} else if (taglen == 3 && tag[1] == 0x21) {
			type = "working EF";
			file->record_length = tag[2];
			file->type = SC_FILE_TYPE_WORKING_EF;
			/* linear fixed, cyclic or compute */
			switch ( tag[0] )
			{
				case 0x02:
					structure = "linear fixed";
					file->ef_structure = SC_FILE_EF_LINEAR_FIXED;
					break;
				case 0x07:
					structure = "cyclic";
					file->ef_structure = SC_FILE_EF_CYCLIC;
					break;
				case 0x17:
					structure = "compute";
					file->ef_structure = SC_FILE_EF_UNKNOWN;
					break;
				default:
					structure = "unknown";
					file->ef_structure = SC_FILE_EF_UNKNOWN;
					file->record_length = 0;
					break;
			}
		}

		if (ctx->debug >= 3) {
	 		sc_debug(ctx, "  type: %s\n", type);
			sc_debug(ctx, "  EF structure: %s\n", structure);
		}
	}
	file->magic = SC_FILE_MAGIC;

	return SC_SUCCESS;
}

static int starcos_select_aid(sc_card_t *card,
			      u8 aid[16], size_t len,
			      sc_file_t **file_out)
{
	sc_apdu_t apdu;
	int r;
	size_t i = 0;

	if (!card )
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
  
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xA4, 0x04, 0x0C);
	apdu.lc = len;
	apdu.data = (u8*)aid;
	apdu.datalen = len;
	apdu.resplen = 0;
	apdu.le = 0;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");

	/* check return value */
	if (!(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) && apdu.sw1 != 0x61 )
    		SC_FUNC_RETURN(card->ctx, 2, sc_check_sw(card, apdu.sw1, apdu.sw2));
  
	/* update cache */
	card->cache.current_path.type = SC_PATH_TYPE_DF_NAME;
	card->cache.current_path.len = len;
	memcpy(card->cache.current_path.value, aid, len);

	if (file_out) {
		sc_file_t *file = sc_file_new();
		if (!file)
			SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
		file->type = SC_FILE_TYPE_DF;
		file->ef_structure = SC_FILE_EF_UNKNOWN;
		file->path.len = 0;
		file->size = 0;
		/* AID */
		for (i = 0; i < len; i++)  
			file->name[i] = aid[i];
		file->namelen = len;
		file->id = 0x0000;
		file->magic = SC_FILE_MAGIC;
		*file_out = file;
	}
	SC_FUNC_RETURN(card->ctx, 2, SC_SUCCESS);
}

static int starcos_select_fid(sc_card_t *card,
			      unsigned int id_hi, unsigned int id_lo,
			      sc_file_t **file_out)
{
	sc_apdu_t apdu;
	u8 data[] = {id_hi & 0xff, id_lo & 0xff};
	u8 resp[SC_MAX_APDU_BUFFER_SIZE];
	int bIsDF = 0, r;

	if (!card )
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);

	/* request FCI to distinguish between EFs and DFs */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0xA4, 0x00, 0x00);
	apdu.p2   = (file_out != NULL) ? 0x00 : 0x0C;
	apdu.resp = (u8*)resp;
	apdu.resplen = SC_MAX_APDU_BUFFER_SIZE;
	apdu.le = 256;
	apdu.lc = 2;
	apdu.data = (u8*)data;
	apdu.datalen = 2;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");

	if (apdu.p2 == 0x00 && apdu.sw1 == 0x62 && apdu.sw2 == 0x84 ) {
		/* no FCI => we have a DF (see comment in process_fci()) */
		bIsDF = 1;
		apdu.p2 = 0x0C;
		apdu.cse = SC_APDU_CASE_3_SHORT;
		apdu.resplen = 0;
		apdu.le = 0;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU re-transmit failed");
    	} else if (apdu.sw1 == 0x61 || (apdu.sw1 == 0x90 && apdu.sw2 == 0x00)) {
		/* SELECT returned some data (possible FCI) =>
		 * try a READ BINARY to see if a EF is selected */
		sc_apdu_t apdu2;
		u8 resp2[2];
		sc_format_apdu(card, &apdu2, SC_APDU_CASE_2_SHORT, 0xB0, 0, 0);
		apdu2.resp = (u8*)resp2;
		apdu2.resplen = 2;
		apdu2.le = 1;
		apdu2.lc = 0;
		r = sc_transmit_apdu(card, &apdu2);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu2.sw1 == 0x69 && apdu2.sw2 == 0x86)
			/* no current EF is selected => we have a DF */
			bIsDF = 1;
	}

	if (apdu.sw1 != 0x61 && (apdu.sw1 != 0x90 || apdu.sw2 != 0x00))
		SC_FUNC_RETURN(card->ctx, 2, sc_check_sw(card, apdu.sw1, apdu.sw2));

	/* update cache */
	if (bIsDF) {
		card->cache.current_path.type = SC_PATH_TYPE_PATH;
		card->cache.current_path.value[0] = 0x3f;
		card->cache.current_path.value[1] = 0x00;
		if (id_hi == 0x3f && id_lo == 0x00)
			card->cache.current_path.len = 2;
		else {
			card->cache.current_path.len = 4;
			card->cache.current_path.value[2] = id_hi;
			card->cache.current_path.value[3] = id_lo;
		}
	}

	if (file_out) {
		sc_file_t *file = sc_file_new();
		if (!file)
			SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
		file->id   = (id_hi << 8) + id_lo;
		file->path = card->cache.current_path;

		if (bIsDF) {
			/* we have a DF */
			file->type = SC_FILE_TYPE_DF;
			file->ef_structure = SC_FILE_EF_UNKNOWN;
			file->size = 0;
			file->namelen = 0;
			file->magic = SC_FILE_MAGIC;
			*file_out = file;
		} else {
			/* ok, assume we have a EF */
			r = process_fci(card->ctx, file, apdu.resp, 
					apdu.resplen);
			if (r != SC_SUCCESS) {
				sc_file_free(file);
				return r;
			}

			*file_out = file;
		}
	}

	SC_FUNC_RETURN(card->ctx, 2, SC_SUCCESS);
}

static int starcos_select_file(sc_card_t *card,
			       const sc_path_t *in_path,
			       sc_file_t **file_out)
{
	u8 pathbuf[SC_MAX_PATH_SIZE], *path = pathbuf;
	int    r;
	size_t i, pathlen;

	if ( !card || !in_path )
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);

	SC_FUNC_CALLED(card->ctx, 1);

	if (card->ctx->debug >= 4)
	{
		char buf[128], *p_buf = buf;
		for (i = 0; i < card->cache.current_path.len; i++) 
		{
			sprintf(p_buf, "%02X", card->cache.current_path.value[i]);
			p_buf += 2;
		}
		p_buf[0] = 0x00;
		sc_debug(card->ctx, "current path (%s, %s): %s (len: %u)\n",
			(card->cache.current_path.type==SC_PATH_TYPE_DF_NAME?"aid":"path"),
			(card->cache_valid?"valid":"invalid"),
			buf, card->cache.current_path.len);
	}
  
	memcpy(path, in_path->value, in_path->len);
	pathlen = in_path->len;

	if (in_path->type == SC_PATH_TYPE_FILE_ID)
	{	/* SELECT EF/DF with ID */
		/* Select with 2byte File-ID */
		if (pathlen != 2)
			SC_FUNC_RETURN(card->ctx,2,SC_ERROR_INVALID_ARGUMENTS);
		return starcos_select_fid(card, path[0], path[1], file_out);
	}
	else if (in_path->type == SC_PATH_TYPE_DF_NAME)
      	{	/* SELECT DF with AID */
		/* Select with 1-16byte Application-ID */
		if (card->cache_valid 
		    && card->cache.current_path.type == SC_PATH_TYPE_DF_NAME
		    && card->cache.current_path.len == pathlen
		    && memcmp(card->cache.current_path.value, pathbuf, pathlen) == 0 )
		{
			if (card->ctx->debug >= 4)
				sc_debug(card->ctx, "cache hit\n");
			SC_FUNC_RETURN(card->ctx, 2, SC_SUCCESS);
		}
		else
			return starcos_select_aid(card, pathbuf, pathlen, file_out);
	}
	else if (in_path->type == SC_PATH_TYPE_PATH)
	{
		u8 n_pathbuf[SC_MAX_PATH_SIZE];
		int bMatch = -1;

		/* Select with path (sequence of File-IDs) */
		/* Starcos (S 2.1 and SPK 2.3) only supports one
		 * level of subdirectories, therefore a path is
		 * at most 3 FID long (the last one being the FID
		 * of a EF) => pathlen must be even and less than 6
		 */
		if (pathlen%2 != 0 || pathlen > 6 || pathlen <= 0)
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
		/* if pathlen == 6 then the first FID must be MF (== 3F00) */
		if (pathlen == 6 && ( path[0] != 0x3f || path[1] != 0x00 ))
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);

		/* unify path (the first FID should be MF) */
		if (path[0] != 0x3f || path[1] != 0x00)
		{
			n_pathbuf[0] = 0x3f;
			n_pathbuf[1] = 0x00;
			for (i=0; i< pathlen; i++)
				n_pathbuf[i+2] = pathbuf[i];
			path = n_pathbuf;
			pathlen += 2; 
		}
	
		/* check current working directory */
		if (card->cache_valid 
		    && card->cache.current_path.type == SC_PATH_TYPE_PATH
		    && card->cache.current_path.len >= 2
		    && card->cache.current_path.len <= pathlen )
		{
			bMatch = 0;
			for (i=0; i < card->cache.current_path.len; i+=2)
				if (card->cache.current_path.value[i] == path[i] 
				    && card->cache.current_path.value[i+1] == path[i+1] )
					bMatch += 2;
		}

		if ( card->cache_valid && bMatch >= 0 )
		{
			if ( pathlen - bMatch == 2 )
				/* we are in the rigth directory */
				return starcos_select_fid(card, path[bMatch], path[bMatch+1], file_out);
			else if ( pathlen - bMatch > 2 )
			{
				/* two more steps to go */
				sc_path_t new_path;
	
				/* first step: change directory */
				r = starcos_select_fid(card, path[bMatch], path[bMatch+1], NULL);
				SC_TEST_RET(card->ctx, r, "SELECT FILE (DF-ID) failed");
		
				new_path.type = SC_PATH_TYPE_PATH;
				new_path.len  = pathlen - bMatch-2;
				memcpy(new_path.value, &(path[bMatch+2]), new_path.len);
				/* final step: select file */
				return starcos_select_file(card, &new_path, file_out);
      			}
			else /* if (bMatch - pathlen == 0) */
			{
				/* done: we are already in the
				 * requested directory */
				if ( card->ctx->debug >= 4 )
					sc_debug(card->ctx, "cache hit\n");
				/* copy file info (if necessary) */
				if (file_out) {
					sc_file_t *file = sc_file_new();
					if (!file)
						SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
					file->id = (path[pathlen-2] << 8) +
						   path[pathlen-1];
					file->path = card->cache.current_path;
					file->type = SC_FILE_TYPE_DF;
					file->ef_structure = SC_FILE_EF_UNKNOWN;
					file->size = 0;
					file->namelen = 0;
					file->magic = SC_FILE_MAGIC;
					*file_out = file;
				}
				/* nothing left to do */
				return SC_SUCCESS;
			}
		}
		else
		{
			/* no usable cache */
			for ( i=0; i<pathlen-2; i+=2 )
			{
				r = starcos_select_fid(card, path[i], path[i+1], NULL);
				SC_TEST_RET(card->ctx, r, "SELECT FILE (DF-ID) failed");
			}
			return starcos_select_fid(card, path[pathlen-2], path[pathlen-1], file_out);
		}
	}
	else
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
}

#define STARCOS_AC_ALWAYS	0x9f
#define STARCOS_AC_NEVER	0x5f
#define STARCOS_PINID2STATE(a)	((((a) & 0x0f) == 0x01) ? ((a) & 0x0f) : (0x0f - ((0x0f & (a)) >> 1)))

static u8 process_acl_entry(sc_file_t *in, unsigned int method, unsigned int in_def)
{
	u8 def = (u8)in_def;
	const sc_acl_entry_t *entry = sc_file_get_acl_entry(in, method);
	if (!entry)
		return def;
	else if (entry->method & SC_AC_CHV) {
		unsigned int key_ref = entry->key_ref;
		if (key_ref == SC_AC_KEY_REF_NONE)
			return def;
		else if ((key_ref & 0x0f) == 1)
			/* SOPIN */
			return (key_ref & 0x80 ? 0x10 : 0x00) | 0x01;
		else
			return (key_ref & 0x80 ? 0x10 : 0x00) | STARCOS_PINID2STATE(key_ref);
	} else if (entry->method & SC_AC_NEVER)
		return STARCOS_AC_NEVER;
	else
		return def;
}

/** starcos_process_acl
 * \param card pointer to the sc_card object
 * \param file pointer to the sc_file object
 * \param data pointer to a sc_starcos_create_data structure
 * \return SC_SUCCESS if no error occured otherwise error code
 *
 * This function tries to create a somewhat useable Starcos spk 2.3 acl
 * from the OpenSC internal acl (storing the result in the supplied
 * sc_starcos_create_data structure). 
 */
static int starcos_process_acl(sc_card_t *card, sc_file_t *file,
	sc_starcos_create_data *data)
{
	int    r;
	u8     tmp, *p;
	static const u8 def_key[] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};

	if (file->type == SC_FILE_TYPE_DF && file->id == 0x3f00) {
		p    = data->data.mf.header;
		memcpy(p, def_key, 8);
		p   += 8;
		*p++ = (file->size >> 8) & 0xff;
		*p++ = file->size & 0xff;
		/* guess isf size (mf_size / 4) */
		*p++ = (file->size >> 10) & 0xff;
		*p++ = (file->size >> 2)  & 0xff;
		/* ac create ef  */
		*p++ = process_acl_entry(file,SC_AC_OP_CREATE,STARCOS_AC_ALWAYS);
		/* ac create key */
		*p++ = process_acl_entry(file,SC_AC_OP_CREATE,STARCOS_AC_ALWAYS);
		/* ac create df  */
		*p++ = process_acl_entry(file,SC_AC_OP_CREATE,STARCOS_AC_ALWAYS);
		/* use the same ac for register df and create df */
		*p++ = data->data.mf.header[14];
		/* if sm is required use combined mode */
		if (file->acl[SC_AC_OP_CREATE] && (sc_file_get_acl_entry(file, SC_AC_OP_CREATE))->method & SC_AC_PRO)
			tmp = 0x03;	/* combinde mode */
		else
			tmp = 0x00;	/* no sm */
		*p++ = tmp;	/* use the same sm mode for all ops */
		*p++ = tmp;
		*p++ = tmp;
		data->type = SC_STARCOS_MF_DATA;

		return SC_SUCCESS;
	} else if (file->type == SC_FILE_TYPE_DF){
		p    = data->data.df.header;
		*p++ = (file->id >> 8) & 0xff;
		*p++ = file->id & 0xff;
		if (file->namelen) {
			/* copy aid */
			*p++ = file->namelen & 0xff;
			memset(p, 0, 16);
			memcpy(p, file->name, (u8)file->namelen);
			p   += 16;
		} else {
			/* (mis)use the fid as aid */
			*p++ = 2;
			memset(p, 0, 16);
			*p++ = (file->id >> 8) & 0xff;
			*p++ = file->id & 0xff;
			p   += 14;
		}
		/* guess isf size */
		*p++ = (file->size >> 10) & 0xff;	/* ISF space */
		*p++ = (file->size >> 2)  & 0xff;	/* ISF space */
		/* ac create ef  */
		*p++ = process_acl_entry(file,SC_AC_OP_CREATE,STARCOS_AC_ALWAYS);
		/* ac create key */
		*p++ = process_acl_entry(file,SC_AC_OP_CREATE,STARCOS_AC_ALWAYS);
		/* set sm byte (same for keys and ef) */
		if (file->acl[SC_AC_OP_CREATE] &&
		    (sc_file_get_acl_entry(file, SC_AC_OP_CREATE)->method &
		     SC_AC_PRO))
			tmp = 0x03;
		else
			tmp = 0x00;
		*p++ = tmp;	/* SM CR  */
		*p++ = tmp;	/* SM ISF */

		data->data.df.size[0] = (file->size >> 8) & 0xff;
		data->data.df.size[1] = file->size & 0xff;
		data->type = SC_STARCOS_DF_DATA;

		return SC_SUCCESS;
	} else if (file->type == SC_FILE_TYPE_WORKING_EF) {
		p    = data->data.ef.header;
		*p++ = (file->id >> 8) & 0xff;
		*p++ = file->id & 0xff;
		/* ac read  */
		*p++ = process_acl_entry(file, SC_AC_OP_READ,STARCOS_AC_ALWAYS);
		/* ac write */
		*p++ = process_acl_entry(file, SC_AC_OP_WRITE,STARCOS_AC_ALWAYS);
		/* ac erase */
		*p++ = process_acl_entry(file, SC_AC_OP_ERASE,STARCOS_AC_ALWAYS);
		*p++ = STARCOS_AC_ALWAYS;	/* AC LOCK     */
		*p++ = STARCOS_AC_ALWAYS;	/* AC UNLOCK   */
		*p++ = STARCOS_AC_ALWAYS;	/* AC INCREASE */
		*p++ = STARCOS_AC_ALWAYS;	/* AC DECREASE */
		*p++ = 0x00;			/* rfu         */
		*p++ = 0x00;			/* rfu         */
		/* use sm (in combined mode) if wanted */
		for (tmp = 0, r = 0; tmp != 0 && r < 4; r++)
			if (file->acl[r] && (sc_file_get_acl_entry(file, r))->method &  SC_AC_PRO)
				tmp = 0x03;
		*p++ = tmp;			/* SM byte     */
		*p++ = 0x0;			/* use the least significant 5 bits
					 	 * of the FID as SID */
		switch (file->type)
		{
		case SC_FILE_EF_TRANSPARENT:
			*p++ = 0x81;
			*p++ = (file->size >> 8) & 0xff;
			*p++ = file->size & 0xff;
			break;
		case SC_FILE_EF_LINEAR_FIXED:
			*p++ = 0x82;
			*p++ = file->record_count  & 0xff;
			*p++ = file->record_length & 0xff;
			break;
		case SC_FILE_EF_CYCLIC:
			*p++ = 0x84;
			*p++ = file->record_count  & 0xff;
			*p++ = file->record_length & 0xff;
			break;
		default:
			return SC_ERROR_INVALID_ARGUMENTS;
		}
		data->type = SC_STARCOS_EF_DATA;

		return SC_SUCCESS;
	} else
                return SC_ERROR_INVALID_ARGUMENTS;
}

/** starcos_create_mf
 * internal function to create the MF
 * \param card pointer to the sc_card structure
 * \param data pointer to a sc_starcos_create_data object
 * \return SC_SUCCESS or error code
 * 
 * This function creates the MF based on the information stored
 * in the sc_starcos_create_data.mf structure. Note: CREATE END must be
 * called separately to activate the ACs.
 */
static int starcos_create_mf(sc_card_t *card, sc_starcos_create_data *data)
{
	int    r;
	sc_apdu_t       apdu;
	sc_context_t   *ctx = card->ctx;

	if (ctx->debug >= 3)
			sc_debug(ctx, "creating MF \n");
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE0, 0x00, 0x00);
	apdu.cla |= 0x80;
	apdu.lc   = 19;
	apdu.datalen = 19;
	apdu.data = (u8 *) data->data.mf.header;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(ctx, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);	
}

/** starcos_create_df
 * internal function to create a DF
 * \param card pointer to the sc_card structure
 * \param data pointer to a sc_starcos_create_data object
 * \return SC_SUCCESS or error code
 *
 * This functions registers and creates a DF based in the information
 * stored in a sc_starcos_create_data.df data structure. Note: CREATE END must
 * be called separately to activate the ACs.
 */
static int starcos_create_df(sc_card_t *card, sc_starcos_create_data *data)
{
	int    r;
	size_t len;
	sc_apdu_t       apdu;
	sc_context_t   *ctx = card->ctx;

	if (ctx->debug >= 3)
		sc_debug(ctx, "creating DF\n");
	/* first step: REGISTER DF */
	if (ctx->debug >= 3)
		sc_debug(ctx, "calling REGISTER DF\n");

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x52,
		       data->data.df.size[0], data->data.df.size[1]);
	len  = 3 + data->data.df.header[2];
	apdu.cla |= 0x80;
	apdu.lc   = len;
	apdu.datalen = len;
	apdu.data = data->data.df.header;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(ctx, r, "APDU transmit failed");
	/* second step: CREATE DF */
	if (ctx->debug >= 3)
		sc_debug(ctx, "calling CREATE DF\n");

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE0, 0x01, 0x00);
	apdu.cla |= 0x80;
	apdu.lc   = 25;
	apdu.datalen = 25;
	apdu.data = data->data.df.header;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(ctx, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
}

/** starcos_create_ef
 * internal function to create a EF
 * \param card pointer to the sc_card structure
 * \param data pointer to a sc_starcos_create_data object
 * \return SC_SUCCESS or error code
 *
 * This function creates a EF based on the information stored in
 * the sc_starcos_create_data.ef data structure.
 */
static int starcos_create_ef(sc_card_t *card, sc_starcos_create_data *data)
{	
	int    r;
	sc_apdu_t       apdu;
	sc_context_t   *ctx = card->ctx;

	if (ctx->debug >= 3)
		sc_debug(ctx, "creating EF\n");

	sc_format_apdu(card,&apdu,SC_APDU_CASE_3_SHORT,0xE0,0x03,0x00);
	apdu.cla |= 0x80;
	apdu.lc   = 16;
	apdu.datalen = 16;
	apdu.data = (u8 *) data->data.ef.header;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
}

/** starcos_create_end
 * internal function to activate the ACs
 * \param card pointer to the sc_card structure
 * \param file pointer to a sc_file object
 * \return SC_SUCCESS or error code
 *
 * This function finishs the creation of a DF (or MF) and activates
 * the ACs.
 */
static int starcos_create_end(sc_card_t *card, sc_file_t *file)
{
	int r;
	u8  fid[2];
	sc_apdu_t       apdu;

	if (file->type != SC_FILE_TYPE_DF)
		return SC_ERROR_INVALID_ARGUMENTS;

	fid[0] = (file->id >> 8) & 0xff;
	fid[1] = file->id & 0xff;
	sc_format_apdu(card,&apdu,SC_APDU_CASE_3_SHORT, 0xE0, 0x02, 0x00);
	apdu.cla |= 0x80;
	apdu.lc   = 2;
	apdu.datalen = 2;
	apdu.data = fid;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
}

/** starcos_create_file
 * \param card pointer to the sc_card structure
 * \param file pointer to a sc_file object
 * \return SC_SUCCESS or error code
 *
 * This function creates MF, DF or EF based on the supplied
 * information in the sc_file structure (using starcos_process_acl).
 */
static int starcos_create_file(sc_card_t *card, sc_file_t *file)
{	
	int    r;
	sc_starcos_create_data data;

	SC_FUNC_CALLED(card->ctx, 1);

	if (file->type == SC_FILE_TYPE_DF) {
		if (file->id == 0x3f00) {
			/* CREATE MF */
			r = starcos_process_acl(card, file, &data);
			if (r != SC_SUCCESS)
				return r;
			return starcos_create_mf(card, &data);
		} else {
			/* CREATE DF */
			r = starcos_process_acl(card, file, &data);
			if (r != SC_SUCCESS)
				return r;
			return starcos_create_df(card, &data);
		}
	} else if (file->type == SC_FILE_TYPE_WORKING_EF) {
		/* CREATE EF */
		r = starcos_process_acl(card, file, &data);
		if (r != SC_SUCCESS)
			return r;
		return starcos_create_ef(card, &data);
	} else
		return SC_ERROR_INVALID_ARGUMENTS;
}

/** starcos_erase_card
 * internal function to restore the delivery state
 * \param card pointer to the sc_card object
 * \return SC_SUCCESS or error code
 *
 * This function deletes the MF (for 'test cards' only).
 */
static int starcos_erase_card(sc_card_t *card)
{	/* restore the delivery state */
	int r;
	u8  sbuf[2];
	sc_apdu_t apdu;

	sbuf[0] = 0x3f;
	sbuf[1] = 0x00;
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE4, 0x00, 0x00);
	apdu.cla |= 0x80;
	apdu.lc   = 2;
	apdu.datalen = 2;
	apdu.data = sbuf;
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	/* invalidate cache */
	card->cache_valid = 0;
	if (apdu.sw1 == 0x69 && apdu.sw2 == 0x85)
		/* no MF to delete, ignore error */
		return SC_SUCCESS;
	else return sc_check_sw(card, apdu.sw1, apdu.sw2);
}

#define STARCOS_WKEY_CSIZE	124

/** starcos_write_key
 * set key in isf
 * \param card pointer to the sc_card object
 * \param data pointer to a sc_starcos_wkey_data structure
 * \return SC_SUCCESS or error code
 *
 * This function installs a key header in the ISF (based on the
 * information supplied in the sc_starcos_wkey_data structure)
 * and set a supplied key (depending on the mode).
 */
static int starcos_write_key(sc_card_t *card, sc_starcos_wkey_data *data)
{
	int       r;
	u8        sbuf[SC_MAX_APDU_BUFFER_SIZE];
	const u8 *p;
	size_t    len = sizeof(sbuf), tlen, offset = 0;
	sc_apdu_t       apdu;

	if (!data->mode)	/* mode == 0 => install */
	{	/* install key header */
		sbuf[0] = 0xc1;	/* key header tag    */
		sbuf[1]	= 0x0c;	/* key header length */
		memcpy(sbuf + 2, data->key_header, 12);
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xf4,
			       data->mode, 0x00);
		apdu.cla |= 0x80;
		apdu.lc   = 14;
		apdu.datalen = 14;
		apdu.data = sbuf;

		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
			return sc_check_sw(card, apdu.sw1, apdu.sw2);
	}

	if (data->key == NULL && data->mode == 0)
		return SC_SUCCESS;

	p    = data->key;
	tlen = data->key_len;
	while (tlen)
	{	/* transmit the key in chunks of STARCOS_WKEY_CSIZE bytes */
		u8 clen = tlen < STARCOS_WKEY_CSIZE ? tlen : STARCOS_WKEY_CSIZE;
		sbuf[0] = 0xc2;
		sbuf[1] = 3 + clen;
		sbuf[2] = data->kid;
		sbuf[3] = (offset >> 8) & 0xff;
		sbuf[4] = offset & 0xff;
		memcpy(sbuf+5, p, clen);
		len     = 5 + clen;
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xf4,
			       data->mode, 0x00);
		apdu.cla    |= 0x80;
		apdu.lc      = len;
		apdu.datalen = len;
		apdu.data    = sbuf;

		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
			return sc_check_sw(card, apdu.sw1, apdu.sw2);
		offset += clen;
		p      += clen;
		tlen   -= clen;
	}
	return SC_SUCCESS;
}

/** starcos_gen_key
 * generate public key pair
 * \param card pointer to the sc_card object
 * \param data pointer to a sc_starcos_gen_key_data structure
 * \return SC_SUCCESS or error code
 *
 * This function generates a public key pair and stores the created
 * private key in the ISF (specified by the KID).
 */
static int starcos_gen_key(sc_card_t *card, sc_starcos_gen_key_data *data)
{
	int	r;
	size_t	i, len = data->key_length >> 3;
	sc_apdu_t apdu;
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 sbuf[2], *p, *q;
	/* generate key */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x46,  0x00, 
			data->key_id);
	apdu.le      = 0;
	sbuf[0] = (u8)(data->key_length >> 8);
	sbuf[1] = (u8)(data->key_length);
	apdu.data    = sbuf;
	apdu.lc      = 2;
	apdu.datalen = 2;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
		return sc_check_sw(card, apdu.sw1, apdu.sw2);
	/* read public key via READ PUBLIC KEY */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xf0,  0x9c, 0x00);
	sbuf[0]      = data->key_id;
	apdu.cla    |= 0x80;
	apdu.data    = sbuf;
	apdu.datalen = 1;
	apdu.lc      = 1;
	apdu.resp    = rbuf;
	apdu.resplen = sizeof(rbuf);
	apdu.le      = 256;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
		return sc_check_sw(card, apdu.sw1, apdu.sw2);

	data->modulus = (u8 *) malloc(len);
	if (!data->modulus)
		return SC_ERROR_OUT_OF_MEMORY;
	p = data->modulus;
	/* XXX use tags to find starting position of the modulus */
	q = &rbuf[18];
	/* LSB to MSB -> MSB to LSB */
	for (i = len; i != 0; i--)
		*p++ = q[i - 1];

	return SC_SUCCESS;
}

/** starcos_set_security_env
 * sets the security enviroment
 * \param card pointer to the sc_card object
 * \param env pointer to a sc_security_env object
 * \param se_num not used here
 * \return SC_SUCCESS on success or an error code
 *
 * This function sets the security enviroment (using the starcos spk 2.3
 * command MANAGE SECURITY ENVIROMENT). In case a COMPUTE SIGNATURE
 * operation is requested , this function tries to detect whether
 * COMPUTE SIGNATURE or INTERNAL AUTHENTICATE must be used for signature
 * calculation.
 */
static int starcos_set_security_env(sc_card_t *card,
				    const sc_security_env_t *env,
				    int se_num)
{
	u8              *p, *pp, keyID;
	int              r, operation = env->operation;
	sc_apdu_t   apdu;
	u8               sbuf[SC_MAX_APDU_BUFFER_SIZE];
	starcos_ex_data *ex_data = (starcos_ex_data *)card->drv_data;

	p     = sbuf;
	keyID = env->key_ref[0];

	/* copy key reference, if present */
	if (env->flags & SC_SEC_ENV_KEY_REF_PRESENT) {
		if (env->flags & SC_SEC_ENV_KEY_REF_ASYMMETRIC)
			*p++ = 0x83;
		else
			*p++ = 0x84;
		*p++ = env->key_ref_len;
		memcpy(p, env->key_ref, env->key_ref_len);
		p += env->key_ref_len;
	}
	pp = p;
	if (operation == SC_SEC_OPERATION_DECIPHER){
		if (env->algorithm_flags & SC_ALGORITHM_RSA_PAD_PKCS1) {
			*p++ = 0x80;
			*p++ = 0x01;
			*p++ = 0x02;
		} else
			return SC_ERROR_INVALID_ARGUMENTS;
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x81,
		               0xb8);
		apdu.data    = sbuf;
		apdu.datalen = p - sbuf;
		apdu.lc      = p - sbuf;
		apdu.le      = 0;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
			SC_FUNC_RETURN(card->ctx, 4, sc_check_sw(card, apdu.sw1, apdu.sw2));
		return SC_SUCCESS;
	}
	/* try COMPUTE SIGNATURE */
	if (operation == SC_SEC_OPERATION_SIGN && (
	    env->algorithm_flags & SC_ALGORITHM_RSA_PAD_PKCS1 ||
	    env->algorithm_flags & SC_ALGORITHM_RSA_PAD_ISO9796)) {
		if (env->flags & SC_SEC_ENV_ALG_REF_PRESENT) {
			*p++ = 0x80;
			*p++ = 0x01;
			*p++ = env->algorithm_ref & 0xFF;
		} else if (env->flags & SC_SEC_ENV_ALG_PRESENT &&
		            env->algorithm == SC_ALGORITHM_RSA) {
			/* set the method to use based on the algorithm_flags */
			*p++ = 0x80;
			*p++ = 0x01;
			if (env->algorithm_flags & SC_ALGORITHM_RSA_PAD_PKCS1) {
				if (env->algorithm_flags & SC_ALGORITHM_RSA_HASH_SHA1)
					*p++ = 0x12;
				else if (env->algorithm_flags & SC_ALGORITHM_RSA_HASH_RIPEMD160)
					*p++ = 0x22;
				else if (env->algorithm_flags & SC_ALGORITHM_RSA_HASH_MD5)
					*p++ = 0x32;
				else {
					/* can't use COMPUTE SIGNATURE =>
					 * try INTERNAL AUTHENTICATE */
					p = pp;
					operation = SC_SEC_OPERATION_AUTHENTICATE;
					goto try_authenticate;
				}
			} else if (env->algorithm_flags & SC_ALGORITHM_RSA_PAD_ISO9796) {
				if (env->algorithm_flags & SC_ALGORITHM_RSA_HASH_SHA1)
					*p++ = 0x11;
				else if (env->algorithm_flags & SC_ALGORITHM_RSA_HASH_RIPEMD160)
					*p++ = 0x21;
				else
					return SC_ERROR_INVALID_ARGUMENTS;
			} else
				return SC_ERROR_INVALID_ARGUMENTS;
		}
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x41, 0xb6);
		apdu.data    = sbuf;
		apdu.datalen = p - sbuf;
		apdu.lc      = p - sbuf;
		apdu.le      = 0;
		/* suppress errors, as don't know whether to use 
		 * COMPUTE SIGNATURE or INTERNAL AUTHENTICATE */
		sc_ctx_suppress_errors_on(card->ctx);
		r = sc_transmit_apdu(card, &apdu);
		sc_ctx_suppress_errors_off(card->ctx);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
			ex_data->fix_digestInfo = 0;
			ex_data->sec_ops        = SC_SEC_OPERATION_SIGN;
			return SC_SUCCESS;
		}
		/* reset pointer */
		p = pp;
		/* doesn't work => try next op */
		operation = SC_SEC_OPERATION_AUTHENTICATE;
	}
try_authenticate:
	/* try INTERNAL AUTHENTICATE */
	if (operation == SC_SEC_OPERATION_AUTHENTICATE && 
	    env->algorithm_flags & SC_ALGORITHM_RSA_PAD_PKCS1) {
		*p++ = 0x80;
		*p++ = 0x01;
		*p++ = 0x01;
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x41,
		               0xa4);
		apdu.data    = sbuf;
		apdu.datalen = p - sbuf;
		apdu.lc      = p - sbuf;
		apdu.le      = 0;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
			SC_FUNC_RETURN(card->ctx, 4, sc_check_sw(card, apdu.sw1, apdu.sw2));
		ex_data->fix_digestInfo = env->algorithm_flags;
		ex_data->sec_ops        = SC_SEC_OPERATION_AUTHENTICATE;
		return SC_SUCCESS;
	}

	return SC_ERROR_INVALID_ARGUMENTS;
}

static int starcos_compute_signature(sc_card_t *card,
				     const u8 * data, size_t datalen,
				     u8 * out, size_t outlen)
{
	int r;
	sc_apdu_t apdu;
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	starcos_ex_data *ex_data = (starcos_ex_data *)card->drv_data;

	if (datalen > SC_MAX_APDU_BUFFER_SIZE)
		SC_FUNC_RETURN(card->ctx, 4, SC_ERROR_INVALID_ARGUMENTS);

	if (ex_data->sec_ops == SC_SEC_OPERATION_SIGN) {
		/* compute signature with the COMPUTE SIGNATURE command */
		
		/* set the hash value     */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x2A,
			       0x90, 0x81);
		apdu.resp = rbuf;
		apdu.resplen = sizeof(rbuf);
		apdu.le = 0;
		memcpy(sbuf, data, datalen);
		apdu.data = sbuf;
		apdu.lc = datalen;
		apdu.datalen = datalen;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
			SC_FUNC_RETURN(card->ctx, 4, 
				       sc_check_sw(card, apdu.sw1, apdu.sw2));

		/* call COMPUTE SIGNATURE */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0x2A,
			       0x9E, 0x9A);
		apdu.resp = rbuf;
		apdu.resplen = sizeof(rbuf);
		apdu.le = 256;

		apdu.lc = 0;
		apdu.datalen = 0;
		apdu.sensitive = 1;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
			size_t len = apdu.resplen > outlen ? outlen : apdu.resplen;
			memcpy(out, apdu.resp, len);
			SC_FUNC_RETURN(card->ctx, 4, len);
		}
	} else if (ex_data->sec_ops == SC_SEC_OPERATION_AUTHENTICATE) {
		size_t tmp_len;
		/* call INTERNAL AUTHENTICATE */
		sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x88, 0x10, 0x00);
		/* fix/create DigestInfo structure (if necessary) */
		if (ex_data->fix_digestInfo) {
			unsigned int flags = ex_data->fix_digestInfo & SC_ALGORITHM_RSA_HASHES;
			if (flags == 0x0)
				/* XXX: assume no hash is wanted */
				flags = SC_ALGORITHM_RSA_HASH_NONE;
			tmp_len = sizeof(sbuf);
			r = sc_pkcs1_encode(card->ctx, flags, data, datalen,
					sbuf, &tmp_len, sizeof(sbuf));
			if (r < 0)
				return r;
		} else {
			memcpy(sbuf, data, datalen);
			tmp_len = datalen;
		}
		apdu.lc = tmp_len;
		apdu.data = sbuf;
		apdu.datalen = tmp_len;
		apdu.resp = rbuf;
		apdu.resplen = sizeof(rbuf);
		apdu.le = 256;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
			size_t len = apdu.resplen > outlen ? outlen : apdu.resplen;

			memcpy(out, apdu.resp, len);
			SC_FUNC_RETURN(card->ctx, 4, len);
		}
	} else
		SC_FUNC_RETURN(card->ctx, 4, SC_ERROR_INVALID_ARGUMENTS);

	/* clear old state */
	ex_data->sec_ops = 0;
	ex_data->fix_digestInfo = 0;

	SC_FUNC_RETURN(card->ctx, 4, sc_check_sw(card, apdu.sw1, apdu.sw2));
}

static int starcos_check_sw(sc_card_t *card, unsigned int sw1, unsigned int sw2)
{
	const int err_count = sizeof(starcos_errors)/sizeof(starcos_errors[0]);
	int i;

	if (card->ctx->debug >= 3)
		sc_debug(card->ctx, "sw1 = 0x%02x, sw2 = 0x%02x\n", sw1, sw2);
  
	if (sw1 == 0x90)
		return SC_NO_ERROR;
	if (sw1 == 0x63 && (sw2 & ~0x0fU) == 0xc0 )
	{
		sc_error(card->ctx, "Verification failed (remaining tries: %d)\n",
		(sw2 & 0x0f));
		return SC_ERROR_PIN_CODE_INCORRECT;
	}
  
	/* check starcos error messages */
	for (i = 0; i < err_count; i++)
		if (starcos_errors[i].SWs == ((sw1 << 8) | sw2))
		{
			sc_error(card->ctx, "%s\n", starcos_errors[i].errorstr);
			return starcos_errors[i].errorno;
		}
  
	/* iso error */
	return iso_ops->check_sw(card, sw1, sw2);
}

static int starcos_get_serialnr(sc_card_t *card, sc_serial_number_t *serial)
{
	int r;
	u8  rbuf[SC_MAX_APDU_BUFFER_SIZE];
	sc_apdu_t apdu;

	if (!serial)
		return SC_ERROR_INVALID_ARGUMENTS;
	/* see if we have cached serial number */
	if (card->serialnr.len) {
		memcpy(serial, &card->serialnr, sizeof(*serial));
		return SC_SUCCESS;
	}
	/* get serial number via GET CARD DATA */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0xf6, 0x00, 0x00);
	apdu.cla |= 0x80;
	apdu.resp = rbuf;
	apdu.resplen = sizeof(rbuf);
	apdu.le   = 256;
	apdu.lc   = 0;
	apdu.datalen = 0;
        r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
		return SC_ERROR_INTERNAL;
	/* cache serial number */
	memcpy(card->serialnr.value, apdu.resp, apdu.resplen);
	card->serialnr.len = apdu.resplen;
	/* copy and return serial number */
	memcpy(serial, &card->serialnr, sizeof(*serial));
	return SC_SUCCESS;
}

static int starcos_card_ctl(sc_card_t *card, unsigned long cmd, void *ptr)
{
	sc_starcos_create_data *tmp;

	switch (cmd)
	{
	case SC_CARDCTL_STARCOS_CREATE_FILE:
		tmp = (sc_starcos_create_data *) ptr;
		if (tmp->type == SC_STARCOS_MF_DATA)
			return starcos_create_mf(card, tmp);
		else if (tmp->type == SC_STARCOS_DF_DATA)
			return starcos_create_df(card, tmp);
		else if (tmp->type == SC_STARCOS_EF_DATA)
			return starcos_create_ef(card, tmp);
		else
			return SC_ERROR_INTERNAL;
	case SC_CARDCTL_STARCOS_CREATE_END:
		return starcos_create_end(card, (sc_file_t *)ptr);
	case SC_CARDCTL_STARCOS_WRITE_KEY:
		return starcos_write_key(card, (sc_starcos_wkey_data *)ptr);
	case SC_CARDCTL_STARCOS_GENERATE_KEY:
		return starcos_gen_key(card, (sc_starcos_gen_key_data *)ptr);
	case SC_CARDCTL_ERASE_CARD:
		return starcos_erase_card(card);
	case SC_CARDCTL_GET_SERIALNR:
		return starcos_get_serialnr(card, (sc_serial_number_t *)ptr);
	default:
		return SC_ERROR_NOT_SUPPORTED;
	}
}

static int starcos_logout(sc_card_t *card)
{
	int r;
	sc_apdu_t apdu;
	const u8 mf_buf[2] = {0x3f, 0x00};

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xA4, 0x00, 0x0C);
	apdu.le = 0;
	apdu.lc = 2;
	apdu.data    = mf_buf;
	apdu.datalen = 2;
	apdu.resplen = 0;
	
	sc_ctx_suppress_errors_on(card->ctx);
	r = sc_transmit_apdu(card, &apdu);
	sc_ctx_suppress_errors_off(card->ctx);
	SC_TEST_RET(card->ctx, r, "APDU re-transmit failed");

	if (apdu.sw1 == 0x69 && apdu.sw2 == 0x85)
		/* the only possible reason for this error here is, afaik,
		 * that no MF exists, but then there's no need to logout
		 * => return SC_SUCCESS
		 */
		return SC_SUCCESS;
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
}

static struct sc_card_driver * sc_get_driver(void)
{
	struct sc_card_driver *iso_drv = sc_get_iso7816_driver();
	if (iso_ops == NULL)
		iso_ops = iso_drv->ops;
  
	starcos_ops = *iso_drv->ops;
	starcos_ops.match_card = starcos_match_card;
	starcos_ops.init   = starcos_init;
	starcos_ops.finish = starcos_finish;
	starcos_ops.select_file = starcos_select_file;
	starcos_ops.check_sw    = starcos_check_sw;
	starcos_ops.create_file = starcos_create_file;
	starcos_ops.delete_file = NULL;
	starcos_ops.set_security_env  = starcos_set_security_env;
	starcos_ops.compute_signature = starcos_compute_signature;
	starcos_ops.card_ctl    = starcos_card_ctl;
	starcos_ops.logout      = starcos_logout;
  
	return &starcos_drv;
}

struct sc_card_driver * sc_get_starcos_driver(void)
{
	return sc_get_driver();
}
