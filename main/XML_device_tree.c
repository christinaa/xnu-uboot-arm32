/*
 * XML_device_tree.c
 * Copyright (c) 2013 Kristina Brooks
 *
 * Read an XML device tree and construct our thing
 * from ii.
 */

#include <bootkit/runtime.h>

#include <bootkit/xml.h>
#include <bootkit/device_tree.h>
#include <bootkit/mach-o/macho.h>

typedef struct {
	unsigned int node_count;
} XML_device_tree_context;

static void PopulateDeviceTreeNode(XML_device_tree_context* ctx, TagPtr tag, Node* node);
static void WalkDeviceTreeNodeChildren(XML_device_tree_context* ctx, TagPtr tag, Node* parent);

/* Array with either ints or strings to DT data. */
static void* ArrayToDeviceTreeData(TagPtr tag, uint32_t* len)
{
	TagPtr next;
	unsigned cnt = 0;
	uint8_t* buf;
	
	assert(tag);
	assert(tag->tag);
	assert(tag->type == kTagTypeArray);
	
	next = tag->tag;
	
	/* Count up first */
	while (next) {
		if (next->type == kTagTypeInteger) {
			cnt += sizeof(uint32_t);
		}
		else if (next->type == kTagTypeString) {
			cnt += strlen(next->string)+1;
		}
		else {
			panic("unknown type %d", next->type);
		}
		next = next->tagNext;
	}
	
	buf = malloc(cnt);
	next = tag->tag;
	cnt = 0;
	
	/* Populate buffer */
	while (next) {
		if (next->type == kTagTypeInteger) {
			uint32_t* buf32 = (uint32_t*)&buf[cnt];
			*buf32 = (uint32_t)next->string;
			cnt += sizeof(uint32_t);
		}
		else if (next->type == kTagTypeString) {
			size_t len = strlen(next->string)+1;
			bcopy(next->string, &buf[cnt], len);
			cnt += len;
		}
		next = next->tagNext;
	}
	
	*len = cnt;
	return (void*)buf;
}

/* Integer to DT data */
static void* IntegerToDeviceTreeData(unsigned long value, uint32_t* len)
{
	uint32_t* buf = malloc(sizeof(uint32_t));
	*buf = value;
	*len = sizeof(uint32_t);
	return (void*)buf;
}

/* String to DT data */
static void* StringToDeviceTreeData(const char* value, uint32_t* len)
{
	size_t slen = strlen(value)+1;
	void* buf = malloc(slen);
	bcopy(value, buf, slen);
	
	if (len != NULL) {
		*len = slen;
	}
	
	return (void*)buf;
}

static void WalkDeviceTreeNodeChildren(XML_device_tree_context* ctx, TagPtr tag, Node* parent)
{
	TagPtr next;
	assert(tag);
	assert(tag->tag);
	assert(tag->type == kTagTypeArray);
	
	next = tag->tag;
	
	while (next)
	{
		Node* new_node;
		
		new_node = DT__AddChild(parent, NULL);
		PopulateDeviceTreeNode(ctx, next, new_node);
		
		next = next->tagNext;
	}
}

#define CopyKey(next) ((char*)StringToDeviceTreeData(next->string, NULL))

static void PopulateDeviceTreeNode(XML_device_tree_context* ctx, TagPtr tag, Node* node)
{
	TagPtr next;
	size_t plen;
	void* pval;
	
	assert(tag->type == kTagTypeDict);
	
	next = tag->tag;
	
	/* tally up the nodes for info */
	ctx->node_count += 1;

	while (next)
	{
		assert(next->type == kTagTypeKey);

		/* next->tag has the value of the key */
		if (next->tag) {
			if (next->tag->type == kTagTypeArray) {
				if (next->string && next->string[0] == '@') {
					/* @children */
					WalkDeviceTreeNodeChildren(ctx, next->tag, node);
				}
				else {
					/* Property array */
					pval = ArrayToDeviceTreeData(next->tag, &plen);
					DT__AddProperty(node, CopyKey(next), plen, pval);
				}
			}
			else if (next->tag->type == kTagTypeInteger) {
				pval = IntegerToDeviceTreeData((uint32_t)next->tag->string, &plen);
				DT__AddProperty(node, CopyKey(next), plen, pval);
			}
			else if (next->tag->type == kTagTypeString) {
				pval = StringToDeviceTreeData(next->tag->string, &plen);
				DT__AddProperty(node, CopyKey(next), plen, pval);
			}
		}
		
		next = next->tagNext;
	}
}

/* main routine */
boolean_t parse_xml_device_tree(uint32_t base)
{
	char* buffer = (char*)base;
	long length, pos;
	TagPtr tag;
	Node* root;

	XML_device_tree_context ctx;

	pos = 0;
	
	printf(KPROC(DTRE) "parsing XML device tree at 0x%08x ...\n", base);

	/* Initialize device tree */
	DT__Initialize();
	root = DT__RootNode();
	
	while (true)
	{
		length = XMLParseNextTag(buffer + pos, &tag);
		
		if (length == -1) {
			break;
		}
		pos += length;
		if (tag == 0) {
			continue;
		}
		
		if (tag->type == kTagTypeDict) {
			/* Found it! */
			PopulateDeviceTreeNode(&ctx, tag, root);
			XMLFreeTag(tag);

			printf(KDONE "loaded XML device tree with %u nodes\n", ctx.node_count);
			
			return true;
		}

		XMLFreeTag(tag);
	}
	
	printf(KERR "root dictionary not found in the XML device tree\n");
	return false;
}