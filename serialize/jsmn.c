/*=============================================================================
Copyright (C) 2013-2017 Kristina Brooks
All rights reserved.

File     :  jsmn.c
Revision :  

*** WARNING ***
THIS IS PROVIDED UNDER GPLv3 PROVIDING THIS NOTICE IS RETAINED AND NOT ALTERED
IN ANY WAY OR FORM. THIS FILE IS FROM AN OLD AND UNMAINTAINED PROJECT SINCE
2013 AND IS NOT MODIFIED IN ANY WAY SINCE THEN ASIDE FROM THE ADDITION OF THIS
NOTICE.
*** WARNING ***

FILE DESCRIPTION
JSMN parser for JSDT, allows comments, single quotes, hexdecimals and a special
child token. This is an informal superset of JSON.

The type enum in the header should be defined as:
	typedef enum {
		JSMN_PRIMITIVE = 0,
		JSMN_OBJECT = 1,
		JSMN_ARRAY = 2,
		JSMN_STRING = 3,
		JSMN_CHILDREN_TOKEN = 4
	} jsmntype_t;
=============================================================================*/

#include <bootkit/runtime.h>

#include "jsmn.h"

#define LOG_INVAL 

/**
 * Allocates a fresh unused token from the token pull.
 */
static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser,
								   jsmntok_t *tokens, size_t num_tokens) {
	jsmntok_t *tok;
	if (parser->toknext >= num_tokens) {
		return NULL;
	}
	tok = &tokens[parser->toknext++];
	tok->start = tok->end = -1;
	tok->size = 0;
#ifdef JSMN_PARENT_LINKS
	tok->parent = -1;
#endif
	return tok;
}

/**
 * Fills token type and boundaries.
 */
static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type,
                            int start, int end) {
	token->type = type;
	token->start = start;
	token->end = end;
	token->size = 0;
}

/**
 * Fills next available token with JSON primitive.
 */
static jsmnerr_t jsmn_parse_primitive(jsmn_parser *parser, const char *js,
									  jsmntok_t *tokens, size_t num_tokens) {
	jsmntok_t *token;
	int start;
	
	start = parser->pos;
	
	for (; js[parser->pos] != '\0'; parser->pos++) {
		switch (js[parser->pos]) {
#ifndef JSMN_STRICT
				/* In strict mode primitive must be followed by "," or "}" or "]" */
			case ':':
#endif
			case '\t' : case '\r' : case '\n' : case ' ' :
			case ','  : case ']'  : case '}' :
				goto found;
		}
		if (js[parser->pos] < 32 || js[parser->pos] >= 127) {
			parser->pos = start;
			LOG_INVAL;
			return JSMN_ERROR_INVAL;
		}
	}
#ifdef JSMN_STRICT
	/* In strict mode primitive must be followed by a comma/object/array */
	parser->pos = start;
	return JSMN_ERROR_PART;
#endif
	
found:
	token = jsmn_alloc_token(parser, tokens, num_tokens);
	if (token == NULL) {
		parser->pos = start;
		return JSMN_ERROR_NOMEM;
	}
	jsmn_fill_token(token, JSMN_PRIMITIVE, start, parser->pos);
#ifdef JSMN_PARENT_LINKS
	token->parent = parser->toksuper;
#endif
	parser->pos--;
	return JSMN_SUCCESS;
}

/**
 * Filsl next token with JSON string. Want to allow both ' and ".
 */
static jsmnerr_t jsmn_parse_string(jsmn_parser *parser, const char *js,
								   jsmntok_t *tokens, size_t num_tokens, char delim) {
	jsmntok_t *token;
	
	int start = parser->pos;
	
	parser->pos++;
	
	/* Skip starting quote */
	for (; js[parser->pos] != '\0'; parser->pos++) {
		char c = js[parser->pos];
		
		/* Quote: end of string */
		if (c == delim) {
			token = jsmn_alloc_token(parser, tokens, num_tokens);
			if (token == NULL) {
				parser->pos = start;
				return JSMN_ERROR_NOMEM;
			}
			jsmn_fill_token(token, JSMN_STRING, start+1, parser->pos);
#ifdef JSMN_PARENT_LINKS
			token->parent = parser->toksuper;
#endif
			return JSMN_SUCCESS;
		}
		
		/* Backslash: Quoted symbol expected */
		if (c == '\\') {
			parser->pos++;
			switch (js[parser->pos]) {
					/* Allowed escaped symbols */
				case '\"': case '\'': case '/' : case '\\' : case 'b' :
				case 'f' : case 'r' : case 'n'  : case 't' :
					break;
					/* Allows escaped symbol \uXXXX */
				case 'u':
					/* TODO */
					break;
					/* Unexpected symbol */
				default:
					parser->pos = start;
					LOG_INVAL;
					return JSMN_ERROR_INVAL;
			}
		}
	}
	parser->pos = start;
	return JSMN_ERROR_PART;
}

