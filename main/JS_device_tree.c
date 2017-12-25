/*=============================================================================
Copyright (C) 2013-2017 Kristina Brooks
All rights reserved.

File     :  JS_device_tree.c
Revision :  

*** WARNING ***
THIS IS PROVIDED UNDER GPLv3 PROVIDING THIS NOTICE IS RETAINED AND NOT ALTERED
IN ANY WAY OR FORM. THIS FILE IS FROM AN OLD AND UNMAINTAINED PROJECT SINCE
2013 AND IS NOT MODIFIED IN ANY WAY SINCE THEN ASIDE FROM THE ADDITION OF THIS
NOTICE.
*** WARNING ***

FILE DESCRIPTION
JSDT Device Tree format support based on JSMN parser.
=============================================================================*/
/*
 * JS_device_tree.c
 * Copyright (c) 2013 Kristina Brooks
 *
 *  
 */

#define HOST_CODE 0

#if HOST_CODE
#include <stdio.h>
#include <string.h>

#include "HostUtils.h"
#include "jsmn.h"

#define Node int
#define PAD(x) for (int j=0; j<(*x); j++) printf("\t");
#define BASE_PTR const char*
#else
#include <bootkit/runtime.h>

#include <bootkit/device_tree.h>
#include <bootkit/mach-o/macho.h>

#include "../serialize/jsmn.h"

#define PAD(x)
#define BASE_PTR uint32_t
#endif

typedef struct {
	unsigned int node_count;
	const char* raw;
	jsmntok_t* tokens;
} Context;

#define PARSER_ERROR -1

#define NOTE_PARSE_ERROR

#define DT_INT unsigned long

static int build_node(jsmntok_t* tokens, Node* parent, Context* ctx);

static int walk_children(jsmntok_t* tokens, Node* node, Context* ctx) {
	int nested = tokens[0].size;
	int i = 1; /* skip ARRAY token */
	
	while (nested != 0) {
		int skip;
#if HOST_CODE
		Node __me = *(node)+1;
		Node* me = &__me;
#else
		Node* me = DT__AddChild(node, NULL);
#endif
		
		skip = build_node(&tokens[i], me, ctx);
		
		if (skip == PARSER_ERROR)
			return PARSER_ERROR;
		
		nested-=1; i+=skip;
	}
	
	return i;
}

static int parse_data_array(jsmntok_t* tokens, size_t* out_len, void** out_data, Context* ctx) {
	int nested = tokens[0].size;
	int i = 1;
	uint8_t* buf;
	
	unsigned int cnt = 0;
	
	while (nested != 0) {
		jsmntok_t* token = &tokens[i];
		
		if (tokens[i].type == JSMN_STRING) {
			cnt += token->end-token->start+1; /* NUL terminated */
		}
		else if (tokens[i].type == JSMN_PRIMITIVE) {
			cnt += sizeof(DT_INT);
		}
		else {
			NOTE_PARSE_ERROR;
			return PARSER_ERROR;
		}
		
		nested-=1; i+=1;
	}
	
	nested = tokens[0].size;
	i = 1;
	buf = malloc(cnt);
	cnt = 0;
	
	while (nested != 0) {
		jsmntok_t* token = &tokens[i];
		
		if (token->type == JSMN_STRING) {
			size_t len = (token->end-token->start);
			
			bcopy(&(ctx->raw[token->start]), &buf[cnt], len);
			buf[cnt+len] = '\0'; /* NUL terminate */
			
			cnt += (len + 1);
		}
		else if (token->type == JSMN_PRIMITIVE) {
			DT_INT value;
			DT_INT* bufDTINT = (DT_INT*)&buf[cnt];
			
			value = strtoul(&(ctx->raw[token->start]), NULL, 0);
			*bufDTINT = (DT_INT)value;
			
			cnt += sizeof(DT_INT);
		}
		else {
			NOTE_PARSE_ERROR;
			return PARSER_ERROR;
		}
		
		nested-=1; i+=1;
	}
	
	*out_len = cnt;
	*out_data = buf;
	
	return i;
}

static void* token_to_integer_data(jsmntok_t* token, Context* ctx, size_t* out_len)
{
	unsigned long* buf;
	DT_INT value;
	
	value = strtoul(&(ctx->raw[token->start]), NULL, 0);
	
#if HOST_CODE
	printf("0x%lx\n", value);
#endif
	
	buf = malloc(sizeof(DT_INT));
	assert(buf);
	
	*buf = value;
	*out_len = sizeof(DT_INT);
	
	return (void*)buf;
}

static void* token_to_string(jsmntok_t* token, Context* ctx, size_t* out_len)
{
	int len = token->end-token->start;
	size_t slen = len+1;
	char* buf;
	
	buf = malloc(slen);
	assert(buf);
	
	bcopy(&(ctx->raw[token->start]), buf, len);
	buf[len] = '\0';
	
	if (out_len) {
		*out_len = slen;
	}
	
	return (void*)buf;
}