unsigned int jsmn_comment_length(const char* comment_ptr) {
	unsigned int len = 0;
	while (!(comment_ptr[len] == '*' && comment_ptr[len+1] == '/')) {
		len += 1;
	}
	return len+1;
}

/**
 * Parse JSON string and fill tokens.
 */
jsmnerr_t jsmn_parse(jsmn_parser *parser, const char *js, jsmntok_t *tokens,
					 unsigned int num_tokens) {
	jsmnerr_t r;
	int i;
	jsmntok_t *token;
	
	for (; js[parser->pos] != '\0'; parser->pos++) {
		char c;
		jsmntype_t type;
		
		c = js[parser->pos];
		switch (c) {
			case '/':
			{
				unsigned int comment_len =
					jsmn_comment_length(&js[parser->pos]);
				
				parser->pos += comment_len;
				
				break;
			}
			case '@':
			{
				token = jsmn_alloc_token(parser, tokens, num_tokens);
				if (token == NULL)
					return JSMN_ERROR_NOMEM;
				
#ifdef JSMN_PARENT_LINKS
				token->parent = parser->toksuper;
#endif
				
				token->type = JSMN_CHILDREN_TOKEN;
				token->start = parser->pos;
				token->end = parser->pos;
				
				//parser->pos++;
				
				if (parser->toksuper != -1)
					tokens[parser->toksuper].size++;
				
				break;
			}
			case '{': case '[':
				token = jsmn_alloc_token(parser, tokens, num_tokens);
				if (token == NULL)
					return JSMN_ERROR_NOMEM;
				if (parser->toksuper != -1) {
					tokens[parser->toksuper].size++;
#ifdef JSMN_PARENT_LINKS
					token->parent = parser->toksuper;
#endif
				}
				token->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
				token->start = parser->pos;
				parser->toksuper = parser->toknext - 1;
				break;
			case '}': case ']':
				type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
#ifdef JSMN_PARENT_LINKS
				if (parser->toknext < 1) {
					return JSMN_ERROR_INVAL;
				}
				token = &tokens[parser->toknext - 1];
				for (;;) {
					if (token->start != -1 && token->end == -1) {
						if (token->type != type) {
							return JSMN_ERROR_INVAL;
						}
						token->end = parser->pos + 1;
						parser->toksuper = token->parent;
						break;
					}
					if (token->parent == -1) {
						break;
					}
					token = &tokens[token->parent];
				}
#else
				for (i = parser->toknext - 1; i >= 0; i--) {
					token = &tokens[i];
					if (token->start != -1 && token->end == -1) {
						if (token->type != type) {
							LOG_INVAL;
							return JSMN_ERROR_INVAL;
						}
						parser->toksuper = -1;
						token->end = parser->pos + 1;
						break;
					}
				}
				/* Error if unmatched closing bracket */
				if (i == -1) {
					LOG_INVAL;
					return JSMN_ERROR_INVAL;
				}
				for (; i >= 0; i--) {
					token = &tokens[i];
					if (token->start != -1 && token->end == -1) {
						parser->toksuper = i;
						break;
					}
				}
#endif
				break;
			case '\'': case '\"':
				r = jsmn_parse_string(parser, js, tokens, num_tokens, c);
				if (r < 0) return r;
				if (parser->toksuper != -1)
					tokens[parser->toksuper].size++;
				break;
			case '\t' : case '\r' : case '\n' : case ':' : case ',': case ' ':
				break;
#ifdef JSMN_STRICT
				/* In strict mode primitives are: numbers and booleans */
			case '-': case '0': case '1' : case '2': case '3' : case '4':
			case '5': case '6': case '7' : case '8': case '9':
			case 't': case 'f': case 'n' :
#else
				/* In non-strict mode every unquoted value is a primitive */
			default:
#endif
				r = jsmn_parse_primitive(parser, js, tokens, num_tokens);
				if (r < 0) return r;
				if (parser->toksuper != -1)
					tokens[parser->toksuper].size++;
				break;
				
#ifdef JSMN_STRICT
				/* Unexpected char in strict mode */
			default:
				LOG_INVAL;
				return JSMN_ERROR_INVAL;
#endif
				
		}
	}
	
	for (i = parser->toknext - 1; i >= 0; i--) {
		/* Unmatched opened object or array */
		if (tokens[i].start != -1 && tokens[i].end == -1) {
			return JSMN_ERROR_PART;
		}
	}
	
	return JSMN_SUCCESS;
}

/**
 * Creates a new parser based over a given  buffer with an array of tokens
 * available.
 */
void jsmn_init(jsmn_parser *parser) {
	parser->pos = 0;
	parser->toknext = 0;
	parser->toksuper = -1;
}