static int parse_in_node_token(jsmntok_t* token, Node* node, Context* ctx) {
	jsmntok_t* key = token;
	jsmntok_t* value = token+1;
	
	size_t bloblen;
	void* blobdata;
	int ret = 2;
	
	PAD(node);
	
	/* Key */
	if (key->type == JSMN_STRING) {
		char* keystr;
		
		keystr = token_to_string(key, ctx, NULL);
		
#if HOST_CODE
		printf("%s:", keystr);
#endif
		
		/* Value */
		if (value->type == JSMN_STRING) {
			blobdata = (void*)token_to_string(value, ctx, &bloblen);
			
#if HOST_CODE
			printf("%s\n", (char*)blobdata);
#endif
		}
		else if (value->type == JSMN_PRIMITIVE) {
			blobdata = token_to_integer_data(value, ctx, &bloblen);
		}
		else if (value->type == JSMN_ARRAY) {
			
#if HOST_CODE
			printf("ARRAY(%d)\n", value->size);
#endif
			
			ret = parse_data_array(value, &bloblen, &blobdata, ctx);
			
			if (ret != PARSER_ERROR) {
				ret += 1;
			}
			else {
				return PARSER_ERROR;
			}
		}
		else {
			NOTE_PARSE_ERROR;
			return PARSER_ERROR;
		}
		
#if !HOST_CODE
		/* Insert the data into DT */
		DT__AddProperty(node, keystr, bloblen, blobdata);
#endif
	}
	else if (key->type == JSMN_CHILDREN_TOKEN) {
#if HOST_CODE
		printf("CHILDREN:");
#endif
		
		if (value->type == JSMN_ARRAY) {
#if HOST_CODE
			printf("ARRAY(%d)\n", value->size);
#endif
			ret = walk_children(value, node, ctx);
			
			if (ret != PARSER_ERROR) {
				ret += 1;
			}
			else {
				return PARSER_ERROR;
			}
		}
		else {
			NOTE_PARSE_ERROR;
			return PARSER_ERROR;
		}
	}
	else {
		NOTE_PARSE_ERROR;
		return PARSER_ERROR;
	}
	
	return ret;
}

static int build_node(jsmntok_t* tokens, Node* me, Context* ctx) {
	int nested = tokens[0].size;
	int i = 1;
	
	if (tokens[0].type != JSMN_OBJECT) {
		NOTE_PARSE_ERROR;
		return PARSER_ERROR;
	}
	
#if HOST_CODE
	PAD(me); printf("-- Node (Size=%d) --\n", nested);
#endif
	
	while (nested != 0) {
		int skip;
		
		skip = parse_in_node_token(&tokens[i], me, ctx);
		
		if (skip == PARSER_ERROR)
			return PARSER_ERROR;
		
		nested-=2; i+=skip;
	}
	
	ctx->node_count += 1;
	
	return i;
}

static int build_device_tree(Context* ctx)
{
	int ret;
	
#if HOST_CODE
	Node __me = 0;
	Node* root = &__me;
#else
	Node* root;
	
	DT__Initialize();
	root = DT__RootNode();
#endif
	
	ret = build_node(ctx->tokens, root, ctx);
	if (ret == PARSER_ERROR) {
		return 0;
	}
	return 1;
}

int parse_jsdt_device_tree(BASE_PTR raw) {
	jsmn_parser parser;
	jsmntok_t* tokens;
	int token_cnt;
	jsmnerr_t err;
	
	Context ctx;
	
	int ret;
	
#if !HOST_CODE
	printf(KPROC(DTRE) "parsing JSDT device tree at 0x%08x ...\n", raw);
#endif
	
	token_cnt = 40;
	tokens = malloc(sizeof(jsmntok_t) * token_cnt);
	
	assert(tokens);
	
parse_again:
	jsmn_init(&parser);
	err = jsmn_parse(&parser, (const char*)raw, tokens, token_cnt);
	
	if (err == JSMN_ERROR_NOMEM) {
		token_cnt += 100;
		tokens = realloc(tokens, sizeof(jsmntok_t) * token_cnt);
		
		assert(tokens);
		
		goto parse_again;
	}
	
	ctx.raw = raw;
	ctx.tokens = tokens;
	ctx.node_count = 0;
	
	if (err == 0) {
		ret = build_device_tree(&ctx);
		
#if !HOST_CODE
		if (ret) {
			/* SUCCESS */
			printf(KDONE "loaded JSDT device tree with %u nodes\n", ctx.node_count);
		}
		else {
			/* FAIL */
			printf(KERR "malformed JSDT\n");
		}
#endif
	}
	else {
#if !HOST_CODE
		printf(KERR "parse error in JSDT\n");
#endif
		ret = 0;
	}
	
	free(tokens);
	
	return ret;
}

#if HOST_CODE
int main(int argc, const char * argv[])
{
	long sz;
	char* buf;
	
	sz = HostReadFile("devicetree.sampleplatform.jsdt", &buf);
	assert(!HOST_ERR(sz));
	
	parse_jsdt_device_tree(buf);
	
    return 0;
}
#endif